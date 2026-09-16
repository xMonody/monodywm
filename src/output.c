// output.c - 显示器、输出布局与 wlr-output-management
//
// 每个输出创建一个 scene output、首选模式和 xcursor 缩放;
// 布局变化以及 wlr-randr 的 apply/test 请求在这里处理.

#include "server.h"

#include <stdlib.h>
#include <time.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

struct monitor {
	struct server *server;
	struct wlr_output *output;
	struct wlr_scene_output *scene_output;

	struct wl_listener frame;
	struct wl_listener destroy;
};

static void monitor_frame(struct wl_listener *listener, void *data) {
	struct monitor *mon = wl_container_of(listener, mon, frame);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	// 在场景采样之前, 把所有运行中的窗口动画推进到本帧自己的时刻,
	// 这样每个渲染帧显示的都是它自己 vblank 对应的缓动状态
	// (见 animate.c: 不存在更新定时器与 vblank 的节拍错位, 高刷输出会插值出更多状态)
	anim_frame_tick(mon->server,
		(uint32_t)now.tv_sec * 1000u +
		(uint32_t)(now.tv_nsec / 1000000u));
	// 场景采样前, 先渲染所有脏的离屏圆角 FBO
	rounded_render_all(mon->server);
	if (!wlr_scene_output_commit(mon->scene_output, NULL)) {
		return;
	}
	wlr_scene_output_send_frame_done(mon->scene_output, &now);
}

static void monitor_destroy(struct wl_listener *listener, void *data) {
	struct monitor *mon = wl_container_of(listener, mon, destroy);
	wl_list_remove(&mon->frame.link);
	wl_list_remove(&mon->destroy.link);
	free(mon);
}

static void update_output_manager_config(struct server *server) {
	struct wlr_output_configuration_v1 *config =
		wlr_output_configuration_v1_create();
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &server->output_layout->outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(config, lo->output);
		head->state.x = lo->x;
		head->state.y = lo->y;
	}
	wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

static void arrange_layer_surfaces(struct server *server) {
	struct layer_surface *ls;
	wl_list_for_each(ls, &server->layer_surfaces, link) {
		struct wlr_output *output = ls->layer_surface->output;
		if (output == NULL) {
			output = wlr_output_layout_get_center_output(server->output_layout);
		}
		if (output == NULL) {
			continue;
		}
		struct wlr_box full_area;
		wlr_output_layout_get_box(server->output_layout, output, &full_area);
		struct wlr_box usable_area = full_area;
		wlr_scene_layer_surface_v1_configure(ls->scene_layer, &full_area,
			&usable_area);
	}
}

static void update_scene_output_positions(struct server *server) {
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &server->output_layout->outputs, link) {
		struct wlr_scene_output *scene_output = lo->output->data;
		if (scene_output != NULL) {
			wlr_scene_output_set_position(scene_output, lo->x, lo->y);
		}
	}
}

void server_new_output(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *output = data;

	wlr_output_init_render(output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	if (!wlr_output_commit_state(output, &state)) {
		wlr_log(WLR_ERROR, "failed to commit output state");
	}
	wlr_output_state_finish(&state);

	wlr_output_create_global(output, server->display);

	wlr_output_layout_add_auto(server->output_layout, output);
	struct wlr_output_layout_output *lo =
		wlr_output_layout_get(server->output_layout, output);
	if (lo == NULL) {
		wlr_log(WLR_ERROR, "failed to add output to layout");
		return;
	}

	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, output);
	if (scene_output == NULL) {
		wlr_log(WLR_ERROR, "failed to create scene output");
		return;
	}
	wlr_scene_output_set_position(scene_output, lo->x, lo->y);
	output->data = scene_output;

	struct monitor *mon = calloc(1, sizeof(*mon));
	if (mon == NULL) {
		return;
	}
	mon->server = server;
	mon->output = output;
	mon->scene_output = scene_output;
	mon->frame.notify = monitor_frame;
	wl_signal_add(&output->events.frame, &mon->frame);
	mon->destroy.notify = monitor_destroy;
	wl_signal_add(&output->events.destroy, &mon->destroy);

	wlr_xcursor_manager_load(server->xcursor_manager, output->scale);

	update_output_manager_config(server);
	arrange_layer_surfaces(server);
}

void server_layout_change(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, layout_change);
	update_scene_output_positions(server);
	arrange_layer_surfaces(server);
	update_output_manager_config(server);
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		update_toplevel_output(tl->server, tl);
	}
}

void output_manager_apply(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		output_manager_apply);
	struct wlr_output_configuration_v1 *config = data;
	size_t states_len = 0;
	struct wlr_backend_output_state *states =
		wlr_output_configuration_v1_build_state(config, &states_len);
	bool ok = wlr_backend_test(server->backend, states, states_len);
	if (ok) {
		ok = wlr_backend_commit(server->backend, states, states_len);
	}
	free(states);
	if (ok) {
		struct wlr_output_configuration_head_v1 *head;
		wl_list_for_each(head, &config->heads, link) {
			wlr_output_layout_add(server->output_layout, head->state.output,
				head->state.x, head->state.y);
		}
	}
	if (ok) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);
}

void output_manager_test(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		output_manager_test);
	struct wlr_output_configuration_v1 *config = data;
	size_t states_len = 0;
	struct wlr_backend_output_state *states =
		wlr_output_configuration_v1_build_state(config, &states_len);
	bool ok = wlr_backend_test(server->backend, states, states_len);
	free(states);
	if (ok) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);
}
