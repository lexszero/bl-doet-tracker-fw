#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "app/settings.h"
#include "app/lib/lorawan_node.h"

#define DELAY K_MSEC(10000)

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lorawan_node);

static void dl_callback(uint8_t port, uint8_t flags, int16_t rssi, int8_t snr, uint8_t len,
			const uint8_t *hex_data);

static struct lorawan_downlink_cb downlink_cb = {
	.port = LW_RECV_PORT_ANY,
	.cb = dl_callback
};

static void dl_callback(uint8_t port, uint8_t flags, int16_t rssi, int8_t snr, uint8_t len,
			const uint8_t *hex_data)
{
	LOG_INF("Port %d, Pending %d, RSSI %ddB, SNR %ddBm, Time %d", port,
		flags & LORAWAN_DATA_PENDING, rssi, snr, !!(flags & LORAWAN_TIME_UPDATED));
	if (hex_data) {
		LOG_HEXDUMP_INF(hex_data, len, "Payload: ");
	}
}

static void lorwan_datarate_changed(enum lorawan_datarate dr)
{
	uint8_t unused, max_size;

	lorawan_get_payload_sizes(&unused, &max_size);
	LOG_INF("New Datarate: DR_%d, Max Payload %d", dr, max_size);
}

int lorawan_node_init(void)
{
	LOG_INF("Initializing");
	const struct device *lora_dev;
	int ret;

	lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));
	if (!device_is_ready(lora_dev)) {
		LOG_ERR("%s: device not ready.", lora_dev->name);
		return -ENODEV;
	}

	LOG_DBG("lorawan_set_region");
	ret = lorawan_set_region(LORAWAN_REGION_EU868);
	if (ret < 0) {
		LOG_ERR("lorawan_set_region failed: %d", ret);
		return ret;
	}

	LOG_DBG("lorawan_set_class");
	ret = lorawan_set_class(LORAWAN_CLASS_C);
	if (ret < 0) {
		LOG_ERR("lorawan_set_class failed: %d", ret);
		return ret;
	}

	LOG_DBG("lorawan_start");
	ret = lorawan_start();
	if (ret < 0) {
		LOG_ERR("lorawan_start failed: %d", ret);
		return ret;
	}

	lorawan_register_downlink_callback(&downlink_cb);
	lorawan_register_dr_changed_callback(lorwan_datarate_changed);

	return 0;
}

int lorawan_node_join(void)
{
	LOG_INF("Initializing");
	struct lorawan_join_config join_cfg;
	struct tracker_lorawan_settings lorawan_settings;
	char dev_eui_hex[(TRACKER_LORAWAN_EUI_LEN * 2) + 1];

	tracker_lorawan_settings_get(&lorawan_settings);
	(void)bin2hex(lorawan_settings.dev_eui, sizeof(lorawan_settings.dev_eui),
		      dev_eui_hex, sizeof(dev_eui_hex));

	join_cfg.mode = LORAWAN_ACT_OTAA;
	join_cfg.dev_eui = lorawan_settings.dev_eui;
	join_cfg.otaa.join_eui = lorawan_settings.join_eui;
	join_cfg.otaa.app_key = lorawan_settings.app_key;
	join_cfg.otaa.nwk_key = lorawan_settings.app_key;
	join_cfg.otaa.dev_nonce = 0u;

	LOG_INF("Joining network over OTAA (%s DevEUI %s)",
		lorawan_settings.provisioned ? "provisioned" : "fallback",
		dev_eui_hex);
	int ret = lorawan_join(&join_cfg);
	if (ret < 0) {
		LOG_ERR("lorawan_join_network failed: %d", ret);
		return ret;
	}

	return 0;
}
