/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <app_version.h>

#include <app/drivers/led_status.h>
#include <app/lib/gnss.h>
#include <app/lib/led_status.h>
#include <app/lib/lorawan_node.h>

#include <app/settings.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define LEDS_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

#define POSITION_UPLINK_INTERVAL_MS 10000

bool lorawan_joined = false;
int64_t last_uplink_timestamp = 0;

#define EV_GNSS_POSITION 1
K_EVENT_DEFINE(events);

struct msg_up_position {
	int32_t lat;
	int32_t lon;
	uint16_t hdop;
} __attribute__((packed));

void gnss_position_cb(const struct navigation_data *nav_data)
{
	k_event_post(&events, EV_GNSS_POSITION);
}

void pack_angle(int64_t a, uint8_t *buf)
{
	for (int i = 0; i < 8; i++) {
		buf[i] = a & 0xFF;
		a >>= 8;
	}
}

int handle_event_gnss_position()
{
	if (!lorawan_joined)
		return 0;

	int64_t now = k_uptime_get();
	if (now - last_uplink_timestamp < POSITION_UPLINK_INTERVAL_MS)
		return 0;

	/*
	uint8_t buf[8+8+2];
	pack_angle(gnss_data.nav_data.latitude, buf+0);
	pack_angle(gnss_data.nav_data.longitude, buf+8);
	*/
	struct msg_up_position msg = {
		.lat = (int32_t)(gnss_data.nav_data.latitude >> 5),
		.lon = (int32_t)(gnss_data.nav_data.longitude >> 5),
		.hdop = gnss_data.info.hdop
	};

	int ret = lorawan_send(4, (uint8_t *)&msg, sizeof(msg), LORAWAN_MSG_UNCONFIRMED);
	if (ret < 0) {
		LOG_ERR("lorawan_send failed: %d", ret);
		return ret;
	}

	last_uplink_timestamp = now;
	led_status_on(LED_B);
	k_sleep(K_MSEC(1000));
	led_status_off(LED_B);

	return 0;
}


int main(void)
{
	LOG_INF("DoET :: Tracker %s\n", APP_VERSION_STRING);

	gnss_init(gnss_position_cb);
	led_status_blink_once(LED_G, 100, 100, 3);
	lorawan_node_init();

	int ret;

	while (1) {
		if (!lorawan_joined) {
			led_status_blink_continuous(LED_B, 100, 100, 3, 0);
			ret = lorawan_node_join();
			if (ret == 0) {
				lorawan_joined = true;
				led_status_on(LED_B);
				k_sleep(K_MSEC(500));
				led_status_off(LED_B);
				led_status_on(LED_G);
				k_sleep(K_MSEC(500));
				led_status_off(LED_G);

				const uint8_t msg[] = {0xde, 0xad, 0xca, 0xfe};
				int ret = lorawan_send(13, msg, sizeof(msg), LORAWAN_MSG_UNCONFIRMED);
				if (ret < 0) {
					LOG_ERR("lorawan_send failed: %d", ret);
					return ret;
				}

				led_status_on(LED_B);
				k_sleep(K_MSEC(1000));
				led_status_off(LED_B);

			} else {
				led_status_on(LED_B);
				k_sleep(K_MSEC(500));
				led_status_off(LED_B);
				led_status_on(LED_R);
				k_sleep(K_MSEC(500));
				led_status_off(LED_R);
			}

			continue;
		}

		uint32_t ev = k_event_wait(&events, 0xFFF, false, K_MSEC(50));
		if (ev & EV_GNSS_POSITION) {
			handle_event_gnss_position();
		}
	}

	return 0;
}
