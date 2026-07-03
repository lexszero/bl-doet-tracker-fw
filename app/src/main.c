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
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <app_version.h>

#include <app/drivers/led_status.h>
#include <app/diagnostic_log.h>
#include <app/power_monitor.h>
#include <app/lib/gnss.h>
#include <app/lib/led_status.h>
#include <app/lib/lorawan_node.h>

#include <app/settings.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define LEDS_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

#define POSITION_UPLINK_MOVING_INTERVAL_MS 10000
#define POSITION_UPLINK_STATIONARY_INTERVAL_MS 120000
#define POSITION_UPLINK_MOTION_RESUME_MIN_MS 3000
#define POSITION_UPLINK_CONFIRMED_INTERVAL_MS 600000
#define POSITION_UPLINK_FAILURES_BEFORE_REJOIN 3
#define POSITION_UPLINK_PORT 4
#define POSITION_STALE_HDOP UINT16_MAX
#define LORAWAN_RECOVERY_LOG_INTERVAL_MS 900000

#define LORAWAN_JOIN_RETRY_MIN_MS 15000
#define LORAWAN_JOIN_RETRY_MAX_MS 300000
#define LORAWAN_JOIN_RETRY_MOTION_MAX_MS 30000
#define LORAWAN_JOIN_RETRY_POLL_MS 1000
#define LORAWAN_LINK_THREAD_STACK_SIZE 4096
#define LORAWAN_LINK_THREAD_PRIORITY 7

#define GNSS_GOOD_HDOP_MAX 2500
#define GNSS_GOOD_SATELLITES_MIN 4
#define GNSS_DRIFT_SPEED_MM_S 500
#define GNSS_STILL_ALTITUDE_JUMP_MM 5000
#define GNSS_DRIFT_SUPPRESS_MS 60000

#define ACCEL_GRAVITY_MM_S2 9807
#define ACCEL_GRAVITY_DELTA_MM_S2 2000
#define ACCEL_VECTOR_DELTA_MM_S2 1500
#define MOTION_HOLD_MS 30000

static atomic_t lorawan_joined;
static atomic_t lorawan_link_state;
static atomic_t consecutive_position_send_failures;
static atomic_t lorawan_reinit_requested;
static int64_t last_uplink_attempt_timestamp;
static int64_t last_confirmed_uplink_timestamp;
static bool have_attempted_position;
static bool lorawan_recovery_armed;
static int64_t lorawan_link_down_timestamp;
static int64_t lorawan_good_gnss_unjoined_timestamp;
static int64_t lorawan_last_recovery_log_timestamp;
static int64_t last_diagnostic_position_timestamp;
static bool have_logged_diagnostic_position;

#define EV_GNSS_POSITION 1
K_EVENT_DEFINE(events);
K_THREAD_STACK_DEFINE(lorawan_link_thread_stack, LORAWAN_LINK_THREAD_STACK_SIZE);

static struct k_thread lorawan_link_thread_data;
static k_tid_t lorawan_link_thread_id;

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

enum tracker_lorawan_link_state {
	TRACKER_LORAWAN_LINK_BOOTING,
	TRACKER_LORAWAN_LINK_INIT,
	TRACKER_LORAWAN_LINK_JOINING,
	TRACKER_LORAWAN_LINK_JOINED,
	TRACKER_LORAWAN_LINK_REJOINING,
	TRACKER_LORAWAN_LINK_OFFLINE_BACKOFF,
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

static struct msg_up_position last_known_position;
static bool have_last_known_position;

static bool lorawan_is_joined(void)
{
	return atomic_get(&lorawan_joined) != 0;
}

static void lorawan_set_joined(bool joined)
{
	atomic_set(&lorawan_joined, joined ? 1 : 0);
}

static enum tracker_lorawan_link_state lorawan_get_link_state(void)
{
	return (enum tracker_lorawan_link_state)atomic_get(&lorawan_link_state);
}

static void lorawan_set_link_state(enum tracker_lorawan_link_state state)
{
	atomic_set(&lorawan_link_state, state);
}

static const char *lorawan_link_state_name(enum tracker_lorawan_link_state state)
{
	switch (state) {
	case TRACKER_LORAWAN_LINK_BOOTING:
		return "booting";
	case TRACKER_LORAWAN_LINK_INIT:
		return "init";
	case TRACKER_LORAWAN_LINK_JOINING:
		return "joining";
	case TRACKER_LORAWAN_LINK_JOINED:
		return "joined";
	case TRACKER_LORAWAN_LINK_REJOINING:
		return "rejoining";
	case TRACKER_LORAWAN_LINK_OFFLINE_BACKOFF:
		return "offline_backoff";
	default:
		return "unknown";
	}
}

static void log_lorawan_event(int64_t now, uint8_t result, int ret,
			      uint32_t delay_ms,
			      enum lorawan_message_type message_type);

static void lorawan_note_position_send_success(void)
{
	atomic_set(&consecutive_position_send_failures, 0);
	lorawan_good_gnss_unjoined_timestamp = 0;
}

static void lorawan_note_position_send_failure(int ret,
					       enum lorawan_message_type message_type)
{
	atomic_val_t failures;

	if (message_type == LORAWAN_MSG_CONFIRMED && ret == -ETIMEDOUT) {
		LOG_WRN("confirmed uplink timed out; keeping current LoRaWAN session");
		return;
	}

	if (ret == -ECONNREFUSED) {
		LOG_WRN("LoRaWAN send restricted by duty-cycle; keeping current session");
		return;
	}

	failures = atomic_inc(&consecutive_position_send_failures) + 1;
	if (failures < POSITION_UPLINK_FAILURES_BEFORE_REJOIN) {
		return;
	}

	if (lorawan_is_joined()) {
		int64_t now = k_uptime_get();

		LOG_WRN("LoRaWAN link marked down after %d consecutive position send failures (last %d)",
			(int)failures, ret);
		lorawan_set_joined(false);
		lorawan_set_link_state(TRACKER_LORAWAN_LINK_REJOINING);
		lorawan_link_down_timestamp = now;
		lorawan_last_recovery_log_timestamp = now;
		lorawan_recovery_armed = true;
		atomic_set(&lorawan_reinit_requested, 1);
		log_lorawan_event(now, DIAGNOSTIC_LOG_LINK_MARKED_DOWN, ret, 0,
				  message_type);
	}
}

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

static struct msg_up_position current_position_payload(uint16_t hdop)
{
	return (struct msg_up_position){
		.lat = (int32_t)(gnss_data.nav_data.latitude >> 5),
		.lon = (int32_t)(gnss_data.nav_data.longitude >> 5),
		.hdop = hdop,
	};
}

static void remember_last_known_position(void)
{
	last_known_position = current_position_payload(u32_to_u16_saturated(gnss_data.info.hdop));
	have_last_known_position = true;
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
	ARG_UNUSED(accel);

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
	    speed >= GNSS_DRIFT_SPEED_MM_S &&
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

static bool motion_hold_active(int64_t now)
{
	return last_motion_timestamp > 0 &&
	       (now - last_motion_timestamp) < MOTION_HOLD_MS;
}

static enum tracker_motion_state update_motion_state_from_signal(
	const struct accel_motion_sample *accel, int64_t now, uint32_t speed,
	bool motion_now)
{
	enum tracker_motion_state previous_state = motion_state;

	if (motion_now) {
		last_motion_timestamp = now;
	}

	if (motion_now || motion_hold_active(now)) {
		motion_state = TRACKER_MOTION_MOVING;
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

static enum tracker_motion_state update_motion_state(const struct accel_motion_sample *accel,
						     int64_t now)
{
	uint32_t speed = filtered_gnss_speed_mm_s(accel, now);
	bool accel_motion_now = accel->valid && accel->moving;

	return update_motion_state_from_signal(accel, now, speed, accel_motion_now);
}

static uint32_t uplink_interval_ms_for_motion(enum tracker_motion_state state)
{
	switch (state) {
	case TRACKER_MOTION_MOVING:
	case TRACKER_MOTION_ACTIVE:
		return POSITION_UPLINK_MOVING_INTERVAL_MS;
	case TRACKER_MOTION_STATIONARY:
	case TRACKER_MOTION_UNKNOWN:
	default:
		return POSITION_UPLINK_STATIONARY_INTERVAL_MS;
	}
}

static uint32_t lorawan_join_retry_cap_ms(void)
{
	switch (motion_state) {
	case TRACKER_MOTION_MOVING:
	case TRACKER_MOTION_ACTIVE:
		return LORAWAN_JOIN_RETRY_MOTION_MAX_MS;
	case TRACKER_MOTION_STATIONARY:
	case TRACKER_MOTION_UNKNOWN:
	default:
		return LORAWAN_JOIN_RETRY_MAX_MS;
	}
}

static uint32_t lorawan_effective_join_retry_delay_ms(uint32_t retry_delay_ms)
{
	return MIN(retry_delay_ms, lorawan_join_retry_cap_ms());
}

static uint32_t lorawan_sleep_join_retry_delay(uint32_t retry_delay_ms)
{
	uint32_t slept_ms = 0;

	while (slept_ms < retry_delay_ms && !lorawan_is_joined()) {
		uint32_t effective_delay_ms =
			lorawan_effective_join_retry_delay_ms(retry_delay_ms);
		uint32_t sleep_ms;

		if (slept_ms >= effective_delay_ms) {
			break;
		}

		sleep_ms = MIN(effective_delay_ms - slept_ms,
			       (uint32_t)LORAWAN_JOIN_RETRY_POLL_MS);
		k_sleep(K_MSEC(sleep_ms));
		slept_ms += sleep_ms;
	}

	return slept_ms;
}

static void log_position_uplink_decision_for_payload(int64_t now,
					 enum tracker_motion_state state,
					 uint32_t interval_ms,
					 const struct accel_motion_sample *accel,
					 bool resumed_motion, int send_ret,
					 uint8_t result,
					 enum lorawan_message_type message_type,
					 const struct msg_up_position *payload)
{
	struct diagnostic_log_uplink_entry entry = {0};
	struct lorawan_node_status lorawan_status;
	uint32_t utc_packed = diagnostic_log_pack_utc(&gnss_data.utc);
	struct tracker_battery_sample battery = {0};

	lorawan_node_get_status(&lorawan_status);

	entry.uptime_ms = i64_to_u32_saturated(now);
	entry.utc_packed = utc_packed;
	entry.latitude = payload != NULL ? payload->lat :
			 (int32_t)(gnss_data.nav_data.latitude >> 5);
	entry.longitude = payload != NULL ? payload->lon :
			  (int32_t)(gnss_data.nav_data.longitude >> 5);
	entry.speed_cm_s = u32_to_u16_saturated((gnss_data.nav_data.speed + 5U) / 10U);
	entry.hdop = payload != NULL ? payload->hdop :
		     u32_to_u16_saturated(gnss_data.info.hdop);
	entry.interval_s = u32_to_u16_saturated(interval_ms / 1000U);
	entry.send_ret = int_to_i16_saturated(send_ret);
	entry.motion_state = diagnostic_log_motion_state(state);
	entry.result = result;
	entry.satellites = u32_to_u8_saturated(gnss_data.info.satellites_cnt);
	entry.downlink_rssi = lorawan_status.last_downlink_rssi;
	entry.downlink_snr = lorawan_status.last_downlink_snr;
	if (tracker_power_monitor_read_battery_sample(&battery) == 0) {
		if (battery.battery_mv_valid) {
			entry.power_flags |= DIAGNOSTIC_LOG_POWER_FLAG_BATTERY_VALID;
			entry.battery_mv = battery.battery_mv;
		} else {
			entry.battery_mv = DIAGNOSTIC_LOG_BATTERY_MV_INVALID;
		}
		if (battery.pin_mv_valid) {
			entry.power_flags |= DIAGNOSTIC_LOG_POWER_FLAG_BATTERY_PIN_VALID;
			entry.battery_pin_mv = battery.pin_mv;
		}
		if (battery.saturated) {
			entry.power_flags |= DIAGNOSTIC_LOG_POWER_FLAG_BATTERY_SATURATED;
		}
	} else {
		entry.battery_mv = DIAGNOSTIC_LOG_BATTERY_MV_INVALID;
		entry.battery_pin_mv = DIAGNOSTIC_LOG_BATTERY_MV_INVALID;
	}
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

static void log_position_uplink_decision(int64_t now, enum tracker_motion_state state,
					 uint32_t interval_ms,
					 const struct accel_motion_sample *accel,
					 bool resumed_motion, int send_ret,
					 uint8_t result,
					 enum lorawan_message_type message_type)
{
	log_position_uplink_decision_for_payload(now, state, interval_ms, accel,
						 resumed_motion, send_ret, result,
						 message_type, NULL);
}

static enum lorawan_message_type position_message_type(int64_t now)
{
	if (last_confirmed_uplink_timestamp == 0 ||
	    now - last_confirmed_uplink_timestamp >= POSITION_UPLINK_CONFIRMED_INTERVAL_MS) {
		return LORAWAN_MSG_CONFIRMED;
	}

	return LORAWAN_MSG_UNCONFIRMED;
}

static void log_lorawan_event(int64_t now, uint8_t result, int ret,
			      uint32_t delay_ms,
			      enum lorawan_message_type message_type)
{
	struct diagnostic_log_uplink_entry entry = {0};
	struct lorawan_node_status lorawan_status;
	struct tracker_battery_sample battery = {0};
	uint32_t utc_packed = diagnostic_log_pack_utc(&gnss_data.utc);

	lorawan_node_get_status(&lorawan_status);

	entry.uptime_ms = i64_to_u32_saturated(now);
	entry.utc_packed = utc_packed;
	entry.latitude = (int32_t)(gnss_data.nav_data.latitude >> 5);
	entry.longitude = (int32_t)(gnss_data.nav_data.longitude >> 5);
	entry.speed_cm_s = u32_to_u16_saturated((gnss_data.nav_data.speed + 5U) / 10U);
	entry.hdop = u32_to_u16_saturated(gnss_data.info.hdop);
	entry.interval_s = u32_to_u16_saturated(delay_ms / 1000U);
	entry.send_ret = int_to_i16_saturated(ret);
	entry.motion_state = DIAGNOSTIC_LOG_MOTION_UNKNOWN;
	entry.result = result;
	entry.satellites = u32_to_u8_saturated(gnss_data.info.satellites_cnt);
	entry.downlink_rssi = lorawan_status.last_downlink_rssi;
	entry.downlink_snr = lorawan_status.last_downlink_snr;

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

	if (tracker_power_monitor_read_battery_sample(&battery) == 0) {
		if (battery.battery_mv_valid) {
			entry.power_flags |= DIAGNOSTIC_LOG_POWER_FLAG_BATTERY_VALID;
			entry.battery_mv = battery.battery_mv;
		} else {
			entry.battery_mv = DIAGNOSTIC_LOG_BATTERY_MV_INVALID;
		}
		if (battery.pin_mv_valid) {
			entry.power_flags |= DIAGNOSTIC_LOG_POWER_FLAG_BATTERY_PIN_VALID;
			entry.battery_pin_mv = battery.pin_mv;
		}
		if (battery.saturated) {
			entry.power_flags |= DIAGNOSTIC_LOG_POWER_FLAG_BATTERY_SATURATED;
		}
	} else {
		entry.battery_mv = DIAGNOSTIC_LOG_BATTERY_MV_INVALID;
		entry.battery_pin_mv = DIAGNOSTIC_LOG_BATTERY_MV_INVALID;
	}

	(void)diagnostic_log_write_uplink(&entry);
}

static void tracker_motion_init(void)
{
	last_motion_timestamp = 0;
	last_motion_resume_timestamp = 0;

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

static void handle_event_gnss_position(void)
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
		return;
	}

	remember_last_known_position();

	if (!lorawan_is_joined()) {
		if (lorawan_good_gnss_unjoined_timestamp == 0) {
			lorawan_good_gnss_unjoined_timestamp = now;
			lorawan_last_recovery_log_timestamp = now;
			log_lorawan_event(now, DIAGNOSTIC_LOG_LINK_RECOVERY_WAIT,
					  -ENOTCONN, 0, LORAWAN_MSG_UNCONFIRMED);
		}

		if (have_logged_diagnostic_position &&
		    now - last_diagnostic_position_timestamp < interval_ms) {
			return;
		}

		log_position_uplink_decision(now, current_motion, interval_ms, &accel,
					     false, -ENOTCONN,
					     DIAGNOSTIC_LOG_UPLINK_NOT_JOINED,
					     LORAWAN_MSG_UNCONFIRMED);
		return;
	}

	if (have_attempted_position && !resumed_motion &&
	    now - last_uplink_attempt_timestamp < interval_ms) {
		return;
	}

	/*
	uint8_t buf[8+8+2];
	pack_angle(gnss_data.nav_data.latitude, buf+0);
	pack_angle(gnss_data.nav_data.longitude, buf+8);
	*/
	struct msg_up_position msg =
		current_position_payload(u32_to_u16_saturated(gnss_data.info.hdop));
	enum lorawan_message_type message_type = position_message_type(now);

	last_uplink_attempt_timestamp = now;
	have_attempted_position = true;
	if (message_type == LORAWAN_MSG_CONFIRMED) {
		last_confirmed_uplink_timestamp = now;
	}

	int ret = lorawan_send(POSITION_UPLINK_PORT, (uint8_t *)&msg, sizeof(msg),
			       message_type);
	if (ret < 0) {
		LOG_ERR("position uplink failed: %d type=%s state=%s interval=%u ms",
			ret, message_type == LORAWAN_MSG_CONFIRMED ? "confirmed" : "unconfirmed",
			motion_state_name(current_motion), interval_ms);
		log_position_uplink_decision(now, current_motion, interval_ms, &accel,
					     resumed_motion, ret,
					     DIAGNOSTIC_LOG_UPLINK_SEND_FAILED,
					     message_type);
		lorawan_note_position_send_failure(ret, message_type);
		return;
	}

	lorawan_note_position_send_success();

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
}

static bool diagnostic_heartbeat_due(int64_t now, uint32_t interval_ms)
{
	if (!have_logged_diagnostic_position) {
		return now >= interval_ms;
	}

	return now - last_diagnostic_position_timestamp >= interval_ms;
}

static bool send_stale_position_heartbeat(int64_t now,
					  enum tracker_motion_state current_motion,
					  uint32_t interval_ms,
					  const struct accel_motion_sample *accel)
{
	struct msg_up_position msg;
	enum lorawan_message_type message_type;
	int ret;

	if (!lorawan_is_joined() || !have_last_known_position) {
		return false;
	}

	msg = last_known_position;
	msg.hdop = POSITION_STALE_HDOP;
	message_type = position_message_type(now);
	if (message_type == LORAWAN_MSG_CONFIRMED) {
		last_confirmed_uplink_timestamp = now;
	}

	ret = lorawan_send(POSITION_UPLINK_PORT, (uint8_t *)&msg, sizeof(msg),
			   message_type);
	if (ret < 0) {
		LOG_WRN("stale position heartbeat failed: %d type=%s",
			ret, message_type == LORAWAN_MSG_CONFIRMED ?
			"confirmed" : "unconfirmed");
		log_position_uplink_decision_for_payload(now, current_motion,
				 interval_ms, accel, false, ret,
				 DIAGNOSTIC_LOG_UPLINK_NO_FIX, message_type, &msg);
		lorawan_note_position_send_failure(ret, message_type);
		return true;
	}

	lorawan_note_position_send_success();

	log_position_uplink_decision_for_payload(now, current_motion,
				 interval_ms, accel, false, ret,
				 DIAGNOSTIC_LOG_UPLINK_NO_FIX, message_type, &msg);

	LOG_INF("stale position heartbeat: type=%s lat=%d lon=%d hdop=0x%04x",
		message_type == LORAWAN_MSG_CONFIRMED ? "confirmed" : "unconfirmed",
		msg.lat, msg.lon, msg.hdop);

	return true;
}

static void handle_diagnostic_heartbeat(void)
{
	int64_t now = k_uptime_get();
	struct accel_motion_sample accel;
	enum tracker_motion_state current_motion = TRACKER_MOTION_UNKNOWN;
	uint32_t interval_ms;

	read_accel_motion(&accel);
	current_motion = update_motion_state_from_signal(&accel, now, 0,
							 accel.valid && accel.moving);
	interval_ms = uplink_interval_ms_for_motion(current_motion);

	if (gnss_position_is_usable(&accel)) {
		return;
	}

	if (!diagnostic_heartbeat_due(now, interval_ms)) {
		return;
	}

	if (send_stale_position_heartbeat(now, current_motion, interval_ms, &accel)) {
		return;
	}

	log_position_uplink_decision(now, current_motion, interval_ms, &accel,
				     false, -ENODATA,
				     DIAGNOSTIC_LOG_UPLINK_NO_FIX,
				     LORAWAN_MSG_UNCONFIRMED);

	LOG_INF("diagnostic heartbeat: no usable GNSS position (fix=%d quality=%d satellites=%u hdop=%u.%03u)",
		gnss_data.info.fix_status,
		gnss_data.info.fix_quality,
		gnss_data.info.satellites_cnt,
		gnss_data.info.hdop / 1000,
		gnss_data.info.hdop % 1000);
}

static void lorawan_recovery_watchdog(void)
{
	int64_t now = k_uptime_get();
	int64_t good_gnss_unjoined_elapsed;
	int64_t link_down_elapsed;

	if (lorawan_is_joined()) {
		lorawan_good_gnss_unjoined_timestamp = 0;
		return;
	}

	if (lorawan_good_gnss_unjoined_timestamp > 0) {
		good_gnss_unjoined_elapsed = now - lorawan_good_gnss_unjoined_timestamp;

		if (now - lorawan_last_recovery_log_timestamp >=
		    LORAWAN_RECOVERY_LOG_INTERVAL_MS) {
			lorawan_last_recovery_log_timestamp = now;
			log_lorawan_event(now, DIAGNOSTIC_LOG_LINK_RECOVERY_WAIT,
					  -ENOTCONN,
					  i64_to_u32_saturated(good_gnss_unjoined_elapsed),
					  LORAWAN_MSG_UNCONFIRMED);
			LOG_WRN("LoRaWAN still not joined after %lld ms with usable GNSS (state=%s)",
				(long long)good_gnss_unjoined_elapsed,
				lorawan_link_state_name(lorawan_get_link_state()));
		}
	}

	if (!lorawan_recovery_armed || lorawan_link_down_timestamp <= 0) {
		return;
	}

	link_down_elapsed = now - lorawan_link_down_timestamp;
	if (now - lorawan_last_recovery_log_timestamp >=
	    LORAWAN_RECOVERY_LOG_INTERVAL_MS) {
		lorawan_last_recovery_log_timestamp = now;
		log_lorawan_event(now, DIAGNOSTIC_LOG_LINK_RECOVERY_WAIT,
				  -ENOTCONN,
				  i64_to_u32_saturated(link_down_elapsed),
				  LORAWAN_MSG_UNCONFIRMED);
		LOG_WRN("LoRaWAN link unrecovered for %lld ms; continuing rejoin attempts without reboot",
			(long long)link_down_elapsed);
	}
}

static void lorawan_link_thread(void *arg1, void *arg2, void *arg3)
{
	bool initialized = false;
	uint32_t retry_delay_ms = LORAWAN_JOIN_RETRY_MIN_MS;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		int ret;
		uint32_t effective_retry_delay_ms;

		if (!lorawan_is_joined() &&
		    atomic_cas(&lorawan_reinit_requested, 1, 0)) {
			LOG_WRN("LoRaWAN recovery requested; reinitializing MAC before join");
			initialized = false;
			retry_delay_ms = LORAWAN_JOIN_RETRY_MIN_MS;
		}

		if (!initialized) {
			int64_t now = k_uptime_get();

			lorawan_set_link_state(TRACKER_LORAWAN_LINK_INIT);
			log_lorawan_event(now, DIAGNOSTIC_LOG_LINK_INIT_ATTEMPT, 0,
					  retry_delay_ms, LORAWAN_MSG_UNCONFIRMED);
			LOG_INF("LoRaWAN init attempt");
			ret = lorawan_node_init();
			if (ret != 0) {
				lorawan_set_link_state(TRACKER_LORAWAN_LINK_OFFLINE_BACKOFF);
				log_lorawan_event(k_uptime_get(),
						  DIAGNOSTIC_LOG_LINK_INIT_FAILED,
						  ret, retry_delay_ms,
						  LORAWAN_MSG_UNCONFIRMED);
				LOG_WRN("LoRaWAN init failed: %d; retry in %u ms",
					ret, retry_delay_ms);
				k_sleep(K_MSEC(retry_delay_ms));
				retry_delay_ms = MIN(retry_delay_ms * 2U,
						     LORAWAN_JOIN_RETRY_MAX_MS);
				continue;
			}

			initialized = true;
			retry_delay_ms = LORAWAN_JOIN_RETRY_MIN_MS;
			log_lorawan_event(k_uptime_get(), DIAGNOSTIC_LOG_LINK_INIT_OK, 0,
					  0, LORAWAN_MSG_UNCONFIRMED);
			LOG_INF("LoRaWAN initialized");
		}

		if (lorawan_is_joined()) {
			k_sleep(K_SECONDS(1));
			continue;
		}

		lorawan_set_link_state(lorawan_recovery_armed ?
				       TRACKER_LORAWAN_LINK_REJOINING :
				       TRACKER_LORAWAN_LINK_JOINING);
		effective_retry_delay_ms =
			lorawan_effective_join_retry_delay_ms(retry_delay_ms);
		log_lorawan_event(k_uptime_get(), DIAGNOSTIC_LOG_LINK_JOIN_ATTEMPT,
				  0, effective_retry_delay_ms, LORAWAN_MSG_UNCONFIRMED);
		LOG_INF("LoRaWAN join attempt");
		led_status_blink_continuous(LED_B, 100, 100, 3, 0);

		ret = lorawan_node_join();
		if (ret == 0) {
			lorawan_set_joined(true);
			lorawan_set_link_state(TRACKER_LORAWAN_LINK_JOINED);
			atomic_set(&consecutive_position_send_failures, 0);
			last_confirmed_uplink_timestamp = 0;
			lorawan_recovery_armed = false;
			lorawan_link_down_timestamp = 0;
			lorawan_good_gnss_unjoined_timestamp = 0;
			lorawan_last_recovery_log_timestamp = 0;
			retry_delay_ms = LORAWAN_JOIN_RETRY_MIN_MS;
			log_lorawan_event(k_uptime_get(), DIAGNOSTIC_LOG_LINK_JOIN_SUCCESS,
					  0, 0, LORAWAN_MSG_UNCONFIRMED);

			LOG_INF("LoRaWAN joined");
			led_status_on(LED_B);
			k_sleep(K_MSEC(500));
			led_status_off(LED_B);
			led_status_on(LED_G);
			k_sleep(K_MSEC(500));
			led_status_off(LED_G);
			continue;
		}

		lorawan_set_link_state(TRACKER_LORAWAN_LINK_OFFLINE_BACKOFF);
		effective_retry_delay_ms =
			lorawan_effective_join_retry_delay_ms(retry_delay_ms);
		log_lorawan_event(k_uptime_get(), DIAGNOSTIC_LOG_LINK_JOIN_FAILED,
				  ret, effective_retry_delay_ms, LORAWAN_MSG_UNCONFIRMED);
		if (effective_retry_delay_ms < retry_delay_ms) {
			LOG_WRN("LoRaWAN join failed: %d; retry in %u ms while %s (base %u ms)",
				ret, effective_retry_delay_ms,
				motion_state_name(motion_state), retry_delay_ms);
		} else {
			LOG_WRN("LoRaWAN join failed: %d; retry in %u ms",
				ret, retry_delay_ms);
		}
		led_status_on(LED_B);
		k_sleep(K_MSEC(500));
		led_status_off(LED_B);
		led_status_on(LED_R);
		k_sleep(K_MSEC(500));
		led_status_off(LED_R);

		(void)lorawan_sleep_join_retry_delay(retry_delay_ms);
		retry_delay_ms = MIN(retry_delay_ms * 2U, LORAWAN_JOIN_RETRY_MAX_MS);
	}
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

	ret = tracker_power_monitor_init();
	if (ret != 0) {
		LOG_WRN("power monitor disabled: %d", ret);
	} else {
		struct tracker_battery_sample battery = {0};

		ret = tracker_power_monitor_read_battery_sample(&battery);
		if (ret == 0) {
			LOG_INF("battery ADC sample: raw=%u pin=%u mV (%s) battery=%u mV (%s) clipped=%d",
				battery.raw,
				battery.pin_mv,
				battery.pin_mv_valid ? "valid" : "invalid",
				battery.battery_mv,
				battery.battery_mv_valid ? "valid" : "invalid",
				battery.saturated);
		} else {
			LOG_WRN("battery ADC sample failed: %d", ret);
		}
	}

	log_lorawan_event(k_uptime_get(), DIAGNOSTIC_LOG_LINK_BOOT, 0, 0,
			  LORAWAN_MSG_UNCONFIRMED);

	gnss_init(gnss_position_cb);
	tracker_motion_init();
	led_status_blink_once(LED_G, 100, 100, 3);

	lorawan_link_thread_id = k_thread_create(&lorawan_link_thread_data,
						 lorawan_link_thread_stack,
						 K_THREAD_STACK_SIZEOF(lorawan_link_thread_stack),
						 lorawan_link_thread,
						 NULL, NULL, NULL,
						 K_PRIO_PREEMPT(LORAWAN_LINK_THREAD_PRIORITY),
						 0, K_NO_WAIT);
	(void)k_thread_name_set(lorawan_link_thread_id, "lorawan_link");

	while (1) {
		uint32_t ev = k_event_wait(&events, 0xFFF, true, K_SECONDS(1));
		if (ev & EV_GNSS_POSITION) {
			handle_event_gnss_position();
		}

		handle_diagnostic_heartbeat();
		lorawan_recovery_watchdog();
	}

	return 0;
}
