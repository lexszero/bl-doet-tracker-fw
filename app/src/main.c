/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/util.h>

#include <app_version.h>

#include <app/drivers/led_status.h>
#include <app/diagnostic_log.h>
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
#define POSITION_UPLINK_CONFIRMED_INTERVAL_MS 600000

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
static int64_t last_uplink_attempt_timestamp;
static int64_t last_confirmed_uplink_timestamp;
static bool have_attempted_position;
static int64_t last_diagnostic_position_timestamp;
static bool have_logged_diagnostic_position;

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

static uint8_t diagnostic_log_motion_state(enum tracker_motion_state state)
{
	switch (state) {
	case TRACKER_MOTION_STATIONARY:
		return DIAGNOSTIC_LOG_MOTION_STATIONARY;
	case TRACKER_MOTION_ACTIVE:
		return DIAGNOSTIC_LOG_MOTION_ACTIVE;
	case TRACKER_MOTION_MOVING:
		return DIAGNOSTIC_LOG_MOTION_MOVING;
	case TRACKER_MOTION_UNKNOWN:
	default:
		return DIAGNOSTIC_LOG_MOTION_UNKNOWN;
	}
}

static uint16_t u32_to_u16_saturated(uint32_t value)
{
	return value > UINT16_MAX ? UINT16_MAX : (uint16_t)value;
}

static uint8_t u32_to_u8_saturated(uint32_t value)
{
	return value > UINT8_MAX ? UINT8_MAX : (uint8_t)value;
}

static uint32_t i64_to_u32_saturated(int64_t value)
{
	if (value <= 0) {
		return 0U;
	}

	return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static int16_t int_to_i16_saturated(int value)
{
	if (value > INT16_MAX) {
		return INT16_MAX;
	}

	if (value < INT16_MIN) {
		return INT16_MIN;
	}

	return (int16_t)value;
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

static void log_position_uplink_decision(int64_t now, enum tracker_motion_state state,
					 uint32_t interval_ms,
					 const struct accel_motion_sample *accel,
					 bool resumed_motion, int send_ret,
					 uint8_t result,
					 enum lorawan_message_type message_type)
{
	struct diagnostic_log_uplink_entry entry = {0};
	struct lorawan_node_status lorawan_status;
	uint32_t utc_packed = diagnostic_log_pack_utc(&gnss_data.utc);

	lorawan_node_get_status(&lorawan_status);

	entry.uptime_ms = i64_to_u32_saturated(now);
	entry.utc_packed = utc_packed;
	entry.latitude = (int32_t)(gnss_data.nav_data.latitude >> 5);
	entry.longitude = (int32_t)(gnss_data.nav_data.longitude >> 5);
	entry.speed_cm_s = u32_to_u16_saturated((gnss_data.nav_data.speed + 5U) / 10U);
	entry.hdop = u32_to_u16_saturated(gnss_data.info.hdop);
	entry.interval_s = u32_to_u16_saturated(interval_ms / 1000U);
	entry.send_ret = int_to_i16_saturated(send_ret);
	entry.motion_state = diagnostic_log_motion_state(state);
	entry.result = result;
	entry.satellites = u32_to_u8_saturated(gnss_data.info.satellites_cnt);
	entry.downlink_rssi = lorawan_status.last_downlink_rssi;
	entry.downlink_snr = lorawan_status.last_downlink_snr;
	if (accel != NULL && accel->valid) {
		entry.flags |= DIAGNOSTIC_LOG_FLAG_ACCEL_VALID;
		if (accel->moving) {
			entry.flags |= DIAGNOSTIC_LOG_FLAG_ACCEL_MOVING;
		}
	}
	if (gnss_drift_suppressed) {
		entry.flags |= DIAGNOSTIC_LOG_FLAG_DRIFT_SUPPRESSED;
	}
	if (resumed_motion) {
		entry.flags |= DIAGNOSTIC_LOG_FLAG_MOTION_RESUME;
	}
	if (utc_packed != 0U) {
		entry.flags |= DIAGNOSTIC_LOG_FLAG_UTC_VALID;
	}
	if (lorawan_status.adr_enabled) {
		entry.link_flags |= DIAGNOSTIC_LOG_LINK_FLAG_ADR_ENABLED;
	}
	if (lorawan_status.datarate_valid) {
		entry.lorawan_dr = (uint8_t)lorawan_status.datarate;
		entry.link_flags |= DIAGNOSTIC_LOG_LINK_FLAG_DR_VALID;
	}
	if (lorawan_status.last_downlink_valid) {
		entry.link_flags |= DIAGNOSTIC_LOG_LINK_FLAG_DOWNLINK_VALID;
	}
	if (message_type == LORAWAN_MSG_CONFIRMED) {
		entry.link_flags |= DIAGNOSTIC_LOG_LINK_FLAG_CONFIRMED;
	}

	(void)diagnostic_log_write_uplink(&entry);
	last_diagnostic_position_timestamp = now;
	have_logged_diagnostic_position = true;
}

static enum lorawan_message_type position_message_type(int64_t now)
{
	if (last_confirmed_uplink_timestamp == 0 ||
	    now - last_confirmed_uplink_timestamp >= POSITION_UPLINK_CONFIRMED_INTERVAL_MS) {
		return LORAWAN_MSG_CONFIRMED;
	}

	return LORAWAN_MSG_UNCONFIRMED;
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
	int64_t now = k_uptime_get();
	struct accel_motion_sample accel;
	enum tracker_motion_state current_motion;
	uint32_t interval_ms;
	bool resumed_motion;

	read_accel_motion(&accel);
	current_motion = update_motion_state(&accel, now);
	interval_ms = uplink_interval_ms_for_motion(current_motion);
	resumed_motion = last_motion_resume_timestamp > last_uplink_attempt_timestamp &&
			 (now - last_uplink_attempt_timestamp) >= POSITION_UPLINK_MOTION_RESUME_MIN_MS;

	if (!gnss_position_is_usable(&accel)) {
		LOG_DBG("position skipped: weak or drifting fix (satellites=%u, hdop=%u.%03u, speed=%u.%03u)",
			gnss_data.info.satellites_cnt,
			gnss_data.info.hdop / 1000, gnss_data.info.hdop % 1000,
			gnss_data.nav_data.speed / 1000, gnss_data.nav_data.speed % 1000);
		return 0;
	}

	if (!lorawan_joined) {
		if (have_logged_diagnostic_position &&
		    now - last_diagnostic_position_timestamp < interval_ms) {
			return 0;
		}

		log_position_uplink_decision(now, current_motion, interval_ms, &accel,
					     false, -ENOTCONN,
					     DIAGNOSTIC_LOG_UPLINK_NOT_JOINED,
					     LORAWAN_MSG_UNCONFIRMED);
		return 0;
	}

	if (have_attempted_position && !resumed_motion &&
	    now - last_uplink_attempt_timestamp < interval_ms) {
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
	enum lorawan_message_type message_type = position_message_type(now);

	last_uplink_attempt_timestamp = now;
	have_attempted_position = true;
	if (message_type == LORAWAN_MSG_CONFIRMED) {
		last_confirmed_uplink_timestamp = now;
	}

	int ret = lorawan_send(4, (uint8_t *)&msg, sizeof(msg), message_type);
	if (ret < 0) {
		LOG_ERR("position uplink failed: %d type=%s state=%s interval=%u ms",
			ret, message_type == LORAWAN_MSG_CONFIRMED ? "confirmed" : "unconfirmed",
			motion_state_name(current_motion), interval_ms);
		log_position_uplink_decision(now, current_motion, interval_ms, &accel,
					     resumed_motion, ret,
					     DIAGNOSTIC_LOG_UPLINK_SEND_FAILED,
					     message_type);
		return ret;
	}

	log_position_uplink_decision(now, current_motion, interval_ms, &accel,
				     resumed_motion, ret, DIAGNOSTIC_LOG_UPLINK_SENT,
				     message_type);

	LOG_INF("position uplink: state=%s type=%s interval=%u ms speed=%u.%03u m/s hdop=%u.%03u satellites=%u",
		motion_state_name(current_motion),
		message_type == LORAWAN_MSG_CONFIRMED ? "confirmed" : "unconfirmed",
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

	ret = diagnostic_log_init();
	if (ret != 0) {
		LOG_WRN("diagnostic log disabled: %d", ret);
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
