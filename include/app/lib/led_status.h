#ifndef APP_LIB_LED_STATUS_H_
#define APP_LIB_LED_STATUS_H_

#include <app/drivers/led_status.h>

#define LED_R DEVICE_DT_GET(DT_ALIAS(led_r))
#define LED_G DEVICE_DT_GET(DT_ALIAS(led_g))
#define LED_B DEVICE_DT_GET(DT_ALIAS(led_b))

#endif
