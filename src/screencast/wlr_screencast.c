#include "wlr_screencast.h"

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include "screencast.h"
#include "pipewire_screencast.h"
#include "xdpw.h"
#include "logger.h"
#include "fps_limit.h"

static void wlr_frame_buffer_destroy(struct xdpw_screencast_instance *cast) {
	// Even though this check may be deemed unnecessary,
	// this has been found to cause SEGFAULTs, like this one:
	// https://github.com/emersion/xdg-desktop-portal-wlr/issues/50
	if (cast->simple_frame.data != NULL) {
		munmap(cast->simple_frame.data, cast->simple_frame.size);
		cast->simple_frame.data = NULL;
	}

	if (cast->simple_frame.buffer != NULL) {
		wl_buffer_destroy(cast->simple_frame.buffer);
		cast->simple_frame.buffer = NULL;
	}
}

void xdpw_wlr_frame_free(struct xdpw_screencast_instance *cast) {
	zwlr_screencopy_frame_v1_destroy(cast->wlr_frame);
	cast->wlr_frame = NULL;
	if (cast->quit || cast->err) {
		wlr_frame_buffer_destroy(cast);
		logprint(TRACE, "xdpw: simple_frame buffer destroyed");
	}
	logprint(TRACE, "wlroots: frame destroyed");

	if (cast->quit || cast->err) {
		// TODO: revisit the exit condition (remove quit?)
		// and clean up sessions that still exist if err
		// is the cause of the instance_destroy call
		xdpw_screencast_instance_destroy(cast);
		return ;
	}

	uint64_t delay_ns = fps_limit_measure_end(&cast->fps_limit, cast->ctx->state->config->screencast_conf.max_fps);
	if (delay_ns > 0) {
		xdpw_add_timer(cast->ctx->state, delay_ns,
			(xdpw_event_loop_timer_func_t) xdpw_wlr_register_cb, cast);
	} else {
		xdpw_wlr_register_cb(cast);
	}
}

static int anonymous_shm_open(void) {
	char name[] = "/xdpw-shm-XXXXXX";
	int retries = 100;

	do {
		randname(name + strlen(name) - 6);

		--retries;
		// shm_open guarantees that O_CLOEXEC is set
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);

	return -1;
}

static struct wl_buffer *create_shm_buffer(struct xdpw_screencast_instance *cast,
		enum wl_shm_format fmt, int width, int height, int stride,
		void **data_out) {
	struct xdpw_screencast_context *ctx = cast->ctx;
	int size = stride * height;

	int fd = anonymous_shm_open();
	if (fd < 0) {
		logprint(ERROR, "wlroots: shm_open failed");
		return NULL;
	}

	int ret;
	while ((ret = ftruncate(fd, size)) == EINTR);

	if (ret < 0) {
		close(fd);
		logprint(ERROR, "wlroots: ftruncate failed");
		return NULL;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		logprint(ERROR, "wlroots: mmap failed: %m");
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, fd, size);
	close(fd);
	struct wl_buffer *buffer =
		wl_shm_pool_create_buffer(pool, 0, width, height, stride, fmt);
	wl_shm_pool_destroy(pool);

	*data_out = data;
	return buffer;
}

static void wlr_frame_buffer_chparam(struct xdpw_screencast_instance *cast,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
	logprint(DEBUG, "wlroots: reset buffer");
	cast->simple_frame.width = width;
	cast->simple_frame.height = height;
	cast->simple_frame.stride = stride;
	cast->simple_frame.size = stride * height;
	cast->simple_frame.format = format;
	wlr_frame_buffer_destroy(cast);
}

static void wlr_frame_linux_dmabuf(void *data,
		struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height) {
	logprint(TRACE, "wlroots: linux_dmabuf event handler");
}

static void wlr_frame_buffer_done(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: buffer_done event handler");

	zwlr_screencopy_frame_v1_copy_with_damage(frame, cast->simple_frame.buffer);
	logprint(TRACE, "wlroots: frame copied");

	fps_limit_measure_start(&cast->fps_limit, cast->ctx->state->config->screencast_conf.max_fps);
}

static void wlr_frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: buffer event handler");
	cast->wlr_frame = frame;
	if (cast->simple_frame.width != width ||
			cast->simple_frame.height != height ||
			cast->simple_frame.stride != stride ||
			cast->simple_frame.format != format) {
		logprint(TRACE, "wlroots: buffer properties changed");
		wlr_frame_buffer_chparam(cast, format, width, height, stride);
	}

	if (cast->simple_frame.buffer == NULL) {
		logprint(DEBUG, "wlroots: create shm buffer");
		cast->simple_frame.buffer = create_shm_buffer(cast, format, width, height,
			stride, &cast->simple_frame.data);
	} else {
		logprint(TRACE,"wlroots: shm buffer exists");
	}

	if (cast->simple_frame.buffer == NULL) {
		logprint(ERROR, "wlroots: failed to create buffer");
		abort();
	}

	if (zwlr_screencopy_manager_v1_get_version(cast->ctx->screencopy_manager) < 3) {
		wlr_frame_buffer_done(cast,frame);
	}
}

static void wlr_frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t flags) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: flags event handler");
	cast->simple_frame.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void wlr_frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: ready event handler");

	cast->simple_frame.tv_sec = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo);
	cast->simple_frame.tv_nsec = tv_nsec;

	if (!cast->quit && !cast->err && cast->pwr_stream_state) {
		pw_loop_signal_event(cast->ctx->state->pw_loop, cast->event);
		return;
	}

	xdpw_wlr_frame_free(cast);
}

static void wlr_frame_failed(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: failed event handler");
	cast->err = true;

	xdpw_wlr_frame_free(cast);
}

static void wlr_frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: damage event handler");

	cast->simple_frame.damage.x = x;
	cast->simple_frame.damage.y = y;
	cast->simple_frame.damage.width = width;
	cast->simple_frame.damage.height = height;
}

static const struct zwlr_screencopy_frame_v1_listener wlr_frame_listener = {
	.buffer = wlr_frame_buffer,
	.buffer_done = wlr_frame_buffer_done,
	.linux_dmabuf = wlr_frame_linux_dmabuf,
	.flags = wlr_frame_flags,
	.ready = wlr_frame_ready,
	.failed = wlr_frame_failed,
	.damage = wlr_frame_damage,
};

void xdpw_wlr_register_cb(struct xdpw_screencast_instance *cast) {
	cast->frame_callback = zwlr_screencopy_manager_v1_capture_output(
		cast->ctx->screencopy_manager, cast->with_cursor, cast->target_output->output);

	zwlr_screencopy_frame_v1_add_listener(cast->frame_callback,
		&wlr_frame_listener, cast);
	logprint(TRACE, "wlroots: callbacks registered");
}

static void wlr_output_handle_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t phys_width, int32_t phys_height,
		int32_t subpixel, const char *make, const char *model, int32_t transform) {
	struct xdpw_wlr_output *output = data;
	output->make = strdup(make);
	output->model = strdup(model);
}

static void wlr_output_handle_mode(void *data, struct wl_output *wl_output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		struct xdpw_wlr_output *output = data;
		output->framerate = (float)refresh/1000;
	}
}

static void wlr_output_handle_done(void *data, struct wl_output *wl_output) {
	/* Nothing to do */
}

static void wlr_output_handle_scale(void *data, struct wl_output *wl_output,
		int32_t factor) {
	/* Nothing to do */
}

static const struct wl_output_listener wlr_output_listener = {
	.geometry = wlr_output_handle_geometry,
	.mode = wlr_output_handle_mode,
	.done = wlr_output_handle_done,
	.scale = wlr_output_handle_scale,
};

static void wlr_xdg_output_name(void *data, struct zxdg_output_v1 *xdg_output,
		const char *name) {
	struct xdpw_wlr_output *output = data;

	output->name = strdup(name);
};

static void noop() {
	// This space intentionally left blank
}

static const struct zxdg_output_v1_listener wlr_xdg_output_listener = {
	.logical_position = noop,
	.logical_size = noop,
	.done = NULL, /* Deprecated */
	.description = noop,
	.name = wlr_xdg_output_name,
};

static void wlr_add_xdg_output_listener(struct xdpw_wlr_output *output,
		struct zxdg_output_v1 *xdg_output) {
	output->xdg_output = xdg_output;
	zxdg_output_v1_add_listener(output->xdg_output, &wlr_xdg_output_listener,
		output);
}

static void wlr_init_xdg_outputs(struct xdpw_screencast_context *ctx) {
	struct xdpw_wlr_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &ctx->output_list, link) {
		struct zxdg_output_v1 *xdg_output =
			zxdg_output_manager_v1_get_xdg_output(ctx->xdg_output_manager,
				output->output);
		wlr_add_xdg_output_listener(output, xdg_output);
	}
}

static pid_t spawn_chooser(char *cmd, int chooser_in[2], int chooser_out[2]) {
	logprint(TRACE,
			"exec chooser called: cmd %s, pipe chooser_in (%d,%d), pipe chooser_out (%d,%d)",
			cmd, chooser_in[0], chooser_in[1], chooser_out[0], chooser_out[1]);
	pid_t pid = fork();

	if (pid < 0) {
		perror("fork");
		return pid;
	} else if (pid == 0) {
		close(chooser_in[1]);
		close(chooser_out[0]);

		dup2(chooser_in[0], STDIN_FILENO);
		dup2(chooser_out[1], STDOUT_FILENO);
		close(chooser_in[0]);
		close(chooser_out[1]);

		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);

		perror("execl");
		_exit(127);
	}

	close(chooser_in[0]);
	close(chooser_out[1]);

	return pid;
}

static bool wait_chooser(pid_t pid) {
	int status;
	if (waitpid(pid ,&status, 0) != -1 && WIFEXITED(status)) {
		return WEXITSTATUS(status) != 127;
	}
	return false;
}

static bool wlr_output_chooser(struct xdpw_output_chooser *chooser,
		struct wl_list *output_list, struct xdpw_wlr_output **output) {
	logprint(DEBUG, "wlroots: output chooser called");
	struct xdpw_wlr_output *out;
	size_t name_size = 0;
	char *name = NULL;
	*output = NULL;

	int chooser_in[2]; //p -> c
	int chooser_out[2]; //c -> p

	if (pipe(chooser_in) == -1) {
		perror("pipe chooser_in");
		logprint(ERROR, "Failed to open pipe chooser_in");
		goto error_chooser_in;
	}
	if (pipe(chooser_out) == -1) {
		perror("pipe chooser_out");
		logprint(ERROR, "Failed to open pipe chooser_out");
		goto error_chooser_out;
	}

	pid_t pid = spawn_chooser(chooser->cmd, chooser_in, chooser_out);
	if (pid < 0) {
		logprint(ERROR, "Failed to fork chooser");
		goto error_fork;
	}

	switch (chooser->type) {
	case XDPW_CHOOSER_DMENU:;
		FILE *f = fdopen(chooser_in[1], "w");
		if (f == NULL) {
			perror("fdopen pipe chooser_in");
			logprint(ERROR, "Failed to create stream writing to pipe chooser_in");
			goto error_fork;
		}
		wl_list_for_each(out, output_list, link) {
			fprintf(f, "%s\n", out->name);
		}
		fclose(f);
		break;
	default:
		close(chooser_in[1]);
	}

	if (!wait_chooser(pid)) {
		close(chooser_out[0]);
		goto end;
	}

	FILE *f = fdopen(chooser_out[0], "r");
	if (f == NULL) {
		perror("fdopen pipe chooser_out");
		logprint(ERROR, "Failed to create stream reading from pipe chooser_out");
		close(chooser_out[0]);
		goto end;
	}

	ssize_t nread = getline(&name, &name_size, f);
	fclose(f);
	if (nread < 0) {
		perror("getline failed");
		goto end;
	}

	//Strip newline
	char *p = strchr(name, '\n');
	if (p != NULL) {
		*p = '\0';
	}

	logprint(TRACE, "wlroots: output chooser %s selects output %s", chooser->cmd, name);
	wl_list_for_each(out, output_list, link) {
		// TODO: Replugging of outputs can result in a corrupted output_list
		if (out->name && strcmp(out->name, name) == 0) {
			*output = out;
			break;
		}
	}
	free(name);

end:
	return true;

error_fork:
	close(chooser_out[0]);
	close(chooser_out[1]);
error_chooser_out:
	close(chooser_in[0]);
	close(chooser_in[1]);
error_chooser_in:
	*output = NULL;
	return false;
}

static struct xdpw_wlr_output *wlr_output_chooser_default(struct wl_list *output_list) {
	logprint(DEBUG, "wlroots: output chooser called");
	struct xdpw_output_chooser default_chooser[] = {
		{XDPW_CHOOSER_SIMPLE, "slurp -f %o -o"},
		{XDPW_CHOOSER_DMENU, "wofi -d -n"},
		{XDPW_CHOOSER_DMENU, "bemenu"},
	};

	size_t N = sizeof(default_chooser)/sizeof(default_chooser[0]);
	struct xdpw_wlr_output *output = NULL;
	bool ret;
	for (size_t i = 0; i<N; i++) {
		ret = wlr_output_chooser(&default_chooser[i], output_list, &output);
		if (!ret) {
			logprint(DEBUG, "wlroots: output chooser %s not found. Trying next one.",
					default_chooser[i].cmd);
			continue;
		}
		if (output != NULL) {
			logprint(DEBUG, "wlroots: output chooser selects %s", output->name);
		} else {
			logprint(DEBUG, "wlroots: output chooser canceled");
		}
		return output;
	}
	return xdpw_wlr_output_first(output_list);
}

struct xdpw_wlr_output *xdpw_wlr_output_chooser(struct xdpw_screencast_context *ctx) {
	switch (ctx->state->config->screencast_conf.chooser_type) {
	case XDPW_CHOOSER_DEFAULT:
		return wlr_output_chooser_default(&ctx->output_list);
	case XDPW_CHOOSER_NONE:
		if (ctx->state->config->screencast_conf.output_name) {
			return xdpw_wlr_output_find_by_name(&ctx->output_list, ctx->state->config->screencast_conf.output_name);
		} else {
			return xdpw_wlr_output_first(&ctx->output_list);
		}
	case XDPW_CHOOSER_DMENU:
	case XDPW_CHOOSER_SIMPLE:;
		struct xdpw_wlr_output *output = NULL;
		if (!ctx->state->config->screencast_conf.chooser_cmd) {
			logprint(ERROR, "wlroots: no output chooser given");
			goto end;
		}
		struct xdpw_output_chooser chooser = {
			ctx->state->config->screencast_conf.chooser_type,
			ctx->state->config->screencast_conf.chooser_cmd
		};
		logprint(DEBUG, "wlroots: output chooser %s (%d)", chooser.cmd, chooser.type);
		bool ret = wlr_output_chooser(&chooser, &ctx->output_list, &output);
		if (!ret) {
			logprint(ERROR, "wlroots: output chooser %s failed", chooser.cmd);
			goto end;
		}
		if (output) {
			logprint(DEBUG, "wlroots: output chooser selects %s", output->name);
		} else {
			logprint(DEBUG, "wlroots: output chooser canceled");
		}
		return output;
	}
end:
	return NULL;
}

struct xdpw_wlr_output *xdpw_wlr_output_first(struct wl_list *output_list) {
	struct xdpw_wlr_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, output_list, link) {
		return output;
	}
	return NULL;
}

struct xdpw_wlr_output *xdpw_wlr_output_find_by_name(struct wl_list *output_list,
		const char *name) {
	struct xdpw_wlr_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, output_list, link) {
		if (strcmp(output->name, name) == 0) {
			return output;
		}
	}
	return NULL;
}

struct xdpw_wlr_output *xdpw_wlr_output_find(struct xdpw_screencast_context *ctx,
		struct wl_output *out, uint32_t id) {
	struct xdpw_wlr_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &ctx->output_list, link) {
		if ((output->output == out) || (output->id == id)) {
			return output;
		}
	}
	return NULL;
}

static void wlr_remove_output(struct xdpw_wlr_output *out) {
	wl_list_remove(&out->link);
}

static void wlr_registry_handle_add(void *data, struct wl_registry *reg,
		uint32_t id, const char *interface, uint32_t ver) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: interface to register %s  (Version: %u)",interface, ver);
	if (!strcmp(interface, wl_output_interface.name)) {
		struct xdpw_wlr_output *output = malloc(sizeof(*output));

		output->id = id;
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, WL_OUTPUT_VERSION);
		output->output = wl_registry_bind(reg, id, &wl_output_interface, WL_OUTPUT_VERSION);

		wl_output_add_listener(output->output, &wlr_output_listener, output);
		wl_list_insert(&ctx->output_list, &output->link);
	}

	if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
		uint32_t version = ver;
		if (SC_MANAGER_VERSION < ver) {
			version = SC_MANAGER_VERSION;
		} else if (ver < SC_MANAGER_VERSION_MIN) {
			version = SC_MANAGER_VERSION_MIN;
		}
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, version);
		ctx->screencopy_manager = wl_registry_bind(
			reg, id, &zwlr_screencopy_manager_v1_interface, version);
	}

	if (strcmp(interface, wl_shm_interface.name) == 0) {
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, WL_SHM_VERSION);
		ctx->shm = wl_registry_bind(reg, id, &wl_shm_interface, WL_SHM_VERSION);
	}

	if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, XDG_OUTPUT_MANAGER_VERSION);
		ctx->xdg_output_manager =
			wl_registry_bind(reg, id, &zxdg_output_manager_v1_interface, XDG_OUTPUT_MANAGER_VERSION);
	}
}

static void wlr_registry_handle_remove(void *data, struct wl_registry *reg,
		uint32_t id) {
	wlr_remove_output(
		xdpw_wlr_output_find((struct xdpw_screencast_context *)data, NULL, id));
}

static const struct wl_registry_listener wlr_registry_listener = {
	.global = wlr_registry_handle_add,
	.global_remove = wlr_registry_handle_remove,
};

int xdpw_wlr_screencopy_init(struct xdpw_state *state) {
	struct xdpw_screencast_context *ctx = &state->screencast;

	// initialize a list of outputs
	wl_list_init(&ctx->output_list);

	// initialize a list of active screencast instances
	wl_list_init(&ctx->screencast_instances);

	// retrieve registry
	ctx->registry = wl_display_get_registry(state->wl_display);
	wl_registry_add_listener(ctx->registry, &wlr_registry_listener, ctx);

	wl_display_dispatch(state->wl_display);
	wl_display_roundtrip(state->wl_display);

	logprint(DEBUG, "wayland: registry listeners run");

	wlr_init_xdg_outputs(ctx);

	wl_display_dispatch(state->wl_display);
	wl_display_roundtrip(state->wl_display);

	logprint(DEBUG, "wayland: xdg output listeners run");

	// make sure our wlroots supports shm protocol
	if (!ctx->shm) {
		logprint(ERROR, "Compositor doesn't support %s!", "wl_shm");
		return -1;
	}

	// make sure our wlroots supports screencopy protocol
	if (!ctx->screencopy_manager) {
		logprint(ERROR, "Compositor doesn't support %s!",
			zwlr_screencopy_manager_v1_interface.name);
		return -1;
	}

	return 0;
}

void xdpw_wlr_screencopy_finish(struct xdpw_screencast_context *ctx) {
	struct xdpw_wlr_output *output, *tmp_o;
	wl_list_for_each_safe(output, tmp_o, &ctx->output_list, link) {
		wl_list_remove(&output->link);
		zxdg_output_v1_destroy(output->xdg_output);
		wl_output_destroy(output->output);
	}

	struct xdpw_screencast_instance *cast, *tmp_c;
	wl_list_for_each_safe(cast, tmp_c, &ctx->screencast_instances, link) {
		cast->quit = true;
	}

	if (ctx->screencopy_manager) {
		zwlr_screencopy_manager_v1_destroy(ctx->screencopy_manager);
	}
	if (ctx->shm) {
		wl_shm_destroy(ctx->shm);
	}
	if (ctx->xdg_output_manager) {
		zxdg_output_manager_v1_destroy(ctx->xdg_output_manager);
	}
	if (ctx->registry) {
		wl_registry_destroy(ctx->registry);
	}
}
