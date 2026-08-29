# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the NAP cpuidle governor
#

obj-$(CONFIG_CPU_IDLE_GOV_NAP) += cpuidle_gov_nap.o

cpuidle_gov_nap-y := nap.o nap_fpu.o nap_nn_sclr.o

# The kernel builds with -mgeneral-regs-only on arm64, which forbids the
# compiler from using the FP/SIMD register file.  Files that perform
# floating-point / NEON work must have that flag removed so the hardware
# floating-point instructions are available (see lib/raid6/Makefile).
#
# CRITICAL: nap.o is intentionally compiled with NORMAL kernel flags
# (no FPU).  All floating-point code lives in nap_fpu.o and nap_nn_sclr.o,
# which are only invoked from within kernel_neon_begin()/kernel_neon_end()
# blocks in nap_select().  This keeps governor callbacks (nap_select,
# nap_reflect, etc.) free of FP instruction emission.
CFLAGS_REMOVE_nap_fpu.o     += -mgeneral-regs-only
CFLAGS_REMOVE_nap_nn_sclr.o += -mgeneral-regs-only