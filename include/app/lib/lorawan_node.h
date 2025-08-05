#ifndef APP_LIB_LORAWAN_NODE_H_
#define APP_LIB_LORAWAN_NODE_H_

#include <zephyr/lorawan/lorawan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TODO: use settings subsystem for this, hardcoded for now */
#define LORAWAN_DEV_EUI     {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}
#define LORAWAN_JOIN_EUI    {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}
#define LORAWAN_APP_KEY     {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}

struct lorawan_node_config {
	struct lorawan_join_config join_config;
};

int lorawan_node_init();
int lorawan_node_join();

#ifdef __cplusplus
}
#endif

#endif
