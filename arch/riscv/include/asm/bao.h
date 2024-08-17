// SPDX-License-Identifier: GPL-2.0
/*
 * Hypercall for Bao Hypervisor on RISC-V
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#ifndef __ASM_RISCV_BAO_H
#define __ASM_RISCV_BAO_H

#include <asm/sbi.h>
#include <linux/bao.h>

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
	struct bao_virtio_request ret;

	register intptr_t a0 asm("a0") = (uintptr_t)(0x08000ba0);
	register uintptr_t a1 asm("a1") = (uintptr_t)(virtio_hc_id);
	register uintptr_t a2 asm("a2") = (uintptr_t)(virtio_id);
	register uintptr_t a3 asm("a3") = (uintptr_t)(addr);
	register uintptr_t a4 asm("a4") = (uintptr_t)(op);
	register uintptr_t a5 asm("a5") = (uintptr_t)(value);
	register uintptr_t a6 asm("a6") = (uintptr_t)(cpu_id);
	register uintptr_t a7 asm("a7") = (uintptr_t)(vcpu_id);

	asm volatile("ecall"
		     : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4),
		       "+r"(a5), "+r"(a6), "+r"(a7)
		     : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5),
		       "r"(a6), "r"(a7)
		     : "memory");

	ret.ret = a0;
	ret.virtio_id = a1;
	ret.addr = a2;
	ret.op = a3;
	ret.value = a4;
	ret.access_width = a5;
	ret.cpu_id = a6;
	ret.vcpu_id = a7;

	return ret;
}

#endif /* __ASM_RISCV_BAO_H */