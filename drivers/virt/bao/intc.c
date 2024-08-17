// SPDX-License-Identifier: GPL-2.0
/*
 * Bao Hypervisor I/O Dispatcher Interrupt Controller
 *
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 *
 * Authors:
 *	João Peixoto <joaopeixotooficial@gmail.com>
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>

#define BAO_IO_DISPATCHER_INTC_MAX_IRQS 16

// number of configured interrupts
static int irq_number;

// interrupt numbers
static int irq_numbers[BAO_IO_DISPATCHER_INTC_MAX_IRQS];

// handler for the interrupt
static void (*bao_intc_handler)(void);

/**
 * bao_interrupt_handler - Interrupt handler
 * @irq: Interrupt number
 * @dev_id: Device ID
 */
static irqreturn_t bao_interrupt_handler(int irq, void *dev_id)
{
	// if the handler is set, call it
	if (bao_intc_handler)
		bao_intc_handler();

	return IRQ_HANDLED;
}

void bao_intc_setup_handler(void (*handler)(void))
{
	bao_intc_handler = handler;
}

void bao_intc_remove_handler(void)
{
	bao_intc_handler = NULL;
}

/**
 * bao_io_dispatcher_intc_probe - Probe the interrupt handler
 * @pdev: Platform device pointer
 */
static int bao_io_dispatcher_intc_probe(struct platform_device *pdev)
{
	int ret;

	// get the number of interrupts from the device tree
	irq_number = platform_irq_count(pdev);

	if (irq_number < 0) {
		dev_err(&pdev->dev, "Zero interrupts configured\n");
		return irq_number;
	}
	if (irq_number > BAO_IO_DISPATCHER_INTC_MAX_IRQS) {
		dev_err(&pdev->dev, "Too many interrupts\n");
		return -EINVAL;
	}

	for (int i = 0; i < irq_number; i++) {
		// get the interrupt number from the device tree
		ret = platform_get_irq(pdev, i);
		if (ret < 0) {
			dev_err(&pdev->dev, "Failed to read interrupt numbers\n");
			return ret;
		}
		// save the interrupt number
		irq_numbers[i] = ret;

		// request the interrupt to the kernel and register the handler
		ret = request_irq(irq_numbers[i], bao_interrupt_handler, 0,
				"bao-io-dispatcher-intc", pdev);
		if (ret) {
			dev_err(&pdev->dev, "Failed to request IRQ %d\n", irq_numbers[i]);
			return ret;
		}
	}

	return 0;
}

/**
 * bao_io_dispatcher_intc_remove - Remove the interrupt handler
 * @pdev: Platform device pointer
 */
static int bao_io_dispatcher_intc_remove(struct platform_device *pdev)
{
	for (int i = 0; i < irq_number; i++) {
		// free the interrupt
		free_irq(irq_numbers[i], pdev);
	}
	return 0;
}

static const struct of_device_id bao_io_dispatcher_intc_dt_ids[] = {
	{ .compatible = "bao,io-dispatcher-intc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, bao_io_dispatcher_intc_dt_ids);

static struct platform_driver bao_io_dispatcher_intc_driver = {
    .probe = bao_io_dispatcher_intc_probe,
    .remove = bao_io_dispatcher_intc_remove,
    .driver = {
        .name = "bao-io-dispatcher-intc",
        .of_match_table = of_match_ptr(bao_io_dispatcher_intc_dt_ids),
        .owner = THIS_MODULE,
    },
};

module_platform_driver(bao_io_dispatcher_intc_driver);

MODULE_AUTHOR("João Peixoto <joaopeixotooficial@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bao Hypervisor I/O Dispatcher Interrupt Controller");