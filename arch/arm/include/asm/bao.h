// SPDX-License-Identifier: GPL-2.0
/*
 * Hypercall for Bao Hypervisor on ARM
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#ifndef __ASM_ARM_BAO_H
#define __ASM_ARM_BAO_H

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
	register int x0 asm("r0") =
		ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,
				   ARM_SMCCC_OWNER_VENDOR_HYP, REMIO_HC_ID);
	register u32 x1 asm("r1") = request->dm_id;
	register u32 x2 asm("r2") = request->addr;
	register u32 x3 asm("r3") = request->op;
	register u32 x4 asm("r4") = request->value;
	register u32 x5 asm("r5") = request->request_id;
	register u32 x6 asm("r6") = 0;

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

#endif /* __ASM_ARM_BAO_H */
