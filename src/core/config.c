#include "config.h"
#include "xdpw.h"
#include "logger.h"
#include "screencast_common.h"

#include <dictionary.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <iniparser.h>

void print_config(enum LOGLEVEL loglevel, struct xdpw_config *config) {
	logprint(loglevel, "config: outputname  %s", config->screencast_conf.output_name);
	logprint(loglevel, "config: chooser_cmd: %s\n", config->screencast_conf.chooser_cmd);
	logprint(loglevel, "config: chooser_type: %s\n", chooser_type_str(config->screencast_conf.chooser_type));
}

// NOTE: calling finish_config won't prepare the config to be read again from config file
// with init_config since to pointers and other values won't be reset to NULL, or 0
void finish_config(struct xdpw_config *config) {
	logprint(DEBUG, "config: destroying config");

	// screencast
	free(config->screencast_conf.output_name);
	free(config->screencast_conf.exec_before);
	free(config->screencast_conf.exec_after);
	free(config->screencast_conf.chooser_cmd);
}

static void getstring_from_conffile(dictionary *d,
		const char *key, char **dest, const char *fallback) {
	if (*dest != NULL) {
		return;
	}
	const char *c = iniparser_getstring(d, key, fallback);
	if (c == NULL) {
		return;
	}
	// Allow keys without value as default
	if (strcmp(c, "") != 0) {
		*dest = strdup(c);
	} else {
		*dest = fallback ? strdup(fallback) : NULL;
	}
}

static void getdouble_from_conffile(dictionary *d,
		const char *key, double *dest, double fallback) {
	if (*dest != 0) {
		return;
	}
	*dest = iniparser_getdouble(d, key, fallback);
}

static bool file_exists(const char *path) {
	return path && access(path, R_OK) != -1;
}

static void config_parse_file(const char *configfile, struct xdpw_config *config) {
	dictionary *d = NULL;
	if (configfile) {
		logprint(INFO, "config: using config file %s", configfile);
		d = iniparser_load(configfile);
	} else {
		logprint(INFO, "config: no config file found");
	}
	if (configfile && !d) {
		logprint(ERROR, "config: unable to load config file %s", configfile);
	}

	// screencast
	getstring_from_conffile(d, "screencast:output_name", &config->screencast_conf.output_name, NULL);
	getdouble_from_conffile(d, "screencast:max_fps", &config->screencast_conf.max_fps, 0);
	getstring_from_conffile(d, "screencast:exec_before", &config->screencast_conf.exec_before, NULL);
	getstring_from_conffile(d, "screencast:exec_after", &config->screencast_conf.exec_after, NULL);
	getstring_from_conffile(d, "screencast:chooser_cmd", &config->screencast_conf.chooser_cmd, NULL);
	if (!config->screencast_conf.chooser_type) {
		char *chooser_type = NULL;
		getstring_from_conffile(d, "screencast:chooser_type", &chooser_type, "default");
		config->screencast_conf.chooser_type = get_chooser_type(chooser_type);
		free(chooser_type);
	}

	iniparser_freedict(d);
	logprint(DEBUG, "config: config file parsed");
	print_config(DEBUG, config);
}

static char *config_path(const char *prefix, const char *filename) {
	if (!prefix || !prefix[0] || !filename || !filename[0]) {
		return NULL;
	}

	char *config_folder = "xdg-desktop-portal-wlr";

	size_t size = 3 + strlen(prefix) + strlen(config_folder) + strlen(filename);
	char *path = calloc(size, sizeof(char));
	snprintf(path, size, "%s/%s/%s", prefix, config_folder, filename);
	return path;
}

static char *get_config_path(void) {
	const char *home = getenv("HOME");
	size_t size_fallback = 1 + strlen(home) + strlen("/.config");
	char *config_home_fallback = calloc(size_fallback, sizeof(char));
	snprintf(config_home_fallback, size_fallback, "%s/.config", home);

	const char *config_home = getenv("XDG_CONFIG_HOME");
	if (config_home == NULL || config_home[0] == '\0') {
		config_home = config_home_fallback;
	}

	const char *prefix[2];
	prefix[0] = config_home;
	prefix[1] = SYSCONFDIR "/xdg";

	const char *config[2];
	config[0] = getenv("XDG_CURRENT_DESKTOP");
	config[1] = "config";

	for (size_t i = 0; i < 2; i++) {
		for (size_t j = 0; j < 2; j++) {
			char *path = config_path(prefix[i], config[j]);
			if (!path) {
				continue;
			}
			logprint(TRACE, "config: trying config file %s", path);
			if (file_exists(path)) {
				return path;
			}
			free(path);
		}
	}

	return NULL;
}

void init_config(char ** const configfile, struct xdpw_config *config) {
	if (*configfile == NULL) {
		*configfile = get_config_path();
	}

	config_parse_file(*configfile, config);
}
