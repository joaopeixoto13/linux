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
 * asm_bao_hypercall_remio() - Performs a Remote I/O Hypercall
 * @request: VirtIO request structure
 * @return: Remote I/O Hypercall return structure
 */
static inline struct remio_hypercall_ret asm_bao_hypercall_remio(struct bao_virtio_request *request)
{
	struct remio_hypercall_ret ret;
	register uintptr_t a0 asm("a0") = (uintptr_t)(request->dm_id);
	register uintptr_t a1 asm("a1") = (uintptr_t)(request->addr);
	register uintptr_t a2 asm("a2") = (uintptr_t)(request->op);
	register uintptr_t a3 asm("a3") = (uintptr_t)(request->value);
	register uintptr_t a4 asm("a4") = (uintptr_t)(request->request_id);
	register uintptr_t a5 asm("a5") = (uintptr_t)(0);
	register uintptr_t a6 asm("a6") = (uintptr_t)(REMIO_HC_ID);
	register uintptr_t a7 asm("a7") = (uintptr_t)(0x08000ba0);

	asm volatile("ecall"
		     : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7)
		     : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
		     : "memory");

	ret.hyp_ret = a0;
	ret.remio_hyp_ret = a1;
	ret.pending_requests = a7;

	request->addr = a2;
	request->op = a3;
	request->value = a4;
	request->access_width = a5;
	request->request_id = a6;

	return ret;
}

#endif /* __ASM_RISCV_BAO_H */
