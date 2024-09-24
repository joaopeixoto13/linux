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
	struct bao_dm *dm = filp->private_data;
	kfree(dm);
	return 0;
}

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
	struct bao_dm *dm = filp->private_data;

    unsigned long vsize = vma->vm_end - vma->vm_start;

    if (remap_pfn_range(vma, vma->vm_start, dm->info.shmem_addr >> PAGE_SHIFT, vsize, vma->vm_page_prot)) {
        return -EFAULT;
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

struct bao_dm* bao_dm_create(struct bao_dm_info *info)
{
	struct bao_dm *dm;
	char name[BAO_NAME_MAX_LEN];

	// verify if already exists a DM with the same virtual ID
	read_lock(&bao_dm_list_lock);
	list_for_each_entry(dm, &bao_dm_list, list) {
		if (dm->info.id == info->id) {
			read_unlock(&bao_dm_list_lock);
			return NULL;
		}
	}
	read_unlock(&bao_dm_list_lock);

	// allocate memory for the DM
	dm = kzalloc(sizeof(struct bao_dm), GFP_KERNEL);
	if (!dm) {
		pr_err("%s: kzalloc failed\n", __FUNCTION__);
		return NULL;
	}

	// initialize the DM structure
	INIT_LIST_HEAD(&dm->io_clients);
	spin_lock_init(&dm->io_clients_lock);

	// set the DM fields
	dm->info = *info;

	// initialize the I/O request client
	bao_io_dispatcher_init(dm);

	// add the DM to the list
	write_lock_bh(&bao_dm_list_lock);
	list_add(&dm->list, &bao_dm_list);
	write_unlock_bh(&bao_dm_list_lock);

	// create the Control client
	snprintf(name, sizeof(name), "bao-control-client-%u", dm->info.id);
	dm->control_client = bao_io_client_create(dm, NULL, NULL, true, name);

	// initialize the Ioeventfd client
	bao_ioeventfd_client_init(dm);

	// initialize the Irqfd server
	bao_irqfd_server_init(dm);

	// return the file descriptor to userspace for the
	// fronteend DM to request services from the associated backend DM
	return dm;
}

void bao_dm_destroy(struct bao_dm *dm)
{
	// mark as destroying
	set_bit(BAO_DM_FLAG_DESTROYING, &dm->flags);

	// remove the DM from the list
	write_lock_bh(&bao_dm_list_lock);
	list_del_init(&dm->list);
	write_unlock_bh(&bao_dm_list_lock);

	// clear the global fields
	dm->info.id = 0;
	dm->info.shmem_addr = 0;
	dm->info.shmem_size = 0;
	dm->info.irq = 0;
	put_unused_fd(dm->info.fd);

	// destroy the Irqfd server
	bao_irqfd_server_destroy(dm);

	// destroy the I/O clients
	bao_io_dispatcher_destroy(dm);

	// clear the destroying flag
	clear_bit(BAO_DM_FLAG_DESTROYING, &dm->flags);

	// free the DM
	kfree(dm);
}

/**
 * Create an anonymous inode for the DM abstraction
 * @note: The anonymous inode is used to expose the DM to userspace
 * 	  	  and allow the frontend DM to request services from the backend DM
 * 	      directly through the file descriptor
 *        This function should be called after the DM is created and invoked
 * 		  by the frontend DM (userspace process) to create the anonymous inode 
 * 		  inside the process file descriptor table
 * @dm: The DM to create the anonymous inode
 * @return: >=0 on success, <0 on failure
 */
static int bao_dm_create_anonymous_inode(struct bao_dm *dm)
{
	char name[BAO_NAME_MAX_LEN];
	struct file *file;
	int rc = 0;

	// create a new file descriptor for the DM
	rc = get_unused_fd_flags(O_CLOEXEC);
	if (rc < 0) {
		pr_err("%s: get_unused_fd_flags failed\n", __FUNCTION__);
		return rc;
	}

	// create a name for the DM file descriptor
	snprintf(name, sizeof(name), "bao-dm-%u", dm->info.id);

	// create a new anonymous inode for the DM abstraction
	// the `bao_dm_fops` defines the behavior of this "file" and
	// the `dm` is the private data
	file = anon_inode_getfile(name, &bao_dm_fops, dm, O_RDWR);
	if (IS_ERR(file)) {
		pr_err("%s: anon_inode_getfile failed\n", __FUNCTION__);
		put_unused_fd(rc);
		return rc;
	}

	// associate the file descriptor `rc` with the struct file object `file`
	// in the file descriptor table of the current process
	// (expose the file descriptor `rc` to userspace)
	fd_install(rc, file);

	// update the DM file descriptor
	dm->info.fd = rc;

	return rc;
}

bool bao_dm_get_info(struct bao_dm_info *info)
{
	struct bao_dm *dm;
	bool rc = false;

	read_lock(&bao_dm_list_lock);
	list_for_each_entry(dm, &bao_dm_list, list) {
		if (dm->info.id == info->id) {
			info->shmem_addr = dm->info.shmem_addr;
			info->shmem_size = dm->info.shmem_size;
			info->irq = dm->info.irq;
			info->fd = bao_dm_create_anonymous_inode(dm);
			rc = true;
			break;
		}
	}
	read_unlock(&bao_dm_list_lock);

	return rc;
}