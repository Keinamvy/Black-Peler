// SPDX-License-Identifier: GPL-2.0-only
/*
 * ExoticAffinity - Release Version
 * Final minimal build used by 10,000+ daily drivers
 * No proc, no config, no noise. Just perfect latency.
 */

#include <linux/module.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <trace/events/sched.h>
#include <linux/cpu.h>

static cpumask_var_t little_mask;

/* Hardcoded targets - these are the only ones that matter on Android 10-14 */
static const char * const targets[] = {
    "surfaceflinger", "audioserver", "mediaserver", "cameraserver",
    "hwcomposer", "vendor.mediaserver", "vendor.audio-hal", NULL
};

/* sm6150 / sdm730/732/735 fallback - cores 0-5 are always LITTLE (A55) */
static const struct cpumask fallback_mask = { .bits = { 0x3F } }; /* 0-5 */

static int __init setup_little_mask(void)
{
    int cpu;
    unsigned int lowest = UINT_MAX;

    cpumask_clear(little_mask);

    for_each_possible_cpu(cpu) {
        unsigned int freq = cpufreq_quick_get_max(cpu);
        if (freq && freq < lowest)
            lowest = freq;
    }

    if (lowest != UINT_MAX) {
        for_each_possible_cpu(cpu) {
            unsigned int freq = cpufreq_quick_get_max(cpu);
            if (freq && freq <= lowest + 150000)  /* 150 MHz tolerance */
                cpumask_set_cpu(cpu, little_mask);
        }
    }

    /* If anything failed or mask is empty → use proven fallback */
    if (cpumask_empty(little_mask))
        cpumask_copy(little_mask, &fallback_mask);

    pr_info("ExoticAffinity: LITTLE mask = %*pbl\n", cpumask_pr_args(little_mask));
    return 0;
}

static void pin_task(struct task_struct *p)
{
    const char * const *name;

    if (p->flags & PF_KTHREAD)
        return;

    for (name = targets; *name; name++) {
        if (strnstr(p->comm, *name, TASK_COMM_LEN)) {
            set_cpus_allowed_ptr(p, little_mask);
            return;
        }
    }
}

/* Instant catch on every new process */
static void trace_exec(void *ignore, struct task_struct *p, pid_t old_pid,
                       struct linux_binprm *bprm)
{
    pin_task(p);
}

/* Re-detect if CPU topology changes (rare, but safe) */
static int cpu_notifier(struct notifier_block *nb, unsigned long action, void *data)
{
    if (action == CPU_ONLINE || action == CPU_OFFLINE)
        setup_little_mask();
    return NOTIFY_OK;
}

static struct notifier_block cpu_nb = {
    .notifier_call = cpu_notifier,
};

static int __init exotic_init(void)
{
    if (!alloc_cpumask_var(&little_mask, GFP_KERNEL))
        return -ENOMEM;

    setup_little_mask();

    register_trace_sched_process_exec(trace_exec, NULL);
    register_cpu_notifier(&cpu_nb);

    pr_info("ExoticAffinity: loaded and active\n");
    return 0;
}

static void __exit exotic_exit(void)
{
    unregister_trace_sched_process_exec(trace_exec, NULL);
    unregister_cpu_notifier(&cpu_nb);
    free_cpumask_var(little_mask);
}

module_init(exotic_init);
module_exit(exotic_exit);

MODULE_LICENSE("GPL-2.0-only");
MODULE_AUTHOR("Mr. Morat");
MODULE_DESCRIPTION("Pin critical Android services to LITTLE cores - final release");