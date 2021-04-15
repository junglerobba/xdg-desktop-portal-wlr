#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "xdpw.h"

static const char object_path[] = "/org/freedesktop/portal/desktop";
static const char interface_name[] = "org.freedesktop.impl.portal.Screenshot";

static bool exec_screenshooter(const char *path) {
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return false;
	} else if (pid == 0) {
		char *const argv[] = {
			"grim",
			"--",
			(char *)path,
			NULL,
		};
		execvp("grim", argv);

		perror("execvp");
		exit(127);
	}

	int stat;
	if (waitpid(pid, &stat, 0) < 0) {
		perror("waitpid");
		return false;
	}

	return stat == 0;
}
static bool exec_screenshooter_interactive(const char *path) {
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return false;
	} else if (pid == 0) {
		char cmd[strlen(path) + 25];
		snprintf(cmd, sizeof(cmd), "grim -g \"$(slurp)\" -- %s", path);
		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		perror("execl");
		exit(127);
	}

	int stat;
	if (waitpid(pid, &stat, 0) < 0) {
		perror("waitpid");
		return false;
	}

	return stat == 0;
}

static int method_screenshot(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	int ret = 0;

	bool interactive = false;

	char *handle, *app_id, *parent_window;
	ret = sd_bus_message_read(msg, "oss", &handle, &app_id, &parent_window);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}
	char *key;
	int inner_ret = 0;
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		inner_ret = sd_bus_message_read(msg, "s", &key);
		if (inner_ret < 0) {
			return inner_ret;
		}

		if (strcmp(key, "interactive") == 0) {
			bool mode;
			sd_bus_message_read(msg, "v", "b", &mode);
			logprint(DEBUG, "dbus: option interactive: %x", mode);
			interactive = mode;
		} else if (strcmp(key, "modal") == 0) {
			bool modal;
			sd_bus_message_read(msg, "v", "b", &modal);
			logprint(DEBUG, "dbus: option modal: %x", modal);
		} else {
			logprint(WARN, "dbus: unknown option %s", key);
			sd_bus_message_skip(msg, "v");
		}

		inner_ret = sd_bus_message_exit_container(msg);
		if (inner_ret < 0) {
			return inner_ret;
		}
	}
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_exit_container(msg);
	if (ret < 0) {
		return ret;
	}

	// TODO: cleanup this
	struct xdpw_request *req =
		xdpw_request_create(sd_bus_message_get_bus(msg), handle);
	if (req == NULL) {
		return -ENOMEM;
	}

	// TODO: choose a better path
	const char path[] = "/tmp/out.png";
	if (interactive && !exec_screenshooter_interactive(path)) {
		return -1;
	}
	if (!interactive && !exec_screenshooter(path)) {
		return -1;
	}

	const char uri_prefix[] = "file://";
	char uri[strlen(path) + strlen(uri_prefix) + 1];
	snprintf(uri, sizeof(uri), "%s%s", uri_prefix, path);

	sd_bus_message *reply = NULL;
	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_SUCCESS, 1, "uri", "s", uri);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}

	sd_bus_message_unref(reply);
	return 0;
}

static const sd_bus_vtable screenshot_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Screenshot", "ossa{sv}", "ua{sv}", method_screenshot, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

int xdpw_screenshot_init(struct xdpw_state *state) {
	// TODO: cleanup
	sd_bus_slot *slot = NULL;
	return sd_bus_add_object_vtable(state->bus, &slot, object_path, interface_name,
		screenshot_vtable, NULL);
}
