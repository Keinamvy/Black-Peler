// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/workqueue.h>
#include <linux/thermal.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/cpu.h>

extern unsigned int kstat_irqs_cpu(unsigned int irq, int cpu);

#define BALANCE_INTERVAL_LIGHT_MS   8000
#define BALANCE_INTERVAL_HEAVY_MS  30000
#define IRQ_DELTA_THRESHOLD        1200
#define MAX_MIGRATE_PER_RUN        12
#define MAX_CPU_TEMP_ALLOWED       68

static bool exoticbalance_enabled = true;
module_param(exoticbalance_enabled, bool, 0644);

static struct delayed_work balance_work;
static unsigned int *cpu_irq_prev;
static unsigned int *cpu_irq_delta;

// SM6150: big cores = 4,5,6,7
static inline bool is_big_cpu(int cpu) { return cpu >= 4; }

static const char *const irq_blacklist[] = {
    "arch_timer", "IPI", "Rescheduling", "Function call",
    "mdss", "dsi", "mdp", "sde", "kgsl", "adreno", "gpu",
    "synaptics", "goodix", "fts", "touch", "input",
    "ufshcd", "ufs", "sdc", "bam-dmux", "qcom-sps",
    "wlan", "ath", "ipa", "rmnet", "qrtr", "modem",
    "pmic", "qpnp-", "smb", "bms", "tsens", "thermal", NULL
};

static bool is_irq_blacklisted(struct irq_desc *desc)
{
    const char *name;
    int i;

    if (!desc || !desc->action || !desc->action->name)
        return true;

    name = desc->action->name;
    for (i = 0; irq_blacklist[i]; i++) {
        if (strstr(name, irq_blacklist[i]))
            return true;
    }
    return false;
}

static int get_cpu_temperature(void)
{
    struct thermal_zone_device *tz;
    int temp = 0;
    const char *zones[] = {
        "cpu-thermal", "cpu0-thermal", "cpu1-thermal",
        "tsens_tz_sensor9", "tsens_tz_sensor10", NULL
    };
    int i;

    for (i = 0; zones[i]; i++) {
        tz = thermal_zone_get_zone_by_name(zones[i]);
        if (!IS_ERR_OR_NULL(tz) && tz->ops && tz->ops->get_temp) {
            if (tz->ops->get_temp(tz, &temp) == 0)
                return temp / 1000;
        }
    }
    return 0;
}

static void exotic_balance_work(struct work_struct *work)
{
    cpumask_t online_mask;
    int cpu;
    unsigned int max_delta = 0, min_delta = UINT_MAX;
    int max_cpu = -1, min_cpu = -1;
    int migrated = 0;
    unsigned int irq;
    struct irq_desc *desc;

    if (!exoticbalance_enabled)
        goto reschedule;

    cpumask_clear(&online_mask);
    for_each_online_cpu(cpu)
        cpumask_set_cpu(cpu, &online_mask);

    /* Reset deltas */
    for_each_cpu(cpu, &online_mask)
        cpu_irq_delta[cpu] = 0;

    /* Count IRQ delta per CPU */
    for_each_irq_desc(irq, desc) {
        if (is_irq_blacklisted(desc))
            continue;

        for_each_cpu(cpu, &online_mask) {
            unsigned int count = kstat_irqs_cpu(irq, cpu);
            if (count >= cpu_irq_prev[cpu])
                cpu_irq_delta[cpu] += count - cpu_irq_prev[cpu];
            cpu_irq_prev[cpu] = count;
        }
    }

    /* Find hottest and coolest CPU */
    for_each_cpu(cpu, &online_mask) {
        unsigned int delta = cpu_irq_delta[cpu];
        if (delta > max_delta) { max_delta = delta; max_cpu = cpu; }
        if (delta < min_delta) { min_delta = delta; min_cpu = cpu; }
    }

    /* Migrate only from big → little when worth it */
    if (max_cpu >= 0 && min_cpu >= 0 && max_cpu != min_cpu &&
        (max_delta - min_delta) >= IRQ_DELTA_THRESHOLD &&
        is_big_cpu(max_cpu) && !is_big_cpu(min_cpu) &&
        get_cpu_temperature() <= MAX_CPU_TEMP_ALLOWED) {

        for_each_irq_desc(irq, desc) {
            const struct cpumask *affinity;
            cpumask_var_t new_mask;

            if (migrated >= MAX_MIGRATE_PER_RUN)
                break;
            if (is_irq_blacklisted(desc))
                continue;
            if (!irq_can_set_affinity(irq))
                continue;

            affinity = irq_get_affinity_mask(irq);
            if (!affinity)
                continue;

            if (!alloc_cpumask_var(&new_mask, GFP_ATOMIC))
                continue;

            cpumask_copy(new_mask, affinity);

            if (cpumask_test_cpu(max_cpu, new_mask) && !cpumask_test_cpu(min_cpu, new_mask)) {
                cpumask_set_cpu(min_cpu, new_mask);
                irq_set_affinity(irq, new_mask);
                migrated++;
            }

            free_cpumask_var(new_mask);
        }
    }

    pr_info("ExoticBalance: CPU%d→CPU%d | %u→%u irqs | moved:%d | %d°C\n",
            max_cpu, min_cpu, max_delta, min_delta, migrated, get_cpu_temperature());

reschedule:
    unsigned long delay = BALANCE_INTERVAL_LIGHT_MS;
    if (idle_cpu(0) || idle_cpu(1) || idle_cpu(2) || idle_cpu(3))
        delay = BALANCE_INTERVAL_HEAVY_MS;

    schedule_delayed_work(&balance_work, msecs_to_jiffies(delay));
}

static int __init exoticbalance_init(void)
{
    int size = num_possible_cpus() * sizeof(unsigned int);

    cpu_irq_prev  = kzalloc(size, GFP_KERNEL);
    cpu_irq_delta = kzalloc(size, GFP_KERNEL);
    if (!cpu_irq_prev || !cpu_irq_delta) {
        kfree(cpu_irq_prev);
        kfree(cpu_irq_delta);
        return -ENOMEM;
    }

    INIT_DELAYED_WORK(&balance_work, exotic_balance_work);
    schedule_delayed_work(&balance_work, msecs_to_jiffies(10000));

    pr_info("ExoticBalance Hybrid loaded – SM6150 ready\n");
    return 0;
}

static void __exit exoticbalance_exit(void)
{
    cancel_delayed_work_sync(&balance_work);
    kfree(cpu_irq_prev);
    kfree(cpu_irq_delta);
    pr_info("ExoticBalance Hybrid unloaded\n");
}

module_init(exoticbalance_init);
module_exit(exoticbalance_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tobrut Exotic");
MODULE_DESCRIPTION("Safe, one-shot IRQ balancer for big.LITTLE – Morat Engine");