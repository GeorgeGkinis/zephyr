/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT espressif_esp32_gpio

/* Include esp-idf headers first to avoid redefining BIT() macro */
#include <soc/dport_reg.h>
#include <soc/gpio_reg.h>
#include <soc/io_mux_reg.h>
#include <soc/soc.h>

#include <soc.h>
#include <errno.h>
#include <device.h>
#include <drivers/gpio.h>
#include <kernel.h>
#include <sys/util.h>
#include <drivers/pinmux.h>

#include "gpio_utils.h"

#define GET_GPIO_PIN_REG(pin) ((u32_t *)GPIO_REG(pin))

/* ESP3 TRM v4.0 and gpio_reg.h header both incorrectly identify bit3
 * as being the bit selecting PRO CPU interrupt enable. It's actually
 * bit2.  bit4 and bit5 are also shifted.
 */
#define GPIO_CPU0_INT_ENABLE (BIT(2) << GPIO_PIN_INT_ENA_S)

/* ESP3 TRM table 8: CPU Interrupts
 *
 * Edge-triggered are: 10, 22, 28, 30
 * Level-triggered are: 0-5, 8, 9, 12, 13, 17-21, 23-27, 31
 */
#define ESP32_IRQ_EDGE_TRIG 0x50400400
#define ESP32_IRQ_LEVEL_TRIG 0x8fbe333f

struct gpio_esp32_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
	struct device *pinmux;

	struct {
		volatile u32_t *set_reg;
		volatile u32_t *clear_reg;
		volatile u32_t *input_reg;
		volatile u32_t *output_reg;
		volatile u32_t *irq_status_reg;
		volatile u32_t *irq_ack_reg;
		int pin_offset;
	} port;

	u32_t cb_pins;
	sys_slist_t cb;
};

static int gpio_esp32_config(struct device *dev,
			     gpio_pin_t pin,
			     gpio_flags_t flags)
{
	struct gpio_esp32_data *data = dev->driver_data;
	u32_t io_pin = pin + data->port.pin_offset; /* Range from 0 - 39 */
	u32_t *reg = GET_GPIO_PIN_REG(io_pin);
	u32_t func;
	int r;

	/* Query pinmux to validate pin number. */
	r = pinmux_pin_get(data->pinmux, io_pin, &func);
	if (r < 0) {
		return r;
	}

	/* Set pin function as GPIO */
	pinmux_pin_set(data->pinmux, io_pin, PIN_FUNC_GPIO);


	if (flags & GPIO_PULL_UP) {
		pinmux_pin_pullup(data->pinmux, io_pin, PINMUX_PULLUP_ENABLE);
	} else if (flags & GPIO_PULL_DOWN) {
		pinmux_pin_pullup(data->pinmux, io_pin, PINMUX_PULLUP_DISABLE);
	}

	if (flags & GPIO_OUTPUT) {

		if (flags & GPIO_SINGLE_ENDED) {
			if (flags & GPIO_LINE_OPEN_DRAIN) {
				*reg |= GPIO_PIN_PAD_DRIVER;
			} else {
				r = -ENOTSUP;
			}
		} else {
			*reg &= ~GPIO_PIN_PAD_DRIVER;
		}

		/* Set output pin initial value */
		if (flags & GPIO_OUTPUT_INIT_HIGH) {
			*data->port.set_reg = BIT(pin);
		} else if (flags & GPIO_OUTPUT_INIT_LOW) {
			*data->port.clear_reg = BIT(pin);
		}

		r = pinmux_pin_input_enable(data->pinmux, io_pin,
					    PINMUX_OUTPUT_ENABLED);
		if (r < 0) {
			return r;
		}

	} else { /* Input */
		pinmux_pin_input_enable(data->pinmux, io_pin,
					PINMUX_INPUT_ENABLED);
	}

	return 0;
}

static int gpio_esp32_port_get_raw(struct device *port, u32_t *value)
{
	struct gpio_esp32_data *data = port->driver_data;

	*value = *data->port.input_reg;

	return 0;
}

static int gpio_esp32_port_set_masked_raw(struct device *port,
					  u32_t mask, u32_t value)
{
	struct gpio_esp32_data *data = port->driver_data;
	u32_t key;

	key = irq_lock();
	*data->port.output_reg = (*data->port.output_reg & ~mask)
				 | (mask & value);
	irq_unlock(key);

	return 0;
}

static int gpio_esp32_port_set_bits_raw(struct device *port,
					u32_t pins)
{
	struct gpio_esp32_data *data = port->driver_data;

	*data->port.set_reg = pins;
	return 0;
}

static int gpio_esp32_port_clear_bits_raw(struct device *port,
					  u32_t pins)
{
	struct gpio_esp32_data *data = port->driver_data;

	*data->port.clear_reg = pins;
	return 0;
}

static int gpio_esp32_port_toggle_bits(struct device *port,
				       u32_t pins)
{
	struct gpio_esp32_data *data = port->driver_data;
	u32_t key;

	key = irq_lock();
	*data->port.output_reg = (*data->port.output_reg ^ pins);
	irq_unlock(key);

	return 0;
}

static int convert_int_type(enum gpio_int_mode mode,
			    enum gpio_int_trig trig)
{
	/* Reference: "ESP32 Technical Reference Manual" > "IO_MUX and
	 * GPIO matrix" > "GPIO_PINn_INT_TYPE".
	 */

	if (mode == GPIO_INT_MODE_DISABLED) {
		return 0;       /* Disables interrupt for a pin. */
	}

	if (mode == GPIO_INT_MODE_LEVEL) {
		if ((ESP32_IRQ_LEVEL_TRIG & BIT(CONFIG_GPIO_ESP32_IRQ)) == 0) {
			return -ENOTSUP;
		}
		switch (trig) {
		case GPIO_INT_TRIG_LOW:
			return 4;
		case GPIO_INT_TRIG_HIGH:
			return 5;
		default:
			return -EINVAL;
		}
	} else { /* edge interrupts */
		if ((ESP32_IRQ_EDGE_TRIG & BIT(CONFIG_GPIO_ESP32_IRQ)) == 0) {
			return -ENOTSUP;
		}
		switch (trig) {
		case GPIO_INT_TRIG_HIGH:
			return 1;
		case GPIO_INT_TRIG_LOW:
			return 2;
		case GPIO_INT_TRIG_BOTH:
			/* This is supposed to work but doesn't */
			return -ENOTSUP; /* 3 == any edge */
		default:
			return -EINVAL;
		}
	}

	/* Any other type of interrupt triggering is invalid. */
	return -EINVAL;
}

static int gpio_esp32_pin_interrupt_configure(struct device *port,
					      gpio_pin_t pin,
					      enum gpio_int_mode mode,
					      enum gpio_int_trig trig)
{
	struct gpio_esp32_data *data = port->driver_data;
	u32_t io_pin = pin + data->port.pin_offset; /* Range from 0 - 39 */
	u32_t *reg = GET_GPIO_PIN_REG(io_pin);
	int intr_trig_mode = convert_int_type(mode, trig);
	u32_t reg_val;
	u32_t key;

	if (intr_trig_mode < 0) {
		return intr_trig_mode;
	}

	if (mode == GPIO_INT_MODE_DISABLED) {
		data->cb_pins &= ~BIT(pin);
	} else {
		data->cb_pins |= BIT(pin);
	}

	key = irq_lock();

	reg_val = *reg;
	reg_val &= ~(GPIO_PIN_INT_ENA_M | GPIO_PIN_INT_TYPE_M);
	/* Enable Interrupt on CPU0 (PRO_CPU) */
	reg_val |= GPIO_CPU0_INT_ENABLE;
	/* Interrupt triggering mode */
	reg_val |= intr_trig_mode << GPIO_PIN_INT_TYPE_S;
	*reg = reg_val;

	irq_unlock(key);

	return 0;
}

static int gpio_esp32_manage_callback(struct device *dev,
				      struct gpio_callback *callback,
				      bool set)
{
	struct gpio_esp32_data *data = dev->driver_data;

	return gpio_manage_callback(&data->cb, callback, set);
}

static int gpio_esp32_enable_callback(struct device *dev,
				      gpio_pin_t pin)
{
	struct gpio_esp32_data *data = dev->driver_data;

	data->cb_pins |= BIT(pin);

	return 0;
}

static int gpio_esp32_disable_callback(struct device *dev,
				       gpio_pin_t pin)
{
	struct gpio_esp32_data *data = dev->driver_data;

	data->cb_pins &= ~BIT(pin);

	return 0;
}

static void gpio_esp32_fire_callbacks(struct device *device)
{
	struct gpio_esp32_data *data = device->driver_data;
	u32_t values = *data->port.irq_status_reg;

	*data->port.irq_ack_reg = values;
	if (values & data->cb_pins) {
		gpio_fire_callbacks(&data->cb, device, values);
	}
}

static void gpio_esp32_isr(void *param);

static int gpio_esp32_init(struct device *device)
{
	struct gpio_esp32_data *data = device->driver_data;
	static bool isr_connected;

	data->pinmux = device_get_binding(CONFIG_PINMUX_NAME);
	if (!data->pinmux) {
		return -ENOTSUP;
	}

	if (!isr_connected) {
		irq_disable(CONFIG_GPIO_ESP32_IRQ);

		IRQ_CONNECT(CONFIG_GPIO_ESP32_IRQ, 1, gpio_esp32_isr,
			    NULL, 0);

		esp32_rom_intr_matrix_set(0, ETS_GPIO_INTR_SOURCE,
					  CONFIG_GPIO_ESP32_IRQ);

		irq_enable(CONFIG_GPIO_ESP32_IRQ);

		isr_connected = true;
	}

	return 0;
}

static const struct gpio_driver_api gpio_esp32_driver = {
	.pin_configure = gpio_esp32_config,
	.port_get_raw = gpio_esp32_port_get_raw,
	.port_set_masked_raw = gpio_esp32_port_set_masked_raw,
	.port_set_bits_raw = gpio_esp32_port_set_bits_raw,
	.port_clear_bits_raw = gpio_esp32_port_clear_bits_raw,
	.port_toggle_bits = gpio_esp32_port_toggle_bits,
	.pin_interrupt_configure = gpio_esp32_pin_interrupt_configure,
	.manage_callback = gpio_esp32_manage_callback,
	.enable_callback = gpio_esp32_enable_callback,
	.disable_callback = gpio_esp32_disable_callback,
};

#if defined(CONFIG_GPIO_ESP32_0)
static struct gpio_esp32_data gpio_0_data = { /* 0..31 */
	.port = {
		.set_reg = (u32_t *)GPIO_OUT_W1TS_REG,
		.clear_reg = (u32_t *)GPIO_OUT_W1TC_REG,
		.input_reg = (u32_t *)GPIO_IN_REG,
		.output_reg = (u32_t *)GPIO_OUT_REG,
		.irq_status_reg = (u32_t *)GPIO_STATUS_REG,
		.irq_ack_reg = (u32_t *)GPIO_STATUS_W1TC_REG,
		.pin_offset = 0,
	}
};
#endif

#if defined(CONFIG_GPIO_ESP32_1)
static struct gpio_esp32_data gpio_1_data = { /* 32..39 */
	.port = {
		.set_reg = (u32_t *)GPIO_OUT1_W1TS_REG,
		.clear_reg = (u32_t *)GPIO_OUT1_W1TC_REG,
		.input_reg = (u32_t *)GPIO_IN1_REG,
		.output_reg = (u32_t *)GPIO_OUT1_REG,
		.irq_status_reg = (u32_t *)GPIO_STATUS1_REG,
		.irq_ack_reg = (u32_t *)GPIO_STATUS1_W1TC_REG,
		.pin_offset = 32,
	}
};
#endif

#define GPIO_DEVICE_INIT(_id) \
	static struct gpio_driver_config gpio_##_id##_cfg = { \
		.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(_id),  \
	}; \
	DEVICE_AND_API_INIT(gpio_esp32_##_id,				\
			    DT_INST_LABEL(_id),	\
			    gpio_esp32_init,				\
			    &gpio_##_id##_data, &gpio_##_id##_cfg,	\
			    POST_KERNEL,				\
			    CONFIG_KERNEL_INIT_PRIORITY_DEVICE,		\
			    &gpio_esp32_driver)

/* GPIOs are divided in two groups for ESP32 because the callback
 * API works with 32-bit bitmasks to manage interrupt callbacks,
 * and the device has 40 GPIO pins.
 */
#if defined(CONFIG_GPIO_ESP32_0)
GPIO_DEVICE_INIT(0);
#endif

#if defined(CONFIG_GPIO_ESP32_1)
GPIO_DEVICE_INIT(1);
#endif

static void gpio_esp32_isr(void *param)
{

#if defined(CONFIG_GPIO_ESP32_0)
	gpio_esp32_fire_callbacks(DEVICE_GET(gpio_esp32_0));
#endif
#if defined(CONFIG_GPIO_ESP32_1)
	gpio_esp32_fire_callbacks(DEVICE_GET(gpio_esp32_1));
#endif

	ARG_UNUSED(param);
}
