// SPDX-License-Identifier: GPL-2.0
/*
 * nap.c — Neural Adaptive Predictor cpuidle governor
 *
 * A machine-learning-based cpuidle governor that uses a small MLP trunk and an
 * ordinal survival head to predict, per idle-state boundary, the probability
 * that the upcoming idle reaches that state's target_residency.  The decision
 * layer picks the deepest feasible state whose calibrated survival meets a
 * confidence level.  Weights are Xavier-initialized at boot, then refined via
 * online learning (deferred backpropagation with SGD).
 *
 * IMPORTANT: This file is compiled WITHOUT FPU flags (normal kernel
 * compilation, still -mgeneral-regs-only).  All floating-point code lives in
 * nap_fpu.c and nap_nn_sclr.c, which are compiled with the FPU restriction
 * lifted and are only invoked from within kernel_neon_begin()/
 * kernel_neon_end() blocks.  This separation ensures the compiler cannot emit
 * FP instructions in governor callbacks (nap_select, nap_reflect, etc.),
 * which would corrupt userspace FP register state.
 */

#include <linux/cpuidle.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/math64.h>
#include <linux/percpu.h>
#include <linux/sched/clock.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/tick.h>
#include <asm/neon.h>
#include <asm/simd.h>

#include "nap.h"

/**************************************************************
 * Version Information:
 */

#define CPUIDLE_NAP_PROGNAME "Nap CPUIdle Governor"
#define CPUIDLE_NAP_AUTHOR   "Masahito Suzuki"

#define CPUIDLE_NAP_VERSION  "0.5.0"

/* Governor defaults */
#define NAP_DEFAULT_LR_MILLTHS    1     /* 0.001 = 1 millths */
#define NAP_DEFAULT_INTERVAL      4     /* learn every 4 reflects */
#define NAP_DEFAULT_CLAMP_MILLTHS 1000  /* 1.0 = 1000 millths */
#define NAP_DEFAULT_CONF_MILLTHS  500   /* 0.5 = balanced survival confidence */

/* Residency threshold (see 6.18 governors/gov.h) */
#define RESIDENCY_THRESHOLD_NS (15 * NSEC_PER_USEC)

/* ================================================================
 * Per-CPU data
 * ================================================================ */

DEFINE_PER_CPU(struct nap_cpu_data, nap_data);
static struct cpuidle_driver *nap_cached_drv;

/* ================================================================
 * Reflect-time updates (integer-only, no FPU needed)
 * ================================================================ */

static void nap_history_update(struct nap_cpu_data *d, u64 measured_ns)
{
	d->history[d->hist_idx] = measured_ns;
	d->hist_idx = (d->hist_idx + 1) % NAP_HISTORY_SIZE;
	if (d->hist_count < NAP_HISTORY_SIZE)
		d->hist_count++;
}

static void nap_update_external_signals(struct nap_cpu_data *d)
{
	d->prev_idle_exit = local_clock();
}

/* ================================================================
 * Governor callbacks
 * ================================================================ */

static int nap_fallback_heuristic(struct cpuidle_driver *drv,
				  struct cpuidle_device *dev)
{
	s64 latency_req = nap_latency_req(dev->cpu);
	ktime_t delta_tick;
	u64 sleep_length_ns;
	int i;

	sleep_length_ns = ktime_to_ns(tick_nohz_get_sleep_length(&delta_tick));

	for (i = drv->state_count - 1; i > 0; i--) {
		if (dev->states_usage[i].disable)
			continue;
		if (nap_state_exit_latency_ns(&drv->states[i]) > latency_req)
			continue;
		if (nap_state_target_residency_ns(&drv->states[i]) > sleep_length_ns)
			continue;
		return i;
	}
	return 0;
}

/*
 * Return the shallowest enabled C-state that satisfies the current
 * latency request, or 0 if none exists (the shallowest state is the
 * only option on platforms without a POLL state).  Does not consult the NN.
 */
static int nap_find_min_valid_state(struct cpuidle_driver *drv,
				    struct cpuidle_device *dev,
				    s64 latency_req)
{
	int i;

	for (i = 1; i < drv->state_count; i++) {
		if (dev->states_usage[i].disable)
			continue;
		if (nap_state_exit_latency_ns(&drv->states[i]) > latency_req)
			continue;
		return i;
	}
	return 0;
}

/*
 * Cached wrapper around nap_find_min_valid_state().  Invalidated when
 * latency_req changes (immediate PM QoS propagation) or every
 * NAP_MIN_STATE_REFRESH_JIFFIES (bounded staleness for rare sysfs /
 * runtime-driver state-disable events).  Hot-path cost when valid:
 * one s64 compare plus one time_after() check.
 */
static inline int nap_get_min_valid_state(struct nap_cpu_data *d,
					  struct cpuidle_driver *drv,
					  struct cpuidle_device *dev,
					  s64 latency_req)
{
	if (unlikely(latency_req != d->cached_min_state_latency ||
		     time_after(jiffies,
				d->cached_min_state_jiffies +
				NAP_MIN_STATE_REFRESH_JIFFIES))) {
		d->cached_min_state = nap_find_min_valid_state(drv, dev,
							       latency_req);
		d->cached_min_state_latency = latency_req;
		d->cached_min_state_jiffies = jiffies;
	}
	return d->cached_min_state;
}

static int nap_select(struct cpuidle_driver *drv,
		      struct cpuidle_device *dev,
		      bool *stop_tick)
{
	struct nap_cpu_data *d = this_cpu_ptr(&nap_data);
	s64 latency_req;
	ktime_t delta_tick;
	u64 sleep_length_ns;
	int idx, min_state;

	if (unlikely(drv->state_count <= 1))
		return 0;

	latency_req = nap_latency_req(dev->cpu);
	sleep_length_ns = ktime_to_ns(tick_nohz_get_sleep_length(&delta_tick));
	min_state = nap_get_min_valid_state(d, drv, dev, latency_req);

	/*
	 * Fast path: when no C-state can amortize its target residency
	 * within the predicted sleep length, the answer is deterministically
	 * the shallowest state.  Skip NN inference and feature extraction
	 * entirely; nap_reflect also skips the feedback path for
	 * short-circuited events (see the short_circuited check there).
	 */
	if (min_state == 0 ||
	    sleep_length_ns <
	    nap_state_target_residency_ns(&drv->states[min_state])) {
		*stop_tick = false;
		d->last_selected_idx = 0;
		d->short_circuited = true;
		d->stats.total_selects++;
		return 0;
	}

	d->short_circuited = false;

	if (likely(may_use_simd())) {
		kernel_neon_begin();
		idx = nap_fpu_select(drv, dev, d);
		kernel_neon_end();

		if (idx < 0)
			idx = nap_fallback_heuristic(drv, dev);
	} else {
		idx = nap_fallback_heuristic(drv, dev);
	}

	*stop_tick = (nap_state_target_residency_ns(&drv->states[idx]) >
		      RESIDENCY_THRESHOLD_NS);

	d->last_selected_idx = idx;
	d->stats.total_selects++;

	return idx;
}

static void nap_reflect(struct cpuidle_device *dev, int index)
{
	struct nap_cpu_data *d = this_cpu_ptr(&nap_data);
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	u64 measured_ns = (u64)dev->last_residency * NSEC_PER_USEC;

	if (unlikely(!drv))
		return;

	/*
	 * Short-circuited fast path: the NN was not invoked for this idle, so
	 * the residency is not part of its training distribution and must not
	 * feed the floor histogram or the weight update.  Account only the
	 * aggregate residency and return.
	 */
	if (d->short_circuited) {
		d->stats.total_residency_ns += measured_ns;
		return;
	}

	nap_history_update(d, measured_ns);

	d->last_prediction_error = d->last_predicted_ns - (s64)measured_ns;
	nap_update_external_signals(d);

	/* Every idle provides a fresh residency for the floor and reliability EMAs */
	d->learn_actual_ns = measured_ns;
	d->have_sample = true;

	/*
	 * Throttle the expensive trunk/score weight update with a dual
	 * gate: the per-N-reflect counter AND a jiffies floor.  The time
	 * gate caps the learning rate on workloads with very rapid idle
	 * bursts (e.g. cross-CPU ping-pong); learn_jiffies_min == 0
	 * disables it and restores counter-only behavior.
	 */
	if (++d->learn_counter >= d->learn_interval &&
	    time_after_eq(jiffies,
			  d->last_learn_jiffies + d->learn_jiffies_min)) {
		d->learn_counter = 0;
		d->last_learn_jiffies = jiffies;
		d->needs_learn = true;
	}

	d->stats.total_residency_ns += measured_ns;
	if (index > 0 &&
	    measured_ns < nap_state_target_residency_ns(&drv->states[index]))
		d->stats.overshoot_count++;
}

static int nap_enable(struct cpuidle_driver *drv,
		      struct cpuidle_device *dev)
{
	struct nap_cpu_data *d = per_cpu_ptr(&nap_data, dev->cpu);

	memset(d, 0, sizeof(*d));

	/*
	 * Defer weight initialization to the first nap_select() FPU path
	 * via reset_pending.  nap_enable() is called from cpuidle core
	 * (cpuidle_enable_device) which may run on a different CPU than
	 * dev->cpu during governor switch.  Deferring ensures FPU init
	 * happens on the correct CPU in its own idle context.
	 */
	WRITE_ONCE(nap_cached_drv, drv);
	d->learning_rate_millths  = NAP_DEFAULT_LR_MILLTHS;
	d->learn_interval = NAP_DEFAULT_INTERVAL;
	d->max_grad_norm_millths  = NAP_DEFAULT_CLAMP_MILLTHS;
	d->conf_millths = NAP_DEFAULT_CONF_MILLTHS;

	/*
	 * Force a first-call refresh of the min-valid-state cache:
	 * cached_min_state_latency = S64_MIN guarantees the first
	 * nap_select() comparison trips the invalidation branch.
	 */
	d->cached_min_state_latency = S64_MIN;
	d->cached_min_state_jiffies = jiffies - NAP_MIN_STATE_REFRESH_JIFFIES;
	d->learn_jiffies_min = 1;

	d->reset_pending = true;

	return 0;
}

static void nap_disable(struct cpuidle_driver *drv,
			struct cpuidle_device *dev)
{
	WRITE_ONCE(nap_cached_drv, NULL);
}

/* ================================================================
 * sysfs interface  (/sys/devices/system/cpu/nap/)
 * ================================================================ */

static ssize_t stats_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	int cpu, len = 0;
	u64 total_sel = 0, total_res = 0, total_under = 0, total_learn = 0;

	for_each_online_cpu(cpu) {
		struct nap_cpu_data *d = &per_cpu(nap_data, cpu);

		total_sel   += d->stats.total_selects;
		total_res   += d->stats.total_residency_ns;
		total_under += d->stats.overshoot_count;
		total_learn += d->stats.learn_count;
	}

	len += sysfs_emit_at(buf, len, "total_selects: %llu\n", total_sel);
	len += sysfs_emit_at(buf, len, "total_residency_ms: %llu\n",
			     div_u64(total_res, NSEC_PER_MSEC));
	len += sysfs_emit_at(buf, len, "overshoot_count: %llu\n", total_under);
	len += sysfs_emit_at(buf, len, "overshoot_rate_permil: %llu\n",
			     total_sel ? div_u64(total_under * 1000, total_sel) : 0);
	len += sysfs_emit_at(buf, len, "learn_count: %llu\n", total_learn);
	return len;
}

static ssize_t learning_rate_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	int cpu;

	cpu = cpumask_first(cpu_online_mask);
	if (cpu >= nr_cpu_ids)
		return sysfs_emit(buf, "0\n");
	return sysfs_emit(buf, "%u\n",
			  per_cpu(nap_data, cpu).learning_rate_millths);
}

static ssize_t learning_rate_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned int val;
	int cpu;

	if (kstrtouint(buf, 10, &val) || val == 0 || val > 100)
		return -EINVAL;

	for_each_online_cpu(cpu)
		per_cpu(nap_data, cpu).learning_rate_millths = val;

	return count;
}

static ssize_t learn_interval_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	int cpu;

	cpu = cpumask_first(cpu_online_mask);
	if (cpu >= nr_cpu_ids)
		return sysfs_emit(buf, "0\n");
	return sysfs_emit(buf, "%d\n",
			  per_cpu(nap_data, cpu).learn_interval);
}

static ssize_t learn_interval_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned int val;
	int cpu;

	if (kstrtouint(buf, 10, &val) || val == 0 || val > 10000)
		return -EINVAL;

	for_each_online_cpu(cpu)
		per_cpu(nap_data, cpu).learn_interval = val;

	return count;
}

static ssize_t reset_weights_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	cpumask_var_t mask;
	int cpu;

	if (!READ_ONCE(nap_cached_drv))
		return -ENODEV;

	/*
	 * Set a per-CPU flag; each CPU will reinitialize its own weights
	 * inside nap_select() within its own kernel_neon_begin/end context.
	 * This avoids cross-CPU data races on the weight arrays.
	 *
	 * Accepts "all" to reset every online CPU, or a cpulist
	 * (e.g. "0-3,5,7") to reset specific CPUs.
	 */
	if (sysfs_streq(buf, "all")) {
		for_each_online_cpu(cpu)
			per_cpu(nap_data, cpu).reset_pending = true;
		pr_info("nap: weight reset scheduled for all CPUs\n");
		return count;
	}

	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;

	if (cpulist_parse(buf, mask)) {
		free_cpumask_var(mask);
		return -EINVAL;
	}

	for_each_cpu_and(cpu, mask, cpu_online_mask)
		per_cpu(nap_data, cpu).reset_pending = true;

	pr_info("nap: weight reset scheduled for CPUs %*pbl\n",
		cpumask_pr_args(mask));
	free_cpumask_var(mask);
	return count;
}

static ssize_t reset_stats_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	int cpu;

	for_each_online_cpu(cpu)
		memset(&per_cpu(nap_data, cpu).stats, 0,
		       sizeof(struct nap_stats));

	return count;
}

/*
 * confidence: decision confidence level in millths (1..999, default 500).
 * Higher demands more certainty before entering a deeper state, biasing toward
 * responsiveness (shallower); lower biases toward energy (deeper).  This is the
 * single responsiveness dial and replaces the former overshoot_pctl target.
 */
static ssize_t confidence_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	int cpu;

	cpu = cpumask_first(cpu_online_mask);
	if (cpu >= nr_cpu_ids)
		return sysfs_emit(buf, "0\n");
	return sysfs_emit(buf, "%u\n",
			  per_cpu(nap_data, cpu).conf_millths);
}

static ssize_t confidence_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int val;
	int cpu;

	if (kstrtouint(buf, 10, &val) || val == 0 || val >= 1000)
		return -EINVAL;

	for_each_online_cpu(cpu)
		per_cpu(nap_data, cpu).conf_millths = val;

	return count;
}

static ssize_t version_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", CPUIDLE_NAP_VERSION);
}

static ssize_t simd_show(struct kobject *kobj,
			 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "scalar-neon\n");
}

static struct kobj_attribute version_attr        = __ATTR_RO(version);
static struct kobj_attribute simd_attr           = __ATTR_RO(simd);
static struct kobj_attribute stats_attr          = __ATTR_RO(stats);
static struct kobj_attribute learning_rate_attr  = __ATTR_RW(learning_rate);
static struct kobj_attribute learn_interval_attr = __ATTR_RW(learn_interval);
static struct kobj_attribute confidence_attr     = __ATTR_RW(confidence);
static struct kobj_attribute reset_weights_attr  = __ATTR_WO(reset_weights);
static struct kobj_attribute reset_stats_attr    = __ATTR_WO(reset_stats);

static struct attribute *nap_attrs[] = {
	&version_attr.attr,
	&simd_attr.attr,
	&stats_attr.attr,
	&learning_rate_attr.attr,
	&learn_interval_attr.attr,
	&confidence_attr.attr,
	&reset_weights_attr.attr,
	&reset_stats_attr.attr,
	NULL,
};

static const struct attribute_group nap_attr_group = {
	.attrs = nap_attrs,
};

static struct kobject *cpuidle_kobj;

int nap_sysfs_init(void)
{
	int ret;

	/*
	 * 4.14 has no bus_get_dev_root(); cpu_subsys.dev_root is populated
	 * by subsys_system_register() in cpu_dev_init(), which runs well
	 * before this postcore_initcall.
	 */
	cpuidle_kobj = kobject_create_and_add("nap",
					      &cpu_subsys.dev_root->kobj);
	if (!cpuidle_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(cpuidle_kobj, &nap_attr_group);
	if (ret) {
		kobject_put(cpuidle_kobj);
		cpuidle_kobj = NULL;
	}
	return ret;
}

void nap_sysfs_exit(void)
{
	if (cpuidle_kobj) {
		sysfs_remove_group(cpuidle_kobj, &nap_attr_group);
		kobject_put(cpuidle_kobj);
		cpuidle_kobj = NULL;
	}
}

/* ================================================================
 * Governor registration
 * ================================================================ */

static struct cpuidle_governor nap_governor = {
	.name    = "nap",
	.rating  = 26,
	.enable  = nap_enable,
	.disable = nap_disable,
	.select  = nap_select,
	.reflect = nap_reflect,
};

static int __init nap_init(void)
{
	int ret;

	ret = nap_sysfs_init();
	if (ret)
		pr_warn("nap: sysfs init failed: %d (continuing without sysfs)\n", ret);

	ret = cpuidle_register_governor(&nap_governor);
	if (ret) {
		pr_err("nap: register_governor failed: %d\n", ret);
		nap_sysfs_exit();
		return ret;
	}

	pr_info("%s v%s by %s registered (rating=%u, scalar/NEON)\n",
	       CPUIDLE_NAP_PROGNAME, CPUIDLE_NAP_VERSION,
	       CPUIDLE_NAP_AUTHOR, nap_governor.rating);
	return 0;
}
postcore_initcall(nap_init);