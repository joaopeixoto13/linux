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

/* VirtIO Hypercall ID */
#define VIRTIO_HC_ID 0x2

/**
 * bao_hypercall_virtio() - Performs a I/O Hypercall
 * @virtio_id:	Virtual VirtIO ID (used to connect each frontend driver to the backend device)
 * @addr: Access address
 * @op:		Write, Read, Ask or Notify operation
 * @value:	Value to write or read
 * @cpu_id:	Frontend CPU ID of the I/O request
 * @vcpu_id:	Frontend vCPU ID of the I/O request
 *
 * @return: The VirtIO request structure
 */
static inline struct bao_virtio_request
bao_hypercall_virtio(u64 virtio_id, u64 addr, u64 op, u64 value, u64 cpu_id,
		     u64 vcpu_id)
{
	return asm_bao_hypercall_virtio(VIRTIO_HC_ID, virtio_id, addr, op,
					value, cpu_id, vcpu_id);
}

#endif /* __BAO_HYPERCALL_H */