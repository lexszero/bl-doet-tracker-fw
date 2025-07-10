/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <app_version.h>

#include <app/drivers/led_status.h>
#include <app/lib/gnss.h>
#include <app/lib/led_status.h>


LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define LEDS_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)


int main(void)
{
	LOG_INF("DoET :: Tracker %s\n", APP_VERSION_STRING);

	settings_subsys_init();
	gnss_init();

	led_status_blink_once(LED_G, 100, 100, 3);

	while (1) {
		k_sleep(K_MSEC(500));
	}

	return 0;
}
