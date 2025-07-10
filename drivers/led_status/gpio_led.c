/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT led_status_gpio

#include <zephyr/device.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app/drivers/led_status.h>

LOG_MODULE_REGISTER(led_status, CONFIG_LED_STATUS_LOG_LEVEL);

enum led_status_mode {
	LED_STATUS_ON,
	LED_STATUS_OFF,
	LED_STATUS_BLINK_ONCE,
	LED_STATUS_BLINK_CONTINUOUS,
};

struct led_status_state {
	enum led_status_mode mode;
	uint32_t on_ms, off_ms, repeat_ms;
	uint32_t blink_num;

	uint32_t blink_count;
	bool led_state;
};

struct led_status_gpio_data {
	struct k_timer timer;
	struct led_status_state state, previous_state;
};

struct led_status_gpio_config {
	struct gpio_dt_spec led;
};

static int led_status_gpio_set_output(const struct device *dev, bool on)
{
	const struct led_status_gpio_config *config = dev->config;
	struct led_status_gpio_data *data = dev->data;
	struct led_status_state *state = &data->state;

	int ret = gpio_pin_set_dt(&config->led, on);
	if (ret < 0)
		LOG_ERR("Failed to set status LED output GPIO: %d", ret);
	state->led_state = on;

	return ret;
}

static int led_status_gpio_set_mode(const struct device *dev, enum led_status_mode mode)
{
	int ret;
	struct led_status_gpio_data *data = dev->data;
	struct led_status_state *state = &data->state;
	switch (mode) {
		case LED_STATUS_ON:
			ret = led_status_gpio_set_output(dev, true);
			if (ret < 0) 
				return ret;
			break;

		case LED_STATUS_OFF:
			ret = led_status_gpio_set_output(dev, false);
			if (ret < 0) 
				return ret;
			break;

		case LED_STATUS_BLINK_ONCE:
		case LED_STATUS_BLINK_CONTINUOUS:
			ret = led_status_gpio_set_output(dev, true);
			if (ret < 0) 
				return ret;
			state->blink_count = state->blink_num;
			LOG_DBG("start timer on_ms=%d", state->on_ms);
			k_timer_start(&data->timer, K_MSEC(state->on_ms), K_NO_WAIT);
			break;
	}
	state->mode = mode;
	return 0;
}

static void led_status_gpio_on_timer_expire(struct k_timer *timer)
{
	const struct device *dev = k_timer_user_data_get(timer);
	const struct led_status_gpio_config *config = dev->config;
	struct led_status_gpio_data *data = dev->data;
	struct led_status_state *state = &data->state;

	if (!state->led_state) {
		led_status_gpio_set_output(dev, true);
		LOG_DBG("start timer on_ms=%d", state->on_ms);
		k_timer_start(&data->timer, K_MSEC(state->on_ms), K_NO_WAIT);
	} else {
		led_status_gpio_set_output(dev, false);
		if (state->blink_count > 0)
			state->blink_count--;
		if (state->blink_count > 0 && state->off_ms) {
			LOG_DBG("start timer off_ms=%d", state->off_ms);
			k_timer_start(&data->timer, K_MSEC(state->off_ms), K_NO_WAIT);
		} else {
			if (state->mode == LED_STATUS_BLINK_ONCE) {
				memcpy(&data->state, &data->previous_state, sizeof(struct led_status_state));
				led_status_gpio_set_mode(dev, state->mode);
			} else if (state->mode == LED_STATUS_BLINK_CONTINUOUS) {
				state->blink_count = state->blink_num;
				LOG_DBG("start timer repeat_ms=%d", state->repeat_ms);
				k_timer_start(&data->timer, K_MSEC(state->repeat_ms), K_NO_WAIT);
			}
		}
	}
}

static int led_status_gpio_set_solid(const struct device *dev, bool on)
{
	return led_status_gpio_set_mode(dev, on ? LED_STATUS_ON : LED_STATUS_OFF);
}

static int led_status_gpio_set_blink(const struct device *dev, uint32_t on_ms, uint32_t off_ms, uint32_t blink_num, uint32_t repeat_ms)
{
	struct led_status_gpio_data *data = dev->data;
	struct led_status_state *state = &data->state;

	if (!on_ms)
		return -EINVAL;
	if (!off_ms && blink_num > 0)
		return -EINVAL;

	state->on_ms = on_ms;
	state->off_ms = on_ms;
	state->blink_num = blink_num;
	state->repeat_ms = repeat_ms;
	if (repeat_ms) {
		return led_status_gpio_set_mode(dev, LED_STATUS_BLINK_CONTINUOUS);
	} else {
		memcpy(&data->previous_state, &data->state, sizeof(struct led_status_state));
		return led_status_gpio_set_mode(dev, LED_STATUS_BLINK_ONCE);
	}
}

static DEVICE_API(led_status, led_status_gpio_api) = {
	.set_solid = &led_status_gpio_set_solid,
	.set_blink = &led_status_gpio_set_blink,
};

static int led_status_gpio_init(const struct device *dev)
{
	const struct led_status_gpio_config *config = dev->config;
	struct led_status_gpio_data *data = dev->data;
	int ret;
	if (!gpio_is_ready_dt(&config->led)) {
		LOG_ERR("LED GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&config->led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Could not configure status LED GPIO (%d)", ret);
		return ret;
	}

	k_timer_init(&data->timer, led_status_gpio_on_timer_expire, NULL);
	k_timer_user_data_set(&data->timer, (void *)dev);

	return led_status_gpio_set_mode(dev, LED_STATUS_OFF);
}

#define LED_STATUS_GPIO_DEFINE(inst)                                            \
	static struct led_status_gpio_data data##inst;                          \
                                                                               \
	static const struct led_status_gpio_config config##inst = {             \
	    .led = GPIO_DT_SPEC_INST_GET(inst, gpios),                     \
	};                                                                     \
                                                                               \
	DEVICE_DT_INST_DEFINE(inst, led_status_gpio_init, NULL, &data##inst,    \
			      &config##inst, POST_KERNEL,                      \
			      CONFIG_LED_STATUS_INIT_PRIORITY,                      \
			      &led_status_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(LED_STATUS_GPIO_DEFINE)
