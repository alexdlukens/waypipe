/*
 * android_video.c — Android MediaCodec surface-mode decode pathway.
 *
 * Implements the frozen contract in android_video.h (design 2026-09-07,
 * sections 3-7, 10.2, 13). The decode pipeline is:
 *
 *   MediaCodec (surface mode) renders onto a SurfaceTexture
 *   -> av_mediacodec_release_buffer(buf, render=1)
 *   -> ASurfaceTexture_updateTexImage (frame becomes an OES texture)
 *   -> OES-external-sample blit, crop rect only,
 *      into the sfd's BGRA AHardwareBuffer through an EGLImage/FBO
 *   -> glFinish() (phase-1 cross-API sync, design section 6)
 *
 * A process-global pool (struct av_android_hw) keeps open MediaCodec
 * instances alive across sfd teardown/resize (design section 5). Every
 * failure latches a software-fallback flag instead of stalling: the drain
 * loop in video.c always releases every received MEDIACODEC buffer, so the
 * codec's buffer queue can never deadlock (design section 7 invariant).
 *
 * This TU is empty unless compiling for Android with video enabled; the
 * stubs below keep stray includes compilable on Linux.
 */

#include "android_video.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#if defined(__ANDROID__) && defined(HAS_VIDEO)

#include "shadow.h"
#include "latency.h"

/* DIAG (2026-08-02, black-screen investigation): decoder stderr does not
 * reach logcat on Android; write probes directly under gdwaypipe-diag.
 * Grep: gdwaypipe-diag. */
#include <android/log.h>
#define DIAG_ANDROID_LOG(...) \
	__android_log_print(ANDROID_LOG_INFO, "gdwaypipe-diag", __VA_ARGS__)

#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <android/hardware_buffer.h>
#include <android/surface_texture.h>
#include <android/surface_texture_jni.h>
#include <android/native_window.h>
#include <gbm.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/jni.h>
#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The process JavaVM captured by video.c's ladder (JNI_OnLoad /
 * gdwaypipe_arm_jvm / dlsym). video.c owns the definition; this TU only
 * reads it. Capture is retried until it succeeds, never latched. */
extern JavaVM *gdwaypipe_android_jvm;

/* Provided by thirdparty/gbm-android/gbm_android.c: the AHardwareBuffer
 * backing an AHB-shim gbm_bo. Borrowed pointer, valid until
 * gbm_bo_destroy(bo); NULL for non-AHB bos. */
struct AHardwareBuffer *gbm_android_bo_get_ahb(struct gbm_bo *bo);

/* The vendored ffmpeg registers these hardware decoders (scripts/
 * build-ffmpeg.sh: h264,vp9,h264_mediacodec,vp9_mediacodec). Redeclared
 * locally: sharing video.c's statics is prohibited (single-decoder-process
 * hygiene; the strings are identical). */
#define AV_ANDROID_H264_HW_DECODER "h264_mediacodec"
#define AV_ANDROID_VP9_HW_DECODER "vp9_mediacodec"

/* --- Data structures (design section 3) -------------------------------- */

#define AV_POOL_MAX 4

enum av_pool_state {
	AV_POOL_UNUSED,
	AV_POOL_OPENING,
	AV_POOL_IDLE,
	AV_POOL_ACTIVE,
	AV_POOL_FLUSHING,
	AV_POOL_DEAD,
};

struct av_pool_entry {
	enum av_pool_state state;
	enum video_coding_fmt fmt;      /* H264 | VP9 | AV1 */
	uint32_t size_bucket;           /* open-time coded dims (w<<16|h) */
	struct shadow_fd *owner;        /* non-NULL only while ACTIVE */
	/* ffmpeg */
	AVBufferRef *hwdev;             /* AV_HWDEVICE_TYPE_MEDIACODEC with
					 * hwctx->native_window = win */
	AVCodecContext *ctx;            /* surface-mode decoder */
	/* surface stack */
	jobject surf_tex_jobj;          /* JNI global ref */
	jobject surface_jobj;           /* JNI global ref */
	ASurfaceTexture *st;
	ANativeWindow *win;
	/* GL */
	GLuint oes_tex;                 /* ASurfaceTexture attach target */
	GLuint prog;                    /* shared OES program handle */
	float st_matrix[16];            /* refreshed per updateTexImage */
	/* bookkeeping */
	int64_t last_used_ms;
	int stream_serial;              /* bumped on flush; log correlation */
};

struct av_android_sfd {
	struct av_pool_entry *entry;    /* NULL while on SW fallback */
	EGLImage img;                   /* from sfd->dmabuf_bo's AHB */
	struct AHardwareBuffer *bound_ahb; /* AHB identity behind img */
	GLuint fbo;
	GLuint rbo;
	uint32_t target_w, target_h;    /* dmabuf_info dims (real surface) */
	bool sw_fallback;               /* latch: stay SW for this sfd */
};

struct av_android_hw {
	pthread_mutex_t lock;
	EGLDisplay dpy;
	EGLContext ctx;
	EGLConfig cfg;
	struct av_pool_entry entries[AV_POOL_MAX];
	int n_entries;                  /* slots not AV_POOL_UNUSED */
	bool egl_ok;                    /* latch: bootstrap failed -> SW */
	bool inited;
	/* shared GL program (single EGL context in the process) */
	GLuint prog;
	GLint loc_u_st;
	GLint loc_u_crop;
	GLint loc_u_tex;
	/* EGL/GL extension entry points resolved once at bootstrap */
	PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC
			eglGetNativeClientBufferANDROID;
	PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
	PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
	PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
			glEGLImageTargetRenderbufferStorageOES;
};

static struct av_android_hw g_hw = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.dpy = EGL_NO_DISPLAY,
	.ctx = EGL_NO_CONTEXT,
	.cfg = EGL_NO_CONFIG_KHR,
	.egl_ok = false,
	.inited = false,
	.prog = 0,
	.loc_u_st = -1,
	.loc_u_crop = -1,
	.loc_u_tex = -1,
};

/* --- Rollback switch (design section 13) ------------------------------- */

bool av_android_sw_forced(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *env = getenv("GDWAYPIPE_ANDROID_SW");
		cached = (env != NULL && env[0] != '\0' &&
				strcmp(env, "0") != 0) ? 1 : 0;
		if (cached) {
			DIAG_ANDROID_LOG("[decode-pool] GDWAYPIPE_ANDROID_SW=1: legacy CPU path forced\n");
		}
	}
	return cached != 0;
}

/* --- Small helpers ------------------------------------------------------ */

static const char *pool_fmt_name(enum video_coding_fmt fmt)
{
	switch (fmt) {
	case VIDEO_H264:
		return "h264";
	case VIDEO_VP9:
		return "vp9";
	case VIDEO_AV1:
	default:
		return "av1";
	}
}

static int64_t pool_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --- EGL bootstrap and context (design section 4.4) -------------------- */

/* Lazy, once per process. A failure latches g_hw.egl_ok = false and the
 * software ladder takes over for every subsequent frame. */
static bool egl_bootstrap(void)
{
	if (g_hw.egl_ok) {
		return true;
	}
	if (g_hw.inited) {
		return false; /* previous attempt failed; stay latched */
	}
	g_hw.inited = true;

	g_hw.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g_hw.dpy == EGL_NO_DISPLAY) {
		DIAG_ANDROID_LOG("[decode-surface] FAIL eglGetDisplay: EGL_NO_DISPLAY\n");
		return false;
	}
	if (!eglInitialize(g_hw.dpy, NULL, NULL)) {
		DIAG_ANDROID_LOG("[decode-surface] FAIL eglInitialize\n");
		return false;
	}

	const EGLint cfg_attribs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_NONE,
	};
	EGLConfig cfg = EGL_NO_CONFIG_KHR;
	EGLint n_cfg = 0;
	if (!eglChooseConfig(g_hw.dpy, cfg_attribs, &cfg, 1, &n_cfg) ||
			n_cfg < 1) {
		DIAG_ANDROID_LOG("[decode-surface] FAIL eglChooseConfig(ES3)\n");
		return false;
	}
	g_hw.cfg = cfg;

	/* Surfaceless preferred; pbuffer fallback. No sharing: this is the
	 * only GLES context in the process (Godot and gdwlroots are
	 * Vulkan-only on this platform). */
	EGLContext ctx = eglCreateContext(
			g_hw.dpy, cfg, EGL_NO_CONTEXT, NULL);
	if (ctx == EGL_NO_CONTEXT) {
		DIAG_ANDROID_LOG("[decode-surface] FAIL eglCreateContext\n");
		return false;
	}

	const EGLint pbuffer_attribs[] = {
		EGL_WIDTH, 1,
		EGL_HEIGHT, 1,
		EGL_NONE,
	};
	EGLSurface pb = eglCreatePbufferSurface(g_hw.dpy, cfg,
			pbuffer_attribs);
	bool have_pb = pb != EGL_NO_SURFACE;
	if (!eglMakeCurrent(g_hw.dpy, have_pb ? pb : EGL_NO_SURFACE,
			have_pb ? pb : EGL_NO_SURFACE, ctx)) {
		DIAG_ANDROID_LOG("[decode-surface] FAIL eglMakeCurrent (surfaceless=%d pbuffer=%d)\n",
				!have_pb, have_pb);
		eglDestroyContext(g_hw.dpy, ctx);
		return false;
	}
	g_hw.ctx = ctx;

	/* The blit target needs EGL_ANDROID_get_native_client_buffer and
	 * OES_EGL_image; without them the AHB can never be written. */
	g_hw.eglGetNativeClientBufferANDROID =
			(PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
			eglGetProcAddress("eglGetNativeClientBufferANDROID");
	g_hw.eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
			eglGetProcAddress("eglCreateImageKHR");
	g_hw.eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
			eglGetProcAddress("eglDestroyImageKHR");
	g_hw.glEGLImageTargetRenderbufferStorageOES =
			(PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)
			eglGetProcAddress(
			"glEGLImageTargetRenderbufferStorageOES");
	const char *gl_exts = (const char *)glGetString(GL_EXTENSIONS);
	const bool egl_image = gl_exts != NULL &&
			strstr(gl_exts, "GL_OES_EGL_image") != NULL;
	if (!g_hw.eglGetNativeClientBufferANDROID || !g_hw.eglCreateImageKHR ||
			!g_hw.eglDestroyImageKHR ||
			!g_hw.glEGLImageTargetRenderbufferStorageOES ||
			!egl_image) {
		DIAG_ANDROID_LOG("[decode-surface] FAIL EGLImage/AHB extensions missing (native_buffer=%d create_khr=%d oes_img=%d)\n",
				g_hw.eglGetNativeClientBufferANDROID != NULL,
				g_hw.eglCreateImageKHR != NULL,
				egl_image);
		return false;
	}

	g_hw.egl_ok = true;
	DIAG_ANDROID_LOG("[decode-surface] EGL ok dpy=%p ctx=%p surfaceless=%d\n",
			(void *)g_hw.dpy, (void *)g_hw.ctx, !have_pb);
	return true;
}

static bool egl_ensure_current(void)
{
	if (!g_hw.egl_ok) {
		return false;
	}
	if (eglGetCurrentContext() != g_hw.ctx) {
		if (!eglMakeCurrent(g_hw.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
				g_hw.ctx)) {
			DIAG_ANDROID_LOG("[decode-surface] FAIL eglMakeCurrent(rebind)\n");
			return false;
		}
	}
	return true;
}

/* --- OES blit program (design sections 3.2, 4.1) ----------------------- */

static const char *k_vertex_src =
	"attribute vec2 a_pos;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"	v_uv = a_pos;\n"
	"	gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char *k_fragment_src =
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"uniform samplerExternalOES u_tex;\n"
	"uniform mat4 u_st;\n"
	"uniform vec4 u_crop;\n" /* (u0, v0, du, dv) in OES texel coords */
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"	vec2 uv = u_crop.xy + v_uv * u_crop.zw;\n"
	"	gl_FragColor = texture2D(u_tex, (u_st * vec4(uv, 0.0, 1.0)).xy);\n"
	"}\n";

static void android_log_shader(GLuint shader)
{
	GLchar info[512];
	GLsizei len = 0;
	glGetShaderInfoLog(shader, (GLsizei)sizeof(info), &len, info);
	DIAG_ANDROID_LOG("[decode-surface] shader compile failed: %.*s\n",
			(int)len, info);
}

static bool gl_build_program(void)
{
	GLuint prog = glCreateProgram();
	if (prog == 0) {
		DIAG_ANDROID_LOG("[decode-surface] FAIL glCreateProgram\n");
		return false;
	}
	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vs, 1, &k_vertex_src, NULL);
	glCompileShader(vs);
	GLint ok = GL_FALSE;
	glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		android_log_shader(vs);
		glDeleteShader(vs);
		glDeleteProgram(prog);
		return false;
	}
	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fs, 1, &k_fragment_src, NULL);
	glCompileShader(fs);
	ok = GL_FALSE;
	glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		android_log_shader(fs);
		glDeleteShader(vs);
		glDeleteShader(fs);
		glDeleteProgram(prog);
		return false;
	}
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	/* The blit draws through attribute slot 0 explicitly. */
	glBindAttribLocation(prog, 0, "a_pos");
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
	ok = GL_FALSE;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		GLchar info[512];
		GLsizei len = 0;
		glGetProgramInfoLog(prog, (GLsizei)sizeof(info), &len, info);
		DIAG_ANDROID_LOG("[decode-surface] program link failed: %.*s\n",
				(int)len, info);
		glDeleteProgram(prog);
		return false;
	}
	g_hw.prog = prog;
	g_hw.loc_u_st = glGetUniformLocation(prog, "u_st");
	g_hw.loc_u_crop = glGetUniformLocation(prog, "u_crop");
	g_hw.loc_u_tex = glGetUniformLocation(prog, "u_tex");
	return true;
}

/* Per-frame blit geometry: a_pos is set before every draw (it lives on the
 * shared context, so re-pointing per draw is both correct and stateless).
 * Enabled once at program creation below. */
static const GLfloat k_quad[8] = {
	0.0f, 0.0f,
	1.0f, 0.0f,
	0.0f, 1.0f,
	1.0f, 1.0f,
};

/* --- JNI surface stack (design section 5.3 OPENING) -------------------- */

/* NewObject/FindClass failures on the attached thread leave a pending
 * exception that must be cleared before ANY further JNI or ffmpeg JNI call
 * (a pending exception turns every later call into an abort). */
static void jni_clear_exception(JNIEnv *env)
{
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionDescribe(env);
		(*env)->ExceptionClear(env);
	}
}

static bool open_surface_stack(struct av_pool_entry *entry)
{
	if (gdwaypipe_android_jvm == NULL) {
		DIAG_ANDROID_LOG("[decode-surface] FAIL no JavaVM (capture ladder has not armed yet)\n");
		return false;
	}
	JNIEnv *env = NULL;
	if ((*gdwaypipe_android_jvm)->GetEnv(gdwaypipe_android_jvm,
			(void **)&env, JNI_VERSION_1_6) != JNI_OK) {
		if ((*gdwaypipe_android_jvm)->AttachCurrentThread(
				gdwaypipe_android_jvm, &env, NULL) != JNI_OK) {
			DIAG_ANDROID_LOG("[decode-surface] FAIL AttachCurrentThread\n");
			return false;
		}
	}

	/* new SurfaceTexture(0): single-buffer-mode texture image stream.
	 * The surfaceless EGL context means there is no GL consumer yet;
	 * attachToGLContext binds our own OES texture afterwards. */
	jclass cls = (*env)->FindClass(env, "android/graphics/SurfaceTexture");
	if (cls == NULL) {
		jni_clear_exception(env);
		DIAG_ANDROID_LOG("[decode-surface] FAIL FindClass(SurfaceTexture)\n");
		return false;
	}
	jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(I)V");
	if (ctor == NULL) {
		jni_clear_exception(env);
		DIAG_ANDROID_LOG("[decode-surface] FAIL SurfaceTexture.<init>(I) lookup\n");
		return false;
	}
	jobject st_obj = (*env)->NewObject(env, cls, ctor, (jint)0);
	if (st_obj == NULL) {
		jni_clear_exception(env);
		DIAG_ANDROID_LOG("[decode-surface] FAIL SurfaceTexture(0) ctor\n");
		return false;
	}
	/* Both objects live across pool entries and outlive this JNI call:
	 * promote to global refs BEFORE any other code may use them. */
	jobject st_global = (*env)->NewGlobalRef(env, st_obj);
	(*env)->DeleteLocalRef(env, st_obj);
	if (st_global == NULL) {
		DIAG_ANDROID_LOG("[decode-surface] FAIL NewGlobalRef(SurfaceTexture)\n");
		return false;
	}

	jclass surf_cls = (*env)->FindClass(env, "android/view/Surface");
	if (surf_cls == NULL) {
		jni_clear_exception(env);
		DIAG_ANDROID_LOG("[decode-surface] FAIL FindClass(Surface)\n");
		goto fail_st;
	}
	jmethodID surf_ctor = (*env)->GetMethodID(env, surf_cls,
			"<init>", "(Landroid/graphics/SurfaceTexture;)V");
	if (surf_ctor == NULL) {
		jni_clear_exception(env);
		DIAG_ANDROID_LOG("[decode-surface] FAIL Surface.<init>(SurfaceTexture) lookup\n");
		goto fail_st;
	}
	jobject surf_obj = (*env)->NewObject(env, surf_cls, surf_ctor,
			st_global);
	if (surf_obj == NULL) {
		jni_clear_exception(env);
		DIAG_ANDROID_LOG("[decode-surface] FAIL Surface(SurfaceTexture) ctor\n");
		goto fail_st;
	}
	jobject surf_global = (*env)->NewGlobalRef(env, surf_obj);
	(*env)->DeleteLocalRef(env, surf_obj);
	if (surf_global == NULL) {
		DIAG_ANDROID_LOG("[decode-surface] FAIL NewGlobalRef(Surface)\n");
		goto fail_st;
	}

	/* NDK surface entry points (libandroid, API 26+; min API 32 here).
	 * On failure the global refs above must be released here: the entry
	 * does not own them yet (design section 5.5, released exactly
	 * once). */
	ASurfaceTexture *st = ASurfaceTexture_fromSurfaceTexture(env,
			st_global);
	if (st == NULL) {
		DIAG_ANDROID_LOG("[decode-surface] FAIL ASurfaceTexture_fromSurfaceTexture\n");
		goto fail_refs;
	}
	ANativeWindow *win = ASurfaceTexture_acquireANativeWindow(st);
	if (win == NULL) {
		ASurfaceTexture_release(st);
		DIAG_ANDROID_LOG("[decode-surface] FAIL ASurfaceTexture_acquireANativeWindow\n");
		goto fail_refs;
	}

	entry->surf_tex_jobj = st_global;
	entry->surface_jobj = surf_global;
	entry->st = st;
	entry->win = win;
	DIAG_ANDROID_LOG("[decode-surface] ok st=%p win=%p\n", (void *)st,
			(void *)win);
	return true;

fail_st:
	(*env)->DeleteGlobalRef(env, st_global);
	return false;

fail_refs:
	if (surf_global != NULL) {
		(*env)->DeleteGlobalRef(env, surf_global);
	}
	(*env)->DeleteGlobalRef(env, st_global);
	return false;
}

/* Design section 5.2 layer 2: open-time coded dims bucket (each dimension
 * rounded up to 128, packed w<<16|h). The pool only reuses an entry whose
 * bucket matches, so a decoder configured for another resolution is never
 * flushed into a stream the device codec cannot adapt to. */
static uint32_t pool_size_bucket(uint32_t width, uint32_t height)
{
	return ((width + 127u) & ~127u) << 16 | ((height + 127u) & ~127u);
}

/* --- Decoder context (design sections 1.2b, 5.3) ----------------------- */

/* Local re-implementation of video.c's get_decode_format: surface mode
 * requires the MEDIACODEC pixel format; nothing else satisfies this
 * decoder's hw_device_ctx. */
static enum AVPixelFormat av_android_get_format(
		AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
	(void)ctx;
	for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE;
			p++) {
		if (*p == AV_PIX_FMT_MEDIACODEC) {
			return AV_PIX_FMT_MEDIACODEC;
		}
	}
	return AV_PIX_FMT_NONE;
}

/* One MEDIACODEC hw device per entry, output window pre-attached: the
 * decoder renders onto the SurfaceTexture through hwctx->native_window
 * (hwcontext_mediacodec.h). No av_mediacodec_default_init / hwaccel_context
 * anywhere — pool release goes through avcodec_free_context, which unrefs
 * the device and releases the window reference ffmpeg took. */
static bool open_hw_device(struct av_pool_entry *entry)
{
	AVBufferRef *hwdev = av_hwdevice_ctx_alloc(
			AV_HWDEVICE_TYPE_MEDIACODEC);
	if (!hwdev) {
		DIAG_ANDROID_LOG("[hw-init] FAIL av_hwdevice_ctx_alloc(MEDIACODEC)\n");
		return false;
	}
	/* hwdev->data is the AVHWDeviceContext; the codec-specific struct
	 * hangs off its ->hwctx member. For MediaCodec that struct
	 * (MediaCodecDeviceContext) begins with the public
	 * AVMediaCodecDeviceContext, so the cast is ffmpeg's own pattern
	 * (hwcontext_mediacodec.c mc_device_init). A pre-set native_window
	 * makes device_init a no-op that keeps our window reference: it is
	 * released exactly once in teardown_entry, after the codec and
	 * device are freed. */
	AVHWDeviceContext *dev_ctx = (AVHWDeviceContext *)hwdev->data;
	AVMediaCodecDeviceContext *hwctx =
			(AVMediaCodecDeviceContext *)dev_ctx->hwctx;
	hwctx->native_window = entry->win;
	hwctx->create_window = 0;
	int err = av_hwdevice_ctx_init(hwdev);
	if (err < 0) {
		DIAG_ANDROID_LOG("[hw-init] FAIL av_hwdevice_ctx_init: %s\n",
				av_err2str(err));
		av_buffer_unref(&hwdev);
		return false;
	}
	entry->hwdev = hwdev;
	return true;
}

/* Opens the pool entry's AVCodecContext in surface mode. h264 needs
 * SPS/PPS extradata at avcodec_open2 (the stream carries parameter sets
 * in-band only); the caller passes the start-code-prefixed blob here for
 * the first open. ffmpeg owns the copy (it frees ctx->extradata in
 * ff_codec_close); the caller keeps ownership of its own buffer. */
static int open_decoder_ctx(struct av_pool_entry *entry,
		enum video_coding_fmt fmt, uint32_t width, uint32_t height,
		const uint8_t *h264_extradata, int h264_extradata_size)
{
	const char *name = fmt == VIDEO_H264 ?
			AV_ANDROID_H264_HW_DECODER : AV_ANDROID_VP9_HW_DECODER;
	const struct AVCodec *codec = avcodec_find_decoder_by_name(name);
	if (!codec) {
		DIAG_ANDROID_LOG("[decode-setup] FAIL decoder %s not registered in this ffmpeg build\n",
				name);
		return -1;
	}
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) {
		DIAG_ANDROID_LOG("[decode-setup] FAIL avcodec_alloc_context3(%s)\n",
				name);
		return -1;
	}
	ctx->delay = 0;
	ctx->width = (int)width;
	ctx->height = (int)height;
	if (fmt == VIDEO_H264 && h264_extradata && h264_extradata_size > 0) {
		ctx->extradata = av_malloc((size_t)h264_extradata_size +
				AV_INPUT_BUFFER_PADDING_SIZE);
		if (!ctx->extradata) {
			DIAG_ANDROID_LOG("[decode-setup] FAIL extradata alloc\n");
			avcodec_free_context(&ctx);
			return -1;
		}
		memcpy(ctx->extradata, h264_extradata,
				(size_t)h264_extradata_size);
		memset(ctx->extradata + h264_extradata_size, 0,
				AV_INPUT_BUFFER_PADDING_SIZE);
		ctx->extradata_size = h264_extradata_size;
	}
	ctx->hw_device_ctx = av_buffer_ref(entry->hwdev);
	if (!ctx->hw_device_ctx) {
		DIAG_ANDROID_LOG("[decode-setup] FAIL av_buffer_ref(hwdev)\n");
		avcodec_free_context(&ctx);
		return -1;
	}
	ctx->get_format = av_android_get_format;
	int err = avcodec_open2(ctx, codec, NULL);
	if (err < 0) {
		DIAG_ANDROID_LOG("[decode-setup] FAIL avcodec_open2(%s) err=%d (%s)\n",
				name, err, av_err2str(err));
		avcodec_free_context(&ctx);
		return err;
	}
	entry->ctx = ctx;
	entry->fmt = fmt;
	entry->size_bucket = pool_size_bucket(width, height);
	entry->stream_serial = 1;
	return 0;
}

/* --- Entry teardown (design section 5.5) ------------------------------- */

/* Full teardown: avcodec_free_context -> ff_mediacodec_dec_unref releases
 * the codec and the ffmpeg-side window reference; then our ASurfaceTexture
 * and ANativeWindow handles; the JNI global refs exactly once, last. */
static void teardown_entry(struct av_pool_entry *entry)
{
	if (entry->ctx) {
		avcodec_free_context(&entry->ctx);
	}
	if (entry->hwdev) {
		av_buffer_unref(&entry->hwdev);
	}
	if (entry->win) {
		ANativeWindow_release(entry->win);
		entry->win = NULL;
	}
	if (entry->st) {
		ASurfaceTexture_release(entry->st);
		entry->st = NULL;
	}
	if (gdwaypipe_android_jvm != NULL &&
			(entry->surf_tex_jobj != NULL ||
			entry->surface_jobj != NULL)) {
		JNIEnv *env = NULL;
		if ((*gdwaypipe_android_jvm)->GetEnv(gdwaypipe_android_jvm,
				(void **)&env,
				JNI_VERSION_1_6) == JNI_OK && env != NULL) {
			if (entry->surf_tex_jobj) {
				(*env)->DeleteGlobalRef(env,
						entry->surf_tex_jobj);
			}
			if (entry->surface_jobj) {
				(*env)->DeleteGlobalRef(env,
						entry->surface_jobj);
			}
		} else {
			/* Env attach failed at shutdown: leak two refs
			 * rather than risk a crash; the process is on its
			 * way out either way. */
			DIAG_ANDROID_LOG("[decode-pool] WARN JNI env unavailable at teardown; leaking global refs\n");
		}
	}
	entry->surf_tex_jobj = NULL;
	entry->surface_jobj = NULL;
	if (entry->oes_tex != 0 && eglGetCurrentContext() == g_hw.ctx) {
		glDeleteTextures(1, &entry->oes_tex);
		entry->oes_tex = 0;
	}
	entry->prog = 0;
	entry->owner = NULL;
	entry->fmt = VIDEO_H264;
	entry->stream_serial = 0;
	entry->last_used_ms = 0;
	entry->size_bucket = 0;
	entry->state = AV_POOL_UNUSED;
}

/* --- Per-sfd target (design section 4.3) ------------------------------- */

/* GL/EGL resources of an sfd's blit target. Caller: GL current, lock held.
 * The old image is destroyed AFTER the new one is bound by the rebuild. */
static void sfd_target_destroy_gl(struct av_android_sfd *as)
{
	if (as->rbo != 0) {
		glDeleteRenderbuffers(1, &as->rbo);
		as->rbo = 0;
	}
	if (as->fbo != 0) {
		glDeleteFramebuffers(1, &as->fbo);
		as->fbo = 0;
	}
	if (as->img) {
		g_hw.eglDestroyImageKHR(g_hw.dpy, as->img);
		as->img = NULL;
		as->bound_ahb = NULL;
	}
}

/* (Re)build the EGLImage/FBO from the current sfd->dmabuf_bo AHB.
 * Idempotent when the target is unchanged. Caller: GL current, lock held.
 * Returns false without touching the old target on failure. */
static bool sfd_target_rebuild(struct shadow_fd *sfd,
		struct av_android_sfd *as)
{
	/* Currency predicate: bound AHB identity AND dims. The AHB pointer is
	 * authoritative: a resized allocation can reuse the same dimensions
	 * while re-pointing dmabuf_bo (validation finding 2). */
	struct AHardwareBuffer *ahb = sfd->dmabuf_bo ?
			gbm_android_bo_get_ahb(sfd->dmabuf_bo) : NULL;
	if (as->img != NULL && as->bound_ahb == ahb &&
			as->target_w == sfd->dmabuf_info.width &&
			as->target_h == sfd->dmabuf_info.height) {
		return true; /* unchanged */
	}
	if (!ahb) {
		DIAG_ANDROID_LOG("[decode-blit] FAIL target rebuild RID=%d: no AHB behind dmabuf_bo=%p\n",
				sfd->remote_id, (void *)sfd->dmabuf_bo);
		return false;
	}
	EGLClientBuffer cbuf = g_hw.eglGetNativeClientBufferANDROID(ahb);
	if (!cbuf) {
		DIAG_ANDROID_LOG("[decode-blit] FAIL target rebuild RID=%d: eglGetNativeClientBufferANDROID\n",
				sfd->remote_id);
		return false;
	}
	const EGLint img_attribs[] = {
		EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
		EGL_NONE,
	};
	EGLImage img = g_hw.eglCreateImageKHR(g_hw.dpy, EGL_NO_CONTEXT,
			EGL_NATIVE_BUFFER_ANDROID, cbuf, img_attribs);
	if (img == EGL_NO_IMAGE_KHR) {
		DIAG_ANDROID_LOG("[decode-blit] FAIL target rebuild RID=%d: eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) 0x%x\n",
				sfd->remote_id, eglGetError());
		return false;
	}

	GLuint fbo = 0, rbo = 0;
	glGenFramebuffers(1, &fbo);
	glGenRenderbuffers(1, &rbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glBindRenderbuffer(GL_RENDERBUFFER, rbo);
	g_hw.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
			(GLeglImageOES)img);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_RENDERBUFFER, rbo);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		DIAG_ANDROID_LOG("[decode-blit] FAIL target rebuild RID=%d: FBO incomplete 0x%x\n",
				sfd->remote_id, status);
		glDeleteRenderbuffers(1, &rbo);
		glDeleteFramebuffers(1, &fbo);
		g_hw.eglDestroyImageKHR(g_hw.dpy, img);
		return false;
	}

	/* New target bound; only now destroy the old one. */
	sfd_target_destroy_gl(as);
	as->img = img;
	as->fbo = fbo;
	as->rbo = rbo;
	as->bound_ahb = ahb;
	as->target_w = sfd->dmabuf_info.width;
	as->target_h = sfd->dmabuf_info.height;
	return true;
}

/* --- Pool (design section 5) ------------------------------------------- */

/* Subsequent streams rebind an open entry with a flush only; MediaCodec
 * accepts in-band parameter sets once running, so extradata is needed
 * exactly once per codec (design section 5.4). */
static void entry_flush(struct av_pool_entry *entry)
{
	avcodec_flush_buffers(entry->ctx);
	entry->stream_serial++;
	entry->last_used_ms = pool_now_ms();
}

/* LRU over IDLE. Design section 5.2: at most 2 idle entries; extras are
 * evicted after a successful acquire. force=true also evicts an idle entry
 * below the cap when a setup needs the slot. */
static bool pool_evict_idle(bool force)
{
	struct av_pool_entry *lru = NULL;
	int n_idle = 0;
	for (int i = 0; i < AV_POOL_MAX; i++) {
		struct av_pool_entry *e = &g_hw.entries[i];
		if (e->state != AV_POOL_IDLE) {
			continue;
		}
		n_idle++;
		if (!lru || e->last_used_ms < lru->last_used_ms) {
			lru = e;
		}
	}
	if (n_idle == 0 || lru == NULL) {
		return false;
	}
	if (!force && n_idle <= 2) {
		return false;
	}
	DIAG_ANDROID_LOG("[decode-pool] evict fmt=%s RID=%d idle_ms=%" PRId64 " serial=%d\n",
			pool_fmt_name(lru->fmt),
			lru->owner != NULL ? lru->owner->remote_id : -1,
			pool_now_ms() - lru->last_used_ms,
			lru->stream_serial);
	teardown_entry(lru);
	g_hw.n_entries--;
	return true;
}

/* IDLE entry matching fmt AND size bucket, else UNUSED slot; *evicted set
 * when an idle entry was torn down to make room. Returns NULL when the
 * pool is full. The size bucket (design section 5.2 layer 2) keeps a
 * decoder configured for another resolution from being reused when the
 * device codec ignores adaptive playback. */
static struct av_pool_entry *pool_free_slot(enum video_coding_fmt fmt,
		uint32_t bucket, bool *evicted)
{
	struct av_pool_entry *free_slot = NULL;
	for (int i = 0; i < AV_POOL_MAX; i++) {
		struct av_pool_entry *e = &g_hw.entries[i];
		if (e->state == AV_POOL_IDLE && e->fmt == fmt &&
				e->size_bucket == bucket) {
			return e; /* cheapest: no open needed */
		}
		if (e->state == AV_POOL_UNUSED && !free_slot) {
			free_slot = e;
		}
	}
	if (free_slot) {
		g_hw.n_entries++;
		return free_slot;
	}
	if (!pool_evict_idle(true)) {
		return NULL;
	}
	*evicted = true;
	for (int i = 0; i < AV_POOL_MAX; i++) {
		if (g_hw.entries[i].state == AV_POOL_UNUSED) {
			g_hw.n_entries++;
			return &g_hw.entries[i];
		}
	}
	return NULL;
}

/* --- Public API -------------------------------------------------------- */

int av_android_pool_setup(struct shadow_fd *sfd, struct render_data *rd,
		const uint8_t *h264_extradata, int h264_extradata_size,
		struct AVCodecContext **out_ctx)
{
	if (!sfd || !out_ctx || (sfd->video_fmt != VIDEO_H264 &&
			sfd->video_fmt != VIDEO_VP9)) {
		return -1;
	}
	if (av_android_sw_forced()) {
		return -1;
	}
	if (sfd->video_android == NULL) {
		sfd->video_android = calloc(1, sizeof(struct av_android_sfd));
		if (!sfd->video_android) {
			DIAG_ANDROID_LOG("[decode-pool] FAIL sfd state alloc RID=%d\n",
					sfd->remote_id);
			return -1;
		}
	}
	struct av_android_sfd *as = sfd->video_android;
	(void)rd;

	pthread_mutex_lock(&g_hw.lock);

	/* Rebind of an sfd whose entry is still bound (duplicate setup):
	 * reuse it as-is. An entry already released to IDLE is picked up
	 * by the pool_free_slot path below. */
	if (as->entry != NULL && as->entry->owner == sfd &&
			as->entry->state == AV_POOL_ACTIVE) {
		struct av_pool_entry *entry = as->entry;
		entry->last_used_ms = pool_now_ms();
		DIAG_ANDROID_LOG("[decode-pool] hit RID=%d fmt=%s bucket=%ux%u serial=%d (bound)\n",
				sfd->remote_id, pool_fmt_name(entry->fmt),
				(entry->size_bucket >> 16) & 0xffffu,
				entry->size_bucket & 0xffffu,
				entry->stream_serial);
		if (!egl_ensure_current()) {
			entry->state = AV_POOL_DEAD;
			DIAG_ANDROID_LOG("[decode-pool] dead RID=%d: EGL context lost\n",
					sfd->remote_id);
			pthread_mutex_unlock(&g_hw.lock);
			return -1;
		}
		if (!sfd_target_rebuild(sfd, as)) {
			entry->state = AV_POOL_DEAD;
			as->sw_fallback = true;
			DIAG_ANDROID_LOG("[decode-blit] fail RID=%d (target rebuild)\n",
					sfd->remote_id);
			pthread_mutex_unlock(&g_hw.lock);
			return -1;
		}
		*out_ctx = entry->ctx;
		pthread_mutex_unlock(&g_hw.lock);
		return 0;
	}
	as->entry = NULL;

	if (as->sw_fallback) {
		/* Rung 4/6 latch: this sfd stays on the software ladder. */
		pthread_mutex_unlock(&g_hw.lock);
		return -1;
	}
	if (!egl_bootstrap()) {
		/* Rung 3: bootstrap fail latches egl_ok = false; every sfd
		 * runs software from here on. */
		pthread_mutex_unlock(&g_hw.lock);
		return -1;
	}
	/* Current before any teardown/LRU work below: teardown_entry and
	 * sfd_target_rebuild both delete GL objects. */
	if (!egl_ensure_current()) {
		pthread_mutex_unlock(&g_hw.lock);
		return -1;
	}

	bool evicted = false;
	uint32_t bucket = pool_size_bucket(sfd->dmabuf_info.width,
			sfd->dmabuf_info.height);
	struct av_pool_entry *entry = pool_free_slot(sfd->video_fmt, bucket,
			&evicted);
	if (!entry) {
		DIAG_ANDROID_LOG("[decode-pool] miss RID=%d fmt=%s bucket=%ux%u: pool full, no idle slot\n",
				sfd->remote_id,
				pool_fmt_name(sfd->video_fmt),
				(bucket >> 16) & 0xffffu, bucket & 0xffffu);
		pthread_mutex_unlock(&g_hw.lock);
		return -1;
	}

	if (entry->state == AV_POOL_IDLE) {
		/* Reuse of an open decoder for the same codec: flush instead
		 * of recreate (the entire point of the pool). */
		entry_flush(entry);
		entry->state = AV_POOL_ACTIVE;
		entry->owner = sfd;
		as->entry = entry;
		DIAG_ANDROID_LOG("[decode-pool] hit RID=%d fmt=%s bucket=%ux%u serial=%d\n",
				sfd->remote_id, pool_fmt_name(entry->fmt),
				(entry->size_bucket >> 16) & 0xffffu,
				entry->size_bucket & 0xffffu,
				entry->stream_serial);
	} else {
		/* OPENING: build the whole stack, then the decoder. */
		entry->state = AV_POOL_OPENING;
		entry->owner = NULL;
		entry->prog = 0;
		memset(entry->st_matrix, 0, sizeof(entry->st_matrix));
		entry->st_matrix[0] = 1.0f;
		entry->st_matrix[5] = 1.0f;
		entry->st_matrix[10] = 1.0f;
		entry->st_matrix[15] = 1.0f;

		DIAG_ANDROID_LOG("[decode-pool] miss RID=%d fmt=%s bucket=%ux%u: opening %dx%d%s\n",
				sfd->remote_id,
				pool_fmt_name(sfd->video_fmt),
				(bucket >> 16) & 0xffffu, bucket & 0xffffu,
				(int)sfd->dmabuf_info.width,
				(int)sfd->dmabuf_info.height,
				evicted ? " (evicted idle)" : "");

		if (!open_surface_stack(entry)) {
			DIAG_ANDROID_LOG("[decode-surface] fail RID=%d fmt=%s\n",
					sfd->remote_id,
					pool_fmt_name(sfd->video_fmt));
			teardown_entry(entry);
			g_hw.n_entries--;
			pthread_mutex_unlock(&g_hw.lock);
			return -1;
		}

		if (!egl_ensure_current()) {
			DIAG_ANDROID_LOG("[decode-pool] dead RID=%d: EGL context lost\n",
					sfd->remote_id);
			teardown_entry(entry);
			g_hw.n_entries--;
			pthread_mutex_unlock(&g_hw.lock);
			return -1;
		}
		if (g_hw.prog == 0 && !gl_build_program()) {
			teardown_entry(entry);
			g_hw.n_entries--;
			pthread_mutex_unlock(&g_hw.lock);
			return -1;
		}
		glGenTextures(1, &entry->oes_tex);
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, entry->oes_tex);
		glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER,
				GL_LINEAR);
		glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER,
				GL_LINEAR);
		glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S,
				GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T,
				GL_CLAMP_TO_EDGE);
		int aerr = ASurfaceTexture_attachToGLContext(entry->st,
				entry->oes_tex);
		if (aerr != 0) {
			DIAG_ANDROID_LOG("[decode-surface] FAIL attachToGLContext tex=%u err=%d\n",
					entry->oes_tex, aerr);
			teardown_entry(entry);
			g_hw.n_entries--;
			pthread_mutex_unlock(&g_hw.lock);
			return -1;
		}
		entry->prog = g_hw.prog;

		if (!open_hw_device(entry)) {
			DIAG_ANDROID_LOG("[decode-pool] dead RID=%d fmt=%s: hw device\n",
					sfd->remote_id,
					pool_fmt_name(sfd->video_fmt));
			teardown_entry(entry);
			g_hw.n_entries--;
			pthread_mutex_unlock(&g_hw.lock);
			return -1;
		}

		/* h264 without extradata cannot open here (design rung 5):
		 * the caller keeps running the deferred software decoder
		 * until the first SPS+PPS packet and retries. */
		int oerr = open_decoder_ctx(entry, sfd->video_fmt,
				sfd->dmabuf_info.width,
				sfd->dmabuf_info.height, h264_extradata,
				h264_extradata_size);
		if (oerr < 0) {
			/* Rung 4: open fail (e.g. tiny surfaces MediaCodec
			 * rejects) is per-sfd; other sfds keep hw. */
			DIAG_ANDROID_LOG("[decode-pool] dead RID=%d fmt=%s: decoder open\n",
					sfd->remote_id,
					pool_fmt_name(sfd->video_fmt));
			teardown_entry(entry);
			g_hw.n_entries--;
			pthread_mutex_unlock(&g_hw.lock);
			return oerr;
		}
		entry->state = AV_POOL_ACTIVE;
		entry->owner = sfd;
		as->entry = entry;
	}

	entry->last_used_ms = pool_now_ms();

	if (!sfd_target_rebuild(sfd, as)) {
		entry->state = AV_POOL_DEAD;
		as->sw_fallback = true;
		DIAG_ANDROID_LOG("[decode-blit] fail RID=%d (target rebuild)\n",
				sfd->remote_id);
		pthread_mutex_unlock(&g_hw.lock);
		return -1;
	}

	pool_evict_idle(false);
	*out_ctx = entry->ctx;
	pthread_mutex_unlock(&g_hw.lock);
	return 0;
}

void av_android_pool_release(struct shadow_fd *sfd, bool destroy_entry)
{
	if (!sfd || sfd->video_android == NULL) {
		return;
	}
	struct av_android_sfd *as = sfd->video_android;

	pthread_mutex_lock(&g_hw.lock);

	/* GL deletions below need the shared context; when it is gone
	 * (process teardown), leak the GL/EGL objects and still free the
	 * sfd state. */
	bool gl_ok = egl_ensure_current();
	struct av_pool_entry *entry = as->entry;
	if (entry != NULL && entry->owner == sfd) {
		if ((entry->state == AV_POOL_ACTIVE ||
				entry->state == AV_POOL_FLUSHING) &&
				(destroy_entry || !gl_ok ||
				entry->state == AV_POOL_DEAD)) {
			/* DEAD/LRU/forced path: full teardown, slot
			 * reusable (design section 5.3). */
			DIAG_ANDROID_LOG("[decode-pool] teardown RID=%d fmt=%s serial=%d destroy=%d\n",
					sfd->remote_id,
					pool_fmt_name(entry->fmt),
					entry->stream_serial,
					destroy_entry ? 1 : 0);
			teardown_entry(entry);
			g_hw.n_entries--;
		} else if (entry->state == AV_POOL_ACTIVE ||
				entry->state == AV_POOL_FLUSHING) {
			/* sfd teardown / stream end: flush + IDLE; the
			 * open MediaCodec instance survives for the next
			 * sfd or resize (the entire point of the pool). */
			entry_flush(entry);
			entry->state = AV_POOL_IDLE;
			entry->owner = NULL;
			DIAG_ANDROID_LOG("[decode-pool] flush RID=%d fmt=%s serial=%d (release)\n",
					sfd->remote_id,
					pool_fmt_name(entry->fmt),
					entry->stream_serial);
		} else if (entry->state == AV_POOL_DEAD) {
			teardown_entry(entry);
			g_hw.n_entries--;
		}
		as->entry = NULL;
	}
	as->entry = NULL;

	if (!gl_ok) {
		free(as);
		sfd->video_android = NULL;
		pthread_mutex_unlock(&g_hw.lock);
		return;
	}
	sfd_target_destroy_gl(as);
	free(as);
	sfd->video_android = NULL;
	pthread_mutex_unlock(&g_hw.lock);
}

void av_android_pool_flush(struct shadow_fd *sfd)
{
	if (!sfd || sfd->video_android == NULL) {
		return;
	}
	struct av_android_sfd *as = sfd->video_android;
	pthread_mutex_lock(&g_hw.lock);
	struct av_pool_entry *entry = as->entry;
	if (entry != NULL && entry->owner == sfd &&
			(entry->state == AV_POOL_ACTIVE ||
			entry->state == AV_POOL_IDLE)) {
		/* Transient FLUSHING: in-flight buffers are discarded; the
		 * serial bump makes ffmpeg release any stale output buffer
		 * with render=0 (mediacodecdec_common.c serial check). */
		enum av_pool_state prev = entry->state;
		entry->state = AV_POOL_FLUSHING;
		entry_flush(entry);
		entry->state = prev == AV_POOL_ACTIVE ? AV_POOL_ACTIVE
				: AV_POOL_IDLE;
		DIAG_ANDROID_LOG("[decode-pool] flush RID=%d fmt=%s serial=%d\n",
				sfd->remote_id, pool_fmt_name(entry->fmt),
				entry->stream_serial);
	}
	pthread_mutex_unlock(&g_hw.lock);
}

/* Target (re)build. Called from the same place that re-runs the dmabuf
 * allocation; the pool entry survives the resize (design section 4.3). */
void av_android_sfd_target_changed(struct shadow_fd *sfd)
{
	if (!sfd || sfd->video_android == NULL) {
		return;
	}
	struct av_android_sfd *as = sfd->video_android;
	pthread_mutex_lock(&g_hw.lock);
	if (as->entry == NULL || as->entry->owner != sfd ||
			as->entry->state != AV_POOL_ACTIVE) {
		pthread_mutex_unlock(&g_hw.lock);
		return;
	}
	if (!egl_ensure_current()) {
		as->entry->state = AV_POOL_DEAD;
		DIAG_ANDROID_LOG("[decode-pool] dead RID=%d: EGL context lost\n",
				sfd->remote_id);
		pthread_mutex_unlock(&g_hw.lock);
		return;
	}
	if (sfd_target_rebuild(sfd, as)) {
		DIAG_ANDROID_LOG("[decode-blit] target rebuilt RID=%d %ux%u\n",
				sfd->remote_id, as->target_w, as->target_h);
	} else {
		as->sw_fallback = true;
		DIAG_ANDROID_LOG("[decode-blit] fail RID=%d (target rebuild)\n",
				sfd->remote_id);
	}
	pthread_mutex_unlock(&g_hw.lock);
}

int av_android_blit_latest(struct shadow_fd *sfd, struct AVFrame *hw_frame,
		int64_t *lat_us)
{
	int64_t t0 = wp_lat_now_us();
	if (lat_us) {
		*lat_us = 0;
	}
	if (!sfd || !hw_frame || sfd->video_android == NULL) {
		return -1;
	}
	struct av_android_sfd *as = sfd->video_android;
	if (as->sw_fallback) {
		return -1;
	}

	pthread_mutex_lock(&g_hw.lock);
	struct av_pool_entry *entry = as->entry;
	if (entry == NULL || entry->owner != sfd ||
			entry->state != AV_POOL_ACTIVE) {
		pthread_mutex_unlock(&g_hw.lock);
		return -1;
	}
	/* data[3] carries the AVMediaCodecBuffer in surface mode
	 * (mediacodecdec_common.c mediacodec_wrap_hw_buffer). */
	AVMediaCodecBuffer *buf = (AVMediaCodecBuffer *)hw_frame->data[3];
	if (buf == NULL) {
		DIAG_ANDROID_LOG("[decode-blit] fail RID=%d: MEDIACODEC frame without buffer\n",
				sfd->remote_id);
		as->sw_fallback = true;
		entry->state = AV_POOL_DEAD;
		pthread_mutex_unlock(&g_hw.lock);
		if (lat_us) {
			*lat_us = wp_lat_now_us() - t0;
		}
		return -1;
	}

	/* Render onto the entry's SurfaceTexture. This is THE render path:
	 * releaseOutputBuffer(render=true) is the only way to make the
	 * decoded frame visible (n6.1.2 has no av_mediacodec_render_buffer).
	 * A failure here is entry-scoped: latch DEAD for the next release
	 * to tear down, and drop this sfd to software. The buffer is
	 * consumed either way — the drain loop never stalls. */
	int rerr = av_mediacodec_release_buffer(buf, 1);
	if (rerr < 0) {
		DIAG_ANDROID_LOG("[decode-blit] fail RID=%d: release_buffer(render=1) %d\n",
				sfd->remote_id, rerr);
		as->sw_fallback = true;
		entry->state = AV_POOL_DEAD;
		pthread_mutex_unlock(&g_hw.lock);
		if (lat_us) {
			*lat_us = wp_lat_now_us() - t0;
		}
		return -1;
	}

	/* Target currency: buffer identity AND dims are re-checked every
	 * frame inside sfd_target_rebuild (validation finding 2): a resize
	 * or bo swap between frames rebuilds even without a
	 * target_changed notification. */
	if (!egl_ensure_current()) {
		as->sw_fallback = true;
		DIAG_ANDROID_LOG("[decode-blit] fail RID=%d: EGL context lost\n",
				sfd->remote_id);
		pthread_mutex_unlock(&g_hw.lock);
		if (lat_us) {
			*lat_us = wp_lat_now_us() - t0;
		}
		return -1;
	}
	if (!sfd_target_rebuild(sfd, as)) {
		as->sw_fallback = true;
		DIAG_ANDROID_LOG("[decode-blit] fail RID=%d (target rebuild)\n",
				sfd->remote_id);
		pthread_mutex_unlock(&g_hw.lock);
		if (lat_us) {
			*lat_us = wp_lat_now_us() - t0;
		}
		return -1;
	}
	int uerr = ASurfaceTexture_updateTexImage(entry->st);
	if (uerr != 0) {
		/* Entry-scoped: the SurfaceTexture/GL link is broken for
		 * this entry; latch DEAD (teardown at next release) and
		 * demote this sfd (design rung 6). */
		as->sw_fallback = true;
		entry->state = AV_POOL_DEAD;
		DIAG_ANDROID_LOG("[decode-blit] fail RID=%d: updateTexImage %d\n",
				sfd->remote_id, uerr);
		pthread_mutex_unlock(&g_hw.lock);
		if (lat_us) {
			*lat_us = wp_lat_now_us() - t0;
		}
		return -1;
	}
	ASurfaceTexture_getTransformMatrix(entry->st, entry->st_matrix);

	/* Crop policy (design section 4.2): the remote encoder pads coded
	 * frames to 16 px; the dmabuf target is the real surface size.
	 * 1. frame->crop_* when the bitstream carries crop.
	 * 2. else min(decoded, target). */
	uint32_t tw = as->target_w, th = as->target_h;
	int fw = hw_frame->width, fh = hw_frame->height;
	int cx = (int)hw_frame->crop_left;
	int cy = (int)hw_frame->crop_top;
	int cr = (int)hw_frame->crop_right;
	int cb = (int)hw_frame->crop_bottom;
	int src_w, src_h, src_x, src_y;
	if (cx != 0 || cy != 0 || cr != 0 || cb != 0) {
		src_x = cx;
		src_y = cy;
		src_w = fw - cx - cr;
		src_h = fh - cy - cb;
	} else {
		src_x = 0;
		src_y = 0;
		src_w = fw < (int)tw ? fw : (int)tw;
		src_h = fh < (int)th ? fh : (int)th;
	}
	/* Design rung 7: drop guards (no ladder demotion). The buffer is
	 * already released (render=1), so the codec keeps flowing. */
	if (src_w <= 0 || src_h <= 0 || fw < (int)tw || fh < (int)th) {
		wp_error("Decoded frame %dx%d (crop %dx%d@%d,%d) cannot fill surface %ux%u for RID=%d; dropping",
				fw, fh, src_w, src_h, src_x, src_y, tw, th,
				sfd->remote_id);
		DIAG_ANDROID_LOG("[decode-blit] drop RID=%d decoded=%dx%d surface=%ux%u\n",
				sfd->remote_id, fw, fh, tw, th);
		pthread_mutex_unlock(&g_hw.lock);
		if (lat_us) {
			*lat_us = wp_lat_now_us() - t0;
		}
		return 0;
	}

	/* OES blit: sample the OES texture at the crop rect (mapped through
	 * the SurfaceTexture transform matrix) and emit the full
	 * dmabuf_info rect (0,0)-(1,1) into the FBO. */
	glBindFramebuffer(GL_FRAMEBUFFER, as->fbo);
	glViewport(0, 0, (GLsizei)tw, (GLsizei)th);
	glUseProgram(g_hw.prog);
	glUniformMatrix4fv(g_hw.loc_u_st, 1, GL_FALSE, entry->st_matrix);
	glUniform4f(g_hw.loc_u_crop,
			(GLfloat)src_x / (GLfloat)fw,
			(GLfloat)src_y / (GLfloat)fh,
			(GLfloat)src_w / (GLfloat)fw,
			(GLfloat)src_h / (GLfloat)fh);
	glUniform1i(g_hw.loc_u_tex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, entry->oes_tex);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, k_quad);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	/* Phase-1 sync (design section 6): the compositor samples the same
	 * AHB through Vulkan; cross-API visibility needs glFinish before
	 * the frame is signaled. */
	glFinish();
	entry->last_used_ms = pool_now_ms();
	pthread_mutex_unlock(&g_hw.lock);
	if (lat_us) {
		*lat_us = wp_lat_now_us() - t0;
	}
	return 0;
}

#else /* !__ANDROID__ || !HAS_VIDEO */

/* Never called on Linux (call sites are guarded); the definitions keep
 * stray includes compiling. */
int av_android_pool_setup(struct shadow_fd *sfd, struct render_data *rd,
		const uint8_t *h264_extradata, int h264_extradata_size,
		struct AVCodecContext **out_ctx)
{
	(void)sfd;
	(void)rd;
	(void)h264_extradata;
	(void)h264_extradata_size;
	(void)out_ctx;
	return -1;
}

void av_android_pool_release(struct shadow_fd *sfd, bool destroy_entry)
{
	(void)sfd;
	(void)destroy_entry;
}

void av_android_pool_flush(struct shadow_fd *sfd)
{
	(void)sfd;
}

int av_android_blit_latest(struct shadow_fd *sfd, struct AVFrame *hw_frame,
		int64_t *lat_us)
{
	(void)sfd;
	(void)hw_frame;
	(void)lat_us;
	return -1;
}

void av_android_sfd_target_changed(struct shadow_fd *sfd)
{
	(void)sfd;
}

#endif /* __ANDROID__ && HAS_VIDEO */
