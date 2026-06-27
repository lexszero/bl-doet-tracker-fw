#ifndef APP_LIB_LORAWAN_NODE_H_
#define APP_LIB_LORAWAN_NODE_H_

#include <zephyr/lorawan/lorawan.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lorawan_node_config {
	struct lorawan_join_config join_config;
};

int lorawan_node_init(void);
int lorawan_node_join(void);

#ifdef __cplusplus
}
#endif

#endif
