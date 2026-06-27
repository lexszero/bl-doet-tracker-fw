#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/logging/log.h>

#include "app/lib/led_status.h"
#include "app/lib/gnss.h"

LOG_MODULE_REGISTER(app_gnss, CONFIG_APP_GNSS_LOG_LEVEL);

#define GNSS_DEVICE DEVICE_DT_GET(DT_ALIAS(gnss))

struct gnss_data gnss_data;

static gnss_position_cb_t gnss_position_cb;

static const char *gnss_fix_status_to_str(enum gnss_fix_status fix_status)
{
	switch (fix_status) {
	case GNSS_FIX_STATUS_NO_FIX:
		return "NO_FIX";
	case GNSS_FIX_STATUS_GNSS_FIX:
		return "GNSS_FIX";
	case GNSS_FIX_STATUS_DGNSS_FIX:
		return "DGNSS_FIX";
	case GNSS_FIX_STATUS_ESTIMATED_FIX:
		return "ESTIMATED_FIX";
	}

	return "unknown";
}

static const char *gnss_fix_quality_to_str(enum gnss_fix_quality fix_quality)
{
	switch (fix_quality) {
	case GNSS_FIX_QUALITY_INVALID:
		return "INVALID";
	case GNSS_FIX_QUALITY_GNSS_SPS:
		return "GNSS_SPS";
	case GNSS_FIX_QUALITY_DGNSS:
		return "DGNSS";
	case GNSS_FIX_QUALITY_GNSS_PPS:
		return "GNSS_PPS";
	case GNSS_FIX_QUALITY_RTK:
		return "RTK";
	case GNSS_FIX_QUALITY_FLOAT_RTK:
		return "FLOAT_RTK";
	case GNSS_FIX_QUALITY_ESTIMATED:
		return "ESTIMATED";
	}

	return "unknown";
}

#if CONFIG_GNSS_SATELLITES
static void gnss_satellites_cb(const struct device *dev, const struct gnss_satellite *satellites,
			       uint16_t size)
{
	unsigned int n_tracked = 0;

	for (unsigned int i = 0; i != size; ++i) {
		n_tracked += satellites[i].is_tracked;
	}
	LOG_INF("satellites: %u / %u", n_tracked, size);
}
GNSS_SATELLITES_CALLBACK_DEFINE(GNSS_DEVICE, gnss_satellites_cb);
#endif

static void gnss_data_cb(const struct device *dev, const struct gnss_data *data)
{
	const struct gnss_info *info = &data->info;
	const struct navigation_data *nav_data = &data->nav_data;
	const struct gnss_time *utc = &data->utc;
	enum gnss_fix_status previous_fix_status = gnss_data.info.fix_status;

	if (info->fix_status == GNSS_FIX_STATUS_NO_FIX) {
		led_status_blink_once(LED_R, 50, 50, 2);
		if (previous_fix_status != GNSS_FIX_STATUS_NO_FIX) {
			LOG_INF("fix lost");
		}
	} else {
		led_status_blink_once(LED_G, 50, 50, 2);
		if (previous_fix_status == GNSS_FIX_STATUS_NO_FIX) {
			LOG_INF("fix acquired");
		}
		LOG_DBG("info: satellites_cnt: %u, hdop: %u.%u, fix_status: %s, fix_quality: %s",
				info->satellites_cnt,
				info->hdop / 1000, info->hdop % 1000,
				gnss_fix_status_to_str(info->fix_status),
				gnss_fix_quality_to_str(info->fix_quality));


		char *lat_sign = nav_data->latitude < 0 ? "-" : "+";
		char *lon_sign = nav_data->longitude < 0 ? "-" : "+";
		char *alt_sign = nav_data->altitude < 0 ? "-" : "+";

		LOG_DBG("position: lat: %s%lli.%09lli, lon: %s%lli.%09lli, "
				"bearing: %u.%03u, speed: %u.%03u, altitude: %s%i.%03i",
				lat_sign,
				llabs(nav_data->latitude) / 1000000000,
				llabs(nav_data->latitude) % 1000000000,
				lon_sign,
				llabs(nav_data->longitude) / 1000000000,
				llabs(nav_data->longitude) % 1000000000,
				nav_data->bearing / 1000, nav_data->bearing % 1000,
				nav_data->speed / 1000, nav_data->speed % 1000,
				alt_sign, abs(nav_data->altitude) / 1000, abs(nav_data->altitude) % 1000);

		LOG_DBG("time: 20%02u-%02u-%02u %02u:%02u:%02u.%03u",
			utc->century_year,
			utc->month,
			utc->month_day,
			utc->hour,
			utc->minute,
			utc->millisecond / 1000U,
			utc->millisecond % 1000U
			);

	}

	memcpy(&gnss_data, data, sizeof(struct gnss_data));

	if (info->fix_status != GNSS_FIX_STATUS_NO_FIX && gnss_position_cb) {
		gnss_position_cb(&gnss_data.nav_data, gnss_data.info.hdop);
	}
}

GNSS_DATA_CALLBACK_DEFINE(GNSS_DEVICE, gnss_data_cb);

int gnss_init(gnss_position_cb_t position_cb) {
	gnss_position_cb = position_cb;

	uint32_t fix_interval;
	int rc;
	rc = gnss_get_fix_rate(GNSS_DEVICE, &fix_interval);
	if (rc < 0) {
		printf("Failed to query fix rate (%d)\n", rc);
		return rc;
	}
	printf("Fix rate = %d ms\n", fix_interval);

	return 0;
}

