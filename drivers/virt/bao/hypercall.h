// SPDX-License-Identifier: GPL-2.0
/*
 * Hypercall API for Bao Hypervisor
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#ifndef __BAO_HYPERCALL_H
#define __BAO_HYPERCALL_H

#include <asm/bao.h>
#include <linux/bao.h>

/* Remote I/O Hypercall ID */
#define REMIO_HC_ID 0x2

/**
 * bao_hypercall_remio() - Performs a Remote I/O Hypercall
 * @dm_id:	Device Model ID
 * @addr: Access address
 * @op:		Write, Read, Ask or Notify operation
 * @value:	Value to write or read
 * @request_id: Request ID
 *
 * @return: The VirtIO request structure
 */
static inline struct bao_virtio_request
bao_hypercall_remio(u64 dm_id, u64 addr, u64 op, u64 value, u64 request_id)
{
	return asm_bao_hypercall_remio(REMIO_HC_ID, dm_id, addr, op, value, request_id);
}

#endif /* __BAO_HYPERCALL_H */