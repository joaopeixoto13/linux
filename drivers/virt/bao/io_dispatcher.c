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
static timer_t timerids[BAO_IO_MAX_DMS];
#endif

// Define a wrapper structure that contains both work_struct and the private data (bao_dm)
struct bao_io_dispatcher_work {
    struct work_struct work;
    struct bao_dm *dm;
};

static struct bao_io_dispatcher_work io_dispatcher_work[BAO_IO_MAX_DMS];

/**
 * Responsible for dispatching I/O requests for all I/O DMs
 * This function is called by the workqueue
 * @work: The work struct
 */
static void io_dispatcher(struct work_struct *work);
// Workqueue for the I/O requests
static struct workqueue_struct *bao_io_dispatcher_wq[BAO_IO_MAX_DMS];

void bao_io_dispatcher_destroy(struct bao_dm *dm)
{
	// if the workqueue exists
	if (bao_io_dispatcher_wq[dm->info.id]) {
		// pause the I/O Dispatcher
		bao_io_dispatcher_pause(dm);
		// destroy the I/O Dispatcher workqueue
		destroy_workqueue(bao_io_dispatcher_wq[dm->info.id]);
#ifdef CONFIG_BAO_IO_DISPATCHER_INTERRUPT_MODE
		// remove the interrupt handler
		bao_intc_remove_handler();
#else
		// stop the pooling timer
		timer_delete(timerids[dm->info.id]);
#endif
	}
}

int bao_io_dispatcher_remio_hypercall(struct bao_virtio_request *req)
{
	// notify the Hypervisor that the request was completed
	*req = bao_hypercall_remio(req->dm_id, req->addr, req->op, req->value, req->request_id);

	return req->ret;
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
	req.request_id = 0;

	// perform a Hypercall to get the I/O request from the Remote I/O system
	// the rc value holds the number of requests that still need to be processed
	rc = bao_io_dispatcher_remio_hypercall(&req);

	if (rc < 0) {
		return rc;
	}

	// find the I/O client that the I/O request belongs to
	down_read(&dm->io_clients_lock);
	client = bao_io_client_find(dm, &req);
	if (!client) {
		up_read(&dm->io_clients_lock);
		return rc;
	}

	// add the request to the end of the virtio_request list
	bao_io_client_push_request(client, &req);

	// wake up the handler thread which is waiting for requests on the wait queue
	wake_up_interruptible(&client->wq);
	up_read(&dm->io_clients_lock);

	// return the number of request that still need to be processed
	return rc;
}

static void io_dispatcher(struct work_struct *work)
{
	struct bao_io_dispatcher_work *bao_dm_work = container_of(work, struct bao_io_dispatcher_work, work);
    struct bao_dm *dm = bao_dm_work->dm;

	// dispatch the I/O request for the device model
	while (bao_dispatch_io(dm) > 0)
		; // while there are requests to be processed
}

/**
 * Interrupt Controller handler for the I/O requests
 * @note: This function is called by the interrupt controller
 * when an interrupt is triggered (when a new I/O request is available)
 * @dm: The DM that triggered the interrupt
 */
static void io_dispatcher_intc_handler(struct bao_dm *dm)
{
	// add the work to the workqueue
	queue_work(bao_io_dispatcher_wq[dm->info.id], &io_dispatcher_work[dm->info.id].work);
}

void bao_io_dispatcher_pause(struct bao_dm *dm)
{
#ifdef CONFIG_BAO_IO_DISPATCHER_INTERRUPT_MODE
	// remove the interrupt handler
	bao_intc_remove_handler();
#endif
	// drain the workqueue (wait for all the work to finish)
	drain_workqueue(bao_io_dispatcher_wq[dm->info.id]);
}

void bao_io_dispatcher_resume(struct bao_dm *dm)
{
#ifdef CONFIG_BAO_IO_DISPATCHER_INTERRUPT_MODE
	// setup the interrupt handler
	bao_intc_setup_handler(io_dispatcher_intc_handler);
#endif
	// add the work to the workqueue
	queue_work(bao_io_dispatcher_wq[dm->info.id], &io_dispatcher_work[dm->info.id].work);
}

/**
 * Responsible for dispatching I/O requests for all DMs
 * if selected the pooling mode
 */
#ifndef CONFIG_BAO_IO_DISPATCHER_INTERRUPT_MODE
int bao_io_dispatcher_pooling_handler(void *data)
{
	struct bao_dm *dm = (struct bao_dm *)data;
	// resume the I/O requests dispatcher
	bao_io_dispatcher_resume(dm);
}
#endif

int bao_io_dispatcher_init(struct bao_dm *dm)
{
	char name[BAO_NAME_MAX_LEN];
    snprintf(name, BAO_NAME_MAX_LEN, "bao-iodwq%u", dm->info.id);

	// Create the I/O Dispatcher workqueue with high priority
	bao_io_dispatcher_wq[dm->info.id] = alloc_workqueue(name, WQ_HIGHPRI | WQ_MEM_RECLAIM, 1);
	if (!bao_io_dispatcher_wq[dm->info.id]) {
		return -ENOMEM;
	}

	// Assign the custom data to the work
	io_dispatcher_work[dm->info.id].dm = dm;

	// Initialize the work_struct
	INIT_WORK(&io_dispatcher_work[dm->info.id].work, io_dispatcher);

#ifdef CONFIG_BAO_IO_DISPATCHER_INTERRUPT_MODE
	// setup the interrupt handler
	bao_intc_setup_handler(io_dispatcher_intc_handler);
#else
	struct sigevent sev;

	// Create timer
	sev.sigev_notify = SIGEV_THREAD; // Notify via thread
	sev.sigev_notify_function =
		bao_io_dispatcher_pooling_handler; // Callback function
	sev.sigev_notify_attributes = dm; // Pass DM to callback
	sev.sigev_value.sival_ptr = &timerids[dm->info.id]; // Pass timer ID to callback

	if (timer_create(CLOCK_REALTIME, &sev, &timerids[dm->info.id]) == -1) {
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
	if (timer_settime(timerids[dm->info.id], 0, &its, NULL) == -1) {
		perror("Bao I/O Dispatcher pooling mode for DM %s: timer_settime", dm->info.id);
		return -1;
	}
#endif

	return 0;
}

int bao_io_dispatcher_setup(void)
{
	// Do nothing
	return 0;
}

void bao_io_dispatcher_remove(void)
{
	// Do nothing
}