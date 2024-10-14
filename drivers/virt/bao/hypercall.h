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

/**
 * bao_hypercall_remio() - Performs a Remote I/O Hypercall
 * @request: VirtIO request structure
 * @return: Remote I/O Hypercall return structure
 */
static inline struct remio_hypercall_ret bao_hypercall_remio(struct bao_virtio_request *request)
{
	return asm_bao_hypercall_remio(request);
}

#endif /* __BAO_HYPERCALL_H */