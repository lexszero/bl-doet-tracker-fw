#include <app/settings.h>

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

LOG_MODULE_REGISTER(tracker_settings, CONFIG_APP_LOG_LEVEL);

#define TRACKER_SETTINGS_SUBTREE "tracker"

#define TRACKER_LORAWAN_DEV_EUI_KEY "lorawan/dev_eui"
#define TRACKER_LORAWAN_JOIN_EUI_KEY "lorawan/join_eui"
#define TRACKER_LORAWAN_APP_KEY_KEY "lorawan/app_key"
#define TRACKER_LORAWAN_IDENTITY_CRC_KEY "lorawan/identity_crc"

#define TRACKER_LORAWAN_DEV_EUI_SETTING \
	TRACKER_SETTINGS_SUBTREE "/" TRACKER_LORAWAN_DEV_EUI_KEY
#define TRACKER_LORAWAN_JOIN_EUI_SETTING \
	TRACKER_SETTINGS_SUBTREE "/" TRACKER_LORAWAN_JOIN_EUI_KEY
#define TRACKER_LORAWAN_APP_KEY_SETTING \
	TRACKER_SETTINGS_SUBTREE "/" TRACKER_LORAWAN_APP_KEY_KEY
#define TRACKER_LORAWAN_IDENTITY_CRC_SETTING \
	TRACKER_SETTINGS_SUBTREE "/" TRACKER_LORAWAN_IDENTITY_CRC_KEY

static const struct tracker_lorawan_settings default_lorawan_settings = {
	.dev_eui = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF},
	.join_eui = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF},
	.app_key = {
		0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
		0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
	},
	.provisioned = false,
};

static struct tracker_lorawan_settings stored_lorawan_settings;
static bool has_dev_eui;
static bool has_join_eui;
static bool has_app_key;
static bool has_applied_identity_crc;
static uint32_t applied_identity_crc;

static const char *const lorawan_nvm_keys[] = {
	"lorawan/nvm/Crypto",
	"lorawan/nvm/MacGroup1",
	"lorawan/nvm/MacGroup2",
	"lorawan/nvm/SecureElement",
	"lorawan/nvm/RegionGroup1",
	"lorawan/nvm/RegionGroup2",
	"lorawan/nvm/ClassB",
};

static void tracker_lorawan_reset_loaded_settings(void)
{
	stored_lorawan_settings = default_lorawan_settings;
	has_dev_eui = false;
	has_join_eui = false;
	has_app_key = false;
	has_applied_identity_crc = false;
	applied_identity_crc = 0;
}

static int read_exact_setting(settings_read_cb read_cb, void *cb_arg, void *dst,
			      size_t expected_len, size_t stored_len,
			      const char *name)
{
	ssize_t bytes_read;

	if (stored_len != expected_len) {
		LOG_WRN("ignoring %s: expected %zu bytes, found %zu", name,
			expected_len, stored_len);
		return -EINVAL;
	}

	bytes_read = read_cb(cb_arg, dst, expected_len);
	if (bytes_read < 0) {
		return (int)bytes_read;
	}

	if (bytes_read != expected_len) {
		LOG_WRN("ignoring %s: short read %zd/%zu", name, bytes_read,
			expected_len);
		return -EINVAL;
	}

	return 0;
}

static int tracker_settings_handle_set(const char *name, size_t len,
				       settings_read_cb read_cb, void *cb_arg)
{
	int ret;

	if (strcmp(name, TRACKER_LORAWAN_DEV_EUI_KEY) == 0) {
		ret = read_exact_setting(read_cb, cb_arg,
					 stored_lorawan_settings.dev_eui,
					 sizeof(stored_lorawan_settings.dev_eui),
					 len, name);
		if (ret == 0) {
			has_dev_eui = true;
		}

		return 0;
	}

	if (strcmp(name, TRACKER_LORAWAN_JOIN_EUI_KEY) == 0) {
		ret = read_exact_setting(read_cb, cb_arg,
					 stored_lorawan_settings.join_eui,
					 sizeof(stored_lorawan_settings.join_eui),
					 len, name);
		if (ret == 0) {
			has_join_eui = true;
		}

		return 0;
	}

	if (strcmp(name, TRACKER_LORAWAN_APP_KEY_KEY) == 0) {
		ret = read_exact_setting(read_cb, cb_arg,
					 stored_lorawan_settings.app_key,
					 sizeof(stored_lorawan_settings.app_key),
					 len, name);
		if (ret == 0) {
			has_app_key = true;
		}

		return 0;
	}

	if (strcmp(name, TRACKER_LORAWAN_IDENTITY_CRC_KEY) == 0) {
		ret = read_exact_setting(read_cb, cb_arg, &applied_identity_crc,
					 sizeof(applied_identity_crc), len, name);
		if (ret == 0) {
			has_applied_identity_crc = true;
		}

		return 0;
	}

	LOG_WRN("unknown tracker setting: %s", name);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(tracker, TRACKER_SETTINGS_SUBTREE, NULL,
			       tracker_settings_handle_set, NULL, NULL);

bool tracker_lorawan_settings_is_provisioned(void)
{
	return has_dev_eui && has_join_eui && has_app_key;
}

void tracker_lorawan_settings_get(struct tracker_lorawan_settings *settings)
{
	if (settings == NULL) {
		return;
	}

	if (tracker_lorawan_settings_is_provisioned()) {
		*settings = stored_lorawan_settings;
		settings->provisioned = true;
		return;
	}

	*settings = default_lorawan_settings;
}

static uint32_t fnv1a32_update(uint32_t hash, const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		hash ^= data[i];
		hash *= 16777619U;
	}

	return hash;
}

static uint32_t tracker_lorawan_identity_crc(
	const struct tracker_lorawan_settings *settings)
{
	uint32_t hash = 2166136261U;

	hash = fnv1a32_update(hash, settings->dev_eui, sizeof(settings->dev_eui));
	hash = fnv1a32_update(hash, settings->join_eui, sizeof(settings->join_eui));
	hash = fnv1a32_update(hash, settings->app_key, sizeof(settings->app_key));

	return hash;
}

static int remember_first_error(int current, int next)
{
	return current == 0 ? next : current;
}

static int tracker_lorawan_clear_nvm(void)
{
	int ret = 0;

	for (size_t i = 0; i < ARRAY_SIZE(lorawan_nvm_keys); i++) {
		int key_ret = settings_delete(lorawan_nvm_keys[i]);

		if (key_ret != 0) {
			LOG_WRN("failed to delete %s: %d", lorawan_nvm_keys[i],
				key_ret);
			ret = remember_first_error(ret, key_ret);
		}
	}

	return ret;
}

static int tracker_lorawan_prepare_identity(void)
{
	uint32_t identity_crc;
	int ret;
	int save_ret;

	if (!tracker_lorawan_settings_is_provisioned()) {
		LOG_WRN("LoRaWAN identity is not provisioned; using development fallback");
		return 0;
	}

	identity_crc = tracker_lorawan_identity_crc(&stored_lorawan_settings);
	if (has_applied_identity_crc && applied_identity_crc == identity_crc) {
		LOG_INF("LoRaWAN identity loaded from tracker settings");
		return 0;
	}

	LOG_INF("LoRaWAN identity changed; clearing stored LoRaWAN session state");
	ret = tracker_lorawan_clear_nvm();

	save_ret = settings_save_one(TRACKER_LORAWAN_IDENTITY_CRC_SETTING,
				     &identity_crc, sizeof(identity_crc));
	if (save_ret != 0) {
		LOG_WRN("failed to save LoRaWAN identity marker: %d", save_ret);
		ret = remember_first_error(ret, save_ret);
	} else {
		applied_identity_crc = identity_crc;
		has_applied_identity_crc = true;
	}

	return ret;
}

int tracker_settings_init(void)
{
	int ret;

	tracker_lorawan_reset_loaded_settings();

	ret = settings_subsys_init();
	if (ret != 0) {
		LOG_ERR("settings subsystem init failed: %d", ret);
		return ret;
	}

	ret = settings_load_subtree(TRACKER_SETTINGS_SUBTREE);
	if (ret != 0) {
		LOG_ERR("tracker settings load failed: %d", ret);
		return ret;
	}

	return tracker_lorawan_prepare_identity();
}

int tracker_lorawan_settings_save(const struct tracker_lorawan_settings *settings)
{
	int ret;

	if (settings == NULL) {
		return -EINVAL;
	}

	ret = settings_save_one(TRACKER_LORAWAN_DEV_EUI_SETTING,
				settings->dev_eui, sizeof(settings->dev_eui));
	if (ret != 0) {
		return ret;
	}

	ret = settings_save_one(TRACKER_LORAWAN_JOIN_EUI_SETTING,
				settings->join_eui, sizeof(settings->join_eui));
	if (ret != 0) {
		return ret;
	}

	ret = settings_save_one(TRACKER_LORAWAN_APP_KEY_SETTING,
				settings->app_key, sizeof(settings->app_key));
	if (ret != 0) {
		return ret;
	}

	stored_lorawan_settings = *settings;
	stored_lorawan_settings.provisioned = true;
	has_dev_eui = true;
	has_join_eui = true;
	has_app_key = true;

	return tracker_lorawan_prepare_identity();
}

int tracker_lorawan_settings_clear(void)
{
	int ret = 0;

	ret = remember_first_error(ret, settings_delete(TRACKER_LORAWAN_DEV_EUI_SETTING));
	ret = remember_first_error(ret, settings_delete(TRACKER_LORAWAN_JOIN_EUI_SETTING));
	ret = remember_first_error(ret, settings_delete(TRACKER_LORAWAN_APP_KEY_SETTING));
	ret = remember_first_error(ret,
				   settings_delete(TRACKER_LORAWAN_IDENTITY_CRC_SETTING));
	ret = remember_first_error(ret, tracker_lorawan_clear_nvm());

	tracker_lorawan_reset_loaded_settings();

	return ret;
}

#if IS_ENABLED(CONFIG_SHELL)
static int parse_hex_arg(const char *arg, uint8_t *dst, size_t dst_len)
{
	if (strlen(arg) != dst_len * 2) {
		return -EINVAL;
	}

	if (hex2bin(arg, strlen(arg), dst, dst_len) != dst_len) {
		return -EINVAL;
	}

	return 0;
}

static int cmd_tracker_provision_status(const struct shell *shell, size_t argc,
					char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct tracker_lorawan_settings settings;
	char dev_eui[(TRACKER_LORAWAN_EUI_LEN * 2) + 1];
	char join_eui[(TRACKER_LORAWAN_EUI_LEN * 2) + 1];

	tracker_lorawan_settings_get(&settings);
	(void)bin2hex(settings.dev_eui, sizeof(settings.dev_eui), dev_eui,
		      sizeof(dev_eui));
	(void)bin2hex(settings.join_eui, sizeof(settings.join_eui), join_eui,
		      sizeof(join_eui));

	shell_print(shell, "LoRaWAN identity: %s",
		    settings.provisioned ? "provisioned" : "development fallback");
	shell_print(shell, "DevEUI: %s", dev_eui);
	shell_print(shell, "JoinEUI: %s", join_eui);
	shell_print(shell, "AppKey: %s",
		    settings.provisioned ? "set" : "development fallback");

	return 0;
}

static int cmd_tracker_provision_set(const struct shell *shell, size_t argc,
				     char **argv)
{
	ARG_UNUSED(argc);

	struct tracker_lorawan_settings settings = {
		.provisioned = true,
	};
	int ret;

	ret = parse_hex_arg(argv[1], settings.dev_eui, sizeof(settings.dev_eui));
	if (ret != 0) {
		shell_error(shell, "DevEUI must be 16 hex characters");
		return -EINVAL;
	}

	ret = parse_hex_arg(argv[2], settings.join_eui, sizeof(settings.join_eui));
	if (ret != 0) {
		shell_error(shell, "JoinEUI must be 16 hex characters");
		return -EINVAL;
	}

	ret = parse_hex_arg(argv[3], settings.app_key, sizeof(settings.app_key));
	if (ret != 0) {
		shell_error(shell, "AppKey must be 32 hex characters");
		return -EINVAL;
	}

	ret = tracker_lorawan_settings_save(&settings);
	if (ret != 0) {
		shell_error(shell, "failed to save LoRaWAN identity: %d", ret);
		return -ENOEXEC;
	}

	shell_print(shell, "LoRaWAN identity saved; reboot the tracker to apply it");
	return 0;
}

static int cmd_tracker_provision_clear(const struct shell *shell, size_t argc,
				       char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = tracker_lorawan_settings_clear();

	if (ret != 0) {
		shell_error(shell, "failed to clear LoRaWAN identity: %d", ret);
		return -ENOEXEC;
	}

	shell_print(shell, "LoRaWAN identity cleared; reboot the tracker to apply fallback");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	tracker_provision_cmds,
	SHELL_CMD_ARG(status, NULL, "Show provisioned LoRaWAN identity status",
		      cmd_tracker_provision_status, 1, 0),
	SHELL_CMD_ARG(set, NULL,
		      "Set LoRaWAN OTAA identity: <dev_eui> <join_eui> <app_key>",
		      cmd_tracker_provision_set, 4, 0),
	SHELL_CMD_ARG(clear, NULL, "Delete provisioned LoRaWAN identity",
		      cmd_tracker_provision_clear, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	tracker_cmds,
	SHELL_CMD(provision, &tracker_provision_cmds,
		  "Tracker provisioning commands", NULL),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(tracker, &tracker_cmds, "Tracker commands", NULL);
#endif
