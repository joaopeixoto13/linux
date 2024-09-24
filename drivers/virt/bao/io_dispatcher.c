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

void bao_io_dispatcher_destroy(struct bao_dm *dm)
{
	struct bao_io_client *client, *next;

	// destroy all the I/O clients
	list_for_each_entry_safe(client, next, &dm->io_clients, list) {
		bao_io_client_destroy(client);
	}
}

int bao_io_dispatcher_remio_hypercall(struct bao_virtio_request *req)
{
	// notify the Hypervisor that the request was completed
	*req = bao_hypercall_remio(req->dm_id, req->addr, req->op, req->value, req->cpu_id, req->vcpu_id);

	return req->ret;
}

/**
 * Check if the I/O request is in the range
 * @range: The I/O request range
 * @req: The I/O request to be checked
 * @return True if the I/O request is in the range, False otherwise
*/
static bool bao_io_request_in_range(struct bao_io_range *range, struct bao_virtio_request *req)
{
	// check if the I/O request is in the range
	if ((req->addr >= range->start) && ((req->addr + req->access_width - 1) <= range->end))
		return true;

	return false;
}

/**
 * Find the I/O client that the I/O request belongs to
 * @dm: The DM that the I/O request belongs to
 * @req: The I/O request
 * @return The I/O client that the I/O request belongs to, or NULL if there is no client
 */
static struct bao_io_client *bao_find_io_client(struct bao_dm *dm,
					    struct bao_virtio_request *req)
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
			if (bao_io_request_in_range(range, req)) {
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

int bao_dispatch_io(struct bao_dm *dm)
{
	struct bao_io_client *client;
	struct bao_virtio_request req;
	int rc = 0;

	// update the request
	// the dm_id is the Virtual Remote I/O ID
	req.dm_id = dm->info.id;
	// BAO_IO_ASK will extract the I/O request from the Remote I/O system
	req.op = BAO_IO_ASK;
	// clear the other fields (convention)
	req.addr = 0;
	req.value = 0;
	req.cpu_id = 0;
	req.vcpu_id = 0;

	// perform a Hypercall to get the I/O request from the Remote I/O system
	// the rc value holds the number of requests that still need to be processed
	rc = bao_io_dispatcher_remio_hypercall(&req);

	if (rc < 0) {
		return rc;
	}

	// find the I/O client that the I/O request belongs to
	spin_lock_bh(&dm->io_clients_lock);
	client = bao_find_io_client(dm, &req);
	if (!client) {
		spin_unlock_bh(&dm->io_clients_lock);
		return rc;
	}

	// add the request to the end of the virtio_request list
	bao_io_client_push_request(client, &req);

	// wake up the handler thread which is waiting for requests on the wait queue
	wake_up_interruptible(&client->wq);
	spin_unlock_bh(&dm->io_clients_lock);

	// return the number of request that still need to be processed
	return rc;
}

static void io_dispatcher(struct work_struct *work)
{
	struct bao_dm *dm;
	// for each DM, dispatch the I/O requests
	read_lock(&bao_dm_list_lock);
	list_for_each_entry(dm, &bao_dm_list, list) {
		while (bao_dispatch_io(dm) > 0)
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

void bao_io_dispatcher_pause(void)
{
#ifdef CONFIG_BAO_IO_DISPATCHER_INTERRUPT_MODE
	// remove the interrupt handler
	bao_intc_remove_handler();
#endif
	// drain the workqueue (wait for all the work to finish)
	drain_workqueue(bao_io_dispatcher_wq);
}

void bao_io_dispatcher_resume(void)
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
	bao_io_dispatcher_resume();
}
#endif

int bao_io_dispatcher_init(struct bao_dm *dm)
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
		bao_io_dispatcher_pause();
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