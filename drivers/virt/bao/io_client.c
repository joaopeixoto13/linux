// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor I/O Client
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#include "bao_drv.h"
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/io.h>

/**
 * Contains the specific parameters of a Bao I/O request
 * @list: List node for this request
 * @virtio_request: The I/O request
*/
struct bao_io_request {
	struct list_head list;
	struct bao_virtio_request virtio_request;
};

/**
 * Check if there are pending requests
 * @client: The I/O client
 * @return: True if there are pending requests, false otherwise
*/
static inline bool bao_io_client_has_pending_requests(struct bao_io_client *client)
{
    return !list_empty(&client->virtio_requests);
}

/**
 * Check if the I/O client is being destroyed
 * @client: The I/O client
 * @return: bool
*/
static inline bool bao_io_client_is_destroying(struct bao_io_client *client)
{
	return test_bit(BAO_IO_CLIENT_DESTROYING, &client->flags);
}

void bao_io_client_push_request(struct bao_io_client *client,
				struct bao_virtio_request *req)
{
    struct bao_io_request *io_req;

    // allocate the I/O request object
    io_req = kzalloc(sizeof(*io_req), GFP_KERNEL);

    // copy the request to the I/O request object
    io_req->virtio_request = *req;

	// add the request to the end of the requests list
	write_lock_bh(&client->virtio_requests_lock);
	list_add_tail(&io_req->list, &client->virtio_requests);
	write_unlock_bh(&client->virtio_requests_lock);
}

struct bao_virtio_request bao_io_client_pop_request(struct bao_io_client *client)
{
	struct bao_io_request *req;
    struct bao_virtio_request ret;

    // initialize the return value with an error
    ret.ret = -EINVAL;

	// pop the first request from the list
	write_lock_bh(&client->virtio_requests_lock);
	req = list_first_entry_or_null(&client->virtio_requests,
				       struct bao_io_request, list);
	write_unlock_bh(&client->virtio_requests_lock);

	if (req == NULL) {
		write_unlock_bh(&client->virtio_requests_lock);
		return ret;
	}

	// copy the request to the return value
	ret = req->virtio_request;

	// delete the request from the list
	write_lock_bh(&client->virtio_requests_lock);
	list_del(&req->list);
	write_unlock_bh(&client->virtio_requests_lock);

	// free the request
	kfree(req);

	return ret;
}

void bao_io_client_destroy(struct bao_io_client *client)
{
	struct bao_io_client *range, *next;
	struct bao_dm *dm = client->dm;

	// pause the I/O requests dispatcher
	bao_io_dispatcher_pause();

	// set the destroying flag
	set_bit(BAO_IO_CLIENT_DESTROYING, &client->flags);

	// stop the client
	if (client->is_control)
		wake_up_interruptible(&client->wq);
	else {
		bao_ioeventfd_client_destroy(dm);
		kthread_stop(client->thread);
	}

	// remove the I/O ranges
	write_lock_bh(&client->range_lock);
	list_for_each_entry_safe(range, next, &client->range_list, list) {
		list_del(&range->list);
		kfree(range);
	}
	write_unlock_bh(&client->range_lock);

	// remove the I/O client
	spin_lock_bh(&dm->io_clients_lock);
	if (client->is_control)
		dm->control_client = NULL;
	else
		dm->ioeventfd_client = NULL;
	list_del(&client->list);
	spin_unlock_bh(&dm->io_clients_lock);

	// free the allocated I/O client object
	kfree(client);

	// resume the I/O requests dispatcher
	bao_io_dispatcher_resume();
}

int bao_io_client_attach(struct bao_io_client *client)
{
	if (client->is_control) {
		/*
		 * In the Control client, a user space thread waits on the waitqueue.
		 * The thread should wait until:
		 * - there are pending I/O requests to be processed
		 * - the I/O client is going to be destroyed
		 */
		wait_event_interruptible(client->wq,
					 bao_io_client_has_pending_requests(client) ||
						 bao_io_client_is_destroying(client));
		if (bao_io_client_is_destroying(client))
			return -EPERM;
	} else {
		/*
		 * In the non-control client (e.g., Ioeventfd Client), a kernel space thread waits on the waitqueue.
		 * The thread should wait until:
		 * - there are pending I/O requests to be processed
		 * - the I/O client is going to be destroyed
		 * - the kernel thread is going to be stopped
		 */
		wait_event_interruptible(client->wq,
					 bao_io_client_has_pending_requests(client) ||
						 bao_io_client_is_destroying(client) ||
							 kthread_should_stop());
		if (bao_io_client_is_destroying(client) || kthread_should_stop()) {
			if (kthread_should_stop()) {
				bao_io_client_destroy(client);
			}
			return -EPERM;
		}
	}

	return 0;
}

/**
 * Execution entity thread for a kernel I/O client (e.g., Ioeventfd client)
 * @data: The I/O client
 */
static int bao_io_client_kernel_thread(void *data)
{
	struct bao_io_client *client = data;
	struct bao_virtio_request req;
	int ret = -EINVAL;
	int stop = false;

	while (!stop) {
		// attach the client
		stop = bao_io_client_attach(client);
		while (bao_io_client_has_pending_requests(client) && !stop) {
			// get the first kernel handled I/O request
            req = bao_io_client_pop_request(client);
			if (req.ret < 0) {
				return -EFAULT;
			}
			// call the handler callback of the I/O client
			// (e.g bao_ioeventfd_handler() for an ioeventfd client)
			ret = client->handler(client, &req);
			if (ret < 0) {
				break;
			}
			// complete the request
			else {
				bao_io_dispatcher_remio_hypercall(&req);
			}
		}
	}

	return 0;
}

struct bao_io_client *bao_io_client_create(struct bao_dm *dm,
					   bao_io_client_handler_t handler, void *data,
					   bool is_control, const char *name)
{
	struct bao_io_client *client;

	// if the I/O client is implemenmted in the kernel, it must have a kernel handler (e.g., Ioevendfd client)
	if (!handler && !is_control) {
		return NULL;
	}

	// allocate the I/O client object
	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return NULL;

	// initialize the I/O client
	client->handler = handler;
	client->dm = dm;
	client->priv = data;
	client->is_control = is_control;
	if (name)
		strncpy(client->name, name, sizeof(client->name) - 1);
	rwlock_init(&client->virtio_requests_lock);
	INIT_LIST_HEAD(&client->virtio_requests);
	rwlock_init(&client->range_lock);
	INIT_LIST_HEAD(&client->range_list);
	init_waitqueue_head(&client->wq);

	// if the I/O client is implemented in the kernel, create the kernel thread
	if (client->handler) {
		client->thread = kthread_run(bao_io_client_kernel_thread, client,
					     "%s-kthread", client->name);
		if (IS_ERR(client->thread)) {
			kfree(client);
			return NULL;
		}
	}

	// add the I/O client to the I/O clients list
	spin_lock_bh(&dm->io_clients_lock);
	if (is_control)
		dm->control_client = client;
	else
		dm->ioeventfd_client = client;
	list_add(&client->list, &dm->io_clients);
	spin_unlock_bh(&dm->io_clients_lock);

	// back up any pending requests that could potentially be lost
	// (e.g., if the backend VM is initialized after the frontend VM)
	if (is_control) {
		read_lock(&bao_dm_list_lock);
		while (bao_dispatch_io(dm) > 0)
			;
		read_unlock(&bao_dm_list_lock);
	} 

	return client;
}

int bao_io_client_request(struct bao_io_client *client,
			  struct bao_virtio_request *req)
{
	// check if the Control client exists
	if (!client) {
		return -EEXIST;
	}

    // pop the first request from the list
    *req = bao_io_client_pop_request(client);

    // return the request return value
	return req->ret;
}

int bao_io_client_range_add(struct bao_io_client *client, u64 start, u64 end)
{
	struct bao_io_range *range;

	// check if the range is valid
	if (end < start) {
		return -EINVAL;
	}

	// allocate the range object
	range = kzalloc(sizeof(*range), GFP_KERNEL);
	if (!range)
		return -ENOMEM;

	// initialize the range
	range->start = start;
	range->end = end;

	// add the range to the list
	write_lock_bh(&client->range_lock);
	list_add(&range->list, &client->range_list);
	write_unlock_bh(&client->range_lock);

	return 0;
}

void bao_io_client_range_del(struct bao_io_client *client, u64 start, u64 end)
{
	struct bao_io_range *range;

	// delete the range from the list
	write_lock_bh(&client->range_lock);
	list_for_each_entry(range, &client->range_list, list) {
		if (start == range->start && end == range->end) {
			list_del(&range->list);
			kfree(range);
			break;
		}
	}
	write_unlock_bh(&client->range_lock);
}