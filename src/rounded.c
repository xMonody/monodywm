// rounded.c - 离屏圆角合成 (wlroots gles2 + FBO)
//
// 每个 toplevel 缓存圆角化副本: 客户端内容合成到 buf1, 再用 SDF 掩码着色器重绘到
// buf2; buf2 显示在客户端内容下面 (内容留在场景收输入/frame, 透明度 0).
// 只重绘脏区域 (两个 pass 按 damage scissor); FBO 取 window geometry, CSD 阴影被裁掉.

#include "server.h"

#include <drm_fourcc.h>
#include <math.h>
#include <pixman.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

// 全屏四边形顶点着色器. a_pos 在 [0,1], (0,0) 在左上角;
// y 映射与 wlroots gles2 渲染器一致 (buffer 首行在 framebuffer 底部).
static const char *rounded_vert_src =
	"attribute vec2 a_pos;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_pos;\n"
	"  gl_Position = vec4(a_pos.x * 2.0 - 1.0, a_pos.y * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

// 圆角矩形 SDF 掩码, 可选地在圆角内侧画边框环 (顶部三段各一色, 对应标题栏
// 手势区; 其余用依赖焦点的基础色). 聚焦窗口还在外侧画高斯投影
// (颜色/峰值透明度来自 u_shadow_color/alpha, 与边框色无关).
// (着色器内注释保持 ASCII: GLSL ES 1.00 源码字符集是 ASCII.)
static const char *rounded_frag_src =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_size;\n"
	"uniform vec2 u_window_origin;\n"
	"uniform vec2 u_window_size;\n"
	"uniform float u_radius;\n"
	"uniform float u_border_width;\n"
	"uniform vec4 u_border_color;\n"
	"uniform vec4 u_border_top_left;\n"
	"uniform vec4 u_border_top_mid;\n"
	"uniform vec4 u_border_top_right;\n"
	"uniform float u_border_gradient;\n"
	"uniform float u_shadow_sigma;\n"
	"uniform vec4 u_shadow_color;\n"
	"uniform float u_shadow_alpha;\n"
	"void main() {\n"
	"  vec2 p = v_uv * u_size - u_window_origin;\n"
	"  vec2 b = u_window_size * 0.5;\n"
	"  vec2 q = abs(p - b) - (b - vec2(u_radius));\n"
	"  float sd = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - u_radius;\n"
	"  float aa = 1.0;\n"
	"  float outer = 1.0 - smoothstep(-aa, aa, sd);\n"
	"  float inner = 1.0 - smoothstep(-aa, aa, sd + u_border_width);\n"
	"  float border = outer - inner;\n"
	"  float third = u_window_size.x / 3.0;\n"
	"  // horizontal: blend across the two junctions instead of hard cuts\n"
	"  float t1 = smoothstep(third - u_border_gradient, third + u_border_gradient, p.x);\n"
	"  float t2 = smoothstep(2.0 * third - u_border_gradient, 2.0 * third + u_border_gradient, p.x);\n"
	"  vec4 top_color = mix(u_border_top_left, u_border_top_mid, t1);\n"
	"  top_color = mix(top_color, u_border_top_right, t2);\n"
	"  // vertical fade: the top accent colors bleed down the left/right\n"
	"  // edges over u_border_gradient px, then fall back to base color\n"
	"  float vfade = 1.0 - smoothstep(u_border_width, u_border_width + u_border_gradient, p.y);\n"
	"  vec4 bcolor = mix(u_border_color, top_color, vfade);\n"
	"  vec4 c = texture2D(u_tex, v_uv);\n"
	"  // window (content + border), premultiplied\n"
	"  vec3 win_rgb = c.rgb * inner + bcolor.rgb * bcolor.a * border;\n"
	"  float win_a = c.a * inner + bcolor.a * border;\n"
	"  // soft gaussian drop shadow outside the rounded rect\n"
	"  float shadow_a = 0.0;\n"
	"  if (u_shadow_sigma > 0.0) {\n"
	"    // soft gaussian drop shadow: exp(-d^2/2s^2) in the SDF distance,\n"
	"    // a blurred box shadow (scenefx style) instead of the old hard\n"
	"    // outward-fading ring.  The FBO padding (3.5 * sigma) is where\n"
	"    // the gaussian has faded to ~0.2%, so the cut at the padding\n"
	"    // edge is invisible.\n"
	"    float s2 = u_shadow_sigma * u_shadow_sigma;\n"
	"    shadow_a = u_shadow_alpha * exp(-0.5 * sd * sd / s2);\n"
	"  }\n"
	"  // composite: window over its shadow (both premultiplied); the\n"
	"  // shadow color is independent of the border color\n"
	"  vec3 rgb = win_rgb + u_shadow_color.rgb * shadow_a * (1.0 - win_a);\n"
	"  float a = win_a + shadow_a * (1.0 - win_a);\n"
	"  gl_FragColor = vec4(rgb, a);\n"
	"}\n";

// 只要 damage 区域还简单, 局部重绘就划算; 过度碎片化的区域退回整幅重绘
#define ROUNDED_MAX_DAMAGE_RECTS 64

struct rounded_cache {
	struct server *server;
	struct toplevel *tl;
	struct wlr_scene_buffer *node;

	// 离屏缓存: 内容先合成到这里, 再掩码进 rounded_buf, 后者才是场景实际显示的
	struct wlr_buffer *content_buf;
	struct wlr_buffer *rounded_buf;
	GLuint content_tex;              // content_buf 的 GPU 侧副本
	int content_tex_width, content_tex_height;

	int fbo_width, fbo_height;       // 当前 FBO 尺寸 (物理像素)
	int window_pw, window_ph;        // 窗口尺寸 (物理像素)
	int shadow_px;                   // 阴影边距 (物理像素)
	int shadow_i;                    // 阴影边距 (布局像素, 已对齐到物理像素)
	int logical_width, logical_height; // 窗口尺寸 (布局像素)
	float shadow_logical;            // 阴影宽度 (布局像素)
	float scale;                     // FBO 渲染时使用的输出缩放
	float shadow_sigma_baked;        // 上次成功烘焙进 FBO 的阴影 sigma

	bool content_dirty;              // 客户端内容变化: 两个 pass 都跑
	bool mask_dirty;                 // 仅边框/阴影参数变化: 只跑掩码 pass
	bool failed;                     // 永久禁用 (无 gles2/GL)

	// 自上次 FBO 渲染以来累积的 damage. content_damage 是主 surface 的
	// surface 局部 damage (由 rounded_cache_content_commit 填入);
	// fbo_damage 是每次渲染的暂存区域, 采用 FBO 物理坐标
	pixman_region32_t content_damage;
	pixman_region32_t fbo_damage;
	// 上次发布时主 surface 的几何, 这样调整 surface 尺寸、改缩放/变换或
	// viewport source 而不带 buffer damage 的提交也会触发整幅重绘
	int surf_w, surf_h;
	int surf_scale;
	enum wl_output_transform surf_transform;
	bool vp_has_src;
	struct wlr_fbox vp_src;
	// 主 surface 直接 subsurface 堆叠顺序的快照 (struct wlr_subsurface *),
	// 用于检测不带 buffer damage 的 place_above/place_below 重排
	struct wl_array subsurface_order;
};

// 前向声明: 供下面的 commit 收集器使用, 定义在 rounded_note_surface_state() 附近
static bool rounded_subsurface_order_changed(struct rounded_cache *rc);

// --- 少量 GL 辅助 ---

struct egl_context_state {
	EGLDisplay display;
	EGLContext context;
	EGLSurface draw, read;
};

static bool rounded_begin_gl(struct wlr_renderer *renderer,
		struct egl_context_state *save) {
	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer);
	if (egl == NULL) {
		return false;
	}

	save->display = eglGetCurrentDisplay();
	save->context = eglGetCurrentContext();
	save->draw = eglGetCurrentSurface(EGL_DRAW);
	save->read = eglGetCurrentSurface(EGL_READ);

	if (!eglMakeCurrent(wlr_egl_get_display(egl), EGL_NO_SURFACE,
			EGL_NO_SURFACE, wlr_egl_get_context(egl))) {
		wlr_log(WLR_ERROR, "rounded: eglMakeCurrent failed");
		return false;
	}
	return true;
}

static void rounded_end_gl(struct egl_context_state *save) {
	if (save->display != EGL_NO_DISPLAY) {
		eglMakeCurrent(save->display, save->draw, save->read, save->context);
	}
}

static GLuint rounded_compile_shader(GLenum type, const char *src) {
	GLuint shader = glCreateShader(type);
	if (shader == 0) {
		return 0;
	}
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "rounded: shader compile failed: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

// 边框/圆角 uniform 位置 (mask 程序用这一套 uniform 名字),
// 这里集中一次, 上传也走同一个 helper.
struct border_uniforms {
	GLint radius, width, color;
	GLint top_left, top_mid, top_right, gradient;
};

// 上传与边框/圆角相关的 uniform (值都取自 border.c 的策略函数)
static void rounded_upload_border_uniforms(const struct border_uniforms *u,
		struct toplevel *tl, float scale) {
	glUniform1f(u->radius, (float)CONFIG_ROUNDED_RADIUS * scale);
	glUniform1f(u->width, border_width(tl) * scale);
	struct wlr_render_color border = border_color(tl->server, tl);
	glUniform4f(u->color, border.r, border.g, border.b, border.a);
	struct wlr_render_color left, mid, right;
	border_top_colors(tl, &left, &mid, &right);
	glUniform4f(u->top_left, left.r, left.g, left.b, left.a);
	glUniform4f(u->top_mid, mid.r, mid.g, mid.b, mid.a);
	glUniform4f(u->top_right, right.r, right.g, right.b, right.a);
	glUniform1f(u->gradient, border_gradient_width(tl) * scale);
}

// 圆角掩码程序全进程共享 (一个 renderer): 每个窗口的着色器完全一样, 差异全在
// uniform 里. 之前每个 rounded_cache 各编译链接一套 (N 个窗口 = N 套 program),
// 现在只在首次使用时编译一次, VBO 也一样.
static struct {
	bool tried;
	bool ready;
	GLuint program;
	GLuint vbo;
	GLint a_pos;
	GLint u_tex;
	GLint u_size;
	GLint u_window_origin;
	GLint u_window_size;
	GLint u_shadow_sigma;
	GLint u_shadow_color;
	GLint u_shadow_alpha;
	struct border_uniforms border;
} mask_gl;

// 惰性编译共享的掩码程序并建立全屏四边形 VBO. 调用时 GL 上下文必须已 current.
static bool rounded_mask_gl_init(void) {
	if (mask_gl.tried) {
		return mask_gl.ready;
	}
	mask_gl.tried = true;

	GLuint vert = rounded_compile_shader(GL_VERTEX_SHADER, rounded_vert_src);
	GLuint frag = rounded_compile_shader(GL_FRAGMENT_SHADER, rounded_frag_src);
	if (vert == 0 || frag == 0) {
		if (vert != 0) {
			glDeleteShader(vert);
		}
		if (frag != 0) {
			glDeleteShader(frag);
		}
		return false;
	}

	GLuint program = glCreateProgram();
	glAttachShader(program, vert);
	glAttachShader(program, frag);
	glLinkProgram(program);

	GLint ok = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	glDeleteShader(vert);
	glDeleteShader(frag);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "rounded: program link failed: %s", log);
		glDeleteProgram(program);
		return false;
	}

	mask_gl.program = program;
	mask_gl.a_pos = glGetAttribLocation(program, "a_pos");
	mask_gl.u_tex = glGetUniformLocation(program, "u_tex");
	mask_gl.u_size = glGetUniformLocation(program, "u_size");
	mask_gl.u_window_origin = glGetUniformLocation(program, "u_window_origin");
	mask_gl.u_window_size = glGetUniformLocation(program, "u_window_size");
	mask_gl.u_shadow_sigma = glGetUniformLocation(program, "u_shadow_sigma");
	mask_gl.u_shadow_color = glGetUniformLocation(program, "u_shadow_color");
	mask_gl.u_shadow_alpha = glGetUniformLocation(program, "u_shadow_alpha");
	mask_gl.border.radius = glGetUniformLocation(program, "u_radius");
	mask_gl.border.width = glGetUniformLocation(program, "u_border_width");
	mask_gl.border.color = glGetUniformLocation(program, "u_border_color");
	mask_gl.border.top_left = glGetUniformLocation(program,
		"u_border_top_left");
	mask_gl.border.top_mid = glGetUniformLocation(program,
		"u_border_top_mid");
	mask_gl.border.top_right = glGetUniformLocation(program,
		"u_border_top_right");
	mask_gl.border.gradient = glGetUniformLocation(program,
		"u_border_gradient");

	static const float quad[] = {
		0.0f, 0.0f, // 左上
		1.0f, 0.0f, // 右上
		0.0f, 1.0f, // 左下
		1.0f, 1.0f, // 右下
	};
	glGenBuffers(1, &mask_gl.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, mask_gl.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	mask_gl.ready = true;
	return true;
}

// 每个窗口只删自己的内容纹理; 共享的 program/VBO 进程级保留.
static void rounded_gl_fini(struct rounded_cache *rc) {
	if (rc->content_tex != 0) {
		glDeleteTextures(1, &rc->content_tex);
		rc->content_tex = 0;
	}
}

// --- buffer 缓存 ---

static struct wlr_buffer *rounded_alloc_buffer_server(struct server *server,
		int width, int height) {
	if (width <= 0 || height <= 0) {
		return NULL;
	}
	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format fmt = {
		.format = DRM_FORMAT_ARGB8888,
		.len = 1,
		.capacity = 1,
		.modifiers = mods,
	};
	return wlr_allocator_create_buffer(server->allocator, width, height,
		&fmt);
}

static void rounded_drop_buffers(struct rounded_cache *rc) {
	if (rc->content_buf != NULL) {
		wlr_buffer_drop(rc->content_buf);
		rc->content_buf = NULL;
	}
	if (rc->rounded_buf != NULL) {
		wlr_buffer_drop(rc->rounded_buf);
		rc->rounded_buf = NULL;
	}
}

static void rounded_release_buffers(struct rounded_cache *rc) {
	rounded_drop_buffers(rc);
	rc->fbo_width = rc->fbo_height = 0;
	rc->logical_width = rc->logical_height = 0;
}

// 选取 >= shadow_logical 的最小整数布局边距, 使它与输出缩放的乘积落在物理像素上,
// 从而场景能 1:1 采样 FBO (dest_logical*scale == FBO 尺寸), 分数缩放不重采样.
// (前提: 窗口逻辑尺寸 * scale 也是整数, 即 wp_fractional_scale 客户端的约定.)
static int rounded_shadow_logical_aligned(float shadow_logical, float scale) {
	int base = (int)ceilf(shadow_logical);
	if (scale <= 0.0f) {
		return base;
	}
	for (int i = base; i < base + 64; i++) {
		float v = (float)i * scale;
		if (fabsf(v - roundf(v)) < 1e-3f) {
			return i;
		}
	}
	return base;
}

static bool rounded_alloc_buffers(struct rounded_cache *rc,
		int logical_width, int logical_height, float scale,
		float shadow_logical) {
	int shadow_i = rounded_shadow_logical_aligned(shadow_logical, scale);
	int shadow_px = (int)lroundf((float)shadow_i * scale);
	int window_pw = (int)lroundf((float)logical_width * scale);
	int window_ph = (int)lroundf((float)logical_height * scale);
	int fw = window_pw + 2 * shadow_px;
	int fh = window_ph + 2 * shadow_px;
	if (fw <= 0 || fh <= 0) {
		return false;
	}

	// FBO 尺寸不变时复用现有的一对
	if (rc->content_buf != NULL && rc->rounded_buf != NULL &&
			rc->fbo_width == fw && rc->fbo_height == fh) {
		rc->logical_width = logical_width;
		rc->logical_height = logical_height;
		rc->shadow_logical = shadow_logical;
		rc->scale = scale;
		rc->window_pw = window_pw;
		rc->window_ph = window_ph;
		rc->shadow_px = shadow_px;
		rc->shadow_i = shadow_i;
		return true;
	}

	// 尺寸变化: 分配新的一对, 之后再丢弃旧的
	struct wlr_buffer *content_buf =
		rounded_alloc_buffer_server(rc->server, fw, fh);
	struct wlr_buffer *rounded_buf =
		rounded_alloc_buffer_server(rc->server, fw, fh);
	if (content_buf == NULL || rounded_buf == NULL) {
		if (content_buf != NULL) {
			wlr_buffer_drop(content_buf);
		}
		if (rounded_buf != NULL) {
			wlr_buffer_drop(rounded_buf);
		}
		return false;
	}

	rounded_drop_buffers(rc); // 场景节点对旧的 rounded buffer 保留自己的引用,
	                           // 直到它被重新发布
	rc->content_buf = content_buf;
	rc->rounded_buf = rounded_buf;
	rc->fbo_width = fw;
	rc->fbo_height = fh;
	rc->logical_width = logical_width;
	rc->logical_height = logical_height;
	rc->shadow_logical = shadow_logical;
	rc->scale = scale;
	rc->window_pw = window_pw;
	rc->window_ph = window_ph;
	rc->shadow_px = shadow_px;
	rc->shadow_i = shadow_i;
	return true;
}

// --- 内容合成 pass (wlroots render pass) ---

struct content_texture {
	struct wlr_texture *texture;
	bool owned; // 由我们创建, pass 之后必须销毁
};

struct content_pass_ctx {
	struct rounded_cache *rc;
	struct wlr_render_pass *pass;
	struct wlr_surface *root_surface;
	float scale;
	const pixman_region32_t *clip; // NULL = 渲染整个 FBO
	bool rendered_main;
	bool main_texture_failed;
	struct wl_array textures; // struct content_texture
};

// 场景 buffer 在离屏 FBO 内的目标框 (物理像素);
// 内容 pass 和 damage 收集遍历共用
static void rounded_buffer_dst_box(struct rounded_cache *rc,
		struct wlr_scene_buffer *buffer, int sx, int sy,
		struct wlr_box *dst_box) {
	// wlr_scene_node_for_each_buffer() 报告布局 (绝对) 坐标;
	// FBO 锚定在场景树原点, 所以缩放前先把位置变成树相对坐标
	int rel_x = sx - rc->tl->scene_tree->node.x;
	int rel_y = sy - rc->tl->scene_tree->node.y;
	*dst_box = (struct wlr_box){
		.x = rc->shadow_px + (int)lroundf((float)rel_x * rc->scale),
		.y = rc->shadow_px + (int)lroundf((float)rel_y * rc->scale),
		.width = (int)lroundf((float)buffer->dst_width * rc->scale),
		.height = (int)lroundf((float)buffer->dst_height * rc->scale),
	};
}

// surface 的 subsurface 链的根 surface (不是 subsurface 时就是它自己)
static struct wlr_surface *rounded_surface_root(struct wlr_surface *surface) {
	struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(surface);
	while (sub != NULL && sub->parent != NULL) {
		surface = sub->parent;
		sub = wlr_subsurface_try_from_wlr_surface(surface);
	}
	return surface;
}

static void rounded_content_pass_cb(struct wlr_scene_buffer *buffer,
		int sx, int sy, void *data) {
	struct content_pass_ctx *ctx = data;

	// 只合成 toplevel 自己的 surface 树 (surface + subsurface);
	// 跳过 popup 和我们自己的圆角节点
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (scene_surface == NULL) {
		return;
	}
	if (rounded_surface_root(scene_surface->surface) != ctx->root_surface) {
		return;
	}
	if (buffer->buffer == NULL) {
		return;
	}

	struct wlr_texture *texture = NULL;
	bool owns_texture = false;

	// 优先使用客户端 buffer 的缓存纹理 - 正是场景渲染器采样的那个.
	// 这里若改用 wlr_texture_from_buffer() 会重新导入 buffer, 而对源已被客户端
	// 释放的 SHM buffer, 那次 data 指针访问会失败 (source == NULL), 让 FBO 变陈旧.
	struct wlr_client_buffer *client_buffer =
		wlr_client_buffer_get(buffer->buffer);
	if (client_buffer != NULL && client_buffer->texture != NULL) {
		texture = client_buffer->texture;
	} else {
		texture = wlr_texture_from_buffer(ctx->rc->server->renderer,
			buffer->buffer);
		if (texture == NULL) {
			if (scene_surface->surface == ctx->root_surface) {
				ctx->main_texture_failed = true;
				wlr_log(WLR_ERROR, "rounded: failed to create texture for "
					"main surface buffer %p (%dx%d)",
					(void *)buffer->buffer,
					buffer->buffer->width, buffer->buffer->height);
			}
			return;
		}
		owns_texture = true;
	}

	// 遵守显式 buffer 同步 (linux-drm-syncobj-v1), 与 wlroots 的场景渲染器一致:
	// 采样 buffer 前等待客户端的 acquire timeline
	struct wlr_linux_drm_syncobj_surface_v1_state *syncobj_state =
		wlr_linux_drm_syncobj_v1_get_surface_state(scene_surface->surface);
	struct wlr_drm_syncobj_timeline *wait_timeline = NULL;
	uint64_t wait_point = 0;
	if (syncobj_state != NULL) {
		wait_timeline = syncobj_state->acquire_timeline;
		wait_point = syncobj_state->acquire_point;
	}

	struct content_texture *slot =
		wl_array_add(&ctx->textures, sizeof(struct content_texture));
	if (slot == NULL) {
		if (owns_texture) {
			wlr_texture_destroy(texture);
		}
		return;
	}
	slot->texture = texture;
	slot->owned = owns_texture;

	float alpha = 1.0f; // 内容在场景里通过透明度 0 隐藏, 但 FBO 副本原样传递
	                    // 客户端透明度 (乘数 1.0, 不是 0)
	// 与 wlroots 场景渲染器一致: 为我们不旋转的离屏 FBO 反转 buffer 变换
	enum wl_output_transform transform =
		wlr_output_transform_invert(buffer->transform);
	struct wlr_box dst_box;
	rounded_buffer_dst_box(ctx->rc, buffer, sx, sy, &dst_box);
	wlr_render_pass_add_texture(ctx->pass, &(struct wlr_render_texture_options){
		.texture = texture,
		.src_box = buffer->src_box,
		.dst_box = dst_box,
		.transform = transform,
		.filter_mode = buffer->filter_mode,
		.alpha = &alpha,
		.clip = ctx->clip,
		.wait_timeline = wait_timeline,
		.wait_point = wait_point,
	});

	if (scene_surface->surface == ctx->root_surface) {
		ctx->rendered_main = true;
	}
}

static bool rounded_render_content(struct rounded_cache *rc,
		const pixman_region32_t *clip) {
	struct wlr_xdg_surface *base = rc->tl->xdg_toplevel->base;
	if (base == NULL || base->surface == NULL) {
		return false;
	}

	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(rc->server->renderer, rc->content_buf,
			NULL);
	if (pass == NULL) {
		return false;
	}

	// 先把缓存 buffer 清成透明黑, 未被内容 surface 覆盖的区域才不会显示
	// 未初始化的 DMA-BUF 内存. 局部重绘时只清 damage 区域:
	// 未触及的像素保留 (仍然正确的) 之前内容.
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .x = 0, .y = 0,
			.width = rc->fbo_width, .height = rc->fbo_height },
		.color = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		.clip = clip,
	});

	struct content_pass_ctx ctx = {
		.rc = rc,
		.pass = pass,
		.root_surface = base->surface,
		.scale = rc->scale,
		.clip = clip,
	};
	wl_array_init(&ctx.textures);

	wlr_scene_node_for_each_buffer(&rc->tl->scene_tree->node,
		rounded_content_pass_cb, &ctx);

	bool ok = wlr_render_pass_submit(pass);

	struct content_texture *ref;
	wl_array_for_each(ref, &ctx.textures) {
		if (ref->owned) {
			wlr_texture_destroy(ref->texture);
		}
	}
	wl_array_release(&ctx.textures);

	if (!ok) {
		wlr_log(WLR_ERROR, "rounded: content pass submit failed");
		return false;
	}
	if (ctx.main_texture_failed) {
		wlr_log(WLR_ERROR, "rounded: main surface texture creation failed, "
			"falling back to raw content");
		return false;
	}
	// 只有主 surface 确实被渲染过才认为合成有效; 否则 FBO 会空/不完整,
	// 隐藏客户端内容会让窗口变透明
	if (!ctx.rendered_main) {
		wlr_log(WLR_DEBUG, "rounded: main surface not rendered into FBO, "
			"falling back to raw content");
		return false;
	}
	return true;
}

// --- 圆角掩码 pass (原生 GLES2) ---

// 把内容 FBO 拷进内容纹理 (供掩码着色器采样), 并把 SDF 掩码画到输出 FBO.
// region 为 NULL 时刷新整个 FBO, 否则只处理它的矩形 (glScissor 限定,
// 拷贝和采样都不越界).
static bool rounded_render_mask(struct rounded_cache *rc,
		const pixman_region32_t *region) {
	struct wlr_renderer *renderer = rc->server->renderer;
	if (!wlr_renderer_is_gles2(renderer)) {
		return false;
	}

	GLuint content_fbo =
		wlr_gles2_renderer_get_buffer_fbo(renderer, rc->content_buf);
	GLuint out_fbo =
		wlr_gles2_renderer_get_buffer_fbo(renderer, rc->rounded_buf);
	if (content_fbo == 0 || out_fbo == 0) {
		return false;
	}

	struct egl_context_state saved = {0};
	if (!rounded_begin_gl(renderer, &saved)) {
		return false;
	}

	// FBO 尺寸变化时 (重新) 分配 GPU 侧内容纹理
	if (rc->content_tex == 0 || rc->content_tex_width != rc->fbo_width ||
			rc->content_tex_height != rc->fbo_height) {
		if (rc->content_tex != 0) {
			glDeleteTextures(1, &rc->content_tex);
		}
		glGenTextures(1, &rc->content_tex);
		glBindTexture(GL_TEXTURE_2D, rc->content_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rc->fbo_width,
			rc->fbo_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		rc->content_tex_width = rc->fbo_width;
		rc->content_tex_height = rc->fbo_height;
	}

	int n_rects = 0;
	const pixman_box32_t *boxes = NULL;
	bool partial = region != NULL && !pixman_region32_empty(region);
	if (partial) {
		boxes = pixman_region32_rectangles(region, &n_rects);
	}

	// 圆角矩形掩码 pass 写入输出 FBO
	glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
	glViewport(0, 0, rc->fbo_width, rc->fbo_height);
	glDisable(GL_DEPTH_TEST);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_BLEND); // 掩码 pass 覆盖所绘区域

	glUseProgram(mask_gl.program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, rc->content_tex);
	glUniform1i(mask_gl.u_tex, 0);
	glUniform2f(mask_gl.u_size, (float)rc->fbo_width, (float)rc->fbo_height);
	glUniform2f(mask_gl.u_window_origin, (float)rc->shadow_px,
		(float)rc->shadow_px);
	glUniform2f(mask_gl.u_window_size, (float)rc->window_pw,
		(float)rc->window_ph);
	// 边距是常量 (始终是最大高斯阴影范围); 是否画阴影跟随焦点:
	// 未聚焦窗口 sigma 为 0. 阴影颜色与边框无关.
	glUniform1f(mask_gl.u_shadow_sigma, shadow_sigma(rc->tl) * rc->scale);
	glUniform1f(mask_gl.u_shadow_alpha, shadow_alpha());
	struct wlr_render_color shcol = shadow_color();
	glUniform4f(mask_gl.u_shadow_color, shcol.r, shcol.g, shcol.b, shcol.a);
	rounded_upload_border_uniforms(&mask_gl.border, rc->tl, rc->scale);

	glBindBuffer(GL_ARRAY_BUFFER, mask_gl.vbo);
	glEnableVertexAttribArray(mask_gl.a_pos);
	glVertexAttribPointer(mask_gl.a_pos, 2, GL_FLOAT, GL_FALSE, 0, NULL);

	if (partial) {
		// 逐矩形: 把刚合成的内容拷进纹理 (纯 GPU 拷贝, 没有 glReadPixels),
		// 然后在同一 scissor 下画四边形. 拷贝与绘制的坐标都是 buffer 坐标:
		// 着色器的 y 映射在 buffer 行与 GL 窗口行之间是恒等的.
		glEnable(GL_SCISSOR_TEST);
		for (int i = 0; i < n_rects; i++) {
			const pixman_box32_t *b = &boxes[i];
			int x = b->x1, y = b->y1;
			int w = b->x2 - b->x1, h = b->y2 - b->y1;
			glScissor(x, y, w, h);
			glBindFramebuffer(GL_FRAMEBUFFER, content_fbo);
			// 纹理偏移和 framebuffer 源都用该矩形的坐标: 纹理逐行镜像 FBO
			glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x, y, x, y, w, h);
			glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		glDisable(GL_SCISSOR_TEST);
	} else {
		glDisable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_FRAMEBUFFER, content_fbo);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
			rc->fbo_width, rc->fbo_height);
		glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}

	glDisableVertexAttribArray(mask_gl.a_pos);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	// 掩码绘制命令在同一 GL 上下文中排在场景渲染之前,
	// 所以场景采样时能立即看到刚写入的圆角 FBO

	rounded_end_gl(&saved);
	return true;
}

// --- 公共 API ---

// 圆角节点绝不拦截指针输入: 位于其上方的 (透明) 客户端内容才接收点击,
// 所以命中测试行为不变
static bool rounded_no_input(struct wlr_scene_buffer *buffer,
		double *sx, double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

struct content_opacity_ctx {
	struct wlr_surface *root;
	float opacity;
};

static void rounded_set_content_opacity_cb(struct wlr_scene_buffer *buffer,
		int sx, int sy, void *data) {
	struct content_opacity_ctx *ctx = data;
	(void)sx;
	(void)sy;

	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (scene_surface == NULL) {
		return;
	}
	if (rounded_surface_root(scene_surface->surface) != ctx->root) {
		return;
	}
	wlr_scene_buffer_set_opacity(buffer, ctx->opacity);
}

static void rounded_set_content_opacity(struct rounded_cache *rc,
		float opacity) {
	struct wlr_xdg_surface *base = rc->tl->xdg_toplevel->base;
	if (base == NULL || base->surface == NULL) {
		return;
	}
	struct content_opacity_ctx ctx = {
		.root = base->surface,
		.opacity = opacity,
	};
	wlr_scene_node_for_each_buffer(&rc->tl->scene_tree->node,
		rounded_set_content_opacity_cb, &ctx);
}

void rounded_cache_hide_content(struct toplevel *tl) {
	struct rounded_cache *rc = tl->rounded;
	if (rc == NULL || rc->failed || rc->node == NULL) {
		return;
	}
	// 只有存在有效圆角 FBO 可替代时才隐藏客户端内容.
	// 在首次成功发布之前 (或重绘进行中且旧 FBO 已丢弃时), 必须让原始内容保持可见:
	// 这里隐藏会让窗口完全透明.
	if (rc->node->buffer == NULL) {
		return;
	}
	rounded_set_content_opacity(rc, 0.0f);
}

// 淡变整个屏幕上的窗口 (animate.c). 通常唯一可见的窗口 buffer 是圆角 FBO 节点 -
// 原始客户端内容以透明度 0 隐藏 - 所以淡变只作用该节点 (边框和阴影都在同一个 FBO 里,
// 随之一同淡变). 在首次 FBO 发布之前 (或关闭圆角时), 场景绘制的是原始内容:
// 此时改为淡变窗口树下每个场景 buffer (主 surface + subsurface + popup).
static void rounded_window_opacity_cb(struct wlr_scene_buffer *buffer,
		int sx, int sy, void *data) {
	(void)sx;
	(void)sy;
	float opacity = *(float *)data;
	wlr_scene_buffer_set_opacity(buffer, opacity);
}

void rounded_window_set_opacity(struct toplevel *tl, float opacity) {
	if (opacity < 0.0f) {
		opacity = 0.0f;
	} else if (opacity > 1.0f) {
		opacity = 1.0f;
	}
	struct rounded_cache *rc = tl->rounded;
	if (rc != NULL && !rc->failed && rc->node != NULL &&
			rc->node->buffer != NULL) {
		wlr_scene_buffer_set_opacity(rc->node, opacity);
		return;
	}
	if (tl->scene_tree == NULL) {
		return;
	}
	wlr_scene_node_for_each_buffer(&tl->scene_tree->node,
		rounded_window_opacity_cb, &opacity);
}

struct rounded_cache *rounded_cache_create(struct server *server,
		struct toplevel *tl) {
	struct rounded_cache *rc = calloc(1, sizeof(*rc));
	if (rc == NULL) {
		return NULL;
	}
	rc->server = server;
	rc->tl = tl;
	rc->content_dirty = true;
	rc->mask_dirty = true;
	pixman_region32_init(&rc->content_damage);
	pixman_region32_init(&rc->fbo_damage);
	wl_array_init(&rc->subsurface_order);
	rc->shadow_sigma_baked = -1.0f;

	if (!wlr_renderer_is_gles2(server->renderer)) {
		wlr_log(WLR_INFO, "rounded: renderer is not gles2, disabling rounded corners");
		rc->failed = true;
		return rc;
	}

	rc->node = wlr_scene_buffer_create(tl->scene_tree, NULL);
	if (rc->node == NULL) {
		rc->failed = true;
		return rc;
	}
	rc->node->point_accepts_input = rounded_no_input;
	wlr_scene_node_lower_to_bottom(&rc->node->node);

	return rc;
}

void rounded_cache_destroy(struct rounded_cache *rc) {
	if (rc == NULL) {
		return;
	}
	struct egl_context_state saved = {0};
	// 共享 program 已就绪说明渲染器是 gles2, 且该窗口确实创建过 GL 资源
	bool have_gl = !rc->failed && mask_gl.ready &&
		wlr_renderer_is_gles2(rc->server->renderer);
	if (have_gl && rounded_begin_gl(rc->server->renderer, &saved)) {
		rounded_gl_fini(rc);
		rounded_end_gl(&saved);
	}
	rounded_release_buffers(rc);
	pixman_region32_fini(&rc->content_damage);
	pixman_region32_fini(&rc->fbo_damage);
	wl_array_release(&rc->subsurface_order);
	free(rc);
}

// 在 toplevel 所在输出上调度一帧: 缓存失效本身不必然损坏场景,
// 重绘后的发布才会损坏输出, 所以这里主动推一帧.
static void rounded_schedule_frame(struct toplevel *tl) {
	struct wlr_output *output = toplevel_output(tl->server, tl);
	if (output != NULL) {
		wlr_output_schedule_frame(output);
	}
}

void rounded_cache_dirty(struct toplevel *tl) {
	if (tl->rounded == NULL) {
		return;
	}
	tl->rounded->content_dirty = true;
	tl->rounded->mask_dirty = true;
	// 让缓存失效不一定损坏场景: 从最大化进入全屏且窗口框已等于输出
	// (没有 layer-shell 状态栏缩小作区) 时几何不变, 而没有任何 buffer 变化的
	// configure ack 也不带 damage. 那样就不会有帧被调度, rounded_render_all 永远不会
	// 重绘 FBO, 边框 (宽度/颜色依赖全屏状态) 会保持旧样.
	// 像 rounded_cache_dirty_mask 一样调度一帧.
	rounded_schedule_frame(tl);
}

// 仅内容失效: 附带了 damage 的 surface 提交
void rounded_cache_dirty_content(struct toplevel *tl) {
	if (tl->rounded != NULL) {
		tl->rounded->content_dirty = true;
	}
}

// 仅掩码失效: 内容未变而边框/阴影参数变化 (焦点); 复用缓存的内容 pass.
// 焦点变化本身不损坏场景中的任何东西 (内容提交通过 scene surface 自己的 damage 损坏),
// 所以显式在 toplevel 的输出上调度一帧 - 重绘后的发布才会损坏受影响的输出以重绘.
void rounded_cache_dirty_mask(struct toplevel *tl) {
	if (tl->rounded == NULL) {
		return;
	}
	tl->rounded->mask_dirty = true;
	rounded_schedule_frame(tl);
}

// 主 surface 提交: 收集本次提交的 damage 供局部重绘.
// 缩放以及缩放/变换/viewport source 变化自身不带 buffer damage, 所以退回整 surface damage.
void rounded_cache_content_commit(struct toplevel *tl) {
	struct rounded_cache *rc = tl->rounded;
	if (rc == NULL || rc->failed) {
		return;
	}
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (base == NULL || base->surface == NULL) {
		return;
	}
	struct wlr_surface *surface = base->surface;

	// wlroots 在提交后会把透明度重新应用为 1.0; 再次隐藏它
	// (仅当已经发布了有效的圆角 FBO)
	rounded_cache_hide_content(tl);

	pixman_region32_t dmg;
	pixman_region32_init(&dmg);
	wlr_surface_get_effective_damage(surface, &dmg);

	struct wlr_surface_state *state = &surface->current;
	if (state->width != rc->surf_w || state->height != rc->surf_h ||
			state->scale != rc->surf_scale ||
			state->transform != rc->surf_transform ||
			state->viewport.has_src != rc->vp_has_src ||
			(state->viewport.has_src &&
			 (state->viewport.src.x != rc->vp_src.x ||
			  state->viewport.src.y != rc->vp_src.y ||
			  state->viewport.src.width != rc->vp_src.width ||
			  state->viewport.src.height != rc->vp_src.height)) ||
			rounded_subsurface_order_changed(rc)) {
		// 几何变化但没有 buffer damage: 覆盖新 surface 区域和被腾空的旧区域
		// (缩小时必须清除新尺寸之外的像素)
		pixman_region32_union_rect(&dmg, &dmg, 0, 0, state->width,
			state->height);
		pixman_region32_union_rect(&dmg, &dmg, 0, 0, rc->surf_w,
			rc->surf_h);
	}

	if (!pixman_region32_empty(&dmg)) {
		pixman_region32_union(&rc->content_damage, &rc->content_damage,
			&dmg);
		rounded_cache_dirty_content(tl);
	}
	pixman_region32_fini(&dmg);
}

// subsurface 提交: 收集 damage (surface 局部坐标) 以及 subsurface 移动或改尺寸时
// 的旧位置区域, 这样新区域和被腾空区域都会重绘.
// unmaps 的 subsurface (attach NULL) 退回整幅重绘: 这种情况罕见, 且其旧内容
// 覆盖的区域不是本次提交能损坏的.
void rounded_cache_subsurface_commit(struct toplevel *tl,
		struct toplevel_subsurface *ts) {
	struct rounded_cache *rc = tl->rounded;
	if (rc == NULL || rc->failed) {
		return;
	}
	struct wlr_subsurface *sub = ts->subsurface;
	struct wlr_surface *surface = sub->surface;

	rounded_cache_hide_content(tl);

	int cur_x = sub->current.x;
	int cur_y = sub->current.y;
	int cur_w = surface->current.width;
	int cur_h = surface->current.height;

	if (surface->current.buffer == NULL) {
		// 未映射: 它曾覆盖的一切都必须重绘; 几何跟踪归零, 供将来重新映射
		ts->prev_x = cur_x;
		ts->prev_y = cur_y;
		ts->prev_w = 0;
		ts->prev_h = 0;
		rounded_cache_dirty(tl);
		return;
	}

	pixman_region32_t dmg;
	pixman_region32_init(&dmg);
	// 一次提交也可能重排 subsurface (place_above/below) 而不带任何 buffer damage,
	// 所以总是覆盖该 surface 自己的区域: 重新合成它会应用新的堆叠顺序
	pixman_region32_union_rect(&dmg, &dmg, 0, 0, cur_w, cur_h);

	if (cur_x != ts->prev_x || cur_y != ts->prev_y ||
			cur_w != ts->prev_w || cur_h != ts->prev_h) {
		// 被腾空的旧区域, 用 surface 自己的坐标 (按移动偏移):
		// 必须清除并重新合成
		pixman_region32_union_rect(&dmg, &dmg,
			ts->prev_x - cur_x, ts->prev_y - cur_y,
			ts->prev_w, ts->prev_h);
		ts->prev_x = cur_x;
		ts->prev_y = cur_y;
		ts->prev_w = cur_w;
		ts->prev_h = cur_h;
	}

	pixman_region32_t eff;
	pixman_region32_init(&eff);
	wlr_surface_get_effective_damage(surface, &eff);
	pixman_region32_union(&dmg, &dmg, &eff);
	pixman_region32_fini(&eff);

	pixman_region32_union(&ts->damage, &ts->damage, &dmg);
	rounded_cache_dirty_content(tl);
	pixman_region32_fini(&dmg);
}

// 重绘短暂失败时继续显示上次发布的圆角 FBO, 而不是露出原始 (无圆角) 内容.
// 只有从未发布过圆角 buffer 时才退回原始内容 - 否则窗口会完全透明.
static void rounded_fallback(struct rounded_cache *rc) {
	if (rc->node->buffer == NULL) {
		rounded_set_content_opacity(rc, 1.0f);
	} else {
		// 重新隐藏, 以防某次 surface 提交把透明度重置为 1.0
		rounded_cache_hide_content(rc->tl);
	}
}

// --- damage 收集与发布 ---

// 把 surface 局部 damage 区域经 buffer 的目标框映射到 FBO 坐标
static void rounded_map_damage(pixman_region32_t *dst,
		const pixman_region32_t *src, const struct wlr_box *dst_box,
		int dst_width, int dst_height) {
	if (pixman_region32_empty(src) || dst_width <= 0 || dst_height <= 0 ||
			dst_box->width <= 0 || dst_box->height <= 0) {
		return;
	}
	float xs = (float)dst_box->width / (float)dst_width;
	float ys = (float)dst_box->height / (float)dst_height;
	int n = 0;
	const pixman_box32_t *boxes = pixman_region32_rectangles(src, &n);
	for (int i = 0; i < n; i++) {
		const pixman_box32_t *b = &boxes[i];
		int x1 = dst_box->x + (int)floorf((float)b->x1 * xs);
		int y1 = dst_box->y + (int)floorf((float)b->y1 * ys);
		int x2 = dst_box->x + (int)ceilf((float)b->x2 * xs);
		int y2 = dst_box->y + (int)ceilf((float)b->y2 * ys);
		pixman_region32_union_rect(dst, dst, x1, y1, x2 - x1, y2 - y1);
	}
}

// 在 toplevel 的 subsurface 树里找某个 surface 的记账项
static struct toplevel_subsurface *rounded_find_subsurface(struct toplevel *tl,
		struct wlr_surface *surface) {
	struct toplevel_subsurface *ts;
	wl_list_for_each(ts, &tl->subsurfaces, link) {
		if (ts->subsurface->surface == surface) {
			return ts;
		}
	}
	return NULL;
}

struct damage_collect_ctx {
	struct rounded_cache *rc;
	struct wlr_surface *root_surface;
};

static void rounded_collect_damage_cb(struct wlr_scene_buffer *buffer,
		int sx, int sy, void *data) {
	struct damage_collect_ctx *ctx = data;
	struct rounded_cache *rc = ctx->rc;

	// 与内容 pass 相同的过滤: 只处理带 buffer 的 toplevel 自己的 surface 树
	// (surface + subsurface)
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (scene_surface == NULL ||
			rounded_surface_root(scene_surface->surface) != ctx->root_surface ||
			buffer->buffer == NULL) {
		return;
	}

	const pixman_region32_t *src = NULL;
	if (scene_surface->surface == ctx->root_surface) {
		src = &rc->content_damage;
	} else {
		struct toplevel_subsurface *ts =
			rounded_find_subsurface(rc->tl, scene_surface->surface);
		if (ts != NULL) {
			src = &ts->damage;
		}
	}
	if (src == NULL || pixman_region32_empty(src)) {
		return;
	}

	struct wlr_box dst_box;
	rounded_buffer_dst_box(rc, buffer, sx, sy, &dst_box);
	rounded_map_damage(&rc->fbo_damage, src, &dst_box,
		buffer->dst_width, buffer->dst_height);
}

// 遍历内容树一次, 把累积的 surface damage 收集到 rc->fbo_damage (FBO 物理坐标).
// 在内容 pass 之前运行, 因为内容 pass 需要先拿到完整区域来做 clip.
static void rounded_collect_damage(struct rounded_cache *rc) {
	struct wlr_xdg_surface *base = rc->tl->xdg_toplevel->base;
	if (base == NULL || base->surface == NULL) {
		return;
	}
	struct damage_collect_ctx ctx = {
		.rc = rc,
		.root_surface = base->surface,
	};
	pixman_region32_clear(&rc->fbo_damage);
	wlr_scene_node_for_each_buffer(&rc->tl->scene_tree->node,
		rounded_collect_damage_cb, &ctx);
}

// 每 surface 的 damage 缓存在一次渲染反映出其内容 (整幅或局部) 后被消费.
// 渲染中途到达的提交会添加新 damage 并重新置脏, 下一帧会捡起它.
static void rounded_clear_damage_caches(struct rounded_cache *rc) {
	pixman_region32_clear(&rc->content_damage);
	struct toplevel_subsurface *ts;
	wl_list_for_each(ts, &rc->tl->subsurfaces, link) {
		pixman_region32_clear(&ts->damage);
	}
}

// 把每个 damage 矩形按边框环宽度 (加一个 AA 像素) 扩大, 让混合了下方变化内容的
// 边框像素也重新掩码并重新发布; 阴影环从不采样内容, 无需扩大.
// 结果裁剪到 FBO 内.
static void rounded_expand_ring(struct rounded_cache *rc) {
	if (pixman_region32_empty(&rc->fbo_damage)) {
		return; // 空表示"整幅"
	}
	int expand = (int)ceilf(border_width(rc->tl) * rc->scale) + 1;
	int n = 0;
	const pixman_box32_t *boxes =
		pixman_region32_rectangles(&rc->fbo_damage, &n);
	pixman_region32_t expanded;
	pixman_region32_init(&expanded);
	for (int i = 0; i < n; i++) {
		const pixman_box32_t *b = &boxes[i];
		pixman_region32_union_rect(&expanded, &expanded,
			b->x1 - expand, b->y1 - expand,
			(b->x2 - b->x1) + 2 * expand, (b->y2 - b->y1) + 2 * expand);
	}
	pixman_region32_intersect_rect(&expanded, &expanded, 0, 0,
		rc->fbo_width, rc->fbo_height);
	pixman_region32_copy(&rc->fbo_damage, &expanded);
	pixman_region32_fini(&expanded);
}

// 只把 FBO 节点相对窗口树的位置/尺寸同步到当前显示框, 不重绘. 缓存命中时也要
// 调: 显示框与树原点不一定一起移动 (最大化/全屏等待期间树停在原位),
// 不在每帧同步就会用旧相对偏移把窗口画到左上角.
static void rounded_sync_node(struct rounded_cache *rc,
		const struct wlr_box *box) {
	struct toplevel *tl = rc->tl;
	int shadow_i = rc->shadow_i;
	int tree_x = tl->scene_tree != NULL ? tl->scene_tree->node.x : 0;
	int tree_y = tl->scene_tree != NULL ? tl->scene_tree->node.y : 0;
	wlr_scene_node_set_position(&rc->node->node,
		box->x - tree_x - shadow_i, box->y - tree_y - shadow_i);
	wlr_scene_buffer_set_dest_size(rc->node,
		box->width + 2 * shadow_i, box->height + 2 * shadow_i);
}

// 把刚渲染好的圆角 buffer 发布到场景节点, 并让它的位置/dest 尺寸与窗口 + 阴影边距同步.
// damage == NULL 表示整个 buffer 变了.
static void rounded_publish(struct rounded_cache *rc,
		const struct wlr_box *box, const pixman_region32_t *damage) {
	wlr_scene_buffer_set_buffer_with_damage(rc->node, rc->rounded_buf,
		damage);
	rounded_sync_node(rc, box);
}

// 快照主 surface 的直接 subsurface 堆叠顺序, 供之后的提交检测
// place_above/place_below 重排 (它们不带 buffer damage).
// 顺序在发布时捕获 - 即缓存 FBO 实际反映的状态 - 并在提交时比较.
static void rounded_capture_subsurface_order(struct rounded_cache *rc) {
	struct wlr_xdg_surface *base = rc->tl->xdg_toplevel->base;
	// 保留底层分配; 只重置已用长度
	rc->subsurface_order.size = 0;
	if (base == NULL || base->surface == NULL) {
		return;
	}
	struct wlr_surface *surface = base->surface;
	struct wlr_subsurface *sub;
	wl_list_for_each(sub, &surface->current.subsurfaces_below, current.link) {
		struct wlr_subsurface **slot =
			wl_array_add(&rc->subsurface_order, sizeof(*slot));
		if (slot == NULL) {
			rc->subsurface_order.size = 0;
			return;
		}
		*slot = sub;
	}
	wl_list_for_each(sub, &surface->current.subsurfaces_above, current.link) {
		struct wlr_subsurface **slot =
			wl_array_add(&rc->subsurface_order, sizeof(*slot));
		if (slot == NULL) {
			rc->subsurface_order.size = 0;
			return;
		}
		*slot = sub;
	}
}

// 主 surface 的直接 subsurface 堆叠顺序是否与上次发布的快照不同
// (先 below 列表, 再 above)
static bool rounded_subsurface_order_changed(struct rounded_cache *rc) {
	struct wlr_xdg_surface *base = rc->tl->xdg_toplevel->base;
	if (base == NULL || base->surface == NULL) {
		return false;
	}
	struct wlr_surface *surface = base->surface;

	struct wlr_subsurface **snapshot = rc->subsurface_order.data;
	size_t snapshot_len =
		rc->subsurface_order.size / sizeof(*snapshot);
	size_t index = 0;

	struct wlr_subsurface *sub;
	wl_list_for_each(sub, &surface->current.subsurfaces_below, current.link) {
		if (index >= snapshot_len || snapshot[index] != sub) {
			return true;
		}
		index++;
	}
	wl_list_for_each(sub, &surface->current.subsurfaces_above, current.link) {
		if (index >= snapshot_len || snapshot[index] != sub) {
			return true;
		}
		index++;
	}
	return index != snapshot_len;
}

// 记住已发布 FBO 所反映的主 surface 几何; 调整 surface 尺寸或改变 viewport source
// 而不带 buffer damage 的提交会与它比较来检测变化
static void rounded_note_surface_state(struct rounded_cache *rc) {
	struct wlr_xdg_surface *base = rc->tl->xdg_toplevel->base;
	if (base == NULL || base->surface == NULL) {
		return;
	}
	struct wlr_surface_state *state = &base->surface->current;
	rc->surf_w = state->width;
	rc->surf_h = state->height;
	rc->surf_scale = state->scale;
	rc->surf_transform = state->transform;
	rc->vp_has_src = state->viewport.has_src;
	rc->vp_src = state->viewport.src;
	rounded_capture_subsurface_order(rc);
}

// 渲染所有脏的圆角缓存. 由输出 frame 处理器在提交场景之前调用,
// 所以场景采样的总是新鲜 FBO.
void rounded_render_all(struct server *server) {
	if (!wlr_renderer_is_gles2(server->renderer)) {
		return;
	}

	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		struct rounded_cache *rc = tl->rounded;
		if (rc == NULL || rc->failed) {
			continue;
		}
		struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
		if (base == NULL || base->surface == NULL ||
				!base->surface->mapped || tl->minimized) {
			continue;
		}

		struct wlr_box box;
		toplevel_frame_box(server, tl, &box);
		if (box.width <= 0 || box.height <= 0) {
			continue;
		}

		struct wlr_output *output = toplevel_output(server, tl);
		float scale = output != NULL ? output->scale : 1.0f;
		if (scale <= 0.0f) {
			scale = 1.0f;
		}
		// FBO 始终预留最大阴影边距 (由高斯 sigma 推导), 所以焦点切换
		// 永远不改变其尺寸, 只做仅掩码重绘
		float shadow_w = (float)shadow_padding();

		// 阴影开关 (聚焦 / 最大化 / 全屏) 变化不像尺寸那样体现在 damage 里:
		// 与已烘焙值不同就强制重绘 mask. 内容没变时下面会走 mask-only 分支.
		float sigma_now = shadow_sigma(tl);
		if (sigma_now != rc->shadow_sigma_baked) {
			rc->mask_dirty = true;
		}

		if (!rc->content_dirty && !rc->mask_dirty &&
				rc->logical_width == box.width &&
				rc->logical_height == box.height && rc->scale == scale &&
				rc->shadow_logical == shadow_w) {
			// 每次 surface 提交会重置透明度, 场景渲染前再隐藏一次;
			// 内容没变但显示框原点可能变, 仍要同步节点位置
			rounded_sync_node(rc, &box);
			rounded_cache_hide_content(tl);
			continue;
		}

		// 仅掩码重绘: 边框/阴影参数变化 (焦点), 内容与 buffer 未变.
		// 需要 GL 程序已完全初始化, 且 FBO 已按当前尺寸发布,
		// 这样缓存的内容纹理才是完整的.
		if (rc->mask_dirty && !rc->content_dirty && mask_gl.ready &&
				rc->content_buf != NULL &&
				rc->node->buffer == rc->rounded_buf &&
				rc->logical_width == box.width &&
				rc->logical_height == box.height && rc->scale == scale) {
			rc->mask_dirty = false;
			if (!rounded_render_mask(rc, NULL)) {
				rc->mask_dirty = true;
				continue;
			}
			// 整个边框环和阴影颜色都变了
			rounded_publish(rc, &box, NULL);
			rc->shadow_sigma_baked = sigma_now;
			rounded_note_surface_state(rc);
			rounded_cache_hide_content(tl);
			wlr_log(WLR_DEBUG, "rounded: published mask-only FBO for app_id "
				"\"%s\" (%dx%d logical)",
				tl->app_id != NULL ? tl->app_id : "?", box.width, box.height);
			continue;
		}

		if (!mask_gl.ready) {
			struct egl_context_state saved = {0};
			if (!rounded_begin_gl(server->renderer, &saved)) {
				rounded_set_content_opacity(rc, 1.0f);
				rc->failed = true;
				continue;
			}
			bool gl_ok = rounded_mask_gl_init();
			rounded_end_gl(&saved);
			if (!gl_ok) {
				wlr_log(WLR_ERROR, "rounded: GL init failed, disabling rounded corners");
				rounded_set_content_opacity(rc, 1.0f);
				rc->failed = true;
				continue;
			}
		}

		// 掩码参数 (边框/阴影/焦点) 变化时必须整幅重绘: 这一次的 damage
		// 只来自内容提交, 覆盖不到阴影铺开的整个 FBO 边距. 若仍走局部
		// 路径, 阴影环只会在被内容 damage 触及的那几像素上被更新, 其余
		// 部分保留旧参数 - 例如失焦后仍残留一圈聚焦阴影.
		bool mask_changed = rc->mask_dirty;

		// 在合成之前清除脏标记. 如果客户端在我们渲染期间 (在场景采样之后、
		// 下面发布之前) 又提交了一次, 提交处理器会再次置 dirty=true,
		// 下一帧重绘 - 更新绝不会被静默丢弃.
		rc->content_dirty = false;
		rc->mask_dirty = false;

		if (!rounded_alloc_buffers(rc, box.width, box.height, scale,
				shadow_w)) {
			wlr_log(WLR_ERROR, "rounded: failed to allocate FBO buffers");
			rc->content_dirty = true;
			rc->mask_dirty = true;
			rounded_fallback(rc);
			continue;
		}

		// 把累积的 surface damage 收集到 FBO 坐标, 然后消费缓存:
		// 缓存里的内容都反映在这次渲染中 (整幅或局部)
		rounded_collect_damage(rc);
		rounded_clear_damage_caches(rc);

		// 只对 damage 区域做局部重绘. 需要 FBO 已按当前尺寸发布
		// (调整尺寸后的新 buffer 对内容是未初始化的, 必须整幅渲染),
		// 且矩形数量有界.
		bool partial = !mask_changed &&
			rc->node->buffer == rc->rounded_buf &&
			!pixman_region32_empty(&rc->fbo_damage) &&
			pixman_region32_n_rects(&rc->fbo_damage) <=
				ROUNDED_MAX_DAMAGE_RECTS;
		if (!partial) {
			pixman_region32_clear(&rc->fbo_damage); // 空 = 整幅
		}

		if (!rounded_render_content(rc,
				partial ? &rc->fbo_damage : NULL)) {
			// 客户端内容无法合成进 FBO (例如尚未就绪的 DMA-BUF/纹理):
			// 保留上一个圆角 FBO, 下一帧重试
			wlr_log(WLR_DEBUG, "rounded: content render failed for app_id "
				"\"%s\" (%dx%d), keeping previous FBO",
				tl->app_id != NULL ? tl->app_id : "?",
				box.width, box.height);
			rc->content_dirty = true;
			rounded_fallback(rc);
			continue;
		}

		// 边框环像素会混合其下方的内容: 扩大 damage, 让它们也重新掩码
		// (并重新发布)
		rounded_expand_ring(rc);

		if (!rounded_render_mask(rc, partial ? &rc->fbo_damage : NULL)) {
			wlr_log(WLR_ERROR, "rounded: offscreen render failed");
			rc->content_dirty = true;
			rounded_fallback(rc);
			continue;
		}

		// 发布新结果; damage 为 NULL = 整个 buffer
		rounded_publish(rc, &box, partial ? &rc->fbo_damage : NULL);
		rc->shadow_sigma_baked = sigma_now;
		rounded_note_surface_state(rc);
		// 注意: 不要在这里清除 dirty - 它在渲染前已清除,
		// 而渲染中途到达的提交已经把它重新置 true, 所以下一帧会重绘更新的内容.
		// FBO 现在有效: 隐藏原始客户端内容, 显示圆角副本
		rounded_cache_hide_content(tl);
		wlr_log(WLR_DEBUG, "rounded: published FBO for app_id \"%s\" "
			"(%dx%d logical, %dx%d physical, scale %.2f)",
			tl->app_id != NULL ? tl->app_id : "?",
			box.width, box.height, rc->fbo_width, rc->fbo_height, rc->scale);
	}
}
