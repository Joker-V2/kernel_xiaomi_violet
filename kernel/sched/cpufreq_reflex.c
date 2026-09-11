// SPDX-License-Identifier: GPL-2.0
/*
 * Reflex CPUFreq governor (based on schedutil)
 * Copyright (C) 2026 Masahito Suzuki
 *
 * schedutil + idle-time accounting based hispeed floor with PELT-
 * complementary exponential decay.
 *
 * Frequency scaling is identical to schedutil (including the 1.25×
 * DVFS headroom) except for the hispeed blend:
 *
 * On each observation window, real CPU busy% is measured from
 * kcpustat idle-time counters.  This is blended with PELT util:
 *
 *   blended = pelt + (hispeed - pelt) >> half_lives
 *
 * The blend decays with PELT's 32 ms half-life: every 32 ms, the
 * hispeed contribution halves while PELT fills the same gap,
 * keeping total coverage at ~100%.  After ~320 ms hispeed is
 * negligible and PELT-based proportional scaling takes full control.
 */

#include "sched.h"

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/irq_work.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched/cpufreq.h>
#include <linux/slab.h>
#include <linux/tick.h>

#include <uapi/linux/sched/types.h>

/**************************************************************
 * Version Information:
 */

#define CPUFREQ_REFLEX_PROGNAME "Reflex CPUFreq Governor"
#define CPUFREQ_REFLEX_AUTHOR   "Masahito Suzuki"

#define CPUFREQ_REFLEX_VERSION  "0.3.0"

/**************************************************************
 * Default tunables
 */
#define CPUFREQ_REFLEX_DEFAULT_HISPEED_WINDOW_US   4000
#define CPUFREQ_REFLEX_DEFAULT_HISPEED_FILTER_SHIFT   1


#define IOWAIT_BOOST_MIN	(SCHED_CAPACITY_SCALE / 8)

struct rfx_tunables {
	struct gov_attr_set	attr_set;
	unsigned int		rate_limit_us;
	unsigned int		hispeed_window_us;    /* observation window (usec) */
	unsigned int		hispeed_filter_shift; /* EWMA down-ramp shift (0=off) */
};

struct rfx_policy {
	struct cpufreq_policy	*policy;

	struct rfx_tunables	*tunables;
	struct list_head	tunables_hook;

	raw_spinlock_t		update_lock;
	u64			last_freq_update_time;
	s64			freq_update_delay_ns;
	unsigned int		next_freq;
	unsigned int		cached_raw_freq;

	/* The next fields are only needed if fast switch cannot be used: */
	struct			irq_work irq_work;
	struct			kthread_work work;
	struct			mutex work_lock;
	struct			kthread_worker worker;
	struct task_struct	*thread;
	bool			work_in_progress;

	bool			limits_changed;
	bool			need_freq_update;
};

struct rfx_cpu {
	struct update_util_data	update_util;
	struct rfx_policy	*rfx_policy;
	unsigned int		cpu;

	/* I/O wait boost (schedutil-compatible) */
	bool			iowait_boost_pending;
	unsigned int		iowait_boost;
	u64			last_update;

	unsigned long		util;
	unsigned long		bw_min;

	/* Idle-time accounting for hispeed decisions */
	u64			prev_idle_time;
	u64			prev_wall_time;
	unsigned int		busy_pct;
	unsigned int		filtered_busy_pct;
	bool			hispeed_active;
	u64			hispeed_start_ns;
	s32			log_hispeed;	  /* hispeed_util in log32fpmax_corr */
	unsigned int		hispeed_idle_windows;

	/* The field below is for single-CPU policies only: */
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long		saved_idle_calls;
#endif
};

static DEFINE_PER_CPU(struct rfx_cpu, rfx_cpu);

/*
 * Minimal intfp log-domain helpers for hispeed decay.
 * Specialized from intfp.h (v1.4) for u32 <-> log32fpmax_corr.
 *
 * log32fpmax for u32: sign=1, exponent=5, mantissa(ofp)=26 bits.
 * log2(0.97857206) = -1/32 = -2097152 in Q5.26 fixed-point.
 */
#define RFX_LOG_OFP		26
#define RFX_LOG_0		S32_MIN
#define RFX_LOG_DECAY_PER_MS	(-2097152)	/* log2(0.97857206) in Q5.26 */
#define RFX_LOG_DECAY_MAX_MS	320		/* 10 half-lives */

static const u16 rfx_enc_corr_lut[256] = {
	    0,    89,   177,   264,   350,   436,   521,   606,   690,   773,   855,   937,  1018,  1098,  1178,  1257,
	 1335,  1413,  1489,  1565,  1641,  1716,  1790,  1863,  1936,  2008,  2079,  2150,  2219,  2289,  2357,  2425,
	 2492,  2558,  2624,  2689,  2753,  2817,  2880,  2942,  3004,  3065,  3125,  3184,  3243,  3301,  3358,  3415,
	 3471,  3526,  3581,  3635,  3688,  3740,  3792,  3843,  3894,  3943,  3992,  4041,  4088,  4135,  4182,  4227,
	 4272,  4316,  4360,  4402,  4444,  4486,  4526,  4566,  4606,  4644,  4682,  4719,  4756,  4792,  4827,  4861,
	 4895,  4928,  4960,  4992,  5023,  5053,  5083,  5112,  5140,  5167,  5194,  5220,  5245,  5270,  5294,  5317,
	 5340,  5362,  5383,  5404,  5423,  5443,  5461,  5479,  5496,  5512,  5528,  5543,  5557,  5570,  5583,  5596,
	 5607,  5618,  5628,  5637,  5646,  5654,  5661,  5668,  5674,  5679,  5683,  5687,  5690,  5693,  5695,  5696,
	 5696,  5696,  5695,  5693,  5690,  5687,  5683,  5679,  5674,  5668,  5661,  5654,  5646,  5637,  5628,  5618,
	 5607,  5596,  5583,  5570,  5557,  5543,  5528,  5512,  5496,  5479,  5461,  5443,  5423,  5404,  5383,  5362,
	 5340,  5317,  5294,  5270,  5245,  5220,  5194,  5167,  5140,  5112,  5083,  5053,  5023,  4992,  4960,  4928,
	 4895,  4861,  4827,  4792,  4756,  4719,  4682,  4644,  4606,  4566,  4526,  4486,  4444,  4402,  4360,  4316,
	 4272,  4227,  4182,  4135,  4088,  4041,  3992,  3943,  3894,  3843,  3792,  3740,  3688,  3635,  3581,  3526,
	 3471,  3415,  3358,  3301,  3243,  3184,  3125,  3065,  3004,  2942,  2880,  2817,  2753,  2689,  2624,  2558,
	 2492,  2425,  2357,  2289,  2219,  2150,  2079,  2008,  1936,  1863,  1790,  1716,  1641,  1565,  1489,  1413,
	 1335,  1257,  1178,  1098,  1018,   937,   855,   773,   690,   606,   521,   436,   350,   264,   177,    89,
};

static const u16 rfx_dec_corr_lut[256] = {
	    0,    88,   175,   261,   346,   431,   516,   599,   682,   764,   846,   926,  1006,  1086,  1165,  1243,
	 1320,  1397,  1473,  1548,  1622,  1696,  1770,  1842,  1914,  1985,  2056,  2125,  2194,  2263,  2331,  2398,
	 2464,  2530,  2595,  2659,  2722,  2785,  2848,  2909,  2970,  3030,  3090,  3148,  3206,  3264,  3321,  3377,
	 3432,  3487,  3541,  3594,  3646,  3698,  3750,  3800,  3850,  3899,  3948,  3995,  4042,  4089,  4135,  4180,
	 4224,  4268,  4311,  4353,  4394,  4435,  4476,  4515,  4554,  4592,  4630,  4666,  4702,  4738,  4773,  4807,
	 4840,  4873,  4905,  4936,  4966,  4996,  5026,  5054,  5082,  5109,  5136,  5161,  5186,  5211,  5235,  5258,
	 5280,  5302,  5323,  5343,  5362,  5381,  5400,  5417,  5434,  5450,  5466,  5480,  5494,  5508,  5521,  5533,
	 5544,  5555,  5565,  5574,  5582,  5590,  5598,  5604,  5610,  5615,  5620,  5623,  5626,  5629,  5631,  5632,
	 5632,  5632,  5631,  5629,  5626,  5623,  5620,  5615,  5610,  5604,  5598,  5590,  5582,  5574,  5565,  5555,
	 5544,  5533,  5521,  5508,  5494,  5480,  5466,  5450,  5434,  5417,  5400,  5381,  5362,  5343,  5323,  5302,
	 5280,  5258,  5235,  5211,  5186,  5161,  5136,  5109,  5082,  5054,  5026,  4996,  4966,  4936,  4905,  4873,
	 4840,  4807,  4773,  4738,  4702,  4666,  4630,  4592,  4554,  4515,  4476,  4435,  4394,  4353,  4311,  4268,
	 4224,  4180,  4135,  4089,  4042,  3995,  3948,  3899,  3850,  3800,  3750,  3698,  3646,  3594,  3541,  3487,
	 3432,  3377,  3321,  3264,  3206,  3148,  3090,  3030,  2970,  2909,  2848,  2785,  2722,  2659,  2595,  2530,
	 2464,  2398,  2331,  2263,  2194,  2125,  2056,  1985,  1914,  1842,  1770,  1696,  1622,  1548,  1473,  1397,
	 1320,  1243,  1165,  1086,  1006,   926,   846,   764,   682,   599,   516,   431,   346,   261,   175,    88,
};

/*
 * rfx_lin_to_log - convert u32 to corrected log32fpmax (Q5.26)
 *
 * Specialized from intfp u32_to_log32fpmax_corr().
 */
static inline s32 rfx_lin_to_log(u32 v)
{
	u8 clz;
	u32 m, mf;
	u8 idx;

	if (!v)
		return RFX_LOG_0;

	clz = __builtin_clz(v);
	m = (v << clz) >> (32 - 1 - RFX_LOG_OFP);
	mf = m & ((1U << RFX_LOG_OFP) - 1);
	idx = (u8)(mf >> (RFX_LOG_OFP - 8));
	m += (u32)rfx_enc_corr_lut[idx] << (RFX_LOG_OFP - 16);

	return (s32)(((u32)(30 - clz) << RFX_LOG_OFP) + m);
}

/*
 * rfx_log_to_lin - convert corrected log32fpmax (Q5.26) back to u32
 *
 * Specialized from intfp log32fpmax_to_u32_corr().
 */
static inline u32 rfx_log_to_lin(s32 v)
{
	bool negative;
	s32 e;
	u32 m, norm, mh;
	u8 idx;

	if (v == RFX_LOG_0)
		return 0;

	negative = v < 0;
	if (negative)
		v = -v;
	e = v >> RFX_LOG_OFP;
	if (negative)
		e = -e;

	if (e < 0)
		return 0;
	if (e >= 32)
		return U32_MAX;

	m = v & ((1U << RFX_LOG_OFP) - 1);
	norm = (1U << 31) | (m << (31 - RFX_LOG_OFP));
	mh = m << (31 - RFX_LOG_OFP);
	idx = (u8)(mh >> (31 - 8));
	norm -= (u32)rfx_dec_corr_lut[idx] << (31 - 16);

	return norm >> (31 - e);
}

/************************ Governor internals ***********************/

static bool rfx_should_update_freq(struct rfx_policy *rfx_pol, u64 time)
{
	s64 delta_ns;

	if (cpumask_first(rfx_pol->policy->cpus) != smp_processor_id())
		return false;

	if (unlikely(READ_ONCE(rfx_pol->limits_changed))) {
		WRITE_ONCE(rfx_pol->limits_changed, false);
		rfx_pol->need_freq_update = true;
		smp_mb();
		return true;
	} else if (rfx_pol->need_freq_update) {
		return true;
	}

	delta_ns = time - rfx_pol->last_freq_update_time;

	return delta_ns >= rfx_pol->freq_update_delay_ns;
}

static bool rfx_update_next_freq(struct rfx_policy *rfx_pol, u64 time,
				 unsigned int next_freq)
{
	if (rfx_pol->need_freq_update) {
		rfx_pol->need_freq_update = false;
		if (rfx_pol->limits_changed)
			return false;
	} else if (rfx_pol->next_freq == next_freq) {
		return false;
	}

	rfx_pol->next_freq = next_freq;
	rfx_pol->last_freq_update_time = time;

	return true;
}

static void rfx_deferred_update(struct rfx_policy *rfx_pol)
{
	if (!rfx_pol->work_in_progress) {
		rfx_pol->work_in_progress = true;
		irq_work_queue(&rfx_pol->irq_work);
	}
}

static inline unsigned int map_util_freq(unsigned long util,
                                         unsigned int freq,
                                         unsigned long max)
{
        return (freq + (freq >> 2)) * util / max;
}

static unsigned int rfx_get_next_freq(struct rfx_policy *rfx_pol,
				      unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int freq;

	if (arch_scale_freq_invariant())
		freq = policy->cpuinfo.max_freq;
	else
		freq = policy->cur + (policy->cur >> 2);

	freq = map_util_freq(util, freq, max);

	if (freq == rfx_pol->cached_raw_freq && !rfx_pol->need_freq_update)
		return rfx_pol->next_freq;

	rfx_pol->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

/*
 * Get CPU utilization via exported helper (identical to schedutil).
 */
static void rfx_get_util(struct rfx_cpu *rfx_c, unsigned long boost)
{
	struct rq *rq = cpu_rq(rfx_c->cpu);
	sched_avg_update(rq);
	unsigned long util = boosted_cpu_util(rfx_c->cpu, NULL);
	unsigned long max_cap = arch_scale_cpu_capacity(NULL, rfx_c->cpu);
	
	rfx_c->bw_min = 0;
	rfx_c->util = max(util, boost);
}

/************************ Hispeed (idle-time accounting) ***********************/

/*
 * Update busy% using kernel idle-time accounting (kcpustat).
 * This gives the raw CPU busy ratio without PELT smoothing.
 */
static void rfx_update_busy_pct(struct rfx_cpu *rfx_c,
				unsigned int window_us,
				unsigned int filter_shift, u64 time,
				unsigned long max_cap)
{
	u64 cur_idle, cur_wall;
	unsigned int wall_delta, idle_delta;

	cur_idle = get_cpu_idle_time(rfx_c->cpu, &cur_wall, 1);
	wall_delta = (unsigned int)(cur_wall - rfx_c->prev_wall_time);

	if (wall_delta >= window_us) {
		/*
		 * Phase 1: Window expired.  Reset busy_pct and request
		 * an immediate measurement on the next callback.
		 *
		 * Do NOT touch hispeed_start_ns or hispeed_idle_windows
		 * here: the momentary busy_pct=0 is a two-phase
		 * measurement artifact, not a genuine idle signal.
		 */
		rfx_c->busy_pct = 0;
		rfx_c->hispeed_active = true;
		rfx_c->prev_idle_time = cur_idle;
		rfx_c->prev_wall_time = cur_wall;
		return;
	}

	/*
	 * Within the current window.  Skip unless hispeed_active is
	 * set, which requests an immediate post-reset measurement.
	 */
	if (!rfx_c->hispeed_active)
		return;

	/* Phase 2: immediate post-reset measurement. */
	rfx_c->hispeed_active = false;

	if (cur_idle > rfx_c->prev_idle_time)
		idle_delta = (unsigned int)(cur_idle - rfx_c->prev_idle_time);
	else
		idle_delta = 0;

	if (wall_delta > idle_delta)
		rfx_c->busy_pct = 100 * (wall_delta - idle_delta) / wall_delta;
	else
		rfx_c->busy_pct = 0;

	rfx_c->prev_idle_time = cur_idle;
	rfx_c->prev_wall_time = cur_wall;

	/*
	 * Asymmetric EWMA filter on busy_pct:
	 *   Up:   instant tracking  (filtered = measured)
	 *   Down: slow ramp-down    (filtered -= (filtered - measured) >> shift)
	 *
	 * shift=0 disables the filter.  Higher shift = slower decay.
	 */
	if (!filter_shift || rfx_c->busy_pct >= rfx_c->filtered_busy_pct) {
		rfx_c->filtered_busy_pct = rfx_c->busy_pct;
	} else {
		unsigned int step =
			(rfx_c->filtered_busy_pct - rfx_c->busy_pct)
			>> filter_shift;
		if (step)
			rfx_c->filtered_busy_pct -= step;
		else
			rfx_c->filtered_busy_pct = rfx_c->busy_pct;
	}

	/*
	 * Hispeed decay timer tracking with one-window grace period.
	 *
	 * The decay timer (hispeed_start_ns) is only reset after two
	 * consecutive idle windows.  This prevents rapid idle/busy
	 * spinning from constantly resetting the timer, which would
	 * keep the hispeed weight at 1.0 and defeat decay.
	 */
	if (rfx_c->filtered_busy_pct > 0) {
		rfx_c->hispeed_idle_windows = 0;
		if (!rfx_c->hispeed_start_ns)
			rfx_c->hispeed_start_ns = time;
		rfx_c->log_hispeed = rfx_lin_to_log(
			max_cap * rfx_c->filtered_busy_pct / 100);
	} else {
		rfx_c->hispeed_idle_windows++;
		if (rfx_c->hispeed_idle_windows >= 2) {
			rfx_c->hispeed_start_ns = 0;
			rfx_c->filtered_busy_pct = 0;
			rfx_c->log_hispeed = RFX_LOG_0;
		}
	}
}

/*
 * Blend PELT utilization with hispeed utilization using PELT-complementary
 * continuous exponential decay.
 *
 * The hispeed contribution decays in log-domain at 1 ms granularity
 * using PELT's 32 ms half-life (y = 2^(-1/32) per ms):
 *
 *   hispeed_decayed = 2^(log_hispeed + elapsed_ms × log2(y))
 *   blended = min(pelt + hispeed_decayed, hispeed_util)
 *
 * By decaying hispeed_util directly (not the gap hispeed−pelt), the
 * sum pelt + hispeed_decayed matches actual utilization as PELT ramps
 * up, avoiding the double-decay artifact of the previous formulation.
 *
 * When hispeed_util <= pelt_util, hispeed is transparent.
 * After 320 ms (10 half-lives), the hispeed contribution is negligible.
 */
static unsigned long rfx_blend_util(struct rfx_cpu *rfx_c,
				    unsigned long pelt_util,
				    unsigned long max_cap,
				    u64 time)
{
	unsigned long hispeed_util, hispeed_decayed;
	unsigned int elapsed_ms;
	s32 log_decayed;

	if (!rfx_c->filtered_busy_pct || !rfx_c->hispeed_start_ns)
		return pelt_util;

	hispeed_util = max_cap * rfx_c->filtered_busy_pct / 100;

	if (hispeed_util <= pelt_util)
		return pelt_util;

	elapsed_ms = (unsigned int)((time - rfx_c->hispeed_start_ns)
				    / NSEC_PER_MSEC);
	if (elapsed_ms >= RFX_LOG_DECAY_MAX_MS)
		return pelt_util;

	log_decayed = rfx_c->log_hispeed +
		      (s32)elapsed_ms * RFX_LOG_DECAY_PER_MS;
	hispeed_decayed = rfx_log_to_lin(log_decayed);

	if (hispeed_decayed <= pelt_util)
		return pelt_util;

	return min(pelt_util + hispeed_decayed, hispeed_util);
}

/************************ I/O wait boost ***********************/

static bool rfx_iowait_reset(struct rfx_cpu *rfx_c, u64 time,
			     bool set_iowait_boost)
{
	s64 delta_ns = time - rfx_c->last_update;

	if (delta_ns <= TICK_NSEC)
		return false;

	rfx_c->iowait_boost = set_iowait_boost ? IOWAIT_BOOST_MIN : 0;
	rfx_c->iowait_boost_pending = set_iowait_boost;

	return true;
}

static void rfx_iowait_boost(struct rfx_cpu *rfx_c, u64 time,
			     unsigned int flags)
{
	bool set_iowait_boost = flags & SCHED_CPUFREQ_IOWAIT;

	if (rfx_c->iowait_boost &&
	    rfx_iowait_reset(rfx_c, time, set_iowait_boost))
		return;

	if (!set_iowait_boost)
		return;

	if (rfx_c->iowait_boost_pending)
		return;
	rfx_c->iowait_boost_pending = true;

	if (rfx_c->iowait_boost) {
		rfx_c->iowait_boost =
			min_t(unsigned int, rfx_c->iowait_boost << 1, SCHED_CAPACITY_SCALE);
		return;
	}

	rfx_c->iowait_boost = IOWAIT_BOOST_MIN;
}

static unsigned long rfx_iowait_apply(struct rfx_cpu *rfx_c, u64 time,
				      unsigned long max_cap)
{
	if (!rfx_c->iowait_boost)
		return 0;

	if (rfx_iowait_reset(rfx_c, time, false))
		return 0;

	if (!rfx_c->iowait_boost_pending) {
		rfx_c->iowait_boost >>= 1;
		if (rfx_c->iowait_boost < IOWAIT_BOOST_MIN) {
			rfx_c->iowait_boost = 0;
			return 0;
		}
	}

	rfx_c->iowait_boost_pending = false;

	return (rfx_c->iowait_boost * max_cap) >> SCHED_CAPACITY_SHIFT;
}

/************************ Hold frequency ***********************/

#ifdef CONFIG_NO_HZ_COMMON
static bool rfx_hold_freq(struct rfx_cpu *rfx_c)
{
	unsigned long idle_calls;
	bool ret;

	idle_calls = tick_nohz_get_idle_calls_cpu(rfx_c->cpu);
	ret = idle_calls == rfx_c->saved_idle_calls;

	rfx_c->saved_idle_calls = idle_calls;
	return ret;
}
#else
static inline bool rfx_hold_freq(struct rfx_cpu *rfx_c) { return false; }
#endif

static inline void rfx_ignore_dl_rate_limit(struct rfx_cpu *rfx_c)
{
	if (0 > rfx_c->bw_min)
		rfx_c->rfx_policy->need_freq_update = true;
}

/************************ Core update callbacks ***********************/

static void rfx_update_single_freq(struct update_util_data *hook, u64 time,
				   unsigned int flags)
{
	struct rfx_cpu *rfx_c = container_of(hook, struct rfx_cpu, update_util);
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	struct rfx_tunables *tunables = rfx_pol->tunables;
	unsigned int cached_freq = rfx_pol->cached_raw_freq;
	unsigned long max_cap, boost, effective_util;
	unsigned int next_f;

	max_cap = arch_scale_cpu_capacity(NULL, rfx_c->cpu);

	rfx_iowait_boost(rfx_c, time, flags);
	rfx_c->last_update = time;

	rfx_ignore_dl_rate_limit(rfx_c);

	if (!rfx_should_update_freq(rfx_pol, time))
		return;

	boost = rfx_iowait_apply(rfx_c, time, max_cap);
	rfx_get_util(rfx_c, boost);
	effective_util = max(rfx_c->util, boost);

	/* Blend PELT util with hispeed util (decayed by PELT half-life) */
	rfx_update_busy_pct(rfx_c, tunables->hispeed_window_us,
			    tunables->hispeed_filter_shift, time, max_cap);
	effective_util = rfx_blend_util(rfx_c, effective_util, max_cap, time);

	/* Proportional scaling (schedutil-identical) */
	next_f = rfx_get_next_freq(rfx_pol, effective_util, max_cap);

	/* Hold frequency if CPU has not been idle (schedutil-identical) */
	if (rfx_hold_freq(rfx_c) && next_f < rfx_pol->next_freq &&
	    !rfx_pol->need_freq_update) {
		next_f = rfx_pol->next_freq;
		rfx_pol->cached_raw_freq = cached_freq;
	}

	if (!rfx_update_next_freq(rfx_pol, time, next_f))
		return;

	if (rfx_pol->policy->fast_switch_enabled) {
		cpufreq_driver_fast_switch(rfx_pol->policy, next_f);
	} else {
		raw_spin_lock(&rfx_pol->update_lock);
		rfx_deferred_update(rfx_pol);
		raw_spin_unlock(&rfx_pol->update_lock);
	}
}

/************************ Shared policy support ***********************/

static unsigned int rfx_next_freq_shared(struct rfx_cpu *rfx_c, u64 time)
{
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	struct rfx_tunables *tunables = rfx_pol->tunables;
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned long util = 0, max_cap;
	unsigned int next_f, j;

	max_cap = arch_scale_cpu_capacity(NULL, rfx_c->cpu);

	for_each_cpu(j, policy->cpus) {
		struct rfx_cpu *j_rfx_c = &per_cpu(rfx_cpu, j);
		unsigned long j_boost, j_util;

		j_boost = rfx_iowait_apply(j_rfx_c, time, max_cap);
		rfx_get_util(j_rfx_c, j_boost);
		j_util = max(j_rfx_c->util, j_boost);

		rfx_update_busy_pct(j_rfx_c, tunables->hispeed_window_us,
				    tunables->hispeed_filter_shift, time,
				    max_cap);
		j_util = rfx_blend_util(j_rfx_c, j_util, max_cap, time);

		util = max(j_util, util);
	}

	next_f = rfx_get_next_freq(rfx_pol, util, max_cap);

	return next_f;
}

static void
rfx_update_shared(struct update_util_data *hook, u64 time, unsigned int flags)
{
	struct rfx_cpu *rfx_c = container_of(hook, struct rfx_cpu, update_util);
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	unsigned int next_f;

	raw_spin_lock(&rfx_pol->update_lock);

	rfx_iowait_boost(rfx_c, time, flags);
	rfx_c->last_update = time;

	rfx_ignore_dl_rate_limit(rfx_c);

	if (rfx_should_update_freq(rfx_pol, time)) {
		next_f = rfx_next_freq_shared(rfx_c, time);

		if (!rfx_update_next_freq(rfx_pol, time, next_f))
			goto unlock;

		if (rfx_pol->policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(rfx_pol->policy, next_f);
		else
			rfx_deferred_update(rfx_pol);
	}
unlock:
	raw_spin_unlock(&rfx_pol->update_lock);
}

/************************ Kthread (slow path) ***********************/

static void rfx_work(struct kthread_work *work)
{
	struct rfx_policy *rfx_pol = container_of(work, struct rfx_policy, work);
	unsigned int freq;
	unsigned long flags;

	raw_spin_lock_irqsave(&rfx_pol->update_lock, flags);
	freq = rfx_pol->next_freq;
	rfx_pol->work_in_progress = false;
	raw_spin_unlock_irqrestore(&rfx_pol->update_lock, flags);

	mutex_lock(&rfx_pol->work_lock);
	__cpufreq_driver_target(rfx_pol->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&rfx_pol->work_lock);
}

static void rfx_irq_work(struct irq_work *irq_work)
{
	struct rfx_policy *rfx_pol;

	rfx_pol = container_of(irq_work, struct rfx_policy, irq_work);

	kthread_queue_work(&rfx_pol->worker, &rfx_pol->work);
}

/************************** sysfs interface ************************/

static struct rfx_tunables *rfx_global_tunables;
static DEFINE_MUTEX(rfx_global_tunables_lock);

static inline struct rfx_tunables *to_rfx_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct rfx_tunables, attr_set);
}

/* Macro to define show/store for unsigned int tunables */
#define RFX_TUNABLE_UINT(name)						\
static ssize_t name##_show(struct gov_attr_set *attr_set, char *buf)	\
{									\
	struct rfx_tunables *t = to_rfx_tunables(attr_set);		\
	return sprintf(buf, "%u\n", t->name);				\
}									\
static ssize_t								\
name##_store(struct gov_attr_set *attr_set, const char *buf, size_t count) \
{									\
	struct rfx_tunables *t = to_rfx_tunables(attr_set);		\
	unsigned int val;						\
	if (kstrtouint(buf, 10, &val))					\
		return -EINVAL;						\
	t->name = val;							\
	return count;							\
}									\
static struct governor_attr name = __ATTR_RW(name)

/* rate_limit_us needs special handling to update delay_ns */
static ssize_t rfx_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->rate_limit_us);
}

static ssize_t
rfx_rate_limit_us_store(struct gov_attr_set *attr_set, const char *buf,
			size_t count)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);
	struct rfx_policy *rfx_pol;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->rate_limit_us = rate_limit_us;

	list_for_each_entry(rfx_pol, &attr_set->policy_list, tunables_hook)
		rfx_pol->freq_update_delay_ns = rate_limit_us * NSEC_PER_USEC;

	return count;
}

static struct governor_attr rfx_rate_limit_us =
	__ATTR(rate_limit_us, 0644, rfx_rate_limit_us_show, rfx_rate_limit_us_store);

static ssize_t version_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%s\n", CPUFREQ_REFLEX_VERSION);
}
static struct governor_attr version = __ATTR_RO(version);

RFX_TUNABLE_UINT(hispeed_window_us);
RFX_TUNABLE_UINT(hispeed_filter_shift);

static struct attribute *rfx_attrs[] = {
	&version.attr,
	&rfx_rate_limit_us.attr,
	&hispeed_window_us.attr,
	&hispeed_filter_shift.attr,
	NULL
};
ATTRIBUTE_GROUPS(rfx);

static void rfx_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);

	kfree(to_rfx_tunables(attr_set));
}

static struct kobj_type rfx_tunables_ktype = {
	.default_attrs = rfx_attrs,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &rfx_tunables_free,
};

/********************** cpufreq governor interface *********************/

static struct cpufreq_governor reflex_gov;

static struct rfx_policy *rfx_policy_alloc(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol;

	rfx_pol = kzalloc(sizeof(*rfx_pol), GFP_KERNEL);
	if (!rfx_pol)
		return NULL;

	rfx_pol->policy = policy;
	raw_spin_lock_init(&rfx_pol->update_lock);
	return rfx_pol;
}

static void rfx_policy_free(struct rfx_policy *rfx_pol)
{
	kfree(rfx_pol);
}

static int rfx_kthread_create(struct rfx_policy *rfx_pol)
{
	struct task_struct *thread;
	struct sched_attr attr = {
		.size		= sizeof(struct sched_attr),
		.sched_policy	= SCHED_DEADLINE,
		.sched_flags = 0,
		.sched_nice	= 0,
		.sched_priority	= 0,
		/*
		 * Fake (unused) bandwidth; workaround to "fix"
		 * priority inheritance.
		 */
		.sched_runtime	= NSEC_PER_MSEC,
		.sched_deadline = 10 * NSEC_PER_MSEC,
		.sched_period	= 10 * NSEC_PER_MSEC,
	};
	struct cpufreq_policy *policy = rfx_pol->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&rfx_pol->work, rfx_work);
	kthread_init_worker(&rfx_pol->worker);
	thread = kthread_create(kthread_worker_fn, &rfx_pol->worker,
				"rfxgov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("reflex: failed to create kthread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setattr_nocheck(thread, &attr);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_DEADLINE\n", __func__);
		return ret;
	}

	rfx_pol->thread = thread;
	if (policy->dvfs_possible_from_any_cpu)
		set_cpus_allowed_ptr(thread, policy->related_cpus);
	else
		kthread_bind_mask(thread, policy->related_cpus);

	init_irq_work(&rfx_pol->irq_work, rfx_irq_work);
	mutex_init(&rfx_pol->work_lock);

	wake_up_process(thread);

	return 0;
}

static void rfx_kthread_stop(struct rfx_policy *rfx_pol)
{
	if (rfx_pol->policy->fast_switch_enabled)
		return;

	if (!rfx_pol->thread)
		return;

	kthread_flush_worker(&rfx_pol->worker);
	kthread_stop(rfx_pol->thread);
	mutex_destroy(&rfx_pol->work_lock);
}

static struct rfx_tunables *rfx_tunables_alloc(struct rfx_policy *rfx_pol)
{
	struct rfx_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &rfx_pol->tunables_hook);
		if (!have_governor_per_policy())
			rfx_global_tunables = tunables;
	}
	return tunables;
}

static void rfx_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		rfx_global_tunables = NULL;
}

static int rfx_init(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol;
	struct rfx_tunables *tunables;
	int ret = 0;

	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	rfx_pol = rfx_policy_alloc(policy);
	if (!rfx_pol) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = rfx_kthread_create(rfx_pol);
	if (ret)
		goto free_rfx_pol;

	mutex_lock(&rfx_global_tunables_lock);

	if (rfx_global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = rfx_pol;
		rfx_pol->tunables = rfx_global_tunables;

		gov_attr_set_get(&rfx_global_tunables->attr_set,
				 &rfx_pol->tunables_hook);
		goto out;
	}

	tunables = rfx_tunables_alloc(rfx_pol);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	/* Default tunable values */
	tunables->rate_limit_us = cpufreq_policy_transition_delay_us(policy);
	tunables->hispeed_window_us = CPUFREQ_REFLEX_DEFAULT_HISPEED_WINDOW_US;
	tunables->hispeed_filter_shift = CPUFREQ_REFLEX_DEFAULT_HISPEED_FILTER_SHIFT;

	policy->governor_data = rfx_pol;
	rfx_pol->tunables = tunables;

	ret = kobject_init_and_add(&tunables->attr_set.kobj,
				   &rfx_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   reflex_gov.name);
	if (ret)
		goto fail;

out:
	mutex_unlock(&rfx_global_tunables_lock);
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	rfx_clear_global_tunables();

stop_kthread:
	rfx_kthread_stop(rfx_pol);
	mutex_unlock(&rfx_global_tunables_lock);

free_rfx_pol:
	rfx_policy_free(rfx_pol);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("reflex: initialization failed (error %d)\n", ret);
	return ret;
}

static void rfx_exit(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	struct rfx_tunables *tunables = rfx_pol->tunables;
	unsigned int count;

	mutex_lock(&rfx_global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &rfx_pol->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		rfx_clear_global_tunables();

	mutex_unlock(&rfx_global_tunables_lock);

	rfx_kthread_stop(rfx_pol);
	rfx_policy_free(rfx_pol);
	cpufreq_disable_fast_switch(policy);
}

static int rfx_start(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	unsigned int cpu;

	rfx_pol->freq_update_delay_ns =
		rfx_pol->tunables->rate_limit_us * NSEC_PER_USEC;
	rfx_pol->last_freq_update_time	= 0;
	rfx_pol->next_freq		= 0;
	rfx_pol->work_in_progress	= false;
	rfx_pol->limits_changed		= false;
	rfx_pol->cached_raw_freq	= 0;

	rfx_pol->need_freq_update = false;

	for_each_cpu(cpu, policy->cpus) {
		struct rfx_cpu *rfx_c = &per_cpu(rfx_cpu, cpu);

		memset(rfx_c, 0, sizeof(*rfx_c));
		rfx_c->cpu = cpu;
		rfx_c->rfx_policy = rfx_pol;
		/* Initialize idle-time baseline for hispeed busy% */
		rfx_c->prev_idle_time = get_cpu_idle_time(cpu,
					&rfx_c->prev_wall_time, 1);
		cpufreq_add_update_util_hook(cpu, &rfx_c->update_util,
					     policy_is_shared(policy) ?
							rfx_update_shared :
							rfx_update_single_freq);
	}
	return 0;
}

static void rfx_stop(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&rfx_pol->irq_work);
		kthread_cancel_work_sync(&rfx_pol->work);
	}
}

static void rfx_limits(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&rfx_pol->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&rfx_pol->work_lock);
	}

	smp_wmb();

	WRITE_ONCE(rfx_pol->limits_changed, true);
}

static struct cpufreq_governor reflex_gov = {
	.name			= "reflex",
	.owner			= THIS_MODULE,
	.init			= rfx_init,
	.exit			= rfx_exit,
	.start			= rfx_start,
	.stop			= rfx_stop,
	.limits			= rfx_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_REFLEX
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &reflex_gov;
}
#endif

static int __init reflex_gov_init(void)
{
	pr_info("%s %s by %s\n", CPUFREQ_REFLEX_PROGNAME,
		CPUFREQ_REFLEX_VERSION, CPUFREQ_REFLEX_AUTHOR);
	return cpufreq_register_governor(&reflex_gov);
}

static void __exit reflex_gov_exit(void)
{
	cpufreq_unregister_governor(&reflex_gov);
}

module_init(reflex_gov_init);
module_exit(reflex_gov_exit);

MODULE_AUTHOR(CPUFREQ_REFLEX_AUTHOR);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(CPUFREQ_REFLEX_PROGNAME);
