// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/drivers/cpufreq/cpufreq_performance.c
 *
 *  Copyright (C) 2002 - 2003 Dominik Brodowski <linux@brodo.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/module.h>

#ifdef CONFIG_FACTORY_FREQ_LIMIT
#define DEFAULT_BOOT_FREQ 2169600
#endif

static void cpufreq_gov_performance_limits(struct cpufreq_policy *policy)
{
#ifdef CONFIG_FACTORY_FREQ_LIMIT
	pr_err("setting to %u kHz\n", DEFAULT_BOOT_FREQ);
	__cpufreq_driver_target(policy, DEFAULT_BOOT_FREQ, CPUFREQ_RELATION_H);
#else
	pr_debug("setting to %u kHz\n", policy->max);
	__cpufreq_driver_target(policy, policy->max, CPUFREQ_RELATION_H);
#endif
}

static struct cpufreq_governor cpufreq_gov_performance = {
	.name		= "performance",
	.owner		= THIS_MODULE,
	.flags		= CPUFREQ_GOV_STRICT_TARGET,
	.limits		= cpufreq_gov_performance_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_PERFORMANCE
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &cpufreq_gov_performance;
}
#endif
#ifndef CONFIG_CPU_FREQ_GOV_PERFORMANCE_MODULE
struct cpufreq_governor *cpufreq_fallback_governor(void)
{
	return &cpufreq_gov_performance;
}
#endif

MODULE_AUTHOR("Dominik Brodowski <linux@brodo.de>");
MODULE_DESCRIPTION("CPUfreq policy governor 'performance'");
MODULE_LICENSE("GPL");

cpufreq_governor_init(cpufreq_gov_performance);
cpufreq_governor_exit(cpufreq_gov_performance);
