// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor Backend Device Model (DM)
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#include "bao_drv.h"
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/anon_inodes.h>
#include <linux/miscdevice.h>
#include "hypercall.h"

/* List of all Backend DMs */
LIST_HEAD(bao_dm_list);

/*
 * bao_dm_list is read in a worker thread which dispatch I/O requests and
 * is wrote in DM creation ioctl. This rwlock mechanism is used to protect it.
 */
DEFINE_RWLOCK(bao_dm_list_lock);

static int bao_dm_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int bao_dm_release(struct inode *inode, struct file *filp)
{
	struct bao_io_dm *dm = filp->private_data;
	kfree(dm);
	return 0;
}

static const struct vm_operations_struct bao_mmap_vm_mem_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys
#endif
};

/**
 * ioctl handler for DM mmap
 * @filp: The file pointer of the DM
 * @vma: Contains the information about the virtual address range that is used to access
 *
 * @note:
 * The device driver only has to build suitable page tables for the address range and,
 * if necessary, replace vma->vm_ops with a new set of operations.
 */
static int bao_dm_mmap(struct file *filp, struct vm_area_struct *vma)
{
	/*
	* There are two ways of building the page tables:
	* 1) Doing it all at once with a function called 'remap_pfn_range'
	* 2) Doing it a page at a time via the 'nopage' DMA method.
	* For this case, we will use the first method.
	*/

	// calculate the size
	size_t size = vma->vm_end - vma->vm_start;

	// calculate the offset
	phys_addr_t offset = (phys_addr_t)vma->vm_pgoff << PAGE_SHIFT;

	// verify if the vma exists
	if (!vma) {
		return -EINVAL;
	}

	// verify if the offset is valid
	if (offset >> PAGE_SHIFT != vma->vm_pgoff) {
		return -EINVAL;
	}

	// update the vma operations
	vma->vm_ops = &bao_mmap_vm_mem_ops;

	// remap-pfn-range will mark the range DM_IO
	if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, size,
			    vma->vm_page_prot)) {
		return -EAGAIN;
	}

	return 0;
}

static struct file_operations bao_dm_fops = {
	.owner = THIS_MODULE,
	.open = bao_dm_open,
	.release = bao_dm_release,
	.unlocked_ioctl = bao_dm_ioctl,
	.llseek = noop_llseek,
	.mmap = bao_dm_mmap,
};

int bao_dm_create(unsigned int id)
{
	int rc = 0;
	struct file *file;
	struct bao_io_dm *dm;
	char name[BAO_NAME_MAX_LEN];

	// verify if already exists a DM with the same virtual ID
	write_lock_bh(&bao_dm_list_lock);
	list_for_each_entry(dm, &bao_dm_list, list) {
		if (dm->id == id) {
			write_unlock_bh(&bao_dm_list_lock);
			return -EEXIST;
		}
	}
	write_unlock_bh(&bao_dm_list_lock);

	// allocate memory for the DM
	dm = kzalloc(sizeof(struct bao_io_dm), GFP_KERNEL);
	if (!dm) {
		pr_err("%s: kzalloc failed\n", __FUNCTION__);
		return -ENOMEM;
	}

	// initialize the DM structure
	INIT_LIST_HEAD(&dm->io_clients);
	spin_lock_init(&dm->io_clients_lock);

	// set the DM virtual ID
	dm->id = id;

	// create a new file descriptor for the DM
	rc = get_unused_fd_flags(O_CLOEXEC);
	if (rc < 0) {
		pr_err("%s: get_unused_fd_flags failed\n", __FUNCTION__);
		goto err_unlock;
	}

	snprintf(name, sizeof(name), "bao-dm-%d", id);
	// create a new anonymous inode for the DM abstraction
	// the `bao_dm_fops` defines the behavior of this "file" and
	// the `dm` is the private data
	file = anon_inode_getfile(name, &bao_dm_fops, dm, O_RDWR);
	if (IS_ERR(file)) {
		pr_err("%s: anon_inode_getfile failed\n", __FUNCTION__);
		put_unused_fd(rc);
		goto err_unlock;
	}

	// initialize the I/O request client
	bao_io_dispatcher_init(dm);

	// add the DM to the list
	write_lock_bh(&bao_dm_list_lock);
	list_add(&dm->list, &bao_dm_list);
	write_unlock_bh(&bao_dm_list_lock);

	// create the Control client
	snprintf(name, sizeof(name), "bao-control-client-%u", dm->id);
	dm->control_client = bao_io_client_create(dm, NULL, NULL, true, name);

	// initialize the Ioeventfd client
	bao_ioeventfd_client_init(dm);

	// initialize the Irqfd server
	bao_irqfd_server_init(dm);

	// associate the file descriptor `rc` with the struct file object `file`
	// in the file descriptor table of the current process
	// (expose the file descriptor `rc` to userspace)
	fd_install(rc, file);

	// return the file descriptor to userspace for the
	// fronteend DM to request services from the associated backend DM
	return rc;

err_unlock:
	kfree(dm);
	return rc;
}

int bao_dm_destroy(unsigned int id)
{
	struct bao_io_dm *dm;

	// find the DM in the list
	write_lock_bh(&bao_dm_list_lock);
	list_for_each_entry(dm, &bao_dm_list, list) {
		if (dm->id == id)
			break;
	}
	write_unlock_bh(&bao_dm_list_lock);

	// mark as destroying
	set_bit(BAO_IO_DM_FLAG_DESTROYING, &dm->flags);

	// remove the DM from the list
	write_lock_bh(&bao_dm_list_lock);
	list_del_init(&dm->list);
	write_unlock_bh(&bao_dm_list_lock);

	// destroy the Irqfd server
	bao_irqfd_server_destroy(dm);

	// destroy the I/O clients
	bao_io_dispatcher_destroy(dm);

	// clear the destroying flag
	clear_bit(BAO_IO_DM_FLAG_DESTROYING, &dm->flags);

	// free the DM
	kfree(dm);

	return 0;
}