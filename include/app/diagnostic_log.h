#ifndef APP_DIAGNOSTIC_LOG_H_
#define APP_DIAGNOSTIC_LOG_H_

#include <stdint.h>

#include <zephyr/drivers/gnss.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAGNOSTIC_LOG_MOTION_UNKNOWN 0U
#define DIAGNOSTIC_LOG_MOTION_STATIONARY 1U
#define DIAGNOSTIC_LOG_MOTION_ACTIVE 2U
#define DIAGNOSTIC_LOG_MOTION_MOVING 3U

#define DIAGNOSTIC_LOG_UPLINK_SENT 1U
#define DIAGNOSTIC_LOG_UPLINK_SEND_FAILED 2U
#define DIAGNOSTIC_LOG_UPLINK_NOT_JOINED 3U

#define DIAGNOSTIC_LOG_FLAG_ACCEL_VALID (1U << 0)
#define DIAGNOSTIC_LOG_FLAG_ACCEL_MOVING (1U << 1)
#define DIAGNOSTIC_LOG_FLAG_DRIFT_SUPPRESSED (1U << 2)
#define DIAGNOSTIC_LOG_FLAG_MOTION_RESUME (1U << 3)
#define DIAGNOSTIC_LOG_FLAG_UTC_VALID (1U << 4)

#define DIAGNOSTIC_LOG_LINK_FLAG_ADR_ENABLED (1U << 0)
#define DIAGNOSTIC_LOG_LINK_FLAG_DR_VALID (1U << 1)
#define DIAGNOSTIC_LOG_LINK_FLAG_DOWNLINK_VALID (1U << 2)
#define DIAGNOSTIC_LOG_LINK_FLAG_CONFIRMED (1U << 3)

struct diagnostic_log_uplink_entry {
	uint32_t uptime_ms;
	uint32_t utc_packed;
	int32_t latitude;
	int32_t longitude;
	uint16_t speed_cm_s;
	uint16_t hdop;
	uint16_t interval_s;
	int16_t send_ret;
	uint8_t motion_state;
	uint8_t result;
	uint8_t satellites;
	uint8_t flags;
	uint8_t lorawan_dr;
	uint8_t link_flags;
	int16_t downlink_rssi;
	int8_t downlink_snr;
};

int diagnostic_log_init(void);
int diagnostic_log_write_uplink(const struct diagnostic_log_uplink_entry *entry);
uint32_t diagnostic_log_pack_utc(const struct gnss_time *utc);

#ifdef __cplusplus
}
#endif

#endif
