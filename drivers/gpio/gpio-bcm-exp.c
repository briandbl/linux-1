/*
 *  Broadcom expander GPIO driver
 *
 *  Uses the firmware mailbox service to communicate with the
 *  GPIO expander on the VPU.
 *
 *  Copyright (C) 2017 Raspberry Pi Trading Ltd.
 *
 *  Author: Dave Stevenson <dave.stevenson@raspberrypi.org>
 *  Based on gpio-bcm-virt.c by Dom Cobley <popcornmix@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define MODULE_NAME "brcmexp-gpio"
#define NUM_GPIO 8

struct brcmexp_gpio {
	struct gpio_chip gc;
	struct device *dev;
	struct rpi_firmware *fw;
};

struct gpio_set_config {
	u32 gpio, direction, polarity, term_en, term_pull_up, state;
};

struct gpio_get_config {
	u32 gpio, direction, polarity, term_en, term_pull_up;
};

struct gpio_get_set_state {
	u32 gpio, state;
};

static int brcmexp_gpio_get_polarity(struct gpio_chip *gc, unsigned int off)
{
	struct brcmexp_gpio *gpio;
	struct gpio_get_config get;
	int ret;

	gpio = container_of(gc, struct brcmexp_gpio, gc);

	get.gpio = off + gpio->gc.base;	/* GPIO to update */

	ret = rpi_firmware_property(gpio->fw, RPI_FIRMWARE_GET_GPIO_CONFIG,
				    &get, sizeof(get));
	if (ret) {
		dev_err(gpio->dev,
			"Failed to get GPIO %u config (%d)\n", off, ret);
		return ret;
	}
	return get.polarity;
}

static int brcmexp_gpio_dir_in(struct gpio_chip *gc, unsigned int off)
{
	struct brcmexp_gpio *gpio;
	struct gpio_set_config set_in;
	int ret;

	gpio = container_of(gc, struct brcmexp_gpio, gc);

	set_in.gpio = off + gpio->gc.base;	/* GPIO to update */
	set_in.direction = 0;		/* Input */
	set_in.polarity = brcmexp_gpio_get_polarity(gc, off);
					/* Retain existing setting */
	set_in.term_en = 0;		/* termination disabled */
	set_in.term_pull_up = 0;	/* n/a as termination disabled */
	set_in.state = 0;		/* n/a as configured as an input */

	ret = rpi_firmware_property(gpio->fw, RPI_FIRMWARE_SET_GPIO_CONFIG,
				    &set_in, sizeof(set_in));
	if (ret) {
		dev_err(gpio->dev,
			"Failed to set GPIO %u to input (%d)\n",
			off, ret);
		return ret;
	}
	return 0;
}

static int brcmexp_gpio_dir_out(struct gpio_chip *gc, unsigned int off, int val)
{
	struct brcmexp_gpio *gpio;
	struct gpio_set_config set_out;
	int ret;

	gpio = container_of(gc, struct brcmexp_gpio, gc);

	set_out.gpio = off + gpio->gc.base;	/* GPIO to update */
	set_out.direction = 1;		/* Output */
	set_out.polarity = brcmexp_gpio_get_polarity(gc, off);
					/* Retain existing setting */
	set_out.term_en = 0;		/* n/a as an output */
	set_out.term_pull_up = 0;	/* n/a as termination disabled */
	set_out.state = val;		/* Output state */

	ret = rpi_firmware_property(gpio->fw, RPI_FIRMWARE_SET_GPIO_CONFIG,
				    &set_out, sizeof(set_out));
	if (ret) {
		dev_err(gpio->dev,
			"Failed to set GPIO %u to output (%d)\n", off, ret);
		return ret;
	}
	return 0;
}

static int brcmexp_gpio_get_direction(struct gpio_chip *gc, unsigned int off)
{
	struct brcmexp_gpio *gpio;
	struct gpio_get_config get;
	int ret;

	gpio = container_of(gc, struct brcmexp_gpio, gc);

	get.gpio = off + gpio->gc.base;	/* GPIO to update */

	ret = rpi_firmware_property(gpio->fw, RPI_FIRMWARE_GET_GPIO_CONFIG,
				    &get, sizeof(get));
	if (ret) {
		dev_err(gpio->dev,
			"Failed to get GPIO %u config (%d)\n", off, ret);
		return ret;
	}
	return get.direction ? GPIOF_DIR_OUT : GPIOF_DIR_IN;
}

static int brcmexp_gpio_get(struct gpio_chip *gc, unsigned int off)
{
	struct brcmexp_gpio *gpio;
	struct gpio_get_set_state get;
	int ret;

	gpio = container_of(gc, struct brcmexp_gpio, gc);

	get.gpio = off + gpio->gc.base;	/* GPIO to update */
	get.state = 0;		/* storage for returned value */

	ret = rpi_firmware_property(gpio->fw, RPI_FIRMWARE_GET_GPIO_STATE,
					 &get, sizeof(get));
	if (ret) {
		dev_err(gpio->dev,
			"Failed to get GPIO %u state (%d)\n", off, ret);
		return ret;
	}
	return !!get.state;
}

static void brcmexp_gpio_set(struct gpio_chip *gc, unsigned int off, int val)
{
	struct brcmexp_gpio *gpio;
	struct gpio_get_set_state set;
	int ret;

	gpio = container_of(gc, struct brcmexp_gpio, gc);

	off += gpio->gc.base;

	set.gpio = off + gpio->gc.base;	/* GPIO to update */
	set.state = val;	/* Output state */

	ret = rpi_firmware_property(gpio->fw, RPI_FIRMWARE_SET_GPIO_STATE,
					 &set, sizeof(set));
	if (ret)
		dev_err(gpio->dev,
			"Failed to set GPIO %u state (%d)\n", off, ret);
}

static int brcmexp_gpio_probe(struct platform_device *pdev)
{
	int err = 0;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *fw_node;
	struct rpi_firmware *fw;
	struct brcmexp_gpio *ucb;

	fw_node = of_parse_phandle(np, "firmware", 0);
	if (!fw_node) {
		dev_err(dev, "Missing firmware node\n");
		return -ENOENT;
	}

	fw = rpi_firmware_get(fw_node);
	if (!fw)
		return -EPROBE_DEFER;

	ucb = devm_kzalloc(dev, sizeof(*ucb), GFP_KERNEL);
	if (!ucb)
		return -EINVAL;

	ucb->fw = fw;
	ucb->dev = dev;
	ucb->gc.label = MODULE_NAME;
	ucb->gc.owner = THIS_MODULE;
	ucb->gc.of_node = np;
	ucb->gc.base = 128;
	ucb->gc.ngpio = NUM_GPIO;

	ucb->gc.direction_input = brcmexp_gpio_dir_in;
	ucb->gc.direction_output = brcmexp_gpio_dir_out;
	ucb->gc.get_direction = brcmexp_gpio_get_direction;
	ucb->gc.get = brcmexp_gpio_get;
	ucb->gc.set = brcmexp_gpio_set;
	ucb->gc.can_sleep = true;

	err = gpiochip_add(&ucb->gc);
	if (err)
		return err;

	platform_set_drvdata(pdev, ucb);

	return 0;
}

static int brcmexp_gpio_remove(struct platform_device *pdev)
{
	struct brcmexp_gpio *ucb = platform_get_drvdata(pdev);

	gpiochip_remove(&ucb->gc);

	return 0;
}

static const struct of_device_id __maybe_unused brcmexp_gpio_ids[] = {
	{ .compatible = "brcm,bcm2835-expgpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, brcmexp_gpio_ids);

static struct platform_driver brcmexp_gpio_driver = {
	.driver	= {
		.name		= MODULE_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= of_match_ptr(brcmexp_gpio_ids),
	},
	.probe	= brcmexp_gpio_probe,
	.remove	= brcmexp_gpio_remove,
};
module_platform_driver(brcmexp_gpio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dave Stevenson <dave.stevenson@raspberrypi.org>");
MODULE_DESCRIPTION("brcm-exp GPIO driver");
MODULE_ALIAS("platform:brcmexp-gpio");
