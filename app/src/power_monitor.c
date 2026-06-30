#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <app/power_monitor.h>

LOG_MODULE_REGISTER(power_monitor, CONFIG_APP_LOG_LEVEL);

#define BATTERY_ADC_NODE DT_NODELABEL(adc0)
#define BATTERY_ADC_CHANNEL_NODE DT_CHILD(BATTERY_ADC_NODE, channel_6)
#define BATTERY_DIVIDER_HIGH_KOHM 100
#define BATTERY_DIVIDER_LOW_KOHM 470
#define BATTERY_DIVIDER_NUMERATOR \
	(BATTERY_DIVIDER_HIGH_KOHM + BATTERY_DIVIDER_LOW_KOHM)
#define BATTERY_DIVIDER_DENOMINATOR BATTERY_DIVIDER_LOW_KOHM

#if DT_NODE_HAS_STATUS(BATTERY_ADC_NODE, okay) && DT_NODE_EXISTS(BATTERY_ADC_CHANNEL_NODE)
static const struct adc_dt_spec battery_adc = {
	.dev = DEVICE_DT_GET(BATTERY_ADC_NODE),
	.channel_id = DT_REG_ADDR(BATTERY_ADC_CHANNEL_NODE),
	.channel_cfg_dt_node_exists = true,
	.channel_cfg = ADC_CHANNEL_CFG_DT(BATTERY_ADC_CHANNEL_NODE),
	.vref_mv = DT_PROP_OR(BATTERY_ADC_CHANNEL_NODE, zephyr_vref_mv, 0),
	.resolution = DT_PROP_OR(BATTERY_ADC_CHANNEL_NODE, zephyr_resolution, 0),
	.oversampling = DT_PROP_OR(BATTERY_ADC_CHANNEL_NODE, zephyr_oversampling, 0),
};
static bool battery_ready;

static int setup_battery_adc(void)
{
	int ret;

	if (!adc_is_ready_dt(&battery_adc)) {
		LOG_WRN("battery ADC unavailable: %s is not ready", battery_adc.dev->name);
		return -ENODEV;
	}

	ret = adc_channel_setup_dt(&battery_adc);
	if (ret != 0) {
		LOG_WRN("battery ADC channel setup failed: %d", ret);
		return ret;
	}

	battery_ready = true;
	LOG_INF("battery voltage monitor ready: %s channel %u (IO34)",
		battery_adc.dev->name, battery_adc.channel_id);

	return 0;
}

#endif

int tracker_power_monitor_init(void)
{
#if DT_NODE_HAS_STATUS(BATTERY_ADC_NODE, okay) && DT_NODE_EXISTS(BATTERY_ADC_CHANNEL_NODE)
	return setup_battery_adc();
#else
	LOG_WRN("battery voltage monitor unavailable: missing ADC channel node");
	return -ENODEV;
#endif
}

int tracker_power_monitor_read_battery_mv(uint16_t *battery_mv)
{
#if DT_NODE_HAS_STATUS(BATTERY_ADC_NODE, okay) && DT_NODE_EXISTS(BATTERY_ADC_CHANNEL_NODE)
	int16_t raw_sample = 0;
	int32_t pin_mv;
	int64_t scaled_mv;
	struct adc_sequence sequence = {
		.buffer = &raw_sample,
		.buffer_size = sizeof(raw_sample),
	};
	int ret;

	if (battery_mv == NULL) {
		return -EINVAL;
	}

	*battery_mv = TRACKER_BATTERY_MV_INVALID;

	if (!battery_ready) {
		return -ENODEV;
	}

	ret = adc_sequence_init_dt(&battery_adc, &sequence);
	if (ret != 0) {
		return ret;
	}

	ret = adc_read_dt(&battery_adc, &sequence);
	if (ret != 0) {
		return ret;
	}

	pin_mv = raw_sample;
	ret = adc_raw_to_millivolts_dt(&battery_adc, &pin_mv);
	if (ret != 0) {
		return ret;
	}

	scaled_mv = ((int64_t)pin_mv * BATTERY_DIVIDER_NUMERATOR +
		     (BATTERY_DIVIDER_DENOMINATOR / 2)) /
		    BATTERY_DIVIDER_DENOMINATOR;
	if (scaled_mv < 0 || scaled_mv > UINT16_MAX) {
		return -ERANGE;
	}

	*battery_mv = (uint16_t)scaled_mv;

	return 0;
#else
	ARG_UNUSED(battery_mv);

	return -ENODEV;
#endif
}
