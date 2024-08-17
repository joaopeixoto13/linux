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
 * asm_bao_hypercall_virtio() - Performs a VirtIO Hypercall
 * @virtio_hc_id: VirtIO Hypercall ID
 * @virtio_id: Virtual VirtIO ID (used to connect each frontend driver to the backend device)
 * @addr: Access address
 * @op:	Write, Read, Ask or Notify operation
 * @value: Value to write or read
 * @cpu_id: CPU ID
 * @vcpu_id: VCPU ID
 *
 * @return: The VirtIO request structure
 */
static inline struct bao_virtio_request
asm_bao_hypercall_virtio(u64 virtio_hc_id, u64 virtio_id, u64 addr, u64 op,
			 u64 value, u64 cpu_id, u64 vcpu_id)
{
	register int x0 asm("x0") =
		ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,
				   ARM_SMCCC_OWNER_VENDOR_HYP, virtio_hc_id);
	register u64 x1 asm("x1") = virtio_hc_id;
	register u64 x2 asm("x2") = virtio_id;
	register u64 x3 asm("x3") = addr;
	register u64 x4 asm("x4") = op;
	register u64 x5 asm("x5") = value;
	register u64 x6 asm("x6") = cpu_id;
	register u64 x7 asm("x7") = vcpu_id;

	struct bao_virtio_request ret;

	asm volatile("hvc 0\n\t"
		     : "=r"(x0), "=r"(x1), "=r"(x2), "=r"(x3), "=r"(x4),
		       "=r"(x5), "=r"(x6), "=r"(x7)
		     : "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5),
		       "r"(x6), "r"(x7)
		     : "memory");

	ret.ret = x0;
	ret.virtio_id = x1;
	ret.addr = x2;
	ret.op = x3;
	ret.value = x4;
	ret.access_width = x5;
	ret.cpu_id = x6;
	ret.vcpu_id = x7;

	return ret;
}

#endif /* __ASM_ARM64_BAO_H */