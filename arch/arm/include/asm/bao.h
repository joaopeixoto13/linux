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
 * @remio_hc_id: VirtIO Hypercall ID
 * @dm_id: Device Model ID
 * @addr: Access address
 * @op:	Write, Read, Ask or Notify operation
 * @value: Value to write or read
 * @request_id: Request ID
 *
 * @return: The VirtIO request structure
 */
static inline struct bao_virtio_request
asm_bao_hypercall_remio(u64 remio_hc_id, u64 dm_id, u64 addr, u64 op,
			 u64 value, u64 request_id)
{
	register int x0 asm("r0") =
		ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,
				   ARM_SMCCC_OWNER_VENDOR_HYP, remio_hc_id);
	register u32 x1 asm("r1") = dm_id;
	register u32 x2 asm("r2") = addr;
	register u32 x3 asm("r3") = op;
	register u32 x4 asm("r4") = value;
	register u32 x5 asm("r5") = request_id;

	struct bao_virtio_request ret;

	asm volatile("hvc 0\n\t"
		     : "=r"(x0), "=r"(x1), "=r"(x2), "=r"(x3), "=r"(x4), "=r"(x5)
		     : "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
		     : "memory");

	ret.ret = x0;
	ret.dm_id = dm_id;
	ret.addr = x1;
	ret.op = x2;
	ret.value = x3;
	ret.access_width = x4;
	ret.request_id = x5;

	return ret;
}

#endif /* __ASM_ARM_BAO_H */
