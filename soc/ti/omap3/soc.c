/*
 * Copyright The Zephyr Project Contributors
 * Copyright (c) 2026 Silicon Laboratories Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/arch/arm/cortex_a_r/lib_helpers.h>
#include <zephyr/arch/arm/mmu/arm_mmu.h>
#include <zephyr/devicetree.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <cmsis_core.h>

#include <soc.h>

/* Clock management, CORE power domain */
#define CM_FCLKEN1_CORE 0x48004A00U
#define CM_ICLKEN1_CORE 0x48004A10U
#define CM_IDLEST1_CORE 0x48004A20U
#define CM_FCLKEN_WKUP  0x48004C00U
#define CM_ICLKEN_WKUP  0x48004C10U
#define CM_IDLEST_WKUP  0x48004C20U
#define CM_CLKSEL_WKUP  0x48004C40U
#define CM_FCLKEN_PER   0x48005000U
#define CM_ICLKEN_PER   0x48005010U
#define CM_IDLEST_PER   0x48005020U

#define CORE_EN_I2C_ALL  (BIT(15) | BIT(16) | BIT(17))
#define CORE_EN_SPI_ALL  GENMASK(21, 18)
#define CORE_EN_HSOTGUSB BIT(4)
#define CORE_EN_MCBSP1_5 (BIT(9) | BIT(10))
#define CORE_EN_UART2    BIT(14)
#define WKUP_EN_GPT1     BIT(0)
#define WKUP_EN_GPIO1    BIT(3)
#define WKUP_EN_WDT2     BIT(5)
#define WKUP_CLKSEL_GPT1 BIT(0)
#define PER_EN_MCBSP2_4  GENMASK(2, 0)
#define PER_EN_UART3     BIT(11)
#define PER_EN_WDT3      BIT(12)
#define PER_EN_GPIO_ALL  GENMASK(17, 13)

/* Global warm reset */
#define PRM_RSTCTRL       0x48307250U
#define RSTCTRL_RST_DPLL3 BIT(2)

/* Watchdogs left running by the bootloader: MPU (WDT2) and IVA2 (WDT3) */
#define WDT2_BASE 0x48314000U
#define WDT3_BASE 0x49030000U
#define WDT_WWPS  0x34U
#define WDT_WSPR  0x48U

#define FB_NODE DT_PHANDLE(DT_NODELABEL(dispc), memory_region)

uint32_t omap3_load_addr __noinit;
uint32_t omap3_boot_sctlr __noinit;

static const struct arm_mmu_region mmu_regions[] = {
	/* Entry stub and vector table, in the page before the text region */
	MMU_REGION_FLAT_ENTRY("vectors", (uint32_t)__rom_region_start, 0x1000,
			      MT_NORMAL | MATTR_SHARED | MPERM_R | MPERM_X |
				      MATTR_CACHE_OUTER_WB_nWA | MATTR_CACHE_INNER_WB_nWA),
#if DT_NODE_HAS_PROP(DT_NODELABEL(dispc), memory_region)
	/* Scanned out by the display controller: not cached */
	MMU_REGION_FLAT_ENTRY("framebuffer", DT_REG_ADDR(FB_NODE), DT_REG_SIZE(FB_NODE),
			      MT_NORMAL | MPERM_R | MPERM_W),
#endif
	MMU_REGION_FLAT_ENTRY("l4_core", 0x48000000, 0x1000000,
			      MT_DEVICE | MPERM_R | MPERM_W | MATTR_MAY_MAP_L1_SECTION),
	MMU_REGION_FLAT_ENTRY("l4_per", 0x49000000, 0x100000,
			      MT_DEVICE | MPERM_R | MPERM_W | MATTR_MAY_MAP_L1_SECTION),
};

const struct arm_mmu_config mmu_config = {
	.num_regions = ARRAY_SIZE(mmu_regions),
	.mmu_regions = mmu_regions,
};

/* Address 0 is boot ROM: point VBAR at the vector table in the image */
void relocate_vector_table(void)
{
	write_sctlr(read_sctlr() & ~SCTLR_V_Msk);
	write_vbar((uint32_t)_vector_start);
	barrier_isync_fence_full();
}

/* Bounded so that a module in a powered-down domain cannot hang the boot */
#define IDLEST_POLL_LIMIT 100000U

static void clocks_enable(mm_reg_t fclken, mm_reg_t iclken, mm_reg_t idlest, uint32_t mask)
{
	sys_set_bits(fclken, mask);
	sys_set_bits(iclken, mask);
	for (uint32_t i = 0U; i < IDLEST_POLL_LIMIT; i++) {
		if ((sys_read32(idlest) & mask) == 0U) {
			break;
		}
	}
}

static void wdt_stop(mm_reg_t base)
{
	sys_write32(0xAAAAU, base + WDT_WSPR);
	while (sys_read32(base + WDT_WWPS) != 0U) {
	}
	sys_write32(0x5555U, base + WDT_WSPR);
	while (sys_read32(base + WDT_WWPS) != 0U) {
	}
}

/*
 * The bootloader may leave the MMU and caches in any state. Run from
 * physical addresses with clean, invalid caches until z_arm_mmu_init()
 * turns them on.
 */
void soc_reset_hook(void)
{
	uint32_t sctlr = read_sctlr();

	OMAP3_BOOT_STAGE(2, 0xFFFF00);
	omap3_boot_sctlr = sctlr;

	if ((sctlr & SCTLR_C_Msk) != 0U) {
		L1C_CleanInvalidateDCacheAll();
	} else {
		L1C_InvalidateDCacheAll();
	}

	/* Also no alignment checking and no TEX remap, as the arch code expects */
	sctlr &= ~(SCTLR_M_Msk | SCTLR_C_Msk | SCTLR_I_Msk | SCTLR_A_Msk | SCTLR_TRE_Msk);
	write_sctlr(sctlr);
	barrier_isync_fence_full();

	L1C_InvalidateICacheAll();
	MMU_InvalidateTLB();

	clocks_enable(CM_FCLKEN_WKUP, CM_ICLKEN_WKUP, CM_IDLEST_WKUP, WKUP_EN_WDT2);
	wdt_stop(WDT2_BASE);
	clocks_enable(CM_FCLKEN_PER, CM_ICLKEN_PER, CM_IDLEST_PER, PER_EN_WDT3);
	wdt_stop(WDT3_BASE);
	OMAP3_BOOT_STAGE(3, 0x00FF00);
}

#if defined(CONFIG_SOC_OMAP3_BOOT_STAGES)
void soc_prep_hook(void)
{
	OMAP3_BOOT_STAGE(4, 0x00FFFF);
}
#endif

void soc_early_init_hook(void)
{
	OMAP3_BOOT_STAGE_LATE(0xFF00FF);

	/* GPTIMER1 runs from SYS_CLK */
	sys_set_bits(CM_CLKSEL_WKUP, WKUP_CLKSEL_GPT1);
	clocks_enable(CM_FCLKEN_WKUP, CM_ICLKEN_WKUP, CM_IDLEST_WKUP, WKUP_EN_GPT1);
	clocks_enable(CM_FCLKEN_PER, CM_ICLKEN_PER, CM_IDLEST_PER, PER_EN_UART3);
	clocks_enable(CM_FCLKEN_WKUP, CM_ICLKEN_WKUP, CM_IDLEST_WKUP, WKUP_EN_GPIO1);
	clocks_enable(CM_FCLKEN_PER, CM_ICLKEN_PER, CM_IDLEST_PER, PER_EN_GPIO_ALL);
	clocks_enable(CM_FCLKEN1_CORE, CM_ICLKEN1_CORE, CM_IDLEST1_CORE, CORE_EN_I2C_ALL);
	clocks_enable(CM_FCLKEN1_CORE, CM_ICLKEN1_CORE, CM_IDLEST1_CORE, CORE_EN_SPI_ALL);
	clocks_enable(CM_FCLKEN1_CORE, CM_ICLKEN1_CORE, CM_IDLEST1_CORE, CORE_EN_MCBSP1_5);
	clocks_enable(CM_FCLKEN_PER, CM_ICLKEN_PER, CM_IDLEST_PER, PER_EN_MCBSP2_4);
	/* UART2 goes to the Bluetooth controller */
	if (DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(uart2))) {
		clocks_enable(CM_FCLKEN1_CORE, CM_ICLKEN1_CORE, CM_IDLEST1_CORE, CORE_EN_UART2);
	}
	/* The USB controller has no functional clock; the ULPI PHY clocks it */
	sys_set_bits(CM_ICLKEN1_CORE, CORE_EN_HSOTGUSB);

	if (IS_ENABLED(CONFIG_SOC_OMAP3_DSS_CONSOLE)) {
		omap3_dss_console_init();
	}
}

/* Both reset types trigger the global DPLL3 reset, as Linux does */
void sys_arch_reboot(int type)
{
	ARG_UNUSED(type);

	sys_write32(sys_read32(PRM_RSTCTRL) | RSTCTRL_RST_DPLL3, PRM_RSTCTRL);
	/* Read back so the write reaches the PRM */
	(void)sys_read32(PRM_RSTCTRL);

	for (;;) {
		arch_cpu_idle();
	}
}
