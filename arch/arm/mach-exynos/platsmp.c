/* linux/arch/arm/mach-exynos4/platsmp.c
 *
 * Copyright (c) 2010-2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Cloned from linux/arch/arm/mach-vexpress/platsmp.c
 *
 *  Copyright (C) 2002 ARM Ltd.
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/init.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/smp.h>
#include <linux/io.h>

#include <asm/cacheflush.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>
#include <asm/firmware.h>
#include <asm/cputype.h>

#include <mach/hardware.h>
#include <mach/regs-clock.h>
#include <mach/regs-pmu.h>
#include <mach/pmu.h>

#include <plat/cpu.h>

#include "common.h"

extern void exynos4_secondary_startup(void);

static inline void __iomem *cpu_boot_reg_base(void)
{
	if (soc_is_exynos4210() && samsung_rev() == EXYNOS4210_REV_1_1)
		return EXYNOS_INFORM5;
	return S5P_VA_SYSRAM;
}

static inline void __iomem *cpu_boot_reg(int phys_cpu)
{
	void __iomem *boot_reg;
	unsigned int cluster, core;

	core = MPIDR_AFFINITY_LEVEL(phys_cpu, 0);
	cluster = MPIDR_AFFINITY_LEVEL(phys_cpu, 1);

	boot_reg = cpu_boot_reg_base();
	boot_reg += cluster ? 0 : 0x10;

	if (soc_is_exynos4412() || soc_is_exynos5430() || soc_is_exynos5433()
		|| soc_is_exynos7420())
		boot_reg += 4 * core;

	return boot_reg;
}

/*
 * Write pen_release in a way that is guaranteed to be visible to all
 * observers, irrespective of whether they're taking part in coherency
 * or not.  This is necessary for the hotplug code to work reliably.
 */
static void write_pen_release(int val)
{
	pen_release = val;
	smp_wmb();
	__cpuc_flush_dcache_area((void *)&pen_release, sizeof(pen_release));
	outer_clean_range(__pa(&pen_release), __pa(&pen_release + 1));
}

static void __iomem *scu_base_addr(void)
{
	return (void __iomem *)(S5P_VA_SCU);
}

static DEFINE_SPINLOCK(boot_lock);

static void __cpuinit exynos_secondary_init(unsigned int cpu)
{
	/*
	 * let the primary processor know we're out of the
	 * pen, then head off into the C entry point
	 */
	write_pen_release(-1);

	clear_boot_flag(cpu, HOTPLUG);

	/*
	 * Synchronise with the boot thread.
	 */
	spin_lock(&boot_lock);
	spin_unlock(&boot_lock);
}

static int exynos_power_up_cpu(unsigned int phys_cpu)
{
	unsigned int timeout;

	if (exynos_cpu.power_state(phys_cpu)) {
		printk(KERN_WARNING "%s: Already enabled core power.\n",
			 __func__);
		return 0;
	}

	exynos_cpu.power_up(phys_cpu);

	/* wait max 10 ms until cpu is on */
	timeout = 10;
	while (timeout) {
		if (exynos_cpu.power_state(phys_cpu))
			break;

		mdelay(1);
		timeout--;

		if (timeout == 0) {
			printk(KERN_ERR "cpu%d power up failed\n", phys_cpu);
			return -ETIMEDOUT;
		}
	}

#if !defined(CONFIG_SOC_EXYNOS5433)
#define A7_RESET_NUM	3

	if (phys_cpu < 4) {
		if (soc_is_exynos5430()) {
			u32 val;
			int i;

			for (i = 0; i < A7_RESET_NUM; i++) {
				while(!__raw_readl(EXYNOS_PMU_SPARE2))
					udelay(10);

				udelay(10);

				val = __raw_readl(EXYNOS_ARM_CORE_STATUS(4 + phys_cpu));
				val |= (0xF << 8);
				__raw_writel(val, EXYNOS_ARM_CORE_STATUS(4 + phys_cpu));

				pr_debug("cpu%d: SWRESEET\n", phys_cpu);

				if (i < A7_RESET_NUM -1)
					__raw_writel(0, EXYNOS_PMU_SPARE2);
				__raw_writel((0x1 << 1), EXYNOS_ARM_CORE_RESET(4 + phys_cpu));
			}
		} else if (soc_is_exynos5422()) {
			u32 val;
#ifndef CONFIG_SOC_EXYNOS7420

			while(!__raw_readl(EXYNOS_PMU_SPARE2))
				udelay(10);

			udelay(10);
#endif

			printk(KERN_DEBUG "cpu%d: SWRESET\n", phys_cpu);

			val = ((1 << 20) | (1 << 8)) << phys_cpu;
			__raw_writel(val, EXYNOS_SWRESET);
		}
	}
#endif

	return 0;
}

static int __cpuinit exynos_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned long timeout;
	unsigned long phys_cpu = cpu_logical_map(cpu);
	int ret;

	/*
	 * Set synchronisation state between this boot processor
	 * and the secondary one
	 */
	spin_lock(&boot_lock);

	/*
	 * The secondary processor is waiting to be released from
	 * the holding pen - release it, then wait for it to flag
	 * that it has been released by resetting pen_release.
	 *
	 * Note that "pen_release" is the hardware CPU ID, whereas
	 * "cpu" is Linux's internal ID.
	 */
	write_pen_release(phys_cpu);

	ret = exynos_power_up_cpu(cpu);
	if (ret) {
		spin_unlock(&boot_lock);
		return ret;
	}

	/*
	 * Send the secondary CPU a soft interrupt, thereby causing
	 * the boot monitor to read the system wide flags register,
	 * and branch to the address found there.
	 */

	timeout = jiffies + (1 * HZ);
	while (time_before(jiffies, timeout)) {
		unsigned long boot_addr;

		smp_rmb();

		boot_addr = virt_to_phys(exynos4_secondary_startup);

		/*
		 * Try to set boot address using firmware first
		 * and fall back to boot register if it fails.
		 */
		if (call_firmware_op(set_cpu_boot_addr, phys_cpu, boot_addr))
			__raw_writel(boot_addr, cpu_boot_reg(phys_cpu));

		if (soc_is_exynos5433() || soc_is_exynos5430() || soc_is_exynos5422()
			|| soc_is_exynos7420()) {
			dsb_sev();
		} else {
			call_firmware_op(cpu_boot, phys_cpu);
			arch_send_wakeup_ipi_mask(cpumask_of(cpu));
		}

		if (pen_release == -1)
			break;

		udelay(10);
	}

	/*
	 * now the secondary core is starting up let it run its
	 * calibrations, then wait for it to finish
	 */
	spin_unlock(&boot_lock);

	return pen_release != -1 ? -ENOSYS : 0;
}

/*
 * Initialise the CPU possible map early - this describes the CPUs
 * which may be present or become present in the system.
 */

static void __init exynos_smp_init_cpus(void)
{
	void __iomem *scu_base = scu_base_addr();
	unsigned int i, ncores;

	if (read_cpuid_part_number() == ARM_CPU_PART_CORTEX_A9)
		ncores = scu_base ? scu_get_core_count(scu_base) : 1;
	else
		/*
		 * CPU Nodes are passed thru DT and set_cpu_possible
		 * is set by "arm_dt_init_cpu_maps".
		 */
		return;

	/* sanity check */
	if (ncores > nr_cpu_ids) {
		pr_warn("SMP: %u cores greater than maximum (%u), clipping\n",
			ncores, nr_cpu_ids);
		ncores = nr_cpu_ids;
	}

	for (i = 0; i < ncores; i++)
		set_cpu_possible(i, true);
}

static void __init exynos_smp_prepare_cpus(unsigned int max_cpus)
{
	int i;

	exynos_firmware_init();

	/*
	 * Write the address of secondary startup into the
	 * system-wide flags register. The boot monitor waits
	 * until it receives a soft interrupt, and then the
	 * secondary CPU branches to this address.
	 *
	 * Try using firmware operation first and fall back to
	 * boot register if it fails.
	 */
	for (i = 1; i < max_cpus; ++i) {
		unsigned long phys_cpu;
		unsigned long boot_addr;

		phys_cpu = cpu_logical_map(i);
		boot_addr = virt_to_phys(exynos4_secondary_startup);

		if (call_firmware_op(set_cpu_boot_addr, phys_cpu, boot_addr))
			__raw_writel(boot_addr, cpu_boot_reg(phys_cpu));
	}

	if (soc_is_exynos5430() || soc_is_exynos5433()
		|| soc_is_exynos7420()) {
		void __iomem *noncpu_config_reg;
		unsigned int tmp;
		unsigned int cluster
			= MPIDR_AFFINITY_LEVEL(cpu_logical_map(0), 1);

		if (cluster)
			noncpu_config_reg = EXYNOS5430_KFC_NONCPU_CONFIGURATION;
		else
			noncpu_config_reg = EXYNOS5430_EAGLE_NONCPU_CONFIGURATION;

		tmp = __raw_readl(EXYNOS5430_EAGLE_NONCPU_CONFIGURATION);
		tmp |= EXYNOS_CORE_INIT_WAKEUP_FROM_LOWPWR;
		__raw_writel(tmp, EXYNOS5430_EAGLE_NONCPU_CONFIGURATION);

		if (soc_is_exynos7420()) {
			tmp = __raw_readl(EXYNOS7420_ATLAS_DBG_CONFIGURATION);
			tmp |= (3 << 16);
			__raw_writel(tmp, EXYNOS7420_ATLAS_DBG_CONFIGURATION);
		}

		do {
			tmp = __raw_readl(noncpu_config_reg + 0x4);
		} while ((tmp & 0xf) != 0xf);
	}

	BUG_ON(exynos_cpu.power_up == NULL);
}

struct smp_operations exynos_smp_ops __initdata = {
	.smp_init_cpus		= exynos_smp_init_cpus,
	.smp_prepare_cpus	= exynos_smp_prepare_cpus,
	.smp_secondary_init	= exynos_secondary_init,
	.smp_boot_secondary	= exynos_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die		= exynos_cpu_die,
#endif
};
