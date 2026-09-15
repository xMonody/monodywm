/*
 * rounded.c - offscreen rounded-corner compositing (wlroots gles2 + FBO)
 *
 * Each toplevel keeps a cached, rounded copy of its client content in an
 * offscreen FBO pair:
 *
 *   1. the client content (the xdg surface plus its subsurfaces, excluding
 *      popups) is composited into a first DMA-BUF via a normal wlroots
 *      render pass;
 *   2. a GLES2 fragment shader re-draws that buffer into a second DMA-BUF
 *      through a rounded-rectangle signed-distance-field mask.
 *
 * The rounded result is shown as a wlr_scene_buffer placed *underneath* the
 * client content.  The client content itself stays in the scene (so it keeps
 * receiving input, frame callbacks and popup stacking) but its opacity is set
 * to zero, so the scene only ever draws the rounded FBO copy on screen.
 *
 * The FBO pair is a cache: it is only re-rendered when the content is marked
 * dirty (client commit, subsurface commit, geometry change).  While the
 * content is unchanged the cached buffer is reused without any redraw.
 *
 * Two refinements keep the re-render cost proportional to what actually
 * changed:
 *
 *   - Damage-driven partial re-renders: every surface commit (main surface
 *     and subsurfaces) collects its buffer damage into per-surface regions.
 *     At render time the accumulated damage is mapped into FBO coordinates
 *     and both passes are scissored to it, so a small commit (a typed
 *     character, a blinking cursor) only re-composites and re-masks the
 *     pixels that changed, and only that region is republished to the
 *     scene.  Damage touching the border ring is expanded so ring pixels
 *     that blend the changed content are re-rendered too.
 *
 *   - Mask-only re-renders: border/shadow parameters (focus transitions)
 *     can change without any content change.  The content pass is then
 *     skipped entirely and only the SDF mask pass is re-run over the cached
 *     content.  To make this possible the FBO always reserves the maximum
 *     shadow padding, so a focus change never resizes the buffers.
 *
 * The offscreen FBO is sized to the xdg window geometry, so client-drawn
 * decorations outside the geometry (CSD shadow margins) are clipped away;
 * undecorated windows (geometry == surface) are unaffected.
 *
 * The composited result never leaves the GPU: no glReadPixels is used
 * anywhere in this file.
 */

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

/*
 * Fullscreen-quad vertex shader.  a_pos is in [0,1] with (0,0) at the top
 * left.  The y mapping (clip.y = 2*y - 1) matches wlroots' gles2 renderer,
 * which renders buffers with the first row at the bottom of the GL
 * framebuffer and samples textures with UV (0,0) at the buffer's top row.
 */
static const char *rounded_vert_src =
	"attribute vec2 a_pos;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_pos;\n"
	"  gl_Position = vec4(a_pos.x * 2.0 - 1.0, a_pos.y * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

/* Rounded-rectangle SDF mask over the sampled content texture, plus an
 * optional border ring drawn just inside the rounded edge.  The top band of
 * the ring is split into three thirds (left / middle / right), each with its
 * own color, matching the title-strip gesture zones; the rest of the ring
 * uses the focus-dependent base color.
 *
 * A soft gaussian drop shadow is drawn outside the rounded rect (only
 * when u_shadow_sigma > 0, i.e. for the focused window).  Its color and
 * peak opacity come from u_shadow_color / u_shadow_alpha and are
 * independent of the border color; the falloff is exp(-d^2 / 2 sigma^2)
 * in the SDF distance, which reads as a scenefx-style blurred box
 * shadow instead of a hard outward-fading ring. */
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
	"  /* horizontal: blend across the two junctions instead of hard cuts */\n"
	"  float t1 = smoothstep(third - u_border_gradient, third + u_border_gradient, p.x);\n"
	"  float t2 = smoothstep(2.0 * third - u_border_gradient, 2.0 * third + u_border_gradient, p.x);\n"
	"  vec4 top_color = mix(u_border_top_left, u_border_top_mid, t1);\n"
	"  top_color = mix(top_color, u_border_top_right, t2);\n"
	"  /* vertical fade: the top accent colors bleed down the left/right\n"
	"     edges over u_border_gradient px, then fall back to base color */\n"
	"  float vfade = 1.0 - smoothstep(u_border_width, u_border_width + u_border_gradient, p.y);\n"
	"  vec4 bcolor = mix(u_border_color, top_color, vfade);\n"
	"  vec4 c = texture2D(u_tex, v_uv);\n"
	"  /* window (content + border), premultiplied */\n"
	"  vec3 win_rgb = c.rgb * inner + bcolor.rgb * bcolor.a * border;\n"
	"  float win_a = c.a * inner + bcolor.a * border;\n"
	"  /* soft gaussian drop shadow outside the rounded rect */\n"
	"  float shadow_a = 0.0;\n"
	"  if (u_shadow_sigma > 0.0) {\n"
	"    /* soft gaussian drop shadow: exp(-d^2/2s^2) in the SDF distance,\n"
	"       a blurred box shadow (scenefx style) instead of the old hard\n"
	"       outward-fading ring.  The FBO padding (3.5 * sigma) is where\n"
	"       the gaussian has faded to ~0.2%, so the cut at the padding\n"
	"       edge is invisible. */\n"
	"    float s2 = u_shadow_sigma * u_shadow_sigma;\n"
	"    shadow_a = u_shadow_alpha * exp(-0.5 * sd * sd / s2);\n"
	"  }\n"
	"  /* composite: window over its shadow (both premultiplied); the\n"
	"     shadow color is independent of the border color */\n"
	"  vec3 rgb = win_rgb + u_shadow_color.rgb * shadow_a * (1.0 - win_a);\n"
	"  float a = win_a + shadow_a * (1.0 - win_a);\n"
	"  gl_FragColor = vec4(rgb, a);\n"
	"}\n";

/* partial re-renders are only worth it while the damage region stays
 * simple; a heavily fragmented region falls back to a full re-render */
#define ROUNDED_MAX_DAMAGE_RECTS 64

struct rounded_cache {
	struct server *server;
	struct toplevel *tl;
	struct wlr_scene_buffer *node;

	/* offscreen cache: content is composited here, then masked into the
	 * rounded buffer which is what the scene actually shows */
	struct wlr_buffer *content_buf;
	struct wlr_buffer *rounded_buf;
	GLuint content_tex;              /* GPU-side copy of content_buf */
	int content_tex_width, content_tex_height;

	int fbo_width, fbo_height;       /* current FBO size in physical pixels */
	int window_pw, window_ph;        /* window size in physical pixels */
	int shadow_px;                   /* shadow padding in physical pixels */
	int logical_width, logical_height; /* window size in layout pixels */
	float shadow_logical;            /* shadow width in layout pixels */
	float scale;                     /* output scale the FBO was rendered at */

	bool content_dirty;              /* client content changed: both passes run */
	bool mask_dirty;                 /* only border/shadow params changed: mask pass only */
	bool gl_ready;                   /* shader program compiled + linked */
	bool failed;                     /* permanently disabled (no gles2/GL) */

	/* accumulated damage since the last FBO render.  content_damage is the
	 * main surface's damage in surface-local coordinates (fed by
	 * rounded_cache_content_commit); fbo_damage is the per-render scratch
	 * region in FBO physical coordinates */
	pixman_region32_t content_damage;
	pixman_region32_t fbo_damage;
	/* main-surface geometry at the last publish, so commits that resize the
	 * surface, change its scale/transform or its viewport source without
	 * attaching buffer damage still trigger a full re-render */
	int surf_w, surf_h;
	int surf_scale;
	enum wl_output_transform surf_transform;
	bool vp_has_src;
	struct wlr_fbox vp_src;
	/* snapshot of the main surface's direct subsurface stacking order
	 * (struct wlr_subsurface *), used to detect place_above/place_below
	 * restacks that attach no buffer damage */
	struct wl_array subsurface_order;

	GLuint program;
	GLuint vbo;
	GLint a_pos;
	GLint u_tex;
	GLint u_size;
	GLint u_radius;
	GLint u_border_width;
	GLint u_border_color;
	GLint u_border_top_left;
	GLint u_border_top_mid;
	GLint u_border_top_right;
	GLint u_border_gradient;
	GLint u_window_origin;
	GLint u_window_size;
	GLint u_shadow_sigma;
	GLint u_shadow_color;
	GLint u_shadow_alpha;
};

/* forward decls: used by the commit collectors below, defined near
 * rounded_note_surface_state() */
static bool rounded_subsurface_order_changed(struct rounded_cache *rc);

/* --- small GL helpers -------------------------------------------------- */

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

static bool rounded_gl_init(struct rounded_cache *rc) {
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

	rc->program = glCreateProgram();
	glAttachShader(rc->program, vert);
	glAttachShader(rc->program, frag);
	glLinkProgram(rc->program);

	GLint ok = GL_FALSE;
	glGetProgramiv(rc->program, GL_LINK_STATUS, &ok);
	glDeleteShader(vert);
	glDeleteShader(frag);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(rc->program, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "rounded: program link failed: %s", log);
		glDeleteProgram(rc->program);
		rc->program = 0;
		return false;
	}

	rc->a_pos = glGetAttribLocation(rc->program, "a_pos");
	rc->u_tex = glGetUniformLocation(rc->program, "u_tex");
	rc->u_size = glGetUniformLocation(rc->program, "u_size");
	rc->u_radius = glGetUniformLocation(rc->program, "u_radius");
	rc->u_border_width = glGetUniformLocation(rc->program, "u_border_width");
	rc->u_border_color = glGetUniformLocation(rc->program, "u_border_color");
	rc->u_border_top_left = glGetUniformLocation(rc->program, "u_border_top_left");
	rc->u_border_top_mid = glGetUniformLocation(rc->program, "u_border_top_mid");
	rc->u_border_top_right = glGetUniformLocation(rc->program, "u_border_top_right");
	rc->u_border_gradient = glGetUniformLocation(rc->program, "u_border_gradient");
	rc->u_window_origin = glGetUniformLocation(rc->program, "u_window_origin");
	rc->u_window_size = glGetUniformLocation(rc->program, "u_window_size");
	rc->u_shadow_sigma = glGetUniformLocation(rc->program, "u_shadow_sigma");
	rc->u_shadow_color = glGetUniformLocation(rc->program, "u_shadow_color");
	rc->u_shadow_alpha = glGetUniformLocation(rc->program, "u_shadow_alpha");

	static const float quad[] = {
		0.0f, 0.0f, /* top-left */
		1.0f, 0.0f, /* top-right */
		0.0f, 1.0f, /* bottom-left */
		1.0f, 1.0f, /* bottom-right */
	};
	glGenBuffers(1, &rc->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, rc->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	return true;
}

static void rounded_gl_fini(struct rounded_cache *rc) {
	if (rc->vbo != 0) {
		glDeleteBuffers(1, &rc->vbo);
	}
	if (rc->program != 0) {
		glDeleteProgram(rc->program);
	}
	if (rc->content_tex != 0) {
		glDeleteTextures(1, &rc->content_tex);
		rc->content_tex = 0;
	}
}

/* --- buffer cache ------------------------------------------------------ */

static struct wlr_buffer *rounded_alloc_buffer(struct rounded_cache *rc,
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
	return wlr_allocator_create_buffer(rc->server->allocator, width, height,
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

static bool rounded_alloc_buffers(struct rounded_cache *rc,
		int logical_width, int logical_height, float scale,
		float shadow_logical) {
	int window_pw = (int)ceilf((float)logical_width * scale);
	int window_ph = (int)ceilf((float)logical_height * scale);
	int shadow_px = (int)ceilf(shadow_logical * scale);
	int fw = window_pw + 2 * shadow_px;
	int fh = window_ph + 2 * shadow_px;
	if (fw <= 0 || fh <= 0) {
		return false;
	}

	/* reuse the existing pair while the FBO size stays the same */
	if (rc->content_buf != NULL && rc->rounded_buf != NULL &&
			rc->fbo_width == fw && rc->fbo_height == fh) {
		rc->logical_width = logical_width;
		rc->logical_height = logical_height;
		rc->shadow_logical = shadow_logical;
		rc->scale = scale;
		rc->window_pw = window_pw;
		rc->window_ph = window_ph;
		rc->shadow_px = shadow_px;
		return true;
	}

	/* size changed: allocate a fresh pair and drop the old one afterwards */
	struct wlr_buffer *content_buf = rounded_alloc_buffer(rc, fw, fh);
	struct wlr_buffer *rounded_buf = rounded_alloc_buffer(rc, fw, fh);
	if (content_buf == NULL || rounded_buf == NULL) {
		if (content_buf != NULL) {
			wlr_buffer_drop(content_buf);
		}
		if (rounded_buf != NULL) {
			wlr_buffer_drop(rounded_buf);
		}
		return false;
	}

	rounded_drop_buffers(rc); /* the scene node keeps its own ref to the old
	                             rounded buffer until it is re-published */
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
	return true;
}

/* --- content compositing pass (wlroots render pass) -------------------- */

struct content_texture {
	struct wlr_texture *texture;
	bool owned; /* created by us, must be destroyed after the pass */
};

struct content_pass_ctx {
	struct rounded_cache *rc;
	struct wlr_render_pass *pass;
	struct wlr_surface *root_surface;
	float scale;
	const pixman_region32_t *clip; /* NULL = render the whole FBO */
	bool rendered_main;
	bool main_texture_failed;
	struct wl_array textures; /* struct content_texture */
};

/* destination box of a scene buffer inside the offscreen FBO (physical
 * pixels); shared by the content pass and the damage collection walk */
static void rounded_buffer_dst_box(struct rounded_cache *rc,
		struct wlr_scene_buffer *buffer, int sx, int sy,
		struct wlr_box *dst_box) {
	/* wlr_scene_node_for_each_buffer() reports layout (absolute) coordinates;
	 * the FBO is anchored at the scene tree's origin, so make the position
	 * tree-relative before scaling */
	int rel_x = sx - rc->tl->scene_tree->node.x;
	int rel_y = sy - rc->tl->scene_tree->node.y;
	*dst_box = (struct wlr_box){
		.x = rc->shadow_px + (int)lroundf((float)rel_x * rc->scale),
		.y = rc->shadow_px + (int)lroundf((float)rel_y * rc->scale),
		.width = (int)lroundf((float)buffer->dst_width * rc->scale),
		.height = (int)lroundf((float)buffer->dst_height * rc->scale),
	};
}

/* root surface of a surface's subsurface chain (itself if not a subsurface) */
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

	/* only the toplevel's own surface tree (surface + subsurfaces) is
	 * composited; popups and our own rounded node are skipped */
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

	/* Prefer the client buffer's cached texture - exactly the one the scene
	 * renderer samples.  Calling wlr_texture_from_buffer() here instead would
	 * re-import the buffer and, for SHM buffers whose source has already been
	 * released by the client, that data-pointer access fails (source == NULL),
	 * leaving the FBO stale. */
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

	/* honour explicit buffer synchronization (linux-drm-syncobj-v1), exactly
	 * like wlroots' scene renderer does: wait for the client's acquire
	 * timeline before sampling the buffer */
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

	float alpha = 1.0f; /* content is hidden in the scene via opacity 0, but
	                       the FBO copy passes the client alpha through
	                       untouched (multiplier 1.0, not 0) */
	/* match wlroots' scene renderer: invert the buffer transform for our
	 * non-rotated offscreen FBO */
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

	/* clear the cache buffer to transparent black first, so areas not
	 * covered by the content surface stay transparent instead of showing
	 * uninitialized DMA-BUF memory.  On a partial re-render only the
	 * damaged region is cleared: untouched pixels keep their (still
	 * correct) previous content. */
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
	/* only treat the composite as valid if the main surface was actually
	 * rendered; otherwise the FBO would be empty/partial and hiding the
	 * client content would leave the window transparent */
	if (!ctx.rendered_main) {
		wlr_log(WLR_DEBUG, "rounded: main surface not rendered into FBO, "
			"falling back to raw content");
		return false;
	}
	return true;
}

/* --- rounded mask pass (raw GLES2) ------------------------------------- */

/* Draw the SDF mask over the content into the output FBO.  With region NULL
 * the whole FBO is refreshed: a full copy of the content FBO into the
 * content texture (which also repairs any staleness left by earlier partial
 * passes) followed by one fullscreen quad.  With a region, only its rects
 * are copied and drawn under glScissor, so a partial re-render costs only
 * its damage area.  glCopyTexSubImage2D honours the scissor test and the
 * shader never samples outside it, so stale texels outside the scissor are
 * never read. */
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

	/* (re)allocate the GPU-side content texture when the FBO size changes */
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

	/* rounded-rectangle mask pass into the output FBO */
	glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
	glViewport(0, 0, rc->fbo_width, rc->fbo_height);
	glDisable(GL_DEPTH_TEST);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_BLEND); /* the mask pass overwrites the drawn region */

	glUseProgram(rc->program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, rc->content_tex);
	glUniform1i(rc->u_tex, 0);
	glUniform2f(rc->u_size, (float)rc->fbo_width, (float)rc->fbo_height);
	glUniform2f(rc->u_window_origin, (float)rc->shadow_px,
		(float)rc->shadow_px);
	glUniform2f(rc->u_window_size, (float)rc->window_pw,
		(float)rc->window_ph);
	/* the padding is constant (always the maximum gaussian shadow extent);
	 * whether a shadow is drawn at all follows focus: sigma is 0 for
	 * unfocused windows.  The shadow color is independent of the border. */
	glUniform1f(rc->u_shadow_sigma, shadow_sigma(rc->tl) * rc->scale);
	glUniform1f(rc->u_shadow_alpha, shadow_alpha());
	struct wlr_render_color shcol = shadow_color();
	glUniform4f(rc->u_shadow_color, shcol.r, shcol.g, shcol.b, shcol.a);
	glUniform1f(rc->u_radius, (float)CONFIG_ROUNDED_RADIUS * rc->scale);
	glUniform1f(rc->u_border_width, border_width(rc->tl) * rc->scale);
	struct wlr_render_color border = border_color(rc->server, rc->tl);
	glUniform4f(rc->u_border_color, border.r, border.g, border.b, border.a);
	struct wlr_render_color top_left, top_mid, top_right;
	border_top_colors(rc->tl, &top_left, &top_mid, &top_right);
	glUniform4f(rc->u_border_top_left, top_left.r, top_left.g, top_left.b,
		top_left.a);
	glUniform4f(rc->u_border_top_mid, top_mid.r, top_mid.g, top_mid.b,
		top_mid.a);
	glUniform4f(rc->u_border_top_right, top_right.r, top_right.g,
		top_right.b, top_right.a);
	glUniform1f(rc->u_border_gradient,
		border_gradient_width(rc->tl) * rc->scale);

	glBindBuffer(GL_ARRAY_BUFFER, rc->vbo);
	glEnableVertexAttribArray(rc->a_pos);
	glVertexAttribPointer(rc->a_pos, 2, GL_FLOAT, GL_FALSE, 0, NULL);

	if (partial) {
		/* per-rect: copy the freshly composited content into the texture
		 * (a pure GPU-side copy, no glReadPixels), then draw the quad under
		 * the same scissor.  Both copy and draw coordinates are buffer
		 * coordinates: the shader's y mapping is the identity between
		 * buffer rows and GL window rows. */
		glEnable(GL_SCISSOR_TEST);
		for (int i = 0; i < n_rects; i++) {
			const pixman_box32_t *b = &boxes[i];
			int x = b->x1, y = b->y1;
			int w = b->x2 - b->x1, h = b->y2 - b->y1;
			glScissor(x, y, w, h);
			glBindFramebuffer(GL_FRAMEBUFFER, content_fbo);
			/* both the texture offset and the framebuffer source use the
			 * rect's coordinates: the texture mirrors the FBO row for row */
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

	glDisableVertexAttribArray(rc->a_pos);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	/* the mask draw commands are ordered before the scene render in the same
	 * GL context, so the freshly written rounded FBO is visible when the
	 * scene samples it */

	rounded_end_gl(&saved);
	return true;
}

/* --- public API -------------------------------------------------------- */

/* the rounded node never intercepts pointer input: the (transparent) client
 * content above it is what receives clicks, so hit-testing is unchanged */
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
	/* Only hide the client content once there is a valid rounded FBO to
	 * show in its place.  Before the first successful publish (or while a
	 * re-render is in progress and the old FBO has been dropped) the raw
	 * content must stay visible: hiding it here would leave a fully
	 * transparent window. */
	if (rc->node->buffer == NULL) {
		return;
	}
	rounded_set_content_opacity(rc, 0.0f);
}

/* fade the whole on-screen window (animate.c).  Normally the only visible
 * window buffer is the rounded FBO node - the raw client content is hidden
 * at opacity 0 - so a fade animates just that node (border and shadow are
 * part of the same FBO and fade along).  Before the first FBO publish (or
 * with rounded corners disabled) the raw content is what the scene draws:
 * every scene buffer under the window tree (main surface + subsurfaces +
 * popups) is faded instead. */
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
	bool have_gl = !rc->failed && rc->gl_ready &&
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

void rounded_cache_dirty(struct toplevel *tl) {
	if (tl->rounded == NULL) {
		return;
	}
	tl->rounded->content_dirty = true;
	tl->rounded->mask_dirty = true;
	/* Invalidating the cache does not necessarily damage the scene: entering
	 * fullscreen from a maximized window whose box already equals the output
	 * (no layer-shell bar shrinking the work area) keeps the geometry
	 * unchanged, and a configure ack with no buffer change carries no damage
	 * either.  Nothing would then schedule a frame, so rounded_render_all
	 * would never re-render the FBO and the border (whose width/color depend
	 * on the fullscreen state) would keep its old appearance.  Schedule a
	 * frame like rounded_cache_dirty_mask does. */
	struct wlr_output *output = toplevel_output(tl->server, tl);
	if (output != NULL) {
		wlr_output_schedule_frame(output);
	}
}

/* content-only invalidation: surface commits that attached damage */
void rounded_cache_dirty_content(struct toplevel *tl) {
	if (tl->rounded != NULL) {
		tl->rounded->content_dirty = true;
	}
}

/* mask-only invalidation: border/shadow parameters changed (focus) while
 * the content is untouched; the cached content pass is reused.  A focus
 * change damages nothing in the scene by itself (content commits do, via
 * the scene surface's own damage), so explicitly schedule a frame on the
 * toplevel's output - the publish after the re-render then damages the
 * affected outputs for the repaint. */
void rounded_cache_dirty_mask(struct toplevel *tl) {
	if (tl->rounded == NULL) {
		return;
	}
	tl->rounded->mask_dirty = true;
	struct wlr_output *output = toplevel_output(tl->server, tl);
	if (output != NULL) {
		wlr_output_schedule_frame(output);
	}
}

/* main-surface commit: collect the commit's damage for a partial re-render.
 * Resizes and scale/transform/viewport-source changes carry no buffer
 * damage of their own, so they fall back to full-surface damage. */
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

	/* wlroots re-applies opacity 1.0 to the committed surface; re-hide it
	 * (only if a valid rounded FBO is already published) */
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
		/* geometry changed without buffer damage: cover the new surface
		 * area and the vacated old one (a shrink must clear the pixels
		 * beyond the new size) */
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

/* subsurface commit: collect damage (surface-local coordinates) plus the
 * old-position area when the subsurface moved or resized, so both the new
 * and the vacated region are re-rendered.  A subsurface that unmapped
 * (attach NULL) falls back to a full re-render: it is rare and its old
 * content spans an area the commit does not damage. */
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
		/* unmapped: everything it used to cover must be re-rendered; the
		 * geometry tracking restarts at zero for a future remap */
		ts->prev_x = cur_x;
		ts->prev_y = cur_y;
		ts->prev_w = 0;
		ts->prev_h = 0;
		rounded_cache_dirty(tl);
		return;
	}

	pixman_region32_t dmg;
	pixman_region32_init(&dmg);
	/* A commit can also reorder subsurfaces (place_above/below) without
	 * attaching any buffer damage, so always cover the surface's own area:
	 * re-compositing it re-applies the new stacking order. */
	pixman_region32_union_rect(&dmg, &dmg, 0, 0, cur_w, cur_h);

	if (cur_x != ts->prev_x || cur_y != ts->prev_y ||
			cur_w != ts->prev_w || cur_h != ts->prev_h) {
		/* vacated old area, in the surface's own coordinates (shifted by
		 * the move): it must be cleared and re-composited */
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

/* Keep showing the last published rounded FBO when a re-render fails
 * transiently, instead of revealing the raw (unrounded) content.  Only when
 * no rounded buffer was ever published do we fall back to raw content -
 * otherwise the window would be fully transparent. */
static void rounded_fallback(struct rounded_cache *rc) {
	if (rc->node->buffer == NULL) {
		rounded_set_content_opacity(rc, 1.0f);
	} else {
		/* re-hide in case a surface commit reset the opacity to 1.0 */
		rounded_cache_hide_content(rc->tl);
	}
}

/* --- damage collection and publishing ---------------------------------- */

/* map a surface-local damage region into FBO coordinates through the
 * buffer's destination box */
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
		int x1 = dst_box->x + (int)floorf(b->x1 * xs);
		int y1 = dst_box->y + (int)floorf(b->y1 * ys);
		int x2 = dst_box->x + (int)ceilf(b->x2 * xs);
		int y2 = dst_box->y + (int)ceilf(b->y2 * ys);
		pixman_region32_union_rect(dst, dst, x1, y1, x2 - x1, y2 - y1);
	}
}

/* find the subsurface bookkeeping entry for a surface in the toplevel's
 * subsurface tree */
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

	/* same filtering as the content pass: only the toplevel's own surface
	 * tree (surface + subsurfaces) with a buffer attached */
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

/* walk the content tree once to gather the accumulated surface damage into
 * rc->fbo_damage (FBO physical coordinates).  Runs before the content pass,
 * which needs the complete region up front for its clip. */
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

/* the per-surface damage caches are consumed once a render reflects their
 * content (full or partial).  A commit arriving mid-render adds fresh
 * damage and re-dirties, so the next frame picks it up. */
static void rounded_clear_damage_caches(struct rounded_cache *rc) {
	pixman_region32_clear(&rc->content_damage);
	struct toplevel_subsurface *ts;
	wl_list_for_each(ts, &rc->tl->subsurfaces, link) {
		pixman_region32_clear(&ts->damage);
	}
}

/* expand each damage rect by the border ring width (plus one AA pixel) so
 * border pixels that blend the changed content beneath them are re-masked
 * and republished too; the shadow ring never samples content, so it needs
 * no expansion.  The result is clipped to the FBO. */
static void rounded_expand_ring(struct rounded_cache *rc) {
	if (pixman_region32_empty(&rc->fbo_damage)) {
		return; /* empty means "full" upstream */
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

/* publish the freshly rendered rounded buffer to the scene node and keep
 * its position/dest size in sync with the window + shadow padding.
 * damage == NULL means the whole buffer changed.
 *
 * While the toplevel is running a maximize/restore zoom (tl->morph_active,
 * animate.c) the FBO is shown in the current morph box instead of its
 * natural geometry, so mid-animation re-renders never pop to the natural
 * size.  The FBO node is a child of the scene tree, so its position is
 * relative to the tree origin: offset it by the morph box origin minus the
 * tree origin. */
static void rounded_publish(struct rounded_cache *rc,
		const struct wlr_box *box, const pixman_region32_t *damage) {
	wlr_scene_buffer_set_buffer_with_damage(rc->node, rc->rounded_buf,
		damage);
	struct toplevel *tl = rc->tl;
	int shadow_i = (int)lroundf(rc->shadow_logical);
	int ox = box->x;
	int oy = box->y;
	int width = box->width;
	int height = box->height;
	if (tl->morph_active) {
		ox = tl->morph_x;
		oy = tl->morph_y;
		width = tl->morph_w;
		height = tl->morph_h;
	}
	int tree_x = tl->scene_tree != NULL ? tl->scene_tree->node.x : 0;
	int tree_y = tl->scene_tree != NULL ? tl->scene_tree->node.y : 0;
	wlr_scene_node_set_position(&rc->node->node,
		ox - tree_x - shadow_i, oy - tree_y - shadow_i);
	wlr_scene_buffer_set_dest_size(rc->node,
		width + 2 * shadow_i, height + 2 * shadow_i);
}

/* true while the window is running a maximize/restore zoom (animate.c): the
 * scene tree is anchored at the morph box origin and every publish lands in
 * the morph box */
bool rounded_morph_supported(struct toplevel *tl) {
	struct rounded_cache *rc = tl->rounded;
	return rc != NULL && !rc->failed && rc->node != NULL &&
		rc->node->buffer != NULL;
}

/* the rounded FBO currently holds content rendered at exactly width x
 * height (layout pixels): the maximize/restore zoom uses this to detect
 * that the client has committed the target size and the cache has been
 * re-rendered at it, so the zoom can start/continue without popping */
bool rounded_cache_size_ready(struct toplevel *tl, int width, int height) {
	struct rounded_cache *rc = tl->rounded;
	return rc != NULL && !rc->failed && rc->node != NULL &&
		rc->node->buffer != NULL &&
		rc->logical_width == width && rc->logical_height == height;
}

/* re-apply the current morph box to the scene node (position + dest size);
 * called every animation tick after tl->morph_* changed.  No-op unless a
 * morph is running. */
void rounded_cache_morph_apply(struct toplevel *tl) {
	struct rounded_cache *rc = tl->rounded;
	if (rc == NULL || rc->failed || rc->node == NULL ||
			!tl->morph_active) {
		return;
	}
	int shadow_i = (int)lroundf(rc->shadow_logical);
	int tree_x = tl->scene_tree != NULL ? tl->scene_tree->node.x : 0;
	int tree_y = tl->scene_tree != NULL ? tl->scene_tree->node.y : 0;
	wlr_scene_node_set_position(&rc->node->node,
		tl->morph_x - tree_x - shadow_i,
		tl->morph_y - tree_y - shadow_i);
	wlr_scene_buffer_set_dest_size(rc->node,
		tl->morph_w + 2 * shadow_i, tl->morph_h + 2 * shadow_i);
}

/* snapshot the main surface's direct subsurface stacking order so a later
 * commit can detect place_above/place_below restacks (which carry no buffer
 * damage).  The order is captured at publish time - the state the cached FBO
 * actually reflects - and compared at commit time. */
static void rounded_capture_subsurface_order(struct rounded_cache *rc) {
	struct wlr_xdg_surface *base = rc->tl->xdg_toplevel->base;
	/* keep the backing allocation; only reset the used length */
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

/* true if the main surface's direct subsurface stacking order differs from
 * the last published snapshot (below list first, then above) */
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

/* remember the main-surface geometry the published FBO reflects; commits
 * that resize the surface or change its viewport source compare against
 * this to detect changes that carry no buffer damage */
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

/* Render any dirty rounded caches.  Called from the output frame handler
 * before the scene is committed, so the scene always samples fresh FBOs. */
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
		toplevel_box(tl, &box);
		if (box.width <= 0 || box.height <= 0) {
			continue;
		}

		struct wlr_output *output = toplevel_output(server, tl);
		float scale = output != NULL ? output->scale : 1.0f;
		if (scale <= 0.0f) {
			scale = 1.0f;
		}
		/* the FBO always reserves the maximum shadow padding (derived from
		 * the gaussian sigma), so focus transitions never resize it and
		 * stay mask-only re-renders */
		float shadow_w = (float)shadow_padding();

		if (!rc->content_dirty && !rc->mask_dirty &&
				rc->logical_width == box.width &&
				rc->logical_height == box.height && rc->scale == scale &&
				rc->shadow_logical == shadow_w) {
			/* cache is fresh: wlroots' scene_surface re-applies opacity
			 * 1.0 on every surface commit (surface_reconfigure), so re-hide
			 * the content right before the scene renders.  A valid FBO is
			 * already published, so this never leaves the window
			 * transparent. */
			rounded_cache_hide_content(tl);
			continue;
		}

		/* mask-only re-render: border/shadow parameters changed (focus),
		 * content and buffers are untouched.  Requires a fully initialized
		 * GL program and an FBO published at the current size, so the
		 * cached content texture is complete. */
		if (rc->mask_dirty && !rc->content_dirty && rc->gl_ready &&
				rc->content_buf != NULL &&
				rc->node->buffer == rc->rounded_buf &&
				rc->logical_width == box.width &&
				rc->logical_height == box.height && rc->scale == scale) {
			rc->mask_dirty = false;
			if (!rounded_render_mask(rc, NULL)) {
				rc->mask_dirty = true;
				continue;
			}
			/* the whole border ring and shadow changed color */
			rounded_publish(rc, &box, NULL);
			rounded_note_surface_state(rc);
			rounded_cache_hide_content(tl);
			wlr_log(WLR_DEBUG, "rounded: published mask-only FBO for app_id "
				"\"%s\" (%dx%d logical)",
				tl->app_id != NULL ? tl->app_id : "?", box.width, box.height);
			continue;
		}

		if (!rc->gl_ready) {
			struct egl_context_state saved = {0};
			if (!rounded_begin_gl(server->renderer, &saved)) {
				rounded_set_content_opacity(rc, 1.0f);
				rc->failed = true;
				continue;
			}
			rc->gl_ready = rounded_gl_init(rc);
			rounded_end_gl(&saved);
			if (!rc->gl_ready) {
				wlr_log(WLR_ERROR, "rounded: GL init failed, disabling rounded corners");
				rounded_set_content_opacity(rc, 1.0f);
				rc->failed = true;
				continue;
			}
		}

		/* Clear dirty *before* compositing.  If the client commits again
		 * while we are rendering (between our scene sampling and the
		 * publish below), the commit handler sets dirty=true again and the
		 * next frame re-renders - the update is never silently dropped. */
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

		/* gather the accumulated surface damage into FBO coordinates, then
		 * consume the caches: whatever the caches held is reflected in this
		 * render (full or partial) */
		rounded_collect_damage(rc);
		rounded_clear_damage_caches(rc);

		/* partial re-render only over the damaged area.  Requires an FBO
		 * published at the current size (a fresh pair after a resize has
		 * uninitialized content and must be rendered in full) and a bounded
		 * rect count. */
		bool partial = rc->node->buffer == rc->rounded_buf &&
			!pixman_region32_empty(&rc->fbo_damage) &&
			pixman_region32_n_rects(&rc->fbo_damage) <=
				ROUNDED_MAX_DAMAGE_RECTS;
		if (!partial) {
			pixman_region32_clear(&rc->fbo_damage); /* empty = full */
		}

		if (!rounded_render_content(rc,
				partial ? &rc->fbo_damage : NULL)) {
			/* the client content could not be composited into the FBO (for
			 * example a DMA-BUF/texture that isn't ready yet); keep the
			 * previous rounded FBO and retry on the next frame */
			wlr_log(WLR_DEBUG, "rounded: content render failed for app_id "
				"\"%s\" (%dx%d), keeping previous FBO",
				tl->app_id != NULL ? tl->app_id : "?",
				box.width, box.height);
			rc->content_dirty = true;
			rounded_fallback(rc);
			continue;
		}

		/* border-ring pixels blend the content beneath them: expand the
		 * damage so they are re-masked (and republished) too */
		rounded_expand_ring(rc);

		if (!rounded_render_mask(rc, partial ? &rc->fbo_damage : NULL)) {
			wlr_log(WLR_ERROR, "rounded: offscreen render failed");
			rc->content_dirty = true;
			rounded_fallback(rc);
			continue;
		}

		/* publish the fresh result; NULL damage = whole buffer */
		rounded_publish(rc, &box, partial ? &rc->fbo_damage : NULL);
		rounded_note_surface_state(rc);
		/* NOTE: do not clear dirty here - it was cleared before the render,
		 * and any commit that arrived mid-render has already set it true
		 * again, so the next frame re-renders the newer content. */
		/* the FBO is now valid: hide the raw client content and show the
		 * rounded copy in its place */
		rounded_cache_hide_content(tl);
		wlr_log(WLR_DEBUG, "rounded: published FBO for app_id \"%s\" "
			"(%dx%d logical, %dx%d physical, scale %.2f)",
			tl->app_id != NULL ? tl->app_id : "?",
			box.width, box.height, rc->fbo_width, rc->fbo_height, rc->scale);
	}
}
