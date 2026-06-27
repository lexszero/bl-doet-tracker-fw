#ifndef APP_LIB_GNSS_H_
#define APP_LIB_GNSS_H_

#include <zephyr/drivers/gnss.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*gnss_position_cb_t)(const struct navigation_data *nav_data, uint16_t hdop);

int gnss_init(gnss_position_cb_t position_cb);

extern struct gnss_data gnss_data;

#ifdef __cplusplus
}
#endif

#endif
