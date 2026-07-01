#ifndef APP_POWER_MONITOR_H_
#define APP_POWER_MONITOR_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRACKER_BATTERY_MV_INVALID UINT16_MAX

struct tracker_battery_sample {
	bool raw_valid;
	bool pin_mv_valid;
	bool battery_mv_valid;
	bool saturated;
	uint16_t raw;
	uint16_t pin_mv;
	uint16_t battery_mv;
};

int tracker_power_monitor_init(void);
int tracker_power_monitor_read_battery_sample(struct tracker_battery_sample *sample);
int tracker_power_monitor_read_battery_mv(uint16_t *battery_mv);

#ifdef __cplusplus
}
#endif

#endif
