#ifndef APP_LIB_GNSS_H_
#define APP_LIB_GNSS_H_

#ifdef __cplusplus
extern "C" {
#endif

int gnss_init();
const struct gnss_data * gnss_get_data();

#ifdef __cplusplus
}
#endif

#endif
