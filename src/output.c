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
#include <wlr/util/log.h>

struct monitor {
	struct server *server;
	struct wlr_output *output;
	struct wlr_scene_output *scene_output;

	// 本输出的背景预模糊缓存 (挂在 server->blur_layer 下, 位置/尺寸随输出)
	struct wlr_scene_optimized_blur *optimized_blur;
	struct wl_list link; // server->monitors

	// 渲染帧率统计: 每秒把实际提交的帧数记一条 INFO 日志
	uint32_t fps_frames;
	uint32_t fps_start_ms;

	// 上次 commit 的输出 scale (用于检测缩放变化)
	float scale;
	// 上次 commit 的输出 transform (用于让预模糊缓存转向)
	enum wl_output_transform transform;

	struct wl_listener frame;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static void monitor_frame(struct wl_listener *listener, void *data) {
	struct monitor *mon = wl_container_of(listener, mon, frame);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint32_t now_ms = (uint32_t)now.tv_sec * 1000u +
		(uint32_t)(now.tv_nsec / 1000000u);
	if (!wlr_scene_output_commit(mon->scene_output, NULL)) {
		return;
	}
	wlr_scene_output_send_frame_done(mon->scene_output, &now);

	// 渲染帧率: 每秒把实际提交的帧数记一条 INFO 日志 (空闲不出帧时不计)
	if (mon->fps_start_ms == 0) {
		mon->fps_start_ms = now_ms;
	}
	mon->fps_frames++;
	uint32_t elapsed = now_ms - mon->fps_start_ms;
	if (elapsed >= 1000u) {
		wlr_log(WLR_INFO, "output %s: %.1f fps (%u frames in %u ms)",
			mon->output->name,
			(double)mon->fps_frames * 1000.0 / (double)elapsed,
			mon->fps_frames, elapsed);
		mon->fps_frames = 0;
		mon->fps_start_ms = now_ms;
	}
}

// 同步本输出预模糊缓存的位置与尺寸. set_position / set_size 幂等; set_size
// 在尺寸变化时会自动 mark_dirty. 输出 mode/scale/transform 或布局变化后调用.
static void monitor_update_blur_layer(struct monitor *mon) {
	if (mon->optimized_blur == NULL) {
		return;
	}
	struct wlr_output_layout_output *lo =
		wlr_output_layout_get(mon->server->output_layout, mon->output);
	if (lo == NULL) {
		return;
	}
	wlr_scene_node_set_position(&mon->optimized_blur->node, lo->x, lo->y);
	int w = 0, h = 0;
	wlr_output_effective_resolution(mon->output, &w, &h);
	wlr_scene_optimized_blur_set_size(mon->optimized_blur, w, h);
}

// background/bottom 层变化时让所有输出的预模糊缓存失效, 下一帧会重算.
void output_blur_layer_mark_dirty(struct server *server) {
	struct monitor *mon;
	wl_list_for_each(mon, &server->monitors, link) {
		if (mon->optimized_blur != NULL) {
			wlr_scene_optimized_blur_mark_dirty(mon->optimized_blur);
		}
	}
}

// 输出 scale 变化: 阴影的 blur_sigma 由 decor.c 按输出 scale 补偿 (scenefx
// 不缩放它), 所以缩放改变后必须重刷该输出上窗口的装饰. 节点位置/尺寸与圆角
// 由 scenefx 自行缩放, 不需要在这里处理.
static void monitor_commit(struct wl_listener *listener, void *data) {
	struct monitor *mon = wl_container_of(listener, mon, commit);
	struct wlr_output *output = mon->output;
	// mode/scale/transform 可能变了: 预模糊缓存的位置/尺寸要跟上 (幂等)
	monitor_update_blur_layer(mon);
	// transform 改变时缓存内容方向会变 (旋转 180° 时有效分辨率不变, set_size
	// 不会置脏), 必须显式重算
	if (output->transform != mon->transform) {
		mon->transform = output->transform;
		if (mon->optimized_blur != NULL) {
			wlr_scene_optimized_blur_mark_dirty(mon->optimized_blur);
		}
	}
	if (output->scale == mon->scale) {
		return;
	}
	mon->scale = output->scale;
	wlr_xcursor_manager_load(mon->server->xcursor_manager, output->scale);
	struct toplevel *tl;
	wl_list_for_each(tl, &mon->server->toplevels, link) {
		if (toplevel_output(mon->server, tl) == output) {
			decor_update(tl);
		}
	}
}

static void monitor_destroy(struct wl_listener *listener, void *data) {
	struct monitor *mon = wl_container_of(listener, mon, destroy);
	wl_list_remove(&mon->frame.link);
	wl_list_remove(&mon->commit.link);
	wl_list_remove(&mon->destroy.link);
	wl_list_remove(&mon->link);
	if (mon->optimized_blur != NULL) {
		wlr_scene_node_destroy(&mon->optimized_blur->node);
	}
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
	mon->scale = output->scale;
	mon->transform = output->transform;
	// 背景预模糊缓存: 每个输出一个, 挂在共享的 blur_layer 下, 随输出定位
	if (CONFIG_BLUR && server->blur_layer != NULL) {
		mon->optimized_blur = wlr_scene_optimized_blur_create(
			server->blur_layer, 0, 0);
	}
	wl_list_insert(&server->monitors, &mon->link);
	mon->frame.notify = monitor_frame;
	wl_signal_add(&output->events.frame, &mon->frame);
	mon->commit.notify = monitor_commit;
	wl_signal_add(&output->events.commit, &mon->commit);
	mon->destroy.notify = monitor_destroy;
	wl_signal_add(&output->events.destroy, &mon->destroy);

	wlr_xcursor_manager_load(server->xcursor_manager, output->scale);

	monitor_update_blur_layer(mon);
	update_output_manager_config(server);
	arrange_layer_surfaces(server);
}

void server_layout_change(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, layout_change);
	update_scene_output_positions(server);
	struct monitor *mon;
	wl_list_for_each(mon, &server->monitors, link) {
		monitor_update_blur_layer(mon);
	}
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
