#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <esp_clk_tree.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_err.h>
#include <esp_private/sar_periph_ctrl.h>
#include <hal/adc_hal_common.h>
#include <hal/adc_oneshot_hal.h>
#include <soc/soc_caps.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <app/power_monitor.h>

LOG_MODULE_REGISTER(power_monitor, CONFIG_APP_LOG_LEVEL);

#define BATTERY_ADC_UNIT ADC_UNIT_1
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_6
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12
#define BATTERY_ADC_BITWIDTH ADC_BITWIDTH_12
#define BATTERY_ADC_DEFAULT_VREF_MV 1100
#define BATTERY_DIVIDER_HIGH_KOHM 100
#define BATTERY_DIVIDER_LOW_KOHM 470
#define BATTERY_DIVIDER_NUMERATOR \
	(BATTERY_DIVIDER_HIGH_KOHM + BATTERY_DIVIDER_LOW_KOHM)
#define BATTERY_DIVIDER_DENOMINATOR BATTERY_DIVIDER_LOW_KOHM

static adc_oneshot_hal_ctx_t battery_adc_hal;
static adc_cali_handle_t battery_adc_cali;
static bool battery_ready;
static bool battery_calibrated;

static int esp_err_to_errno(esp_err_t err)
{
	switch (err) {
	case ESP_OK:
		return 0;
	case ESP_ERR_INVALID_ARG:
	case ESP_ERR_INVALID_STATE:
		return -EINVAL;
	case ESP_ERR_NO_MEM:
		return -ENOMEM;
	case ESP_ERR_NOT_FOUND:
		return -ENODEV;
	case ESP_ERR_TIMEOUT:
		return -ETIMEDOUT;
	case ESP_ERR_NOT_SUPPORTED:
		return -ENOTSUP;
	default:
		return -EIO;
	}
}

static int setup_battery_adc(void)
{
	uint32_t clock_src_hz = 0;
	adc_oneshot_hal_chan_cfg_t channel_cfg = {
		.atten = BATTERY_ADC_ATTEN,
		.bitwidth = BATTERY_ADC_BITWIDTH,
	};
	adc_cali_line_fitting_config_t cali_cfg = {
		.unit_id = BATTERY_ADC_UNIT,
		.atten = BATTERY_ADC_ATTEN,
		.bitwidth = BATTERY_ADC_BITWIDTH,
#if CONFIG_IDF_TARGET_ESP32
		.default_vref = BATTERY_ADC_DEFAULT_VREF_MV,
#endif
	};
	esp_err_t err;

	err = esp_clk_tree_src_get_freq_hz(ADC_DIGI_CLK_SRC_DEFAULT,
					   ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
					   &clock_src_hz);
	if (err != ESP_OK) {
		LOG_WRN("battery ADC clock setup failed: 0x%x", err);
		return esp_err_to_errno(err);
	}

	adc_oneshot_hal_cfg_t hal_cfg = {
		.unit = BATTERY_ADC_UNIT,
		.work_mode = ADC_HAL_SINGLE_READ_MODE,
		.clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
		.clk_src_freq_hz = clock_src_hz,
	};

	adc_oneshot_hal_init(&battery_adc_hal, &hal_cfg);
	adc_oneshot_hal_channel_config(&battery_adc_hal, &channel_cfg,
				       BATTERY_ADC_CHANNEL);
	sar_periph_ctrl_adc_oneshot_power_acquire();

	err = adc_cali_create_scheme_line_fitting(&cali_cfg, &battery_adc_cali);
	if (err == ESP_OK) {
		battery_calibrated = true;
	} else {
		LOG_WRN("battery ADC calibration unavailable: 0x%x", err);
	}

	battery_ready = true;
	LOG_INF("battery voltage monitor ready: ESP HAL ADC1 channel 6 (IO34), "
		"12 dB attenuation, calibration %s, clock %u Hz",
		battery_calibrated ? "enabled" : "unavailable", clock_src_hz);
	return 0;
}

int tracker_power_monitor_init(void)
{
	return setup_battery_adc();
}

int tracker_power_monitor_read_battery_sample(struct tracker_battery_sample *sample)
{
	int raw_sample;
	int32_t pin_mv;
	int64_t scaled_mv;
	esp_err_t err;

	if (sample == NULL) {
		return -EINVAL;
	}

	memset(sample, 0, sizeof(*sample));
	sample->battery_mv = TRACKER_BATTERY_MV_INVALID;
	sample->pin_mv = TRACKER_BATTERY_MV_INVALID;

	if (!battery_ready) {
		return -ENODEV;
	}

	adc_oneshot_hal_setup(&battery_adc_hal, BATTERY_ADC_CHANNEL);
#if SOC_ADC_CALIBRATION_V1_SUPPORTED
	adc_set_hw_calibration_code(BATTERY_ADC_UNIT, BATTERY_ADC_ATTEN);
#endif
	if (!adc_oneshot_hal_convert(&battery_adc_hal, &raw_sample)) {
		return -EIO;
	}
	if (raw_sample < 0 || raw_sample > UINT16_MAX) {
		return -ERANGE;
	}
	sample->raw = (uint16_t)raw_sample;
	sample->raw_valid = true;

	if (!battery_calibrated) {
		return 0;
	}

	err = adc_cali_raw_to_voltage(battery_adc_cali, raw_sample, &pin_mv);
	if (err != ESP_OK) {
		return esp_err_to_errno(err);
	}

	if (pin_mv < 0 || pin_mv > UINT16_MAX) {
		return -ERANGE;
	}

	sample->pin_mv = (uint16_t)pin_mv;
	sample->pin_mv_valid = true;

	scaled_mv = ((int64_t)pin_mv * BATTERY_DIVIDER_NUMERATOR +
		     (BATTERY_DIVIDER_DENOMINATOR / 2)) /
		    BATTERY_DIVIDER_DENOMINATOR;
	if (scaled_mv < 0 || scaled_mv > UINT16_MAX) {
		return -ERANGE;
	}

	sample->battery_mv = (uint16_t)scaled_mv;
	sample->battery_mv_valid = true;
	return 0;
}

int tracker_power_monitor_read_battery_mv(uint16_t *battery_mv)
{
	struct tracker_battery_sample sample;
	int ret;

	if (battery_mv == NULL) {
		return -EINVAL;
	}

	*battery_mv = TRACKER_BATTERY_MV_INVALID;

	ret = tracker_power_monitor_read_battery_sample(&sample);
	if (ret != 0) {
		return ret;
	}
	if (!sample.battery_mv_valid) {
		return sample.saturated ? -ERANGE : -ENODATA;
	}

	*battery_mv = sample.battery_mv;
	return 0;
}
