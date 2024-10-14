// SPDX-License-Identifier: GPL-2.0
/*
 * Hypercall for Bao Hypervisor on ARM64
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#ifndef __ASM_ARM64_BAO_H
#define __ASM_ARM64_BAO_H

#include <asm/bao.h>
#include <linux/bao.h>
#include <linux/arm-smccc.h>

/**
 * asm_bao_hypercall_remio() - Performs a Remote I/O Hypercall
 * @request: VirtIO request structure
 * @return: Remote I/O Hypercall return structure
 */
static inline struct remio_hypercall_ret asm_bao_hypercall_remio(struct bao_virtio_request *request)
{
	struct remio_hypercall_ret ret;
	register int x0 asm("x0") =
		ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,
				   ARM_SMCCC_OWNER_VENDOR_HYP, REMIO_HC_ID);
	register u64 x1 asm("x1") = request->dm_id;
	register u64 x2 asm("x2") = request->addr;
	register u64 x3 asm("x3") = request->op;
	register u64 x4 asm("x4") = request->value;
	register u64 x5 asm("x5") = request->request_id;
	register u64 x6 asm("x6") = 0;

	asm volatile("hvc 0\n\t"
		     : "=r"(x0), "=r"(x1), "=r"(x2), "=r"(x3), "=r"(x4), "=r"(x5), "=r"(x6)
		     : "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
		     : "memory");

	ret.hyp_ret = 0;
	ret.remio_hyp_ret = x0;
	ret.pending_requests = x6;

	request->addr = x1;
	request->op = x2;
	request->value = x3;
	request->access_width = x4;
	request->request_id = x5;

	return ret;
}

#endif /* __ASM_ARM64_BAO_H */
