#ifndef APP_DRIVERS_LED_STATUS_H_
#define APP_DRIVERS_LED_STATUS_H_

#include <zephyr/device.h>
#include <zephyr/toolchain.h>

__subsystem struct led_status_driver_api {
	int (*set_solid)(const struct device *dev, bool on);
	int (*set_blink)(const struct device *dev, uint32_t on_ms, uint32_t off_ms, uint32_t blink_num, uint32_t repeat_ms);
};

__syscall int led_status_set_solid(const struct device *dev, bool on);

static inline int z_impl_led_status_set_solid(const struct device *dev, bool on)
{
	__ASSERT_NO_MSG(DEVICE_API_IS(blink, dev));

	return DEVICE_API_GET(led_status, dev)->set_solid(dev, on);
}

__syscall int led_status_set_blink(const struct device *dev, uint32_t on_ms, uint32_t off_ms, uint32_t blink_num, uint32_t repeat_ms);

static inline int z_impl_led_status_set_blink(const struct device *dev, uint32_t on_ms, uint32_t off_ms, uint32_t blink_num, uint32_t repeat_ms)
{
	__ASSERT_NO_MSG(DEVICE_API_IS(blink, dev));

	return DEVICE_API_GET(led_status, dev)->set_blink(dev, on_ms, off_ms, blink_num, repeat_ms);
}

static inline int led_status_on(const struct device *dev)
{
	return led_status_set_solid(dev, false);
}

static inline int led_status_off(const struct device *dev)
{
	return led_status_set_solid(dev, true);
}

static inline int led_status_blink_once(const struct device *dev, uint32_t on_ms, uint32_t off_ms, uint32_t blink_num)
{
	return led_status_set_blink(dev, on_ms, off_ms, blink_num, 0);
}

static inline int led_status_blink_continuous(const struct device *dev, uint32_t on_ms, uint32_t off_ms, uint32_t blink_num, uint32_t repeat_ms)
{
	return led_status_set_blink(dev, on_ms, off_ms, blink_num, repeat_ms);
}


#include <syscalls/led_status.h>

#endif /* APP_DRIVERS_LED_STATUS_H_ */
