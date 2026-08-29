// SPDX-License-Identifier: GPL-2.0
/*
 * nap_nn_sclr.c — scalar forward pass and backpropagation for the nap MLP
 *
 * 8→8 trunk + scalar score s feeding the ordinal survival head.
 *
 * This is a pure scalar C reimplementation of the upstream SSE2 kernels
 * (nap_nn_sse2.c), bit-for-bit matching their arithmetic order and
 * clamping semantics so the trained weights behave identically on ARM64.
 * No SIMD intrinsics; the compiler may still use FP/SIMD registers since
 * this file is compiled with -mgeneral-regs-only lifted.
 *
 * Must be called within kernel_neon_begin/end.
 */

#include "nap.h"

void nap_nn_forward_sclr(const float *input,
			 float *output,
			 float *hidden_save,
			 const struct nap_weights *w)
{
	float acc[NAP_HIDDEN_SIZE];
	int i, j;

	/* === Hidden layer: h[j] = b_h1[j] + sum_i w_h1[i][j] * input[i] === */
	for (j = 0; j < NAP_HIDDEN_SIZE; j++)
		acc[j] = w->b_h1[j];

	for (i = 0; i < NAP_INPUT_SIZE; i++) {
		float x = input[i];

		for (j = 0; j < NAP_HIDDEN_SIZE; j++)
			acc[j] += w->w_h1[i][j] * x;
	}

	/* ReLU */
	for (j = 0; j < NAP_HIDDEN_SIZE; j++)
		acc[j] = acc[j] > 0.0f ? acc[j] : 0.0f;
	for (j = 0; j < NAP_HIDDEN_SIZE; j++)
		hidden_save[j] = acc[j];

	/* === Output layer: dot(hidden[8], w_out[8]) + b_out → 1 scalar === */
	{
		float sum = w->b_out;

		for (j = 0; j < NAP_HIDDEN_SIZE; j++)
			sum += w->w_out[j] * acc[j];

		*output = sum;
	}
}

/*
 * Online learning (backpropagation) — scalar
 *
 * Output: scalar d_out (pre-computed by caller)
 * Hidden layer: 8 neurons
 */
void nap_nn_learn_sclr(struct nap_cpu_data *d)
{
	int i, j;
	float d_out_scalar = d->learn_d_out;
	float *d_hid = d->learn_d_hid;
	float lr = d->learn_lr;
	float clamp_val = (float)d->max_grad_norm_millths / 1000.0f;
	float v_cl_hi = clamp_val;
	float v_cl_lo = -clamp_val;

	/*
	 * Hidden gradient: d_hid[j] = relu'(h[j]) * w_out[j] * d_out.
	 * Must be computed before output weight update to use pre-update
	 * w_out.
	 */
	for (j = 0; j < NAP_HIDDEN_SIZE; j++)
		d_hid[j] = (d->hidden_out[j] > 0.0f)
			   ? d->active_w->w_out[j] * d_out_scalar : 0.0f;

	/* Output weight update: w_out[j] -= lr * clamp(h[j] * d_out) */
	for (j = 0; j < NAP_HIDDEN_SIZE; j++)
		d->active_w->w_out[j] -=
			lr * fclampf(d->hidden_out[j] * d_out_scalar,
				     v_cl_lo, v_cl_hi);

	/* Output bias update: b_out -= lr * clamp(d_out) */
	d->active_w->b_out -= lr * fclampf(d_out_scalar, v_cl_lo, v_cl_hi);

	/* Hidden weight update: w_h1[i][j] -= lr * clamp(feat[i] * d_hid[j]) */
	for (i = 0; i < NAP_INPUT_SIZE; i++) {
		float vf = d->features_f32[i];

		for (j = 0; j < NAP_HIDDEN_SIZE; j++)
			d->active_w->w_h1[i][j] -=
				lr * fclampf(vf * d_hid[j], v_cl_lo, v_cl_hi);
	}

	/* Hidden bias update: b_h1[j] -= lr * clamp(d_hid[j]) */
	for (j = 0; j < NAP_HIDDEN_SIZE; j++)
		d->active_w->b_h1[j] -=
			lr * fclampf(d_hid[j], v_cl_lo, v_cl_hi);
}