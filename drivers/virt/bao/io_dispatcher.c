// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor I/O Dispatcher
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/eventfd.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/interrupt.h>

#include <linux/bao.h>
#include "bao_drv.h"
#include "hypercall.h"

#ifndef CONFIG_BAO_IO_DISPATCHER_INTERRUPT_MODE
#include <time.h>
#define INTERVAL_NS CONFIG_BAO_IO_DISPATCHER_POOLING_INTERVAL
static timer_t timerid;
#endif

/**
 * Responsible for dispatching I/O requests for all I/O DMs
 * This function is called by the workqueue
 * @work: The work struct
 */
static void io_dispatcher(struct work_struct *work);
// Workqueue for the I/O requests
static struct workqueue_struct *bao_io_dispatcher_wq;
// Associate the workqueue with the function io_dispatcher
static DECLARE_WORK(dispatcher_io_work, io_dispatcher);

/**
 * Acquires the I/O requests from the Bao Hypervisor and dispatches them to the respective I/O client
 * @dm: The DM that the I/O request belongs to
 */
static int dispatch_io(struct bao_io_dm *dm);

/**
 * Pause the I/O Dispatcher
 */
static void io_dispatcher_pause(void);

/**
 * Resume the I/O Dispatcher
 */
static void io_dispatcher_resume(void);

/**
 * Check if there are pending requests
 * @client: The I/O client
 * @return: bool
*/
static inline bool has_pending_requests(struct bao_io_client *client)
{
	return !bitmap_empty(client->io_req_map, BAO_IO_REQUEST_MAX);
}

/**
 * Check if the I/O client is being destroyed
 * @client: The I/O client
 * @return: bool
*/
static inline bool is_destroying(struct bao_io_client *client)
{
	return test_bit(BAO_IO_CLIENT_DESTROYING, &client->flags);
}

/**
 * Find the first available bit in the I/O client io_req_map
 * @client: The I/O client
 * @return: int (the first available bit) or -EBUSY (if there is no available bit)
*/
static int next_availabe_bit(struct bao_io_client *client)
{
	int i = -EBUSY;

	for (i = 0; i < BAO_IO_REQUEST_MAX; i++) {
		if (!test_bit(i, client->io_req_map)) {
			break;
		}
	}
	return i;
}

void bao_io_client_destroy(struct bao_io_client *client)
{
	struct bao_io_client *range, *next;
	struct bao_io_dm *dm = client->dm;

	// pause the I/O requests dispatcher
	io_dispatcher_pause();

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
	io_dispatcher_resume();
}

void bao_io_dispatcher_destroy(struct bao_io_dm *dm)
{
	struct bao_io_client *client, *next;

	// destroy all the I/O clients
	list_for_each_entry_safe(client, next, &dm->io_clients, list) {
		bao_io_client_destroy(client);
	}
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
					 has_pending_requests(client) ||
						 is_destroying(client));
		if (is_destroying(client))
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
					 has_pending_requests(client) ||
						 is_destroying(client) ||
							 kthread_should_stop());
		if (is_destroying(client) || kthread_should_stop()) {
			if (kthread_should_stop()) {
				bao_io_client_destroy(client);
			}
			return -EPERM;
		}
	}

	return 0;
}

/**
 * Add the I/O request to the list
 * @client: The I/O Client that the I/O request belongs to
 * @req: The I/O request to be added
 */
static void bao_io_push_request(struct bao_io_client *client,
				struct bao_io_request *req)
{
	// add the request to the end of the requests list
	write_lock_bh(&client->virtio_requests_lock);
	list_add_tail(&req->list, &client->virtio_requests);
	write_unlock_bh(&client->virtio_requests_lock);
}

/**
 * Return the first free I/O request from the list
 * @client: The I/O client that the I/O request belongs to
 * @ret: The I/O request to be returned
 */
static int bao_io_pop_request(struct bao_io_client *client,
			      struct bao_io_request *ret)
{
	struct bao_io_request *req;
	int ret_val = -EFAULT;

	// pop the first request from the list
	write_lock_bh(&client->virtio_requests_lock);
	req = list_first_entry_or_null(&client->virtio_requests,
				       struct bao_io_request, list);
	write_unlock_bh(&client->virtio_requests_lock);

	// check if there are requests pending to be processed
	if (req == NULL) {
		ret_val = -EFAULT;
		goto err_unlock;
	}

	// Copy the request to the return value
	*ret = *req;

	// delete the request from the list
	write_lock_bh(&client->virtio_requests_lock);
	list_del(&req->list);
	write_unlock_bh(&client->virtio_requests_lock);

	// free the request
	kfree(req);

	ret_val = 0;

	return ret_val;

err_unlock:
	// clear the flag to avoid entering in a loop
	clear_bit(find_first_bit(client->io_req_map, BAO_IO_REQUEST_MAX),
		  client->io_req_map);
	// unlock the mutex
	write_unlock_bh(&client->virtio_requests_lock);
	return ret_val;
}

/**
 * Complete the I/O request
 * @client: The I/O client that the I/O request belongs to
 * @req: The I/O request to be completed
 */
static int bao_dispatcher_io_complete_request(struct bao_io_client *client,
					      struct bao_io_request *req)
{
	struct bao_virtio_request ret;

	// clear the bit corresponding to the request in the client io_req_map
	clear_bit(find_first_bit(client->io_req_map, BAO_IO_REQUEST_MAX),
		  client->io_req_map);

	// notify the Hypervisor that the request was completed
	ret = bao_hypercall_virtio(
		req->virtio_request.virtio_id, req->virtio_request.addr,
		req->virtio_request.op, req->virtio_request.value,
		req->virtio_request.cpu_id, req->virtio_request.vcpu_id);

	return 0;
}

/**
 * Execution entity thread for a kernel I/O client (e.g., Ioeventfd client)
 * @data: The I/O client
 */
static int io_client_kernel_thread(void *data)
{
	struct bao_io_client *client = data;
	struct bao_io_request *req = kzalloc(sizeof(*req), GFP_KERNEL);
	int ret = -EINVAL;
	int stop = false;

	while (!stop) {
		// attach the client
		stop = bao_io_client_attach(client);
		while (has_pending_requests(client) && !stop) {
			// get the first kernel handled I/O request
			if (bao_io_pop_request(client, req)) {
				kfree(req);
				return -EFAULT;
			}
			// call the handler callback of the I/O client
			// (e.g bao_ioeventfd_handler() for an ioeventfd client)
			ret = client->handler(client, req);
			if (ret < 0) {
				break;
			}
			// complete the request
			else {
				bao_dispatcher_io_complete_request(client, req);
			}
		}
	}

	kfree(req);
	return 0;
}

struct bao_io_client *bao_io_client_create(struct bao_io_dm *dm,
					   io_handler_t handler, void *data,
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
		client->thread = kthread_run(io_client_kernel_thread, client,
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
		while (dispatch_io(dm) > 0)
			;
		read_unlock(&bao_dm_list_lock);
	} 

	return client;
}

int bao_io_client_request(struct bao_io_client *client,
			  struct bao_virtio_request *ret)
{
	struct bao_io_request *req = kzalloc(sizeof(*req), GFP_KERNEL);
	int ret_val = -EINVAL;

	// check if the Control client exists
	if (!client) {
		return -EEXIST;
	}

	// get the first I/O request
	ret_val = bao_io_pop_request(client, req);

	// if there are no requests pending to be processed return -EEXIST
	if (ret_val) {
		return -EEXIST;
	}

	// copy the request to the return value
	*ret = req->virtio_request;

	// free the request object
	kfree(req);

	return ret_val;
}

int bao_io_client_request_complete(struct bao_io_client *client,
				   struct bao_virtio_request *req)
{
	int ret = -EINVAL;
	struct bao_io_request *io_req = kzalloc(sizeof(*io_req), GFP_KERNEL);

	// update the request
	io_req->virtio_request = *req;

	// complete the request
	spin_lock_bh(&client->dm->io_clients_lock);
	ret = bao_dispatcher_io_complete_request(client, io_req);
	spin_unlock_bh(&client->dm->io_clients_lock);

	// free the request object
	kfree(io_req);

	return ret;
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

/**
 * Check if the I/O request is in the range
 * @range: The I/O request range
 * @req: The I/O request
 * @return bool
*/
static bool bao_io_req_in_range(struct bao_io_range *range, struct bao_io_request *req)
{
	bool ret = false;

	// check if the I/O request is in the range
	if (req->virtio_request.addr >= range->start &&
	    (req->virtio_request.addr + req->virtio_request.access_width - 1) <=
		    range->end)
		ret = true;

	return ret;
}

/**
 * Find the I/O client that the I/O request belongs to
 * @dm: The DM that the I/O request belongs to
 * @req: The I/O request
 * @return struct bao_io_client*
 */
static struct bao_io_client *find_io_client(struct bao_io_dm *dm,
					    struct bao_io_request *req)
{
	struct bao_io_client *client, *found = NULL;
	struct bao_io_range *range;

	lockdep_assert_held(&dm->io_clients_lock);

	// for all the I/O clients
	list_for_each_entry(client, &dm->io_clients, list) {
		read_lock_bh(&client->range_lock);
		// for all the ranges
		list_for_each_entry(range, &client->range_list, list) {
			// check if the I/O request is in the range of a given client
			if (bao_io_req_in_range(range, req)) {
				found = client;
				break;
			}
		}
		read_unlock_bh(&client->range_lock);
		if (found)
			break;
	}

	// if the I/O request is not in the range of any client, return the Control client
	// otherwise, return the client that the I/O request belongs to (e.g., Ioeventfd client)
	return found ? found : dm->control_client;
}

static int dispatch_io(struct bao_io_dm *dm)
{
	struct bao_io_client *client;
	struct bao_io_request *req;
	struct bao_virtio_request ret;
	int next_avail_bit = -EBUSY;
	int rc = 0;

	// allocate the request object
	req = kzalloc(sizeof(*req), GFP_KERNEL);

	// update the request
	// the virtio_id is the Virtual DM id
	req->virtio_request.virtio_id = dm->id;
	// clear the addr field
	req->virtio_request.addr = 0;
	// BAO_IO_ASK will extract the I/O request from the Bao Hypervisor
	req->virtio_request.op = BAO_IO_ASK;
	// clear the other fields
	req->virtio_request.value = 0;
	req->virtio_request.cpu_id = 0;
	req->virtio_request.vcpu_id = 0;

	// perform a Hypercall to get the I/O request from the Bao Hypervisor
	ret = bao_hypercall_virtio(
		req->virtio_request.virtio_id, req->virtio_request.addr,
		req->virtio_request.op, req->virtio_request.value,
		req->virtio_request.cpu_id, req->virtio_request.vcpu_id);

	if (ret.ret < 0) {
		rc = -EFAULT;
		goto err_unlock_1;
	}

	// update the virtio request
	req->virtio_request = ret;

	// find the I/O client that the I/O request belongs to
	spin_lock_bh(&dm->io_clients_lock);
	client = find_io_client(dm, req);
	if (!client) {
		rc = -EINVAL;
		goto err_unlock_2;
	}

	// find next available bit in the client io_req_map
	next_avail_bit = next_availabe_bit(client);
	if (next_avail_bit == -EBUSY) {
		rc = -EBUSY;
		goto err_unlock_2;
	}

	// set the next available bit in the client io_req_map
	// to indicate that there are requests pending for processing
	set_bit(next_avail_bit, client->io_req_map);

	// add the request to the end of the virtio_request list
	bao_io_push_request(client, req);

	// wake up the handler thread which is waiting for requests on the wait queue
	wake_up_interruptible(&client->wq);
	spin_unlock_bh(&dm->io_clients_lock);

	// return the number of request that still need to be processed
	return ret.ret;

err_unlock_2:
	spin_unlock_bh(&dm->io_clients_lock);

err_unlock_1:
	kfree(req);
	return rc;
}

static void io_dispatcher(struct work_struct *work)
{
	struct bao_io_dm *dm;
	// for each DM, dispatch the I/O requests
	read_lock(&bao_dm_list_lock);
	list_for_each_entry(dm, &bao_dm_list, list) {
		while (dispatch_io(dm) > 0)
			; // while there are requests to be processed
	}
	read_unlock(&bao_dm_list_lock);
}

/**
 * Interrupt Controller handler for the I/O requests
 * @note: This function is called by the interrupt controller
 * when an interrupt is triggered (when a new I/O request is available)
 */
static void io_dispatcher_intc_handler(void)
{
	// add the work to the workqueue
	queue_work(bao_io_dispatcher_wq, &dispatcher_io_work);
}

static void io_dispatcher_pause(void)
{
#ifdef CONFIG_BAO_IO_DISPATCHER_INTERRUPT_MODE
	// remove the interrupt handler
	bao_intc_remove_handler();
#endif
	// drain the workqueue (wait for all the work to finish)
	drain_workqueue(bao_io_dispatcher_wq);
}

static void io_dispatcher_resume(void)
{
#ifdef CONFIG_BAO_IO_DISPATCHER_INTERRUPT_MODE
	// setup the interrupt handler
	bao_intc_setup_handler(io_dispatcher_intc_handler);
#endif
	// add the work to the workqueue
	queue_work(bao_io_dispatcher_wq, &dispatcher_io_work);
}

/**
 * Responsible for dispatching I/O requests for all DMs
 * if selected the pooling mode
 */
#ifndef CONFIG_BAO_IO_DISPATCHER_INTERRUPT_MODE
int io_dispatcher_pooling_handler(void *data)
{
	// resume the I/O requests dispatcher
	io_dispatcher_resume();
}
#endif

int bao_io_dispatcher_init(struct bao_io_dm *dm)
{
	// Do nothing
	return 0;
}

int bao_io_dispatcher_setup(void)
{
	// Create the I/O Dispatcher workqueue with high priority
	bao_io_dispatcher_wq = alloc_workqueue("bao_io_dispatcher_wq",
					       WQ_HIGHPRI | WQ_MEM_RECLAIM, 1);
	if (!bao_io_dispatcher_wq) {
		return -ENOMEM;
	}

#ifdef CONFIG_BAO_IO_DISPATCHER_INTERRUPT_MODE
	// setup the interrupt handler
	bao_intc_setup_handler(io_dispatcher_intc_handler);
#else
	struct sigevent sev;

	// Create timer
	sev.sigev_notify = SIGEV_THREAD; // Notify via thread
	sev.sigev_notify_function =
		io_dispatcher_pooling_handler; // Callback function
	sev.sigev_value.sival_ptr = &timerid; // Pass timer ID to callback

	if (timer_create(CLOCK_REALTIME, &sev, &timerid) == -1) {
		perror("Bao I/O Dispatcher pooling mode: timer_create");
		return -1;
	}

	// Configure timer
	struct itimerspec its;
	its.it_value.tv_sec = 0; // Initial expiration time (seconds)
	its.it_value.tv_nsec =
		INTERVAL_NS; // Initial expiration time (nanoseconds)
	its.it_interval.tv_sec = 0; // Interval for periodic timer (seconds)
	its.it_interval.tv_nsec =
		INTERVAL_NS; // Interval for periodic timer (nanoseconds)

	// Start timer
	if (timer_settime(timerid, 0, &its, NULL) == -1) {
		perror("Bao I/O Dispatcher pooling mode: timer_settime");
		return -1;
	}
#endif

	return 0;
}

void bao_io_dispatcher_remove(void)
{
	// if the workqueue exists
	if (bao_io_dispatcher_wq) {
		// pause the I/O Dispatcher
		io_dispatcher_pause();
		// destroy the I/O Dispatcher workqueue
		destroy_workqueue(bao_io_dispatcher_wq);
#ifdef CONFIG_BAO_IO_DISPATCHER_INTERRUPT_MODE
		// remove the interrupt handler
		bao_intc_remove_handler();
#else
		// stop the pooling timer
		timer_delete(timerid);
#endif
	}
}