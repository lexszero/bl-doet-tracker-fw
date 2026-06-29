#ifndef APP_LIB_LORAWAN_NODE_H_
#define APP_LIB_LORAWAN_NODE_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/lorawan/lorawan.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lorawan_node_config {
	struct lorawan_join_config join_config;
};

struct lorawan_node_status {
	bool adr_enabled;
	bool datarate_valid;
	enum lorawan_datarate datarate;
	bool last_downlink_valid;
	uint8_t last_downlink_flags;
	int16_t last_downlink_rssi;
	int8_t last_downlink_snr;
	uint32_t downlink_count;
};

int lorawan_node_init(void);
int lorawan_node_join(void);
void lorawan_node_get_status(struct lorawan_node_status *status);

#ifdef __cplusplus
}
#endif

#endif
