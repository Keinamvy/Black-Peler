// drivers/thermal/exotic_thermal.c
// SPDX-License-Identifier: GPL-2.0

#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/devfreq_cooling.h>
#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>

static unsigned int poll_interval_ms = 30000;
static unsigned int load_threshold = 15;
static unsigned int temp_threshold = 420;        // decidegrees
static unsigned int load_window_ms = 5000;       // not used now (instant snapshot is better)
static bool require_temp = true;
static unsigned int hysteresis_short_sec = 60;
static unsigned int hysteresis_long_sec = 180;
static unsigned int hysteresis_delta = 40;       // 4.0°C

module_param(poll_interval_ms, uint, 0644);
module_param(load_threshold, uint, 0644);
module_param(temp_threshold, uint, 0644);
module_param(require_temp, bool, 0644);
module_param(hysteresis_short_sec, uint, 0644);
module_param(hysteresis_long_sec, uint, 0644);
module_param(hysteresis_delta, uint, 0644);

static struct delayed_work exotic_work;
static unsigned long last_restore_jiffies;
static unsigned int current_hysteresis_sec = 60;

static int get_battery_temp(void)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int ret;

	psy = power_supply_get_by_name("battery");
	if (!psy)
		return INT_MAX;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TEMP, &val);
	power_supply_put(psy);

	return ret ? INT_MAX : val.intval;  // already in 0.1°C units
}

static unsigned int get_recent_cpu_load(void)
{
	unsigned long busy, total;
	int cpu;

	busy = total = 0;
	for_each_online_cpu(cpu) {
		unsigned long u = kcpustat_this_cpu_cpu(cpu).cpustat[CPUTIME_USER];
		unsigned long n = kcpustat_this_cpu_cpu(cpu).cpustat[CPUTIME_NICE];
		unsigned long s = kcpustat_this_cpu_cpu(cpu).cpustat[CPUTIME_SYSTEM];
		unsigned long i = kcpustat_this_cpu_cpu(cpu).cpustat[CPUTIME_IDLE];
		unsigned long io = kcpustat_this_cpu_cpu(cpu).cpustat[CPUTIME_IOWAIT];

		busy += u + n + s;
		total += u + n + s + i + io;
	}

	if (total == 0)
		return 100;

	return (busy * 100) / total;
}

static void exotic_thermal_work(struct work_struct *work)
{
	int temp = get_battery_temp();
	unsigned int load = get_recent_cpu_load();
	bool should_restore = false;

	/* Temperature check */
	if (require_temp && temp >= temp_threshold)
		goto reschedule;

	/* Adaptive hysteresis */
	if (temp >= temp_threshold - 40)  // 4°C delta
		current_hysteresis_sec = 180;
	else
		current_hysteresis_sec = 60;

	if (time_before(jiffies, last_restore_jiffies +
			msecs_to_jiffies(current_hysteresis_sec * 1000)))
		goto reschedule;

	/* Load check */
	if (load < load_threshold)
		goto reschedule;

	should_restore = true;

	/* Restore max freq on all policies */
	if (should_restore) {
		struct cpufreq_policy *policy;
		unsigned int cpu;

		for_each_possible_cpu(cpu) {
			policy = cpufreq_cpu_get(cpu);
			if (!policy)
				continue;

			if (policy->max < policy->cpuinfo.max_freq) {
				cpufreq_enable_fast_switch(policy);
				cpufreq_freq_transition_begin(policy, policy->cpuinfo.max_freq);
				policy->user_policy.max = policy->max = policy->cpuinfo.max_freq;
				cpufreq_freq_transition_end(policy, policy->cpuinfo.max_freq, 0);
				__cpufreq_driver_target(policy, policy->max, CPUFREQ_RELATION_H);
			}
			cpufreq_cpu_put(policy);
		}

		last_restore_jiffies = jiffies;
	}

reschedule:
	schedule_delayed_work(&exotic_work,
			      msecs_to_jiffies(poll_interval_ms));
}

static int __init exotic_thermal_init(void)
{
	INIT_DELAYED_WORK(&exotic_work, exotic_thermal_work);
	schedule_delayed_work(&exotic_work, msecs_to_jiffies(10000));
	pr_info("ExoticThermal Adaptive loaded - safe in-tree version\n");
	return 0;
}

static void __exit exotic_thermal_exit(void)
{
	cancel_delayed_work_sync(&exotic_work);
}

module_init(exotic_thermal_init);
module_exit(exotic_thermal_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Morat Engine");
MODULE_DESCRIPTION("In-tree safe big-core frequency restore with adaptive hysteresis");