/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/util.h>

#include <app_version.h>

#include <app/drivers/led_status.h>
#include <app/lib/gnss.h>
#include <app/lib/led_status.h>
#include <app/lib/lorawan_node.h>

#include <app/settings.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define LEDS_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

#define POSITION_UPLINK_MOVING_INTERVAL_MS 10000
#define POSITION_UPLINK_ACTIVE_INTERVAL_MS 30000
#define POSITION_UPLINK_STATIONARY_INTERVAL_MS 120000
#define POSITION_UPLINK_MOTION_RESUME_MIN_MS 3000

#define GNSS_GOOD_HDOP_MAX 2500
#define GNSS_GOOD_SATELLITES_MIN 4
#define GNSS_ACTIVE_SPEED_MM_S 500
#define GNSS_MOVING_SPEED_MM_S 2000
#define GNSS_STILL_ALTITUDE_JUMP_MM 5000
#define GNSS_DRIFT_SUPPRESS_MS 60000

#define ACCEL_GRAVITY_MM_S2 9807
#define ACCEL_GRAVITY_DELTA_MM_S2 2000
#define ACCEL_VECTOR_DELTA_MM_S2 1500
#define MOTION_HOLD_MS 30000

bool lorawan_joined = false;
int64_t last_uplink_timestamp = 0;
bool have_sent_position = false;

#define EV_GNSS_POSITION 1
K_EVENT_DEFINE(events);

#if DT_NODE_HAS_STATUS(DT_ALIAS(accel_0), okay)
static const struct device *const accel_dev = DEVICE_DT_GET(DT_ALIAS(accel_0));
#else
static const struct device *const accel_dev = NULL;
#endif

enum tracker_motion_state {
	TRACKER_MOTION_UNKNOWN,
	TRACKER_MOTION_STATIONARY,
	TRACKER_MOTION_ACTIVE,
	TRACKER_MOTION_MOVING,
};

struct accel_motion_sample {
	bool valid;
	bool moving;
	int32_t magnitude_mm_s2;
	int32_t vector_delta_mm_s2;
};

static bool accel_ready;
static bool have_last_accel;
static int32_t last_accel_x_mm_s2;
static int32_t last_accel_y_mm_s2;
static int32_t last_accel_z_mm_s2;
static enum tracker_motion_state motion_state = TRACKER_MOTION_UNKNOWN;
static int64_t last_motion_timestamp;
static int64_t last_motion_resume_timestamp;
static bool have_last_gnss_altitude;
static int32_t last_gnss_altitude_mm;
static int64_t gnss_drift_suppressed_until;
static bool gnss_drift_suppressed;

struct msg_up_position {
	int32_t lat;
	int32_t lon;
	uint16_t hdop;
} __attribute__((packed));

static const char *motion_state_name(enum tracker_motion_state state)
{
	switch (state) {
	case TRACKER_MOTION_STATIONARY:
		return "stationary";
	case TRACKER_MOTION_ACTIVE:
		return "active";
	case TRACKER_MOTION_MOVING:
		return "moving";
	case TRACKER_MOTION_UNKNOWN:
	default:
		return "unknown";
	}
}

static int64_t abs64(int64_t value)
{
	return value < 0 ? -value : value;
}

static uint32_t isqrt64(uint64_t value)
{
	uint64_t root = 0;
	uint64_t bit = 1ULL << 62;

	while (bit > value) {
		bit >>= 2;
	}

	while (bit != 0) {
		if (value >= root + bit) {
			value -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}

	return (uint32_t)root;
}

static int32_t sensor_value_to_mm_s2(const struct sensor_value *value)
{
	return (int32_t)(((int64_t)value->val1 * 1000) + (value->val2 / 1000));
}

static bool gnss_position_is_usable(const struct accel_motion_sample *accel)
{
	if (gnss_data.info.fix_status == GNSS_FIX_STATUS_NO_FIX) {
		return false;
	}

	if (gnss_data.info.fix_quality == GNSS_FIX_QUALITY_INVALID) {
		return false;
	}

	if (gnss_data.info.satellites_cnt < GNSS_GOOD_SATELLITES_MIN) {
		return false;
	}

	if (gnss_data.info.hdop != 0 && gnss_data.info.hdop > GNSS_GOOD_HDOP_MAX) {
		return false;
	}

	if (gnss_drift_suppressed) {
		return false;
	}

	if (accel_ready && accel->valid && !accel->moving &&
	    gnss_data.nav_data.speed >= GNSS_ACTIVE_SPEED_MM_S) {
		return false;
	}

	return true;
}

static void read_accel_motion(struct accel_motion_sample *sample)
{
	struct sensor_value accel[3];
	int ret;

	*sample = (struct accel_motion_sample){0};

	if (!accel_ready) {
		return;
	}

	ret = sensor_sample_fetch(accel_dev);
	if (ret < 0) {
		LOG_WRN("accelerometer sample fetch failed: %d", ret);
		return;
	}

	ret = sensor_channel_get(accel_dev, SENSOR_CHAN_ACCEL_XYZ, accel);
	if (ret < 0) {
		LOG_WRN("accelerometer channel read failed: %d", ret);
		return;
	}

	int32_t x = sensor_value_to_mm_s2(&accel[0]);
	int32_t y = sensor_value_to_mm_s2(&accel[1]);
	int32_t z = sensor_value_to_mm_s2(&accel[2]);
	uint64_t mag_sq = ((uint64_t)abs64(x) * (uint64_t)abs64(x)) +
			  ((uint64_t)abs64(y) * (uint64_t)abs64(y)) +
			  ((uint64_t)abs64(z) * (uint64_t)abs64(z));
	int32_t magnitude = (int32_t)isqrt64(mag_sq);
	int32_t vector_delta = 0;

	if (have_last_accel) {
		vector_delta = (int32_t)(abs64((int64_t)x - last_accel_x_mm_s2) +
					 abs64((int64_t)y - last_accel_y_mm_s2) +
					 abs64((int64_t)z - last_accel_z_mm_s2));
	}

	last_accel_x_mm_s2 = x;
	last_accel_y_mm_s2 = y;
	last_accel_z_mm_s2 = z;
	have_last_accel = true;

	sample->valid = true;
	sample->magnitude_mm_s2 = magnitude;
	sample->vector_delta_mm_s2 = vector_delta;
	sample->moving =
		abs64((int64_t)magnitude - ACCEL_GRAVITY_MM_S2) > ACCEL_GRAVITY_DELTA_MM_S2 ||
		vector_delta > ACCEL_VECTOR_DELTA_MM_S2;
}

static uint32_t filtered_gnss_speed_mm_s(const struct accel_motion_sample *accel, int64_t now)
{
	uint32_t speed = gnss_data.nav_data.speed;
	int32_t altitude = gnss_data.nav_data.altitude;
	int32_t altitude_delta = 0;
	bool was_suppressed = gnss_drift_suppressed;

	if (have_last_gnss_altitude) {
		altitude_delta = (int32_t)abs64((int64_t)altitude - last_gnss_altitude_mm);
	}

	if (accel->valid && !accel->moving && have_last_gnss_altitude &&
	    speed >= GNSS_ACTIVE_SPEED_MM_S &&
	    altitude_delta > GNSS_STILL_ALTITUDE_JUMP_MM) {
		gnss_drift_suppressed_until = now + GNSS_DRIFT_SUPPRESS_MS;
	}

	have_last_gnss_altitude = true;
	last_gnss_altitude_mm = altitude;
	gnss_drift_suppressed = accel->valid && !accel->moving &&
				now < gnss_drift_suppressed_until;

	if (!was_suppressed && gnss_drift_suppressed) {
		LOG_INF("GNSS drift guard active (speed=%u.%03u m/s, altitude_delta=%d.%03d m, accel=still)",
			speed / 1000, speed % 1000,
			altitude_delta / 1000, abs(altitude_delta % 1000));
	}

	if (gnss_drift_suppressed) {
		return 0;
	}

	return speed;
}

static enum tracker_motion_state update_motion_state(const struct accel_motion_sample *accel,
						     int64_t now)
{
	uint32_t speed = filtered_gnss_speed_mm_s(accel, now);
	bool accel_motion_now = accel->valid && accel->moving;
	bool allow_gnss_motion = !accel_ready || accel_motion_now;
	bool moving_now = allow_gnss_motion && speed >= GNSS_MOVING_SPEED_MM_S;
	bool active_now = accel_motion_now ||
			  (allow_gnss_motion && speed >= GNSS_ACTIVE_SPEED_MM_S);
	enum tracker_motion_state previous_state = motion_state;

	if (active_now) {
		last_motion_timestamp = now;
	}

	if (moving_now) {
		motion_state = TRACKER_MOTION_MOVING;
	} else if (active_now || (now - last_motion_timestamp) < MOTION_HOLD_MS) {
		motion_state = TRACKER_MOTION_ACTIVE;
	} else {
		motion_state = TRACKER_MOTION_STATIONARY;
	}

	if (previous_state == TRACKER_MOTION_STATIONARY &&
	    motion_state != TRACKER_MOTION_STATIONARY) {
		last_motion_resume_timestamp = now;
	}

	if (previous_state != motion_state) {
		LOG_INF("motion state: %s -> %s (speed=%u.%03u m/s, hdop=%u.%03u, accel=%s mag=%d.%03d delta=%d.%03d)",
			motion_state_name(previous_state),
			motion_state_name(motion_state),
			speed / 1000, speed % 1000,
			gnss_data.info.hdop / 1000, gnss_data.info.hdop % 1000,
			accel->valid ? (accel->moving ? "moving" : "still") : "unavailable",
			accel->magnitude_mm_s2 / 1000, abs(accel->magnitude_mm_s2 % 1000),
			accel->vector_delta_mm_s2 / 1000, abs(accel->vector_delta_mm_s2 % 1000));
	}

	return motion_state;
}

static uint32_t uplink_interval_ms_for_motion(enum tracker_motion_state state)
{
	switch (state) {
	case TRACKER_MOTION_MOVING:
		return POSITION_UPLINK_MOVING_INTERVAL_MS;
	case TRACKER_MOTION_ACTIVE:
		return POSITION_UPLINK_ACTIVE_INTERVAL_MS;
	case TRACKER_MOTION_STATIONARY:
		return POSITION_UPLINK_STATIONARY_INTERVAL_MS;
	case TRACKER_MOTION_UNKNOWN:
	default:
		return POSITION_UPLINK_ACTIVE_INTERVAL_MS;
	}
}

static void tracker_motion_init(void)
{
	last_motion_timestamp = k_uptime_get();

	if (accel_dev == NULL) {
		LOG_WRN("accelerometer unavailable: no accel-0 alias");
		return;
	}

	if (!device_is_ready(accel_dev)) {
		LOG_WRN("accelerometer unavailable: %s is not ready", accel_dev->name);
		return;
	}

	accel_ready = true;
	LOG_INF("accelerometer ready: %s", accel_dev->name);
}

void gnss_position_cb(const struct navigation_data *nav_data, uint16_t hdop)
{
	(void)nav_data;
	(void)hdop;

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
	struct accel_motion_sample accel;
	enum tracker_motion_state current_motion;
	uint32_t interval_ms;
	bool resumed_motion;

	read_accel_motion(&accel);
	current_motion = update_motion_state(&accel, now);
	interval_ms = uplink_interval_ms_for_motion(current_motion);
	resumed_motion = last_motion_resume_timestamp > last_uplink_timestamp &&
			 (now - last_uplink_timestamp) >= POSITION_UPLINK_MOTION_RESUME_MIN_MS;

	if (!gnss_position_is_usable(&accel)) {
		LOG_DBG("position skipped: weak or drifting fix (satellites=%u, hdop=%u.%03u, speed=%u.%03u)",
			gnss_data.info.satellites_cnt,
			gnss_data.info.hdop / 1000, gnss_data.info.hdop % 1000,
			gnss_data.nav_data.speed / 1000, gnss_data.nav_data.speed % 1000);
		return 0;
	}

	if (have_sent_position && !resumed_motion &&
	    now - last_uplink_timestamp < interval_ms) {
		return 0;
	}

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
	have_sent_position = true;
	LOG_INF("position uplink: state=%s interval=%u ms speed=%u.%03u m/s hdop=%u.%03u satellites=%u",
		motion_state_name(current_motion),
		interval_ms,
		gnss_data.nav_data.speed / 1000, gnss_data.nav_data.speed % 1000,
		gnss_data.info.hdop / 1000, gnss_data.info.hdop % 1000,
		gnss_data.info.satellites_cnt);
	led_status_on(LED_B);
	k_sleep(K_MSEC(1000));
	led_status_off(LED_B);

	return 0;
}


int main(void)
{
	int ret;

	LOG_INF("DoET :: Tracker %s\n", APP_VERSION_STRING);

	ret = tracker_settings_init();
	if (ret != 0) {
		LOG_ERR("tracker settings init failed: %d", ret);
	}

	if (IS_ENABLED(CONFIG_TRACKER_PROVISIONING_MODE)) {
		LOG_INF("tracker provisioning mode active; runtime services disabled");
		while (1) {
			k_sleep(K_SECONDS(60));
		}
	}

	gnss_init(gnss_position_cb);
	tracker_motion_init();
	led_status_blink_once(LED_G, 100, 100, 3);
	lorawan_node_init();

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

				uint8_t msg[] = {0xde, 0xad, 0xca, 0xfe};
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

		uint32_t ev = k_event_wait(&events, 0xFFF, true, K_MSEC(50));
		if (ev & EV_GNSS_POSITION) {
			handle_event_gnss_position();
		}
	}

	return 0;
}
