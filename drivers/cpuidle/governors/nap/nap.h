/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NAP_H
#define NAP_H

#include <linux/cpuidle.h>
#include <linux/cpu.h>
#include <linux/ktime.h>
#include <linux/pm_qos.h>

/* ================================================================
 * Neural network dimensions
 * ================================================================ */

#define NAP_INPUT_SIZE    8
#define NAP_HIDDEN_SIZE   8
#define NAP_NUM_CUTS      (CPUIDLE_STATE_MAX - 1)

/*
 * Neural network weights for an 8-input MLP with an ordinal survival head.
 *
 * The trunk maps input[8] → hidden[8] (ReLU), feeding a shared linear score
 *   s = w_out . hidden + b_out
 * which is the input to a proportional-odds ordinal head. For each idle-state
 * boundary k the predicted survival probability that the upcoming idle reaches
 * that state's target_residency is
 *   q_k = sigmoid(s - thr_ord[k-1]).
 * With ordered thresholds this represents the idle-duration distribution at
 * exactly the points the decision needs (the sufficient statistic), rather
 * than a single point estimate. The decision layer compares q_k against a
 * calibrated confidence level (see nap_fpu_select()).
 *
 * Column-major storage: w_h1[j][i] = weight from input j to hidden neuron i.
 * This layout was originally chosen for SIMD column-wise matrix-vector
 * products; the scalar ARM64 port keeps it for bit-identical behaviour.
 *
 * thr_ord is appended after the SIMD-accessed fields so their offsets are
 * unchanged.
 */
struct nap_weights {
	/* Hidden layer: input[8] → hidden[8] */
	float w_h1[NAP_INPUT_SIZE][NAP_HIDDEN_SIZE];  /* 64 params */
	float b_h1[NAP_HIDDEN_SIZE];                   /* 8 params  */
	/* Shared score head: hidden[8] → scalar s */
	float w_out[NAP_HIDDEN_SIZE];                  /* 8 params  */
	float b_out;                                   /* 1 param   */
	/* Ordinal survival head: one ordered threshold per state boundary */
	float thr_ord[NAP_NUM_CUTS];
} __aligned(32);

struct nap_cpu_data;

/* Scalar (ARM64 NEON-safe) forward pass and online learning */
void nap_nn_forward_sclr(const float *input, float *output,
			 float *hidden_save, const struct nap_weights *w);
void nap_nn_learn_sclr(struct nap_cpu_data *d);

/* ================================================================
 * Scalar clamp helper (used by nap_fpu.c and nap_nn_sclr.c)
 * ================================================================ */

static inline float fclampf(float v, float lo, float hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

/* ================================================================
 * 4.14 compatibility layer
 *
 * - exit_latency / target_residency are in microseconds, not ns
 * - dev->last_residency is in microseconds, not ns
 * - no poll_limit_ns mechanism and no POLL state on arm64
 * - no cpuidle_governor_latency_req(); 4.14 uses the menu-governor
 *   pm_qos pattern (us, with a default meaning "unconstrained")
 * ================================================================ */

#define PM_QOS_LATENCY_ANY_NS ((s64)PM_QOS_LATENCY_ANY * NSEC_PER_USEC)

static inline s64 nap_state_exit_latency_ns(const struct cpuidle_state *s)
{
	return (s64)s->exit_latency * NSEC_PER_USEC;
}

static inline u64 nap_state_target_residency_ns(const struct cpuidle_state *s)
{
	return (u64)s->target_residency * NSEC_PER_USEC;
}

/* Effective latency requirement for @cpu, in ns (never below late-state
 * filtering; a 4.14 default constraint means "unconstrained"). */
static inline s64 nap_latency_req(unsigned int cpu)
{
	struct device *device = get_cpu_device(cpu);
	int latency_req = pm_qos_request(PM_QOS_CPU_DMA_LATENCY);
	int resume_latency = device ? dev_pm_qos_raw_read_value(device) : 0;

	if (resume_latency && resume_latency < latency_req)
		latency_req = resume_latency;

	if (latency_req >= PM_QOS_CPU_DMA_LAT_DEFAULT_VALUE)
		return PM_QOS_LATENCY_ANY_NS;

	return (s64)latency_req * NSEC_PER_USEC;
}

/* ================================================================
 * Feature extraction
 * ================================================================ */

#define NAP_HISTORY_SIZE     8

/* Refresh interval for the cached minimum-valid-state lookup.  HZ
 * jiffies (1 s) bounds staleness from sysfs/runtime state-disable
 * events; PM QoS latency changes are detected immediately via the
 * cached latency_req comparison.
 */
#define NAP_MIN_STATE_REFRESH_JIFFIES  HZ

struct nap_stats {
	u64 total_selects;
	u64 total_residency_ns;
	u64 overshoot_count;
	u64 learn_count;
};

struct nap_cpu_data {
	/* Ring buffer */
	u64   history[NAP_HISTORY_SIZE];
	float log_history[NAP_HISTORY_SIZE];
	int   hist_idx;
	int   hist_count;

	/* External signal tracking */
	u64     prev_idle_exit;
	s64     last_predicted_ns;
	s64     last_prediction_error;

	/* Short-circuit fast path */
	bool short_circuited;			/* set in select, read in reflect */
	int  cached_min_state;			/* cached shallowest valid state */
	s64  cached_min_state_latency;		/* latency_req when cache populated */
	unsigned long cached_min_state_jiffies;	/* jiffies when cache populated */

	/* Jiffies-based learning rate floor */
	unsigned long last_learn_jiffies;
	unsigned int  learn_jiffies_min;	/* 0 = disabled */

	/* select/reflect handoff */
	int   last_selected_idx;

	/* Shared ordinal score s (≈ log2 of the predicted idle duration in ns).
	 * Survival at boundary k is sigmoid(s - thr_ord[k-1]).
	 */
	float nn_output;

	/*
	 * hidden_out[], features_f32[] are written via scalar stores in
	 * nap_nn_forward_sclr() / nap_extract_features().  Aligned to 32
	 * bytes to preserve struct layout compat with the upstream port
	 * and keep the memory footprint identical.
	 */
	float hidden_out[NAP_HIDDEN_SIZE] __aligned(32);
	float features_f32[NAP_INPUT_SIZE] __aligned(32);

	/* Backprop scratch */
	float learn_d_out;	/* score gradient g = sum_k (q_k - y_k) */
	float learn_lr;		/* effective learning rate (symmetric) */
	float learn_d_hid[NAP_HIDDEN_SIZE] __aligned(32);

	/* Precomputed per-state log2 thresholds.
	 * log2_tres[i] = log2(target_residency_ns) (ordinal thresholds, timer clamp)
	 */
	float log2_tres[CPUIDLE_STATE_MAX];

	/* Decayed per-bin idle histogram: robustness-floor survival estimate */
	float bin_count[CPUIDLE_STATE_MAX];

	/* Deferred learning data */
	bool  needs_learn;
	bool  have_sample;	/* a fresh residency awaits per-idle processing */
	u64   learn_actual_ns;

	/* Single network: 8→8 trunk + ordinal survival head */
	struct nap_weights weights;
	struct nap_weights *active_w;	/* always &weights; consumed by forward/learn */

	/* Online learning */
	unsigned int learning_rate_millths;
	unsigned int max_grad_norm_millths;
	unsigned int conf_millths;	/* decision confidence level (500 = 0.5) */
	int   learn_interval;
	int   learn_counter;
	bool reset_pending;		/* set by sysfs, consumed by nap_select */

	/* sysfs statistics */
	struct nap_stats stats;
};

DECLARE_PER_CPU(struct nap_cpu_data, nap_data);

/* FPU/NEON entry point (nap_fpu.c) — call only within
 * kernel_neon_begin()/kernel_neon_end() */
int nap_fpu_select(struct cpuidle_driver *drv,
		   struct cpuidle_device *dev,
		   struct nap_cpu_data *d);

/* sysfs interface */
int  nap_sysfs_init(void);
void nap_sysfs_exit(void);

#endif /* NAP_H */