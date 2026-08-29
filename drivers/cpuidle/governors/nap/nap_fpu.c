// SPDX-License-Identifier: GPL-2.0
/*
 * nap_fpu.c — floating-point code for the NAP cpuidle governor
 *
 * This file is compiled with the FPU restriction lifted (CFLAGS_REMOVE
 * -mgeneral-regs-only) so the compiler may use hardware floating-point /
 * SIMD instructions.  ALL functions here MUST be called only from within
 * kernel_neon_begin()/kernel_neon_end() blocks.
 *
 * Keeping FP code in a separate translation unit ensures the compiler
 * cannot emit FP instructions in non-FPU code paths (nap.c), which would
 * silently corrupt userspace FP register state.
 */

#include <linux/cpuidle.h>
#include <linux/math64.h>
#include <linux/percpu.h>
#include <linux/pm_qos.h>
#include <linux/sched/clock.h>
#include <linux/string.h>
#include <linux/tick.h>

#include "nap.h"

/* ================================================================
 * Float math helpers
 * ================================================================ */

static inline float float_min(float a, float b) { return a < b ? a : b; }
static inline float float_max(float a, float b) { return a > b ? a : b; }

/*
 * Kernel-safe sqrtf using the ARM64 fsqrt instruction directly.
 * This file is always compiled with the FPU restriction lifted,
 * so "w" (SIMD/FLOATING-POINT) registers are available.
 */
static inline float nap_sqrtf(float x)
{
	asm("fsqrt %s0, %s1" : "=w"(x) : "w"(x));
	return x;
}

/* Scalar log2 approximation (same algorithm as the upstream fast_log2f_sse) */
static inline float fast_log2f(float x)
{
	union { float f; u32 i; } u = { .f = x };
	int exp = (int)((u.i >> 23) & 0xFFu) - 127;
	float e = (float)exp;
	float m, p;

	u.i = (u.i & 0x7FFFFFu) | (127u << 23);
	m = u.f - 1.0f;

	p = m * 0.4808f;
	p = 0.7213f - p;
	p = m * p;
	p = 1.4425f - p;
	p = m * p;

	return e + p;
}

/*
 * Scalar 2^x approximation: integer part via exponent bits, fractional part
 * via a minimax cubic on [0,1] (error < 1e-4).  Used to build the logistic.
 */
static inline float fast_exp2f(float x)
{
	union { u32 i; float f; } v;
	int xi;
	float f;

	if (x > 60.0f)
		x = 60.0f;
	else if (x < -60.0f)
		x = -60.0f;

	xi = (int)x;
	if (x < (float)xi)
		xi--;			/* floor toward negative infinity */
	f = x - (float)xi;

	v.i = (u32)((xi + 127) << 23);	/* 2^xi */
	return v.f * (1.0f + f * (0.6931472f +
			f * (0.2402265f + f * 0.0555041f)));
}

/* Logistic sigmoid: sigmoid(x) = 1 / (1 + e^-x) = 1 / (1 + 2^(-x*log2(e))) */
static inline float nap_sigmoidf(float x)
{
	return 1.0f / (1.0f + fast_exp2f(-1.4426950f * x));
}

/*
 * Robustness floor and Beta-Binomial shrinkage.
 *
 * bin_count[] is an exponentially decayed histogram (window NAP_FLOOR_WIN, in
 * idles) of which idle-state bin each idle landed in, updated every idle; its
 * survival estimate is a fast, forgetting-resistant memory.  The decision
 * treats the NN survival as a prior worth NAP_PRIOR_K pseudo-observations and
 * the decayed histogram as data:
 *   q_k = (NAP_PRIOR_K * q_nn_k + count(>=k)) / (NAP_PRIOR_K + total).
 * Cold (no data) follows the NN; once the histogram fills it dominates.
 */
#define NAP_FLOOR_WIN  256
#define NAP_PRIOR_K    16

/* ================================================================
 * Deterministic PRNG for weight initialization (LCG)
 * ================================================================ */

static inline float nap_prng_float(u32 *state)
{
	*state = *state * 1664525u + 1013904223u;
	return (float)(s32)*state * (1.0f / 2147483648.0f);
}

/* ================================================================
 * Weight initialization
 *
 * The NN directly outputs predicted sleep time in log2(ns) space.
 * Hidden neuron 0 is initialized as a pass-through for feature[0]
 * (log2(sleep_length)), so the initial output ≈ log2(sleep_length).
 * This matches the pre-learning behavior of selecting the deepest
 * state that fits within sleep_length.
 *
 * Other hidden neurons are Xavier-initialized with near-zero output
 * weights so their initial contribution is negligible.  Biases = 0.
 * ================================================================ */

#define NAP_PRNG_SEED 42u

static void nap_init_weights(struct nap_weights *w)
{
	u32 rng = NAP_PRNG_SEED;
	float scale_h1, scale_out;
	int i, j;

	/* Xavier uniform: U(-sqrt(6/(fan_in+fan_out)), +sqrt(6/(...))) */
	scale_h1  = nap_sqrtf(6.0f / (float)(NAP_INPUT_SIZE + NAP_HIDDEN_SIZE));
	scale_out = 0.01f;

	/* Hidden layer weights */
	for (i = 0; i < NAP_INPUT_SIZE; i++)
		for (j = 0; j < NAP_HIDDEN_SIZE; j++)
			w->w_h1[i][j] = nap_prng_float(&rng) * scale_h1;

	/* Hidden biases: zero (standard) */
	memset(w->b_h1, 0, sizeof(w->b_h1));

	/* Output weights: near-zero for ~0 initial contribution */
	for (j = 0; j < NAP_HIDDEN_SIZE; j++)
		w->w_out[j] = nap_prng_float(&rng) * scale_out;

	/* Output bias: zero */
	w->b_out = 0.0f;

	/*
	 * Neuron 0: pass-through for feature[0] = log2(sleep_length).
	 * hidden[0] = ReLU(1.0 * input[0] + 0) = input[0]  (always > 0)
	 * output += 1.0 * hidden[0] = log2(sleep_length)
	 *
	 * Override the random init above so initial output ≈ input[0].
	 */
	for (i = 0; i < NAP_INPUT_SIZE; i++)
		w->w_h1[i][0] = 0.0f;
	w->w_h1[0][0] = 1.0f;
	w->b_h1[0] = 0.0f;
	w->w_out[0] = 1.0f;
}

/*
 * Precompute log2(target_residency) per state and seed the ordinal
 * thresholds.  log2_tres[k] is the boundary location in score space: it
 * seeds thr_ord[k-1], bounds its learned drift, and clamps the score
 * against the timer in the decision layer.
 */
static void nap_init_log2_tres(struct nap_cpu_data *d,
			       struct cpuidle_driver *drv)
{
	int i;

	for (i = 0; i < drv->state_count; i++) {
		float tres = float_max(
			(float)nap_state_target_residency_ns(&drv->states[i]),
			1.0f);

		d->log2_tres[i] = fast_log2f(tres);
	}

	/*
	 * Seed each ordinal threshold at its boundary's log2(target_residency),
	 * so before learning q_k crosses 0.5 exactly when the score (initially
	 * ~= log2(sleep_length)) reaches that state's target_residency.  This
	 * reproduces the deepest-state-that-fits default until learning adapts.
	 */
	for (i = 1; i < drv->state_count; i++)
		d->weights.thr_ord[i - 1] = d->log2_tres[i];
}

/* ================================================================
 * Feature extraction helpers
 * ================================================================ */

struct logring_stats {
	float avg;
	float min;
	float max;
};

/*
 * Compute log_history statistics: avg, min, max (scalar version).
 */
static void logring_compute(const struct nap_cpu_data *d,
			    struct logring_stats *s)
{
	int i, n = d->hist_count;
	float sum, val;

	if (n == 0) {
		*s = (struct logring_stats){ 0 };
		return;
	}

	sum = d->log_history[0];
	s->min = sum;
	s->max = sum;

	for (i = 1; i < n; i++) {
		val = d->log_history[i];
		sum += val;
		s->min = float_min(s->min, val);
		s->max = float_max(s->max, val);
	}

	s->avg = sum / (float)n;
}

static void nap_extract_features(struct cpuidle_driver *drv,
				 struct cpuidle_device *dev,
				 float out[NAP_INPUT_SIZE],
				 s64 latency_req)
{
	struct nap_cpu_data *d = this_cpu_ptr(&nap_data);
	struct logring_stats lr;
	ktime_t sleep_length, delta_tick;
	u64 busy_ns;

	sleep_length = tick_nohz_get_sleep_length(&delta_tick);
	busy_ns = local_clock() - d->prev_idle_exit;

	/*
	 * Scalar log2 batch (upstream did 4 values per fast_log2f_sse call):
	 *   [0] sleep_length   → out[0]
	 *   [1] last_residency → out[1], also stored to log_history
	 *   [2] busy_ns        → out[6]
	 *   [3] |pred_error_us| + 1 → out[5] (sign restored after)
	 */
	{
		float err_f = (float)(d->last_prediction_error / 1000);
		float abs_err = (err_f >= 0.0f) ? err_f : -err_f;
		float in0, in1, in2, in3;
		float r0, r1, r2, r3;

		in0 = float_max((float)ktime_to_ns(sleep_length), 1.0f);
		in1 = float_max((float)((u64)dev->last_residency *
					NSEC_PER_USEC), 1.0f);
		in2 = float_max((float)busy_ns, 1.0f);
		in3 = abs_err + 1.0f;

		r0 = fast_log2f(in0);
		r1 = fast_log2f(in1);
		r2 = fast_log2f(in2);
		r3 = fast_log2f(in3);

		out[0] = r0;
		out[1] = r1;
		out[6] = r2;

		/* out[5]: sign-preserving log2(|err_us| + 1) */
		{
			union { float f; u32 i; } res = { .f = r3 };
			union { float f; u32 i; } sgn = { .f = err_f };

			res.i |= sgn.i & 0x80000000u;
			out[5] = res.f;
		}

		/* Update log_history ring buffer */
		{
			int prev = (d->hist_idx - 1 + NAP_HISTORY_SIZE) %
				   NAP_HISTORY_SIZE;
			d->log_history[prev] = r1;
		}
	}

	/* Compute log_history statistics: avg, min, max */
	logring_compute(d, &lr);
	out[2] = lr.avg;
	out[3] = lr.min;
	out[4] = lr.max;

	/* out[7]: log2(latency_req) - log2(deepest_lat), 0 if unconstrained */
	{
		u64 deepest_lat = nap_state_exit_latency_ns(
					&drv->states[drv->state_count - 1]);
		bool lat_valid = (latency_req < PM_QOS_LATENCY_ANY_NS &&
				  deepest_lat > 0);

		if (lat_valid)
			out[7] = fast_log2f(float_max((float)latency_req, 1.0f))
			       - fast_log2f(float_max((float)deepest_lat, 1.0f));
		else
			out[7] = 0.0f;
	}

	d->last_predicted_ns = ktime_to_ns(sleep_length);
}

/* ================================================================
 * NEON entry point for nap_select
 *
 * Called within kernel_neon_begin()/kernel_neon_end().
 * Returns: selected idle state index (>= 0), or -1 to fall back
 *          to the integer heuristic.
 * ================================================================ */

int nap_fpu_select(struct cpuidle_driver *drv,
		   struct cpuidle_device *dev,
		   struct nap_cpu_data *d)
{
	s64 latency_req = nap_latency_req(dev->cpu);

	/* Handle deferred weight reset (set by sysfs or nap_enable) */
	if (unlikely(d->reset_pending)) {
		nap_init_weights(&d->weights);
		nap_init_log2_tres(d, drv);
		memset(d->bin_count, 0, sizeof(d->bin_count));
		d->have_sample = false;
		d->stats.learn_count = 0;
		d->needs_learn = false;
		d->reset_pending = false;
	}

	/*
	 * Per-idle feedback against the just-realized idle duration.
	 *
	 * Every idle: update the decayed floor histogram so it stays current.
	 * Only every learn_interval (needs_learn): apply the ordinal-threshold
	 * updates and the trunk/score-head backprop, using the previous pass's
	 * stored score, hidden activations and features.  Under the shared-score
	 * proportional-odds model the gradient w.r.t. the score is the scalar
	 * g = sum_k (q_k - y_k), which drives the existing backprop unchanged.
	 * The loss is symmetric -- any responsiveness bias lives in the decision
	 * layer, not here.
	 */
	if (d->have_sample) {
		float decay = (float)(NAP_FLOOR_WIN - 1) / (float)NAP_FLOOR_WIN;
		int k, label_bin = 0;

		if (d->needs_learn) {
			float base_lr = (float)d->learning_rate_millths / 1000.0f;
			float clamp_val = (float)d->max_grad_norm_millths / 1000.0f;
			float s = d->nn_output;
			float g = 0.0f;

			for (k = 1; k < drv->state_count; k++) {
				float th = d->active_w->thr_ord[k - 1];
				float q = nap_sigmoidf(s - th);
				float y = (d->learn_actual_ns >=
					   nap_state_target_residency_ns(
						&drv->states[k]))
					  ? 1.0f : 0.0f;
				float err = q - y;
				float lo = d->log2_tres[k] - 6.0f;
				float hi = d->log2_tres[k] + 6.0f;

				g += err;
				d->active_w->thr_ord[k - 1] =
					fclampf(th + fclampf(base_lr * err,
							     -clamp_val, clamp_val),
						lo, hi);
			}
			d->learn_d_out = g;
			d->learn_lr = base_lr;
			d->stats.learn_count++;
			nap_nn_learn_sclr(d);
			d->needs_learn = false;
		}

		/* Floor histogram update, every idle */
		for (k = 1; k < drv->state_count; k++)
			if (d->learn_actual_ns >=
			    nap_state_target_residency_ns(&drv->states[k]))
				label_bin = k;
		for (k = 0; k < drv->state_count; k++)
			d->bin_count[k] *= decay;
		d->bin_count[label_bin] += 1.0f;

		d->have_sample = false;
	}

	/*
	 * Feature extraction + NN forward pass.
	 * features_f32 is __aligned(32) in nap_cpu_data.
	 */
	nap_extract_features(drv, dev, d->features_f32, latency_req);

	d->active_w = &d->weights;

	nap_nn_forward_sclr(d->features_f32, &d->nn_output, d->hidden_out,
			    d->active_w);

	/*
	 * Decision layer.
	 *
	 * For each boundary k the survival probability q_k is a Beta-Binomial
	 * shrinkage of the NN survival sigmoid(s - thr_ord) (a prior worth
	 * NAP_PRIOR_K pseudo-observations) toward the decayed histogram (data):
	 * the NN drives cold start, the floor takes over as it fills.  A running
	 * minimum enforces a monotone non-increasing survival curve, and the next
	 * timer event caps the reachable depth (a deeper state cannot be earned
	 * past it).  The confidence level is the single responsiveness dial: pick
	 * the deepest feasible state whose survival still meets it.
	 */
	{
		float conf = (float)d->conf_millths / 1000.0f;
		float s = d->nn_output;
		float sleep_log2 = d->features_f32[0];
		float suffix[CPUIDLE_STATE_MAX];
		float total = 0.0f;
		float qmin = 1.0f;
		int k, m = 0, idx = 0;

		for (k = 0; k < drv->state_count; k++)
			total += d->bin_count[k];

		suffix[drv->state_count - 1] =
			d->bin_count[drv->state_count - 1];
		for (k = drv->state_count - 2; k >= 0; k--)
			suffix[k] = suffix[k + 1] + d->bin_count[k];

		for (k = 1; k < drv->state_count; k++) {
			float q_nn = nap_sigmoidf(s - d->active_w->thr_ord[k - 1]);
			float q = ((float)NAP_PRIOR_K * q_nn + suffix[k]) /
				  ((float)NAP_PRIOR_K + total);

			if (d->log2_tres[k] > sleep_log2)
				q = 0.0f;	/* cannot idle past the next timer */
			if (q < qmin)
				qmin = q;
			q = qmin;

			if (q >= conf)
				m = k;
			else
				break;
		}

		for (k = m; k >= 1; k--) {
			if (dev->states_usage[k].disable)
				continue;
			if (nap_state_exit_latency_ns(&drv->states[k]) >
			    latency_req)
				continue;
			idx = k;
			break;
		}
		return idx;
	}
}