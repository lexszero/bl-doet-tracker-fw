#ifndef APP_LIB_LORAWAN_NODE_H_
#define APP_LIB_LORAWAN_NODE_H_

#include <zephyr/settings/settings.h>
#include <zephyr/lorawan/lorawan.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lorawan_node_config {
	struct lorawan_join_config join_config;

};

#ifdef __cplusplus
}
#endif

#endif
