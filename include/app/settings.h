#ifndef APP_LIB_SETTINGS_H
#define APP_LIB_SETTINGS_H

struct settings {
	char hostname[32];
};

extern struct settings SETTINGS;

int settings_init();

#endif
