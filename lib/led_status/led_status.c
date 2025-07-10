#include <stdlib.h>
#include <zephyr/drivers/led>
#include <app/lib/led_status.h>

LOG_MODULE_REGISTER(app_led_status, CONFIG_APP_LED_STATUS_LOG_LEVEL);

static void led_set(struct led_status *data, bool on)
{
	int ret  = on ? led_on(data->dev, data->led) : led_off(data->dev, data->led);
	if (ret < 0) {
		LOG_ERR("Failed to set LED state: %d", ret);
	}
	data->current.state = on;
}

static void led_status_set_mode(struct led_status *data, enum led_status_mode mode)
{
	struct led_status_state *current = &data->current;
	current.mode = mode;
	switch (mode) {
		case LED_STATUS_ON:
			led_set(data, true);
			break;

		case LED_STATUS_OFF:
			led_set(data, false);
			break;

		case LED_STATUS_BLINK_ONCE:
			led_set(data, true);
			current->blink_count = current->blink_num;
			k_timer_start(&data->blink_timer, K_MSEC(data->current.on_ms), K_NO_WAIT);
			break;

		case LED_STATUS_BLINK_CONTINUOUS:
			led_set(data, true);
			current->blink_count = current->blink_num;
			k_timer_start(&data->blink_timer, K_MSEC(data->current.on_ms), K_NO_WAIT);
			break;
	}
}

static int led_status_timer_expire(struct k_timer *timer)
{
	struct led_status *data = k_timer_user_data_get(timer);
	struct led_status_state *current = &data->current;

	if (!current->state) {
		led_set(data, true);
		k_timer_start(&data->blink_timer, K_MSEC(current->on_ms), K_NO_WAIT);
	} else {
		led_set(data, false);
		if (current->blink_count > 0) {
			current->blink_count--;
		}
		if (current->blink_count > 0) {
			k_timer_start(&data->blink_timer, K_MSEC(current->off_ms), K_NO_WAIT);
		} else {
			if (current->mode == LED_STATUS_BLINK_ONCE) {
				memcpy(&data->current, &data->previous, sizeof(struct led_status_state));
				data->current.state = false;
				led_status_set_mode(data, current->mode);
			} else if (current->mode == LED_STATUS_BLINK_CONTINUOUS) {
				current->blink_count = current->blink_num;
				k_timer_start(&data->blink_timer, K_MSEC(current->repeat_ms), K_NO_WAIT);
			}
		}
	}
	return 0;
}

int led_status_init(struct led_status *data, struct device *dev, uint32_t led)
{
	data->dev = dev;
	data->led = led;
	k_timer_init(&data->timer, led_status_timer_expire, NULL);
	k_timer_user_data_set(&data->timer, (void *)data);

	led_set_mode(data, LED_STATUS_OFF);
	return 0;
}

int led_status_init_all(struct device *dev)
{
	for (int i = 0; i < __LED_STATUS_ID_MAX; i++) {
		int ret = led_status_init(&status_leds[i], dev, i);
		if (ret < 0) {
			LOG_ERR("Failed to init led #%d: %d", i, ret);
			return ret;
		}
	}
	return 0;
}

int led_status_on(struct led_status *data)
{
	data->current.mode = LED_STATUS_ON;
	led_set(data, true);
}

int led_status_off(struct led_status *data)
{
	data->current.mode = LED_STATUS_OFF;
	led_set(data, true);
}

int led_status_blink_once(struct led_status *data, uint32_t on_ms, uint32_t off_ms, uint32_t blink_num)
{
	if (!on_ms)
		return -EINVAL;
	if (!off_ms && blink_num > 0)
		return -EINVAL;

	memcpy(&data->previous, data->current, sizeof(struct led_status_state));
	data->current.on_ms = on_ms;
	data->current.off_ms = on_ms;
	data->current.blink_num = blink_num;
	led_status_set_mode(data, LED_STATUS_BLINK_ONCE)

	return 0;
}

int led_status_blink_continuous(struct led_status *data, uint32_t on_ms, uint32_t off_ms, int32_t blink_num, uint32_t repeat_ms)
{
	if (!on_ms)
		return -EINVAL;
	if (!off_ms && blink_num > 0)
		return -EINVAL;

	data->current.on_ms = on_ms;
	data->current.off_ms = on_ms;
	data->current.blink_num = blink_num;
	led_status_set_mode(data, LED_STATUS_BLINK_CONTINUOUS);

	return 0;
}


