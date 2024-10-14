/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Provides the Bao Hypervisor IOCTLs and global structures
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#ifndef _UAPI_BAO_H
#define _UAPI_BAO_H

#include <linux/types.h>
#include <linux/uuid.h>
#include <linux/list.h>

#define BAO_IO_WRITE 0x0
#define BAO_IO_READ 0x1
#define BAO_IO_ASK 0x2
#define BAO_IO_NOTIFY 0x3

#define BAO_NAME_MAX_LEN 16
#define BAO_IO_REQUEST_MAX 64
#define BAO_IO_MAX_DMS 16

/**
 * Contains the specific parameters of a Bao VirtIO request
 * @dm_id: Device Model ID
 * @addr: Gives the MMIO register address that was accessed
 * @op: Write, Read, Ask or Notify operation
 * @value: Value to write or read
 * @access_width: Access width (VirtIO MMIO only allows 4-byte wide and alligned accesses)
 * @request_id: Request ID
 * @ret: Return value
*/
struct bao_virtio_request {
	__u64 dm_id;
	__u64 addr;
	__u64 op;
	__u64 value;
	__u64 access_width;
	__u64 request_id;
	__s32 ret;
};

/**
 * Contains the specific parameters of a ioeventfd request
 * @fd:		The fd of eventfd associated with a hsm_ioeventfd
 * @flags:	Logical-OR of BAO_IOEVENTFD_FLAG_*
 * @addr:	The start address of IO range of ioeventfd
 * @len:	The length of IO range of ioeventfd
 * @reserved:	Reserved and should be 0
 * @data:	Data for data matching
 */
struct bao_ioeventfd {
	__u32 fd;
	__u32 flags;
	__u64 addr;
	__u32 len;
	__u32 reserved;
	__u64 data;
};

/**
 * Contains the specific parameters of a irqfd request
 * @fd: The file descriptor of the eventfd
 * @flags: The flags of the eventfd
 */
struct bao_irqfd {
	__s32 fd;
	__u32 flags;
};

/**
 * Contains the specific parameters of a Bao DM
 * @id: The virtual ID of the DM
 * @shmem_addr: The base address of the shared memory
 * @shmem_size: The size of the shared memory
 * @irq: The IRQ number
 * @fd: The file descriptor of the DM
 */
struct bao_dm_info {
	__u32 id;
	__u64 shmem_addr;
	__u64 shmem_size;
	__u32 irq;
	__s32 fd;
};

/* The ioctl type, listed in Documentation/userspace-api/ioctl/ioctl-number.rst */
#define BAO_IOCTL_TYPE 0xA6

/*
 * Common IOCTL IDs definition for Bao userspace
 * Follows the convention of the Linux kernel, listed in Documentation/driver-api/ioctl.rst
 */
#define BAO_IOCTL_DM_GET_INFO _IOWR(BAO_IOCTL_TYPE, 0x01, struct bao_dm_info)
#define BAO_IOCTL_IO_CLIENT_ATTACH \
	_IOWR(BAO_IOCTL_TYPE, 0x02, struct bao_virtio_request)
#define BAO_IOCTL_IO_REQUEST_COMPLETE \
	_IOW(BAO_IOCTL_TYPE, 0x03, struct bao_virtio_request)
#define BAO_IOCTL_IOEVENTFD _IOW(BAO_IOCTL_TYPE, 0x04, struct bao_ioeventfd)
#define BAO_IOCTL_IRQFD _IOW(BAO_IOCTL_TYPE, 0x05, struct bao_irqfd)

/* Remote I/O Hypercall ID */
#define REMIO_HC_ID 0x2

/**
 * Remote I/O Hypercall return structure
 * @hyp_ret: The generic return value of Bao's hypercall
 * @remio_hyp_ret: The return value of the Remote I/O Hypercall
 * @pending_requests: The number of pending requests (only used in the Remote I/O Ask Hypercall)
*/
struct remio_hypercall_ret {
	u64 hyp_ret;
	u64 remio_hyp_ret;
	u64 pending_requests;
};

#endif /* _UAPI_BAO_H */