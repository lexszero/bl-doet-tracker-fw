#ifndef APP_SETTINGS_H_
#define APP_SETTINGS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRACKER_LORAWAN_EUI_LEN 8
#define TRACKER_LORAWAN_KEY_LEN 16

struct tracker_lorawan_settings {
	uint8_t dev_eui[TRACKER_LORAWAN_EUI_LEN];
	uint8_t join_eui[TRACKER_LORAWAN_EUI_LEN];
	uint8_t app_key[TRACKER_LORAWAN_KEY_LEN];
	bool provisioned;
};

int tracker_settings_init(void);
void tracker_lorawan_settings_get(struct tracker_lorawan_settings *settings);
bool tracker_lorawan_settings_is_provisioned(void);
int tracker_lorawan_settings_save(const struct tracker_lorawan_settings *settings);
int tracker_lorawan_settings_clear(void);

#ifdef __cplusplus
}
#endif

#endif
