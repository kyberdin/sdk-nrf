/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/ring_buffer.h>

#define LED_STACKSIZE 512
#define UART_STACKSIZE 2048

#define LED_PRIORITY 7
#define UART_PRIORITY 6

#define DEV_CONSOLE DEVICE_DT_GET(DT_CHOSEN(zephyr_console))
#define DEV_UART_IN DEVICE_DT_GET(DT_NODELABEL(uart135))

#define LED0_NODE DT_ALIAS(led0)
#define BLINK_INTERVAL_MS 500
#define RX_RING_SIZE 1024

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

RING_BUF_DECLARE(rx_ring, RX_RING_SIZE);

static volatile bool rx_overflow;

static void uart_rx_irq_cb(const struct device *dev, void *user_data)
{
	uint8_t byte;
	int n;

	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) > 0 && uart_irq_rx_ready(dev)) {
		n = uart_fifo_read(dev, &byte, 1);

		if (n <= 0) {
			break;
		}

		/* Move byte from UART into ring immediately so it can't be overwritten. */
		if (ring_buf_put(&rx_ring, &byte, 1) != 1) {
			rx_overflow = true;
			uart_irq_rx_disable(dev);
			break;
		}
	}
}

static bool forward_to_console(void)
{
	uint8_t byte;

	if (rx_overflow) {
		rx_overflow = false;
		uart_irq_rx_enable(DEV_UART_IN);
		printk("\noverflow\n");
		return true;
	}

	if (ring_buf_get(&rx_ring, &byte, 1) != 1) {
		return false;
	}

	if (byte == '\n') {
		uart_poll_out(DEV_CONSOLE, '\r');
	}

	uart_poll_out(DEV_CONSOLE, byte);

	return true;
}

static void forwarding_thread(void *p1, void *p2, void *p3)
{
	while (1) {
		if (!forward_to_console()) {
			k_msleep(1);
		}
	}
}

static void blink_thread(void *p1, void *p2, void *p3)
{
	const struct gpio_dt_spec *led = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!gpio_is_ready_dt(led) || gpio_pin_configure_dt(led, GPIO_OUTPUT_ACTIVE) < 0) {
		return;
	}

	while (1) {
		gpio_pin_toggle_dt(led);
		k_msleep(BLINK_INTERVAL_MS);
	}
}

K_THREAD_DEFINE(blink_tid, LED_STACKSIZE, blink_thread, &led0, NULL, NULL,
		LED_PRIORITY, 0, 0);

K_THREAD_DEFINE(uart_tid, UART_STACKSIZE, forwarding_thread, NULL, NULL, NULL,
		UART_PRIORITY, 0, 0);

int main(void)
{

	if (!device_is_ready(DEV_CONSOLE)) {
		printk("Console device not ready\n");
		return -1;
	}

	if (!device_is_ready(DEV_UART_IN)) {
		printk("UART input not ready\n");
		return -1;
	}

	uart_irq_callback_user_data_set(DEV_UART_IN, uart_rx_irq_cb, NULL);
	uart_irq_rx_enable(DEV_UART_IN);

	k_thread_start(blink_tid);
	k_thread_start(uart_tid);

	return 0;
}
