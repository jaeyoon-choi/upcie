/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * CUDA 13 maps cuCtxCreate to cuCtxCreate_v4(pctx, ctxCreateParams, flags,
 * dev). The tools call the 3-argument form, so "make CUDA13=1" injects this
 * header, which passes ctxCreateParams = NULL (a regular context).
 */
#ifndef CUDA13_COMPAT_H
#define CUDA13_COMPAT_H
#include <cuda.h>
#if defined(CUDA_VERSION) && (CUDA_VERSION >= 13000)
#undef cuCtxCreate
static inline CUresult cuCtxCreate(CUcontext *pctx, unsigned int flags, CUdevice dev)
{
	return cuCtxCreate_v4(pctx, 0, flags, dev);
}
#endif
#endif
