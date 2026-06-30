#ifndef APP_POWER_MONITOR_H_
#define APP_POWER_MONITOR_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRACKER_BATTERY_MV_INVALID UINT16_MAX

int tracker_power_monitor_init(void);
int tracker_power_monitor_read_battery_mv(uint16_t *battery_mv);

#ifdef __cplusplus
}
#endif

#endif
