// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor IOCTLs Handler for the I/O Dispatcher kernel module
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/cpu.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/anon_inodes.h>

#include <linux/bao.h>
#include "bao_drv.h"

/**
 * DM ioctls handler
 * @filp: The open file pointer
 * @cmd: The ioctl command
 * @ioctl_param: The ioctl parameter
 */
long bao_dm_ioctl(struct file *filp, unsigned int cmd,
		  unsigned long ioctl_param)
{
	struct bao_virtio_request *req;
	int rc = -EINVAL;

	// get the backend DM pointer from the file pointer private data
	struct bao_io_dm *dm = filp->private_data;

	switch (cmd) {
	case BAO_IOCTL_IO_CLIENT_ATTACH:
		req = memdup_user((void __user *)ioctl_param,
				  sizeof(struct bao_virtio_request));
		if (IS_ERR(req)) {
			pr_err("%s: memdup_user failed\n", __FUNCTION__);
			return PTR_ERR(req);
		}
		if (!dm->control_client) {
			pr_err("%s: control client does not exist\n",
			       __FUNCTION__);
			return -EINVAL;
		}
		rc = bao_io_client_attach(dm->control_client);
		if (rc == 0) {
			rc = bao_io_client_request(dm->control_client, req);
			if (copy_to_user((void __user *)ioctl_param, req,
					 sizeof(struct bao_virtio_request))) {
				pr_err("%s: copy_to_user failed\n", __FUNCTION__);
				return -EFAULT;
			}
		}
		kfree(req);
		break;
	case BAO_IOCTL_IO_REQUEST_NOTIFY_COMPLETED:
		req = memdup_user((void __user *)ioctl_param,
				  sizeof(struct bao_virtio_request));
		if (IS_ERR(req)) {
			pr_err("%s: memdup_user failed\n", __FUNCTION__);
			return PTR_ERR(req);
		}
		rc = bao_io_client_request_complete(dm->control_client, req);
		break;
	case BAO_IOCTL_IOEVENTFD:
		struct bao_ioeventfd ioeventfd;
		if (copy_from_user(&ioeventfd, (void __user *)ioctl_param,
				   sizeof(struct bao_ioeventfd))) {
			pr_err("%s: copy_from_user failed\n", __FUNCTION__);
			return -EFAULT;
		}
		rc = bao_ioeventfd_client_config(dm, &ioeventfd);
		break;
	case BAO_IOCTL_IRQFD:
		struct bao_irqfd irqfd;
		if (copy_from_user(&irqfd, (void __user *)ioctl_param,
				   sizeof(struct bao_irqfd))) {
			pr_err("%s: copy_from_user failed\n", __FUNCTION__);
			return -EFAULT;
		}
		rc = bao_irqfd_server_config(dm, &irqfd);
		break;
	default:
		pr_err("%s: unknown ioctl cmd [%d]\n", __FUNCTION__, cmd);
		rc = -ENOTTY;
		break;
	}
	return rc;
}

/**
 * DM ioctls open handler
 * @inode: The inode of the I/O Dispatcher
 * @filp: The file pointer of the I/O Dispatcher
 */
static int bao_io_dispatcher_dev_open(struct inode *inode, struct file *filp)
{
	return 0;
}

/**
 * DM ioctls release handler
 * @inode: The inode of the I/O Dispatcher
 * @filp: The file pointer of the I/O Dispatcher
 */
static int bao_io_dispatcher_dev_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/**
 * I/O Dispatcher kernel module ioctls handler
 * @filp: The open file pointer
 * @cmd: The ioctl command
 * @ioctl_param: The ioctl parameter
 */
static long bao_io_dispatcher_dev_ioctl(struct file *filp, unsigned int cmd,
					unsigned long ioctl_param)
{
	int rc = -EINVAL;
	unsigned int id;

	switch (cmd) {
	case BAO_IOCTL_IO_DM_BACKEND_CREATE:
		if (copy_from_user(&id, (void __user *)ioctl_param,
				   sizeof(unsigned int))) {
			pr_err("%s: copy_from_user failed\n", __FUNCTION__);
			return -EFAULT;
		}
		rc = bao_dm_create(id);
		break;
	case BAO_IOCTL_IO_DM_BACKEND_DESTROY:
		if (copy_from_user(&id, (void __user *)ioctl_param,
				   sizeof(unsigned int))) {
			pr_err("%s: copy_from_user failed\n", __FUNCTION__);
			return -EFAULT;
		}
		rc = bao_dm_destroy(id);
		break;
	default:
		pr_err("%s: unknown ioctl cmd [%d]\n", __FUNCTION__, cmd);
		return -ENOTTY;
	}
	return rc;
}

static const struct attribute_group *bao_io_dispatcher_attr_groups[] = { NULL };

static const struct file_operations bao_io_dispatcher_fops = {
	.owner = THIS_MODULE,
	.open = bao_io_dispatcher_dev_open,
	.release = bao_io_dispatcher_dev_release,
	.unlocked_ioctl = bao_io_dispatcher_dev_ioctl,
};

struct miscdevice bao_io_dispatcher_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "bao-io-dispatcher",
	.fops = &bao_io_dispatcher_fops,
	.groups = bao_io_dispatcher_attr_groups,
};

/**
 * kernel module initialization
 */
static int __init bao_io_dispatcher_dev_init(void)
{
	int ret;

	// create a new character device with minimal configuration (misc device)
	// and register it with the kernel, making it available to userspace
	// through 'dev/bao-io-dispatcher' device node
	ret = misc_register(&bao_io_dispatcher_dev);
	if (ret) {
		pr_err("Create misc dev failed!\n");
		return ret;
	}

	// setup the I/O Dispatcher system
	ret = bao_io_dispatcher_setup();
	if (ret) {
		pr_err("Setup I/O Dispatcher failed!\n");
		misc_deregister(&bao_io_dispatcher_dev);
		return ret;
	}

	return 0;
}

/**
 * kernel module exit
 */
static void __exit bao_io_dispatcher_dev_exit(void)
{
	bao_io_dispatcher_remove();
	misc_deregister(&bao_io_dispatcher_dev);
}
module_init(bao_io_dispatcher_dev_init);
module_exit(bao_io_dispatcher_dev_exit);

MODULE_AUTHOR("João Peixoto <joaopeixotooficial@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bao Hypervisor I/O Dispatcher");
