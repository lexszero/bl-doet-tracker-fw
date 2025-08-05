#include "app/settings.h"

#include <zephyr/settings/settings.h>

struct settings SETTINGS = {
	.hostname = CONFIG_NET_HOSTNAME
};

int common_handle_set(const char *name, size_t len, settings_read_cb read_cb,
		  void *cb_arg);
int common_handle_commit(void);
int common_handle_export(int (*cb)(const char *name,
			       const void *value, size_t val_len));
int common_handle_get(const char *name, char *val, int val_len_max);

SETTINGS_STATIC_HANDLER_DEFINE(common, "common",
		common_handle_get,
		common_handle_set,
		common_handle_commit,
		common_handle_export
		);

int settings_init() {
	int rc;

#if defined(CONFIG_SETTINGS_FILE)
	FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(cstorage);

	/* mounting info */
	static struct fs_mount_t littlefs_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &cstorage,
	.storage_dev = (void *)STORAGE_PARTITION_ID,
	.mnt_point = "/ff"
	};

	rc = fs_mount(&littlefs_mnt);
	if (rc != 0) {
		printk("mounting littlefs error: [%d]\n", rc);
	} else {

		rc = fs_unlink(CONFIG_SETTINGS_FILE_PATH);
		if ((rc != 0) && (rc != -ENOENT)) {
			printk("can't delete config file%d\n", rc);
		} else {
			printk("FS initialized: OK\n");
		}
	}
#endif

	rc = settings_subsys_init();
	if (rc) {
		printk("settings subsys initialization: fail (err %d)\n", rc);
		return;
	}

	printk("settings subsys initialization: OK.\n");

	rc = settings_register(&alpha_handler);
	if (rc) {
		printk("subtree <%s> handler registered: fail (err %d)\n",
		       alpha_handler.name, rc);
	}

	printk("subtree <%s> handler registered: OK\n", alpha_handler.name);
	printk("subtree <alpha/beta> has static handler\n");
}
}
