/*
 * Copyright © 2019 Manuel Stoeckl
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "shadow.h"
#include "latency.h"
/* Android MediaCodec surface-mode pool API (android_video.c, design
 * 2026-09-07 §3.4). The header is fully self-guarded: it preprocesses
 * to nothing unless __ANDROID__ && HAS_VIDEO. */
#include "android_video.h"

/* DIAG (2026-08-02, black-screen investigation): decoder stderr does not
 * reach logcat on Android; write probes directly under gdwaypipe-diag.
 * Grep: gdwaypipe-diag. */
#ifdef __ANDROID__
#include <android/log.h>
#define DIAG_VIDEO_LOG(...) \
	__android_log_print(ANDROID_LOG_INFO, "gdwaypipe-diag", __VA_ARGS__)
#else
#include <stdio.h>
#include <time.h>
#define DIAG_VIDEO_LOG(...) fprintf(stderr, "[gdwaypipe-diag] " __VA_ARGS__)
#endif

#if defined(__ANDROID__) && defined(HAS_VIDEO)
/* Hardware decode gate #2 (Android): ffmpeg's MediaCodec backend needs the
 * process JavaVM. Godot loads GDExtension .so files with plain dlopen
 * (OS_Android::open_dynamic_library), so ART never runs JNI_OnLoad for us,
 * and the GDScript bootstrap is dead on this engine: 4.8.dev exposes the
 * JavaClassWrapper singleton without any bound methods (no callable
 * wrap()/get_exception() from GDScript or Object::call).
 *
 * Capture order, first resolver wins (all cheap, non-crashable calls):
 *   1. dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs") — works only if the
 *      app linker namespace has libjvm/libart symbols in its global
 *      scope. Verified NOT the case on the Quest 3 runtime (2026-09-07:
 *      "JNI_GetCreatedJavaVMs unresolved").
 *   2. dlopen("libjvm.so"/"libart.so", RTLD_NOW|RTLD_NOLOAD) + dlsym —
 *      same-namespace visibility; kept for runtimes that do share the
 *      ART libs.
 *   3. The JvmArm Godot Android plugin (m4gr3d
 *      GDExtension-Android-Plugin-Template pattern; panelspace
 *      android-plugins/jvm-arm): ART loads the shim via
 *      System.loadLibrary, the shim's own JNI_OnLoad receives the VM
 *      legally, and the shim then dlopen()s this exact library file and
 *      calls the exported gdwaypipe_arm_jvm(vm) below. Our own
 *      JNI_OnLoad export also catches any direct System.load of this
 *      file. This is the rung that works on namespace-restricted
 *      runtimes.
 *
 * (A former rung 3 — a manual .dynsym scan of the libart mapping via
 * dl_iterate_phdr — was REMOVED 2026-09-07 after it SIGSEGV'd on device:
 * .dynamic pointer semantics differ per runtime (glibc pre-relocates
 * them, bionic does not) and ARM64 system libs carry MTE-tagged
 * pointers; hand-parsing them from app code is untestable remotely and
 * not worth the tombstones. Do not resurrect it.)
 *
 * The VM pointer is a process singleton; the first successful capture
 * serves every decoder thread. Capture is retried per hw init and never
 * latches, so the plugin arming later in startup is picked up by the
 * next session. */
#include <dlfcn.h>
#include <jni.h>

/* Not static: android_video.c consumes the captured VM via an extern
 * declaration (SurfaceTexture ctor needs JNIEnv). */
JavaVM *gdwaypipe_android_jvm = NULL;

typedef jint (*get_created_vms_fn)(JavaVM **, jsize, jsize *);

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
	(void)reserved;
	gdwaypipe_android_jvm = vm;
	DIAG_VIDEO_LOG("[hw-init] JNI_OnLoad ran; JavaVM=%p captured\n",
			(void *) vm);
	return JNI_VERSION_1_6;
}

/* Entry point for the panelspace JvmArm Godot Android plugin shim
 * (android-plugins/jvm-arm): its own JNI_OnLoad captured the JavaVM and it
 * dlopen()s this exact library file to call this symbol. Explicit C
 * export — visibility("default") survives -fvisibility=hidden builds; the
 * RTLD_NOW undefined-symbol guard only cares about imports. */
__attribute__((visibility("default")))
void gdwaypipe_arm_jvm(void *vm)
{
	if (vm == NULL) {
		return;
	}
	gdwaypipe_android_jvm = (JavaVM *) vm;
	DIAG_VIDEO_LOG("[hw-init] JavaVM=%p armed by JvmArm plugin (gdwaypipe_arm_jvm)\n",
			vm);
}

static void gdwaypipe_capture_jvm(void)
{
	get_created_vms_fn get_created_vms =
			(get_created_vms_fn) dlsym(RTLD_DEFAULT,
					"JNI_GetCreatedJavaVMs");
	if (get_created_vms == NULL) {
		void *lib = dlopen("libjvm.so", RTLD_NOW | RTLD_NOLOAD);
		if (lib != NULL) {
			get_created_vms = (get_created_vms_fn) dlsym(lib,
					"JNI_GetCreatedJavaVMs");
		}
	}
	if (get_created_vms == NULL) {
		void *lib = dlopen("libart.so", RTLD_NOW | RTLD_NOLOAD);
		if (lib != NULL) {
			get_created_vms = (get_created_vms_fn) dlsym(lib,
					"JNI_GetCreatedJavaVMs");
		}
	}
	if (get_created_vms == NULL) {
		DIAG_VIDEO_LOG("[hw-init] FAIL JNI_GetCreatedJavaVMs unresolved (namespace-restricted runtime); awaiting JvmArm plugin arm\n");
		return;
	}
	JavaVM *vms[1];
	jsize n_created = 0;
	if (get_created_vms(vms, 1, &n_created) != JNI_OK ||
			n_created < 1 || vms[0] == NULL) {
		DIAG_VIDEO_LOG("[hw-init] FAIL JNI_GetCreatedJavaVMs returned no VM; awaiting JvmArm plugin\n");
		return;
	}
	gdwaypipe_android_jvm = vms[0];
	DIAG_VIDEO_LOG("[hw-init] JavaVM=%p captured via JNI_GetCreatedJavaVMs\n",
			(void *) vms[0]);
}
#endif

#if !defined(HAS_VIDEO) || !defined(HAS_DMABUF)

void setup_video_logging(void) {}
bool video_supports_dmabuf_format(uint32_t format, uint64_t modifier)
{
	(void)format;
	(void)modifier;
	return false;
}
bool video_supports_shm_format(uint32_t format)
{
	(void)format;
	return false;
}
bool video_supports_coding_format(enum video_coding_fmt fmt)
{
	(void)fmt;
	return false;
}
void cleanup_hwcontext(struct render_data *rd) { (void)rd; }
void destroy_video_data(struct shadow_fd *sfd) { (void)sfd; }
int setup_video_encode(
		struct shadow_fd *sfd, struct render_data *rd, int nthreads)
{
	(void)sfd;
	(void)rd;
	(void)nthreads;
	return -1;
}
int setup_video_decode(struct shadow_fd *sfd, struct render_data *rd)
{
	(void)sfd;
	(void)rd;
	return -1;
}
void collect_video_from_mirror(
		struct shadow_fd *sfd, struct transfer_queue *transfers)
{
	(void)sfd;
	(void)transfers;
}
void apply_video_packet(struct shadow_fd *sfd, struct render_data *rd,
		const struct bytebuf *data)
{
	(void)rd;
	(void)sfd;
	(void)data;
}

#else /* HAS_VIDEO */

#include <libavcodec/avcodec.h>
#ifdef __ANDROID__
#include <libavcodec/jni.h>
/* AVMediaCodecBuffer + av_mediacodec_release_buffer (surface-mode
 * drain, design §4.1). */
#include <libavcodec/mediacodec.h>
#endif
#include <libavutil/display.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <unistd.h>

#ifdef HAS_VAAPI
#include <libavutil/hwcontext_vaapi.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#endif

/* these are equivalent to the GBM formats */
#include <libdrm/drm_fourcc.h>

#ifdef __ANDROID__
/* Android GBM surface: the AHardwareBuffer-backed shim
 * (thirdparty/gbm-android) resolves <gbm.h> and restricts which DRM
 * fourccs gbm_bo_create can allocate (drm_to_ahb_format).
 * gbm_android_format_supported() exposes that exact set to the capability
 * predicate below, so only mappable formats are advertised to peers. */
#include <gbm.h>
/* Android hardware decoders: ffmpeg's MediaCodec wrappers. They are
 * hardware-only (no software fallback inside the decoder), which is why
 * setup_video_decode re-looks-up the native decoder for the SW retry. */
#define VIDEO_H264_HW_DECODER "h264_mediacodec"
#define VIDEO_VP9_HW_DECODER "vp9_mediacodec"
#endif

#define VIDEO_H264_HW_ENCODER "h264_vaapi"
#define VIDEO_H264_SW_ENCODER "libx264"
#define VIDEO_H264_DECODER "h264"

#define VIDEO_VP9_HW_ENCODER "vp9_vaapi"
#define VIDEO_VP9_SW_ENCODER "libvpx-vp9"
#define VIDEO_VP9_DECODER "vp9"

/* librav1e currently is not sufficient as its low-latency mode doesn't
 * appear to entirely turn off lookahead, and a few frames of latency
 * are unavoidable; this may be fixed in the future.
 *
 * libsvtav1 -- might work, if suitable controls for zero latency can be found
 *
 * libaom-av1 -- works, but may be slower than the other options */
// #define VIDEO_AV1_SW_ENCODER "libsvtav1"
#define VIDEO_AV1_SW_ENCODER "libaom-av1"
#define VIDEO_AV1_DECODER "libdav1d"

static enum AVPixelFormat drm_to_av(uint32_t format)
{
	/* The avpixel formats are specified with reversed endianness relative
	 * to DRM formats */
	switch (format) {
	case 0:
		return AV_PIX_FMT_BGR0;

	case DRM_FORMAT_C8:
		/* indexed */
		return AV_PIX_FMT_NONE;

	case DRM_FORMAT_R8:
		return AV_PIX_FMT_GRAY8;

	case DRM_FORMAT_RGB565:
		return AV_PIX_FMT_RGB565LE;

	/* there really isn't a matching format, because no fast video
	 * codec supports alpha. Expect unusual error patterns */
	case DRM_FORMAT_GR88:
		return AV_PIX_FMT_YUYV422;

	case DRM_FORMAT_RGB888:
		return AV_PIX_FMT_BGR24;
	case DRM_FORMAT_BGR888:
		return AV_PIX_FMT_RGB24;

	case DRM_FORMAT_XRGB8888:
		return AV_PIX_FMT_BGR0;
	case DRM_FORMAT_XBGR8888:
		return AV_PIX_FMT_RGB0;
	case DRM_FORMAT_RGBX8888:
		return AV_PIX_FMT_0BGR;
	case DRM_FORMAT_BGRX8888:
		return AV_PIX_FMT_0RGB;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 7, 100)
	/* While X2RGB10LE was available earlier than X2BGR10LE, conversions to
	 * X2RGB10LE were broken until just before X2BGR10LE was added */
	case DRM_FORMAT_XRGB2101010:
		return AV_PIX_FMT_X2RGB10LE;
	case DRM_FORMAT_XBGR2101010:
		return AV_PIX_FMT_X2BGR10LE;
#endif

	case DRM_FORMAT_NV12:
		return AV_PIX_FMT_NV12;
	case DRM_FORMAT_NV21:
		return AV_PIX_FMT_NV21;
	case DRM_FORMAT_YVU410:
	case DRM_FORMAT_YUV410:
		return AV_PIX_FMT_YUV410P;
	case DRM_FORMAT_YVU411:
	case DRM_FORMAT_YUV411:
		return AV_PIX_FMT_YUV411P;
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YUV420:
		return AV_PIX_FMT_YUV420P;
	case DRM_FORMAT_YVU422:
	case DRM_FORMAT_YUV422:
		return AV_PIX_FMT_YUV422P;
	case DRM_FORMAT_YVU444:
	case DRM_FORMAT_YUV444:
		return AV_PIX_FMT_YUV444P;

	case DRM_FORMAT_YUYV:
		return AV_PIX_FMT_NONE;
	case DRM_FORMAT_YVYU:
		return AV_PIX_FMT_UYVY422;
	case DRM_FORMAT_UYVY:
		return AV_PIX_FMT_YVYU422;
	case DRM_FORMAT_VYUY:
		return AV_PIX_FMT_YUYV422;

	default:
		return AV_PIX_FMT_NONE;
	}
}
static bool needs_vu_flip(uint32_t drm_format)
{
	switch (drm_format) {
	case DRM_FORMAT_YVU410:
	case DRM_FORMAT_YVU411:
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YVU422:
	case DRM_FORMAT_YVU444:
		return true;
	}
	return false;
}

bool video_supports_dmabuf_format(uint32_t format, uint64_t modifier)
{
	/* cannot handle CCS modifiers at the moment due to extra 'plane' issues
	 */
	if (modifier == fourcc_mod_code(INTEL, 4) /* Y_TILED_CCS */ ||
			modifier == fourcc_mod_code(INTEL, 5) /* Yf_TILED_CCS */ || modifier == fourcc_mod_code(INTEL, 6) /* Y_TILED_GEN12_RC_CCS */ || modifier == fourcc_mod_code(INTEL, 7) /* Y_TILED_GEN12_MC_CCS */ || modifier == fourcc_mod_code(INTEL, 8) /* Y_TILED_GEN12_RC_CCS_CC */) {
		return false;
	}
#ifdef __ANDROID__
	/* The GBM shim allocates only the formats drm_to_ahb_format maps
	 * (gbm_android_format_supported). Advertising the full Linux set
	 * would let a peer pick a format make_dmabuf() cannot create, and
	 * video decode would never start. */
	return gbm_android_format_supported(format);
#else
	return drm_to_av(format) != AV_PIX_FMT_NONE;
#endif
}
bool video_supports_shm_format(uint32_t format)
{
	if (format == 0) {
		return true;
	}
	return video_supports_dmabuf_format(format, 0);
}

static const struct AVCodec *get_video_sw_encoder(
		enum video_coding_fmt fmt, bool print_error)
{
	const struct AVCodec *codec = NULL;
	switch (fmt) {
	case VIDEO_H264:
		codec = avcodec_find_encoder_by_name(VIDEO_H264_SW_ENCODER);
		if (!codec && print_error) {
			wp_error("Failed to find encoder \"%s\"",
					VIDEO_H264_SW_ENCODER);
		}
		return codec;
	case VIDEO_VP9:
		codec = avcodec_find_encoder_by_name(VIDEO_VP9_SW_ENCODER);
		if (!codec && print_error) {
			wp_error("Failed to find encoder \"%s\"",
					VIDEO_VP9_SW_ENCODER);
		}
		return codec;
	case VIDEO_AV1:
		codec = avcodec_find_encoder_by_name(VIDEO_AV1_SW_ENCODER);
		if (!codec && print_error) {
			wp_error("Failed to find encoder \"%s\"",
					VIDEO_AV1_SW_ENCODER);
		}
		return codec;
	default:
		return NULL;
	}
}

static const struct AVCodec *get_video_hw_encoder(
		enum video_coding_fmt fmt, bool print_error)
{
	const struct AVCodec *codec = NULL;
	switch (fmt) {
	case VIDEO_H264:
		codec = avcodec_find_encoder_by_name(VIDEO_H264_HW_ENCODER);
		if (!codec && print_error) {
			wp_error("Failed to find encoder \"%s\"",
					VIDEO_H264_HW_ENCODER);
		}
		return codec;
	case VIDEO_VP9:
		codec = avcodec_find_encoder_by_name(VIDEO_VP9_HW_ENCODER);
		if (!codec && print_error) {
			wp_error("Failed to find encoder \"%s\"",
					VIDEO_VP9_HW_ENCODER);
		}
		return codec;
	case VIDEO_AV1:
		return NULL;
	default:
		return NULL;
	}
}

static const struct AVCodec *get_video_decoder(
		enum video_coding_fmt fmt, bool print_error, bool prefer_hw)
{
	const struct AVCodec *codec = NULL;
	const char *name = NULL;
	switch (fmt) {
	case VIDEO_H264:
#ifdef __ANDROID__
		name = prefer_hw ? VIDEO_H264_HW_DECODER : VIDEO_H264_DECODER;
#else
		(void)prefer_hw;
		name = VIDEO_H264_DECODER;
#endif
		codec = avcodec_find_decoder_by_name(name);
		if (!codec && print_error) {
			wp_error("Failed to find decoder \"%s\"", name);
		}
		return codec;
	case VIDEO_VP9:
#ifdef __ANDROID__
		name = prefer_hw ? VIDEO_VP9_HW_DECODER : VIDEO_VP9_DECODER;
#else
		name = VIDEO_VP9_DECODER;
#endif
		codec = avcodec_find_decoder_by_name(name);
		if (!codec && print_error) {
			wp_error("Failed to find decoder \"%s\"", name);
		}
		return codec;
	case VIDEO_AV1:
		codec = avcodec_find_decoder_by_name(VIDEO_AV1_DECODER);
		if (!codec && print_error) {
			wp_error("Failed to find decoder \"%s\"",
					VIDEO_AV1_DECODER);
		}
		return codec;
	default:
		return NULL;
	}
}

bool video_supports_coding_format(enum video_coding_fmt fmt)
{
	return get_video_sw_encoder(fmt, false) &&
	       get_video_decoder(fmt, false, false);
}

static void video_log_callback(
		void *aux, int level, const char *fmt, va_list args)
{
	(void)aux;
	enum log_level wp_level =
			(level <= AV_LOG_WARNING) ? WP_ERROR : WP_DEBUG;
	log_handler_func_t fn = log_funcs[wp_level];
	char buf[1024];
	int len = vsnprintf(buf, 1023, fmt, args);
	while (len > 1 && buf[len - 1] == '\n') {
		buf[len - 1] = 0;
		len--;
	}
	if (!fn) {
		/* Embedded GDExtension path: waypipe.c main() never ran, so
		 * log_funcs are unset and decoder diagnostics (incl. the
		 * avcodec_open2 failure) would vanish. Route to logcat. */
#ifdef __ANDROID__
		__android_log_print(
				wp_level == WP_ERROR ? ANDROID_LOG_ERROR :
						       ANDROID_LOG_INFO,
				"gdwaypipe-diag", "ffmpeg: %s", buf);
#endif
		return;
	}
	(*fn)("ffmpeg", 0, wp_level, "%s", buf);
}

void setup_video_logging(void)
{
	if (log_funcs[WP_DEBUG]) {
		av_log_set_level(AV_LOG_INFO);
	} else {
		av_log_set_level(AV_LOG_WARNING);
	}
	av_log_set_callback(video_log_callback);
}

#ifdef HAS_VAAPI

static uint32_t drm_to_va_fourcc(uint32_t drm_fourcc)
{
	switch (drm_fourcc) {
	/* At the moment, Intel/AMD VAAPI implementations only support
	 * various YUY configurations and RGB32. (No other RGB variants).
	 * See also libavutil / hwcontext_vaapi.c / vaapi_drm_format_map[] */
	case DRM_FORMAT_XRGB8888:
		return VA_FOURCC_BGRX;
	case DRM_FORMAT_XBGR8888:
		return VA_FOURCC_RGBX;
	case DRM_FORMAT_RGBX8888:
		return VA_FOURCC_XBGR;
	case DRM_FORMAT_BGRX8888:
		return VA_FOURCC_XRGB;
	case DRM_FORMAT_NV12:
		return VA_FOURCC_NV12;
	}
	return 0;
}
static uint32_t va_fourcc_to_rt(uint32_t va_fourcc)
{
	switch (va_fourcc) {
	case VA_FOURCC_BGRX:
	case VA_FOURCC_RGBX:
		return VA_RT_FORMAT_RGB32;
	case VA_FOURCC_NV12:
		return VA_RT_FORMAT_YUV420;
	}
	return 0;
}

static int setup_vaapi_pipeline(struct shadow_fd *sfd, struct render_data *rd,
		uint32_t width, uint32_t height)
{
	VADisplay vadisp = rd->av_vadisplay;

	uintptr_t buffer_val = (uintptr_t)sfd->fd_local;
	uint32_t va_fourcc = drm_to_va_fourcc(sfd->dmabuf_info.format);
	if (va_fourcc == 0) {
		wp_error("Could not convert DRM format %x to VA fourcc",
				sfd->dmabuf_info.format);
		return -1;
	}
	uint32_t rt_format = va_fourcc_to_rt(va_fourcc);

	VASurfaceAttribExternalBuffers buffer_desc;
	buffer_desc.num_buffers = 1;
	buffer_desc.buffers = &buffer_val;
	buffer_desc.pixel_format = va_fourcc;
	buffer_desc.flags = 0;
	buffer_desc.width = width;
	buffer_desc.height = height;
	buffer_desc.data_size = (uint32_t)sfd->buffer_size;
	buffer_desc.num_planes = (uint32_t)sfd->dmabuf_info.num_planes;
	for (int i = 0; i < (int)sfd->dmabuf_info.num_planes; i++) {
		buffer_desc.offsets[i] = sfd->dmabuf_info.offsets[i];
		buffer_desc.pitches[i] = sfd->dmabuf_info.strides[i];
	}

	VASurfaceAttrib attribs[3];
	attribs[0].type = VASurfaceAttribPixelFormat;
	attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[0].value.type = VAGenericValueTypeInteger;
	attribs[0].value.value.i = 0;
	attribs[1].type = VASurfaceAttribMemoryType;
	attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[1].value.type = VAGenericValueTypeInteger;
	attribs[1].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
	attribs[2].type = VASurfaceAttribExternalBufferDescriptor;
	attribs[2].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[2].value.type = VAGenericValueTypePointer;
	attribs[2].value.value.p = &buffer_desc;

	sfd->video_va_surface = 0;
	sfd->video_va_context = 0;
	sfd->video_va_pipeline = 0;

	VAStatus stat = vaCreateSurfaces(vadisp, rt_format, buffer_desc.width,
			buffer_desc.height, &sfd->video_va_surface, 1, attribs,
			3);
	if (stat != VA_STATUS_SUCCESS) {
		wp_error("Create surface failed: %s", vaErrorStr(stat));
		sfd->video_va_surface = 0;
		return -1;
	}

	stat = vaCreateContext(vadisp, rd->av_copy_config,
			(int)buffer_desc.width, (int)buffer_desc.height, 0,
			&sfd->video_va_surface, 1, &sfd->video_va_context);
	if (stat != VA_STATUS_SUCCESS) {
		wp_error("Create context failed %s", vaErrorStr(stat));
		vaDestroySurfaces(vadisp, &sfd->video_va_surface, 1);
		sfd->video_va_surface = 0;
		sfd->video_va_context = 0;
		return -1;
	}

	stat = vaCreateBuffer(vadisp, sfd->video_va_context,
			VAProcPipelineParameterBufferType,
			sizeof(VAProcPipelineParameterBuffer), 1, NULL,
			&sfd->video_va_pipeline);
	if (stat != VA_STATUS_SUCCESS) {
		wp_error("Failed to create pipeline buffer: %s",
				vaErrorStr(stat));
		vaDestroySurfaces(vadisp, &sfd->video_va_surface, 1);
		vaDestroyContext(vadisp, sfd->video_va_context);
		sfd->video_va_surface = 0;
		sfd->video_va_context = 0;
		sfd->video_va_pipeline = 0;
		return -1;
	}
	return 0;
}

static void cleanup_vaapi_pipeline(struct shadow_fd *sfd)
{
	if (!sfd->video_va_surface && !sfd->video_va_context &&
			!sfd->video_va_pipeline) {
		return;
	}

	AVHWDeviceContext *vwdc =
			(AVHWDeviceContext *)
					sfd->video_context->hw_device_ctx->data;
	if (vwdc->type != AV_HWDEVICE_TYPE_VAAPI) {
		return;
	}
	AVVAAPIDeviceContext *vdctx = (AVVAAPIDeviceContext *)vwdc->hwctx;
	VADisplay vadisp = vdctx->display;

	if (sfd->video_va_surface) {
		vaDestroySurfaces(vadisp, &sfd->video_va_surface, 1);
		sfd->video_va_surface = 0;
	}
	if (sfd->video_va_context) {
		vaDestroyContext(vadisp, sfd->video_va_context);
		sfd->video_va_context = 0;
	}
	if (sfd->video_va_pipeline) {
		vaDestroyBuffer(vadisp, sfd->video_va_pipeline);
		sfd->video_va_pipeline = 0;
	}
}

static void run_vaapi_conversion(struct shadow_fd *sfd, struct render_data *rd,
		struct AVFrame *va_frame)
{
	VADisplay vadisp = rd->av_vadisplay;

	if (va_frame->format != AV_PIX_FMT_VAAPI) {
		wp_error("Non-vaapi pixel format: %s",
				av_get_pix_fmt_name(va_frame->format));
	}
	VASurfaceID src_surf = (VASurfaceID)(ptrdiff_t)va_frame->data[3];

	int stat = vaBeginPicture(
			vadisp, sfd->video_va_context, sfd->video_va_surface);
	if (stat != VA_STATUS_SUCCESS) {
		wp_error("Begin picture config failed: %s", vaErrorStr(stat));
	}

	VAProcPipelineParameterBuffer *pipeline_param;
	stat = vaMapBuffer(vadisp, sfd->video_va_pipeline,
			(void **)&pipeline_param);
	if (stat != VA_STATUS_SUCCESS) {
		wp_error("Failed to map pipeline buffer: %s", vaErrorStr(stat));
	}

	pipeline_param->surface = src_surf;
	pipeline_param->surface_region = NULL;
	pipeline_param->output_region = NULL;
	pipeline_param->output_background_color = 0;
	pipeline_param->filter_flags = VA_FILTER_SCALING_FAST;
	pipeline_param->filters = NULL;
	pipeline_param->filters = 0;

	stat = vaUnmapBuffer(vadisp, sfd->video_va_pipeline);
	if (stat != VA_STATUS_SUCCESS) {
		wp_error("Failed to unmap pipeline buffer: %s",
				vaErrorStr(stat));
	}

	stat = vaRenderPicture(vadisp, sfd->video_va_context,
			&sfd->video_va_pipeline, 1);
	if (stat != VA_STATUS_SUCCESS) {
		wp_error("Failed to render picture: %s", vaErrorStr(stat));
	}

	stat = vaEndPicture(vadisp, sfd->video_va_context);
	if (stat != VA_STATUS_SUCCESS) {
		wp_error("End picture failed: %s", vaErrorStr(stat));
	}

	stat = vaSyncSurface(vadisp, sfd->video_va_surface);
	if (stat != VA_STATUS_SUCCESS) {
		wp_error("Sync surface failed: %s", vaErrorStr(stat));
	}
}
#endif

void destroy_video_data(struct shadow_fd *sfd)
{
	if (sfd->video_context) {
#ifdef HAS_VAAPI
		cleanup_vaapi_pipeline(sfd);
#endif
		/* free contexts (which, theoretically, could have hooks into
		 * frames/packets) first */
#ifdef __ANDROID__
		if (sfd->video_android) {
			/* Surface-mode decode: the AVCodecContext is owned
			 * by the decoder pool (android_video.c) — freeing it
			 * here would destroy a MediaCodec instance the pool
			 * means to reuse. Detach and release instead: the
			 * pool flushes the entry and marks it IDLE (design
			 * §5.5); av_android_pool_release also frees and
			 * NULLs sfd->video_android. */
			sfd->video_context = NULL;
			av_android_pool_release(sfd, false);
		} else
#endif
		{
			avcodec_free_context(&sfd->video_context);
		}
		sws_freeContext(sfd->video_color_context);
		if (sfd->video_yuv_frame_data) {
			av_freep(sfd->video_yuv_frame_data);
		}
		if (sfd->video_local_frame_data) {
			av_freep(sfd->video_local_frame_data);
		}
		av_frame_free(&sfd->video_local_frame);
		av_frame_free(&sfd->video_tmp_frame);
		av_frame_free(&sfd->video_last_frame);
		av_frame_free(&sfd->video_yuv_frame);
		av_packet_free(&sfd->video_packet);
	}
#ifdef __ANDROID__
	else if (sfd->video_android) {
		/* Pool bound but the context pointer already detached (e.g.
		 * teardown raced a blit-failure rebind): still drop the
		 * pool binding so the per-sfd EGLImage/FBO target is
		 * released. */
		av_android_pool_release(sfd, false);
	}
#endif
}

static void copy_onto_video_mirror(const char *buffer, uint32_t map_stride,
		AVFrame *frame, const struct dmabuf_slice_data *info)
{
	for (int i = 0; i < info->num_planes; i++) {
		int j = i;
		if (needs_vu_flip(info->format) && (i == 1 || i == 2)) {
			j = 3 - i;
		}
		for (size_t r = 0; r < info->height; r++) {
			uint8_t *dst = frame->data[j] +
				       frame->linesize[j] * (int)r;
			const char *src = buffer + (size_t)info->offsets[i] +
					  (size_t)map_stride * r;
			/* todo: handle multiplanar strides properly */
			size_t common = (size_t)minu(map_stride,
					(uint64_t)frame->linesize[j]);
			memcpy(dst, src, common);
		}
	}
}
static void copy_from_video_mirror(char *buffer, uint32_t map_stride,
		const AVFrame *frame, const struct dmabuf_slice_data *info)
{
	for (int i = 0; i < info->num_planes; i++) {
		int j = i;
		if (needs_vu_flip(info->format) && (i == 1 || i == 2)) {
			j = 3 - i;
		}
		for (size_t r = 0; r < info->height; r++) {
			const uint8_t *src = frame->data[j] +
					     frame->linesize[j] * (int)r;
			char *dst = buffer + (size_t)info->offsets[i] +
				    (size_t)map_stride * r;
			/* todo: handle multiplanar strides properly */
			size_t common = (size_t)minu(map_stride,
					(uint64_t)frame->linesize[j]);
			memcpy(dst, src, common);
		}
	}
}

static bool pad_hardware_size(
		int width, int height, int *new_width, int *new_height)
{
	/* VAAPI drivers often impose additional alignment restrictions; for
	 * example, requiring that width be 16-aligned, or that tiled buffers be
	 * 128-aligned. See also intel-vaapi-driver, i965_drv_video.c,
	 * i965_suface_external_memory() [sic] ; */
	*new_width = align(width, 16);
	*new_height = align(height, 16);
	if (width % 16 != 0) {
		/* Something goes wrong with VAAPI/buffer state when the
		 * width (or stride?) is not a multiple of 16, and GEM_MMAP
		 * ioctls start failing */
		return false;
	}
	return true;
}

static int init_hwcontext(struct render_data *rd)
{
#ifdef __ANDROID__
	/* Android has no /dev/dri render node and no VAAPI; hardware decode
	 * runs through ffmpeg's MediaCodec decoders. If hardware video was
	 * disabled at startup, take the software branch deterministically
	 * (has_hw = false) as before. */
	if (rd->av_disabled) {
		return -1;
	}
	if (rd->av_hwdevice_ref != NULL) {
		return 0;
	}
	/* The process JavaVM must be handed to ffmpeg before any MediaCodec
	 * decoder is opened; without it decoder init fails. Capture is lazy
	 * (first hw init) — see gdwaypipe_capture_jvm above. A failed capture
	 * deliberately does NOT latch av_disabled: the JvmArm plugin's
	 * System.load (JNI_OnLoad) may arm the VM later, so every
	 * setup_video_decode retries; software decode serves meanwhile. The
	 * JVM can only be set once, so a repeated registration with the same
	 * VM (AVERROR(EEXIST)) counts as success too. */
	if (gdwaypipe_android_jvm == NULL) {
		gdwaypipe_capture_jvm();
	}
	if (gdwaypipe_android_jvm == NULL) {
		return -1;
	}
	{
		int jerr = av_jni_set_java_vm(gdwaypipe_android_jvm, NULL);
		if (jerr < 0 && jerr != AVERROR(EEXIST)) {
			DIAG_VIDEO_LOG("[hw-init] FAIL av_jni_set_java_vm: %s (software decode)\n",
					av_err2str(jerr));
			rd->av_disabled = true;
			return -1;
		}
	}
	if (!av_android_sw_forced()) {
		/* Pool mode (design §5): the JVM above is all the global
		 * state needed here — android_video.c creates one
		 * surface-mode MEDIACODEC hw device per pool entry with
		 * hwctx->native_window set (design §1.2b), so no shared
		 * rd->av_hwdevice_ref is created. Pool/EGL failures are
		 * per-sfd and handled at av_android_pool_setup time. */
		DIAG_VIDEO_LOG("[hw-init] ok JavaVM=%p; surface-mode pool decode\n",
				(void *) gdwaypipe_android_jvm);
		return 0;
	}
	/* DEPRECATED: Android CPU decode path, remove after surface-mode
	 * soak (see android-hw-decode-design.md §13). A surface-less
	 * MediaCodec device runs the decoders in ByteBuffer mode, so
	 * decoded frames come out as plain CPU frames (NV12) and the
	 * existing sws_scale path applies directly. Reachable only when
	 * GDWAYPIPE_ANDROID_SW=1 forces the legacy ladder. */
	if (av_hwdevice_ctx_create(&rd->av_hwdevice_ref,
			AV_HWDEVICE_TYPE_MEDIACODEC, NULL, NULL, 0) < 0) {
		DIAG_VIDEO_LOG("[hw-init] FAIL av_hwdevice_ctx_create(MEDIACODEC); software decode\n");
		rd->av_disabled = true;
		return -1;
	}
	DIAG_VIDEO_LOG("[hw-init] ok MediaCodec hw device created\n");
	return 0;
#else
	if (rd->av_disabled) {
		return -1;
	}
	if (rd->av_hwdevice_ref != NULL) {
		return 0;
	}
	if (init_render_data(rd) == -1) {
		rd->av_disabled = true;
		return -1;
	}

	rd->av_vadisplay = 0;
	rd->av_copy_config = 0;
	rd->av_drmdevice_ref = NULL;

	// Q: what does this even do?
	rd->av_drmdevice_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
	if (!rd->av_drmdevice_ref) {
		wp_error("Failed to allocate AV DRM device context");
		rd->av_disabled = true;
		return -1;
	}
	AVHWDeviceContext *hwdc =
			(AVHWDeviceContext *)rd->av_drmdevice_ref->data;
	AVDRMDeviceContext *dctx = hwdc->hwctx;
	dctx->fd = rd->drm_fd;
	if (av_hwdevice_ctx_init(rd->av_drmdevice_ref)) {
		wp_error("Failed to initialize AV DRM device context");
		rd->av_disabled = true;
		return -1;
	}

	/* We create a derived context here, to ensure that the drm fd matches
	 * that which was used to create the DMABUFs. Also, this ensures that
	 * the VA implementation doesn't look for a connection via e.g. Wayland
	 * or X11 */
	if (av_hwdevice_ctx_create_derived(&rd->av_hwdevice_ref,
			    AV_HWDEVICE_TYPE_VAAPI, rd->av_drmdevice_ref,
			    0) < 0) {
		wp_error("Failed to create VAAPI hardware device");
		rd->av_disabled = true;
		return -1;
	}

#ifdef HAS_VAAPI
	AVHWDeviceContext *vwdc =
			(AVHWDeviceContext *)rd->av_hwdevice_ref->data;
	AVVAAPIDeviceContext *vdctx = (AVVAAPIDeviceContext *)vwdc->hwctx;
	if (!vdctx) {
		wp_error("No vaapi device context");
		rd->av_disabled = true;
		return -1;
	}
	rd->av_vadisplay = vdctx->display;

	int stat = vaCreateConfig(rd->av_vadisplay, VAProfileNone,
			VAEntrypointVideoProc, NULL, 0, &rd->av_copy_config);
	if (stat != VA_STATUS_SUCCESS) {
		wp_error("Create config failed: %s", vaErrorStr(stat));
		rd->av_disabled = true;
		return -1;
	}

#endif

	return 0;
#endif /* __ANDROID__ */
}

void cleanup_hwcontext(struct render_data *rd)
{
	rd->av_disabled = true;
#if HAS_VAAPI
	if (rd->av_vadisplay && rd->av_copy_config) {
		vaDestroyConfig(rd->av_vadisplay, rd->av_copy_config);
	}
#endif

	if (rd->av_hwdevice_ref) {
		av_buffer_unref(&rd->av_hwdevice_ref);
	}
	if (rd->av_drmdevice_ref) {
		av_buffer_unref(&rd->av_drmdevice_ref);
	}
}

static void configure_low_latency_enc_context(struct AVCodecContext *ctx,
		bool sw, enum video_coding_fmt fmt, int bpf, int nthreads)
{
	// "time" is only meaningful in terms of the frames provided
	int nom_fps = 25;
	ctx->time_base = (AVRational){1, nom_fps};
	ctx->framerate = (AVRational){nom_fps, 1};

	/* B-frames are directly tied to latency, since each one
	 * is predicted using its preceding and following
	 * frames. The gop size is chosen by the driver. */
	ctx->gop_size = -1;
	ctx->max_b_frames = 0; // Q: how to get this to zero?
	// low latency
	ctx->delay = 0;

	if (sw) {
		ctx->bit_rate = (int64_t)bpf * nom_fps;
		if (fmt == VIDEO_H264) {
			if (av_opt_set(ctx->priv_data, "preset", "ultrafast",
					    0) != 0) {
				wp_error("Failed to set x264 encode ultrafast preset");
			}
			if (av_opt_set(ctx->priv_data, "tune", "zerolatency",
					    0) != 0) {
				wp_error("Failed to set x264 encode zerolatency");
			}
		} else if (fmt == VIDEO_VP9) {
			if (av_opt_set(ctx->priv_data, "lag-in-frames", "0",
					    0) != 0) {
				wp_error("Failed to set vp9 encode lag");
			}
			if (av_opt_set(ctx->priv_data, "quality", "realtime",
					    0) != 0) {
				wp_error("Failed to set vp9 quality");
			}
			if (av_opt_set(ctx->priv_data, "speed", "8", 0) != 0) {
				wp_error("Failed to set vp9 speed");
			}
		} else if (fmt == VIDEO_AV1) {
			// AOM-AV1
			if (av_opt_set(ctx->priv_data, "usage", "realtime",
					    0) != 0) {
				wp_error("Failed to set av1 usage");
			}
			if (av_opt_set(ctx->priv_data, "lag-in-frames", "0",
					    0) != 0) {
				wp_error("Failed to set av1 lag");
			}
			if (av_opt_set(ctx->priv_data, "cpu-used", "8", 0) !=
					0) {
				wp_error("Failed to set av1 speed");
			}
			// Use multi-threaded encoding
			ctx->thread_count = nthreads;
		}
	} else {
		ctx->bit_rate = (int64_t)bpf * nom_fps;
		if (fmt == VIDEO_H264) {
			/* with i965/gen8, hardware encoding is faster but has
			 * significantly worse quality per bitrate than x264 */
			if (av_opt_set(ctx->priv_data, "profile", "main", 0) !=
					0) {
				wp_error("Failed to set h264 encode main profile");
			}
		}
	}
}

static int setup_hwvideo_encode(
		struct shadow_fd *sfd, struct render_data *rd, int nthreads)
{
	/* NV12 is the preferred format for Intel VAAPI; see also
	 * intel-vaapi-driver/src/i965_drv_video.c . Packed formats like
	 * YUV420P typically don't work. */
	const enum AVPixelFormat videofmt = AV_PIX_FMT_NV12;
	const struct AVCodec *codec =
		get_video_hw_encoder(sfd->video_fmt, false);
	if (!codec) {
		return -1;
	}
	struct AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) {
		wp_error("Failed to allocate codec context");
		return -1;
	}
	configure_low_latency_enc_context(
			ctx, false, sfd->video_fmt, rd->av_bpf, nthreads);
	if (!pad_hardware_size((int)sfd->dmabuf_info.width,
			    (int)sfd->dmabuf_info.height, &ctx->width,
			    &ctx->height)) {
		wp_error("Video dimensions (WxH = %dx%d) not alignable to use hardware video encoding",
				sfd->dmabuf_info.width,
				sfd->dmabuf_info.height);
		goto fail_alignment;
	}

	AVHWFramesConstraints *constraints =
			av_hwdevice_get_hwframe_constraints(
					rd->av_hwdevice_ref, NULL);
	if (!constraints) {
		wp_error("Failed to get hardware frame constraints");
		goto fail_hwframe_constraints;
	}
	if (!constraints->valid_hw_formats) {
		wp_error("No hardware frame formats available");
		av_hwframe_constraints_free(&constraints);
		goto fail_hwframe_constraints;
	}
	enum AVPixelFormat hw_format = constraints->valid_hw_formats[0];
	av_hwframe_constraints_free(&constraints);

	AVBufferRef *frame_ref = av_hwframe_ctx_alloc(rd->av_hwdevice_ref);
	if (!frame_ref) {
		wp_error("Failed to allocate frame reference");
		goto fail_frameref;
	}

	AVHWFramesContext *fctx = (AVHWFramesContext *)frame_ref->data;
	/* hw fmt is e.g. "vaapi_vld" */
	fctx->format = hw_format;
	fctx->sw_format = videofmt;
	fctx->width = ctx->width;
	fctx->height = ctx->height;

	int err = av_hwframe_ctx_init(frame_ref);
	if (err < 0) {
		wp_error("Failed to init hardware frame context, %s",
				av_err2str(err));
		goto fail_hwframe_init;
	}

	ctx->pix_fmt = hw_format;
	ctx->hw_frames_ctx = av_buffer_ref(frame_ref);
	if (!ctx->hw_frames_ctx) {
		wp_error("Failed to reference hardware frame context for codec context");
		goto fail_ctx_hwfctx;
	}

	int open_err = avcodec_open2(ctx, codec, NULL);
	if (open_err < 0) {
		wp_error("Failed to open codec: %s", av_err2str(open_err));
		goto fail_codec_open;
	}

	/* Create a VAAPI frame linked to the sfd DMABUF */
	struct AVDRMFrameDescriptor *framedesc =
			av_mallocz(sizeof(struct AVDRMFrameDescriptor));
	if (!framedesc) {
		wp_error("Failed to allocate DRM frame descriptor");
		goto fail_framedesc_alloc;
	}
	/* todo: multiplanar support */
	framedesc->nb_objects = 1;
	framedesc->objects[0].format_modifier = sfd->dmabuf_info.modifier;
	framedesc->objects[0].fd = sfd->fd_local;
	framedesc->objects[0].size = sfd->buffer_size;
	framedesc->nb_layers = 1;
	framedesc->layers[0].nb_planes = sfd->dmabuf_info.num_planes;
	framedesc->layers[0].format = sfd->dmabuf_info.format;
	for (int i = 0; i < (int)sfd->dmabuf_info.num_planes; i++) {
		framedesc->layers[0].planes[i].object_index = 0;
		framedesc->layers[0].planes[i].offset =
				sfd->dmabuf_info.offsets[i];
		framedesc->layers[0].planes[i].pitch =
				sfd->dmabuf_info.strides[i];
	}

	AVFrame *local_frame = av_frame_alloc();
	if (!local_frame) {
		wp_error("Failed to allocate local frame");
		av_free(framedesc);
		goto fail_frame_alloc;
	}
	local_frame->width = ctx->width;
	local_frame->height = ctx->height;
	local_frame->format = AV_PIX_FMT_DRM_PRIME;
	local_frame->buf[0] = av_buffer_create((uint8_t *)framedesc,
			sizeof(struct AVDRMFrameDescriptor),
			av_buffer_default_free, local_frame, 0);
	if (!local_frame->buf[0]) {
		wp_error("Failed to reference count frame DRM description");
		av_free(framedesc);
		goto fail_framedesc_ref;
	}
	local_frame->data[0] = (uint8_t *)framedesc;
	local_frame->hw_frames_ctx = av_buffer_ref(frame_ref);
	if (!local_frame->hw_frames_ctx) {
		wp_error("Failed to reference hardware frame context for local frame");
		goto fail_frame_hwfctx;
	}

	AVFrame *yuv_frame = av_frame_alloc();
	if (!yuv_frame) {
		wp_error("Failed to allocate yuv frame");
		goto fail_yuv_frame;
	}
	yuv_frame->format = hw_format;
	yuv_frame->hw_frames_ctx = av_buffer_ref(frame_ref);
	if (!yuv_frame->hw_frames_ctx) {
		wp_error("Failed to reference hardware frame context for yuv frame");
		goto fail_yuv_hwfctx;
	}

	int map_err = av_hwframe_map(yuv_frame, local_frame, 0);
	if (map_err) {
		wp_error("Failed to map (DRM) local frame to (hardware) yuv frame: %s",
				av_err2str(map_err));
		goto fail_map;
	}

	struct AVPacket *pkt = av_packet_alloc();
	if (!pkt) {
		wp_error("Failed to allocate av packet");
		goto fail_pkt_alloc;
	}

	av_buffer_unref(&frame_ref);

	sfd->video_context = ctx;
	sfd->video_local_frame = local_frame;
	sfd->video_yuv_frame = yuv_frame;
	sfd->video_packet = pkt;
	return 0;

fail_pkt_alloc:
fail_map:
fail_yuv_hwfctx:
	av_frame_free(&yuv_frame);
fail_yuv_frame:
fail_framedesc_ref:
fail_frame_hwfctx:
	av_frame_free(&local_frame);
fail_frame_alloc:
fail_framedesc_alloc:
fail_codec_open:
fail_ctx_hwfctx:
fail_hwframe_init:
	av_buffer_unref(&frame_ref);
fail_frameref:
fail_hwframe_constraints:
fail_alignment:
	avcodec_free_context(&ctx);

	return -1;
}

int setup_video_encode(
		struct shadow_fd *sfd, struct render_data *rd, int nthreads)
{
	if (sfd->video_context) {
		wp_error("Video context already set up for sfd RID=%d",
				sfd->remote_id);
		return -1;
	}

	bool has_hw = init_hwcontext(rd) == 0;
	/* Attempt hardware encoding, and if it doesn't succeed, fall back
	 * to software encoding */
	if (has_hw) {
		if (setup_hwvideo_encode(sfd, rd, nthreads) == 0) {
			return 0;
		}
		wp_debug("Hardware video encoding unavailable for RID=%d; falling back to software",
				sfd->remote_id);
	}

	enum AVPixelFormat avpixfmt = drm_to_av(sfd->dmabuf_info.format);
	if (avpixfmt == AV_PIX_FMT_NONE) {
		wp_error("Failed to find matching AvPixelFormat	for %x",
				sfd->dmabuf_info.format);
		return -1;
	}
	enum AVPixelFormat videofmt = AV_PIX_FMT_YUV420P;
	if (sws_isSupportedInput(avpixfmt) == 0) {
		wp_error("frame format %s not supported",
				av_get_pix_fmt_name(avpixfmt));
		return -1;
	}
	if (sws_isSupportedInput(videofmt) == 0) {
		wp_error("videofmt %s not supported",
				av_get_pix_fmt_name(videofmt));
		return -1;
	}
	const struct AVCodec *codec =
			get_video_sw_encoder(sfd->video_fmt, true);
	if (!codec) {
		return -1;
	}

	struct AVPacket *pkt = NULL;
	struct AVFrame *local_frame = NULL;
	struct AVFrame *yuv_frame = NULL;
	struct SwsContext *sws = NULL;

	struct AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) {
		wp_error("Failed to allocate codec context");
		return -1;
	}
	ctx->pix_fmt = videofmt;
	configure_low_latency_enc_context(
			ctx, true, sfd->video_fmt, rd->av_bpf, nthreads);

	/* Increase image sizes as needed to ensure codec can run */
	ctx->width = (int)sfd->dmabuf_info.width;
	ctx->height = (int)sfd->dmabuf_info.height;
	int linesize_align[AV_NUM_DATA_POINTERS];
	avcodec_align_dimensions2(
			ctx, &ctx->width, &ctx->height, linesize_align);

	pkt = av_packet_alloc();
	if (!pkt) {
		wp_error("Failed to allocate video packet");
		goto cleanup;
	}

	if (avcodec_open2(ctx, codec, NULL) < 0) {
		wp_error("Failed to open codec");
		goto cleanup;
	}

	local_frame = av_frame_alloc();
	if (!local_frame) {
		wp_error("Could not allocate video frame");
		goto cleanup;
	}
	local_frame->format = avpixfmt;
	/* adopt padded sizes */
	local_frame->width = ctx->width;
	local_frame->height = ctx->height;
	int local_img_size = av_image_alloc(local_frame->data,
			local_frame->linesize, local_frame->width,
			local_frame->height, avpixfmt, 64);
	if (local_img_size < 0) {
		wp_error("Failed to allocate temp image");
		goto cleanup;
	}
	/* av_image_alloc does not zero the buffer; clear padding so
	 * uninitialized bytes are not encoded into the stream */
	memset(local_frame->data[0], 0, (size_t)local_img_size);

	yuv_frame = av_frame_alloc();
	if (!yuv_frame) {
		wp_error("Could not allocate yuv frame");
		goto cleanup;
	}
	yuv_frame->width = ctx->width;
	yuv_frame->height = ctx->height;
	yuv_frame->format = videofmt;
	int yuv_img_size = av_image_alloc(yuv_frame->data,
			yuv_frame->linesize, yuv_frame->width,
			yuv_frame->height, videofmt, 64);
	if (yuv_img_size < 0) {
		wp_error("Failed to allocate temp image");
		goto cleanup;
	}
	memset(yuv_frame->data[0], 0, (size_t)yuv_img_size);
	sws = sws_getContext(local_frame->width,
			local_frame->height, avpixfmt, yuv_frame->width,
			yuv_frame->height, videofmt, SWS_BILINEAR, NULL, NULL,
			NULL);
	if (!sws) {
		wp_error("Could not create software color conversion context");
		goto cleanup;
	}

	sfd->video_yuv_frame = yuv_frame;
	/* recorded pointer to be freed to match av_image_alloc */
	sfd->video_yuv_frame_data = &yuv_frame->data[0];
	sfd->video_local_frame = local_frame;
	sfd->video_local_frame_data = &local_frame->data[0];
	sfd->video_packet = pkt;
	sfd->video_context = ctx;
	sfd->video_color_context = sws;
	return 0;

cleanup:
	avcodec_free_context(&ctx);
	av_packet_free(&pkt);
	if (local_frame) {
		av_freep(&local_frame->data[0]);
		av_frame_free(&local_frame);
	}
	if (yuv_frame) {
		av_freep(&yuv_frame->data[0]);
		av_frame_free(&yuv_frame);
	}
	sws_freeContext(sws);
	return -1;
}

static enum AVPixelFormat get_decode_format(
		AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
	(void)ctx;
	for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE;
			p++) {
#ifdef __ANDROID__
		/* Prefer MediaCodec output, if available. */
		if (*p == AV_PIX_FMT_MEDIACODEC) {
			return AV_PIX_FMT_MEDIACODEC;
		}
#else
		/* Prefer VAAPI output, if available. */
		if (*p == AV_PIX_FMT_VAAPI) {
			return AV_PIX_FMT_VAAPI;
		}
#endif
	}
	/* YUV420P is the typical software option, but this function is only
	 * called when a hardware pixel format was requested */
	return AV_PIX_FMT_NONE;
}

#ifdef __ANDROID__
/* Extract H.264 parameter-set NALs (SPS=7, PPS=8) from an Annex-B packet.
 * Returns the size of a start-code-prefixed SPS+PPS blob written to *out
 * (ownership passes to the caller), 0 while both have not been seen in
 * this packet, or -1 on allocation failure. */
static int h264_extract_ps_extradata(const uint8_t *data, int size,
		uint8_t **out, int *out_size)
{
	uint8_t *buf = NULL;
	int sps_off = -1, sps_size = 0, pps_off = -1, pps_size = 0;

	int pos = 0;
	while (pos + 3 <= size) {
		if (!(data[pos] == 0 && data[pos + 1] == 0 &&
				    data[pos + 2] == 1)) {
			pos++;
			continue;
		}
		int start = pos + 3;
		int end = size;
		for (int j = start; j + 3 <= size; j++) {
			if (data[j] == 0 && data[j + 1] == 0 &&
					data[j + 2] == 1) {
				end = j;
				break;
			}
		}
		/* Trailing zero bytes belong to the next start code. */
		while (end > start && data[end - 1] == 0) {
			end--;
		}
		if (start < end) {
			int type = data[start] & 0x1f;
			if (type == 7 && sps_off < 0) {
				sps_off = start;
				sps_size = end - start;
			} else if (type == 8 && pps_off < 0) {
				pps_off = start;
				pps_size = end - start;
			}
		}
		pos = end > pos ? end : start;
	}
	if (sps_off < 0 || pps_off < 0) {
		return 0;
	}

	buf = av_malloc(4 + sps_size + 4 + pps_size);
	if (!buf) {
		return -1;
	}
	static const uint8_t start_code[4] = { 0, 0, 0, 1 };
	int off = 0;
	memcpy(buf + off, start_code, 4);
	off += 4;
	memcpy(buf + off, data + sps_off, sps_size);
	off += sps_size;
	memcpy(buf + off, start_code, 4);
	off += 4;
	memcpy(buf + off, data + pps_off, pps_size);
	off += pps_size;
	*out = buf;
	*out_size = off;
	return off;
}

/* Upgrade a software-opened decoder to MediaCodec surface mode once a
 * packet carries both SPS and PPS (h264_mediacodec needs them as
 * extradata at avcodec_open2; the stream only delivers them in-band).
 *
 * Pool mode (default, design §5.4): the extracted blob is handed to
 * av_android_pool_setup, which opens the first h264 pool entry with it
 * or rebinds an already-open entry (flush-only rebind; in-band
 * parameter sets suffice once MediaCodec is running, so the blob is
 * unused there). The returned context is pool-owned — it is never
 * freed here, and teardown goes through av_android_pool_release. The
 * still-live software context is only freed after a successful
 * acquire, so a pool failure keeps decoding on the current software
 * decoder and the video_hw_upgrade_failed latch stops further
 * attempts for this sfd (fallback ladder rung 3/5).
 *
 * Legacy CPU path (GDWAYPIPE_ANDROID_SW=1, design §13): the old
 * ByteBuffer-mode open below, which on failure latches
 * video_hw_upgrade_failed and rebuilds the software context so
 * decoding continues. */
static void try_hw_upgrade(struct shadow_fd *sfd, struct render_data *rd,
		const uint8_t *data, int size)
{
	if (sfd->video_hw_upgrade_failed || !sfd->video_hw_upgrade_pending) {
		return;
	}
	if (av_android_sw_forced() && !rd->av_hwdevice_ref) {
		return;
	}
	uint8_t *ps = NULL;
	int ps_size = h264_extract_ps_extradata(data, size, &ps, &ps_size);
	if (ps_size <= 0) {
		av_freep(&ps);
		return;
	}

	if (!av_android_sw_forced()) {
		/* Pool acquire (design §5.4): h264 first upgrade or
		 * flush-only rebind of an already-open entry. */
		struct AVCodecContext *pool_ctx = NULL;
		int pool_err = av_android_pool_setup(sfd, rd, ps, ps_size,
				&pool_ctx);
		/* The pool copies the blob into its freshly opened codec
		 * context; ownership stays with the caller either way. */
		av_freep(&ps);
		if (pool_err == 0) {
			/* Swap in the pool's surface-mode context. The old
			 * software context is ours — free it. video_yuv_frame
			 * (an empty receive target) and video_packet (refilled
			 * by apply_video_packet) carry over unchanged, so the
			 * packet end-of-stream hazard noted in
			 * apply_video_packet does not apply on this path.
			 *
			 * Stream-change handling (design §5.3/§5.5): when the
			 * pool rebound an already-open entry, that entry
			 * still holds the previous stream's reference frames
			 * and in-flight output buffers; av_android_pool_flush
			 * discards them (avcodec_flush_buffers + serial bump)
			 * before this sfd's first packet is sent. Flushing a
			 * freshly opened entry is a harmless no-op, so the
			 * flush is unconditional instead of trying to
			 * distinguish rebind from fresh open here. */
			avcodec_free_context(&sfd->video_context);
			av_android_pool_flush(sfd);
			sfd->video_context = pool_ctx;
			sfd->video_hw_upgrade_pending = false;
			DIAG_VIDEO_LOG("[decode-upgrade] ok RID=%d pool=1 extradata=%d bytes\n",
					sfd->remote_id, ps_size);
			return;
		}
		sfd->video_hw_upgrade_failed = true;
		DIAG_VIDEO_LOG("[decode-upgrade] FAIL RID=%d pool err=%d; staying software\n",
				sfd->remote_id, pool_err);
		return;
	}

	/* Legacy CPU path (DEPRECATED, design §13) below. */

	/* The color-conversion context is keyed to the coded frame
	 * dimensions, which MediaCodec matches (see the coded-size policy
	 * in apply_video_packet); only the codec context swaps here. */
	avcodec_free_context(&sfd->video_context);
	av_frame_free(&sfd->video_yuv_frame);
	av_packet_free(&sfd->video_packet);
	sfd->video_yuv_frame_data = NULL;

	const struct AVCodec *codec =
			get_video_decoder(sfd->video_fmt, false, true);
	struct AVCodecContext *ctx = codec ?
			avcodec_alloc_context3(codec) : NULL;
	if (!ctx) {
		av_freep(&ps);
		sfd->video_hw_upgrade_failed = true;
		DIAG_VIDEO_LOG("[decode-upgrade] FAIL RID=%d no codec; staying software\n",
				sfd->remote_id);
		setup_video_decode(sfd, rd);
		return;
	}
	ctx->delay = 0;
	ctx->width = (int)sfd->dmabuf_info.width;
	ctx->height = (int)sfd->dmabuf_info.height;
	/* Ownership of extradata passes to the AVCodecContext; ffmpeg
	 * frees it in ff_codec_close. */
	ctx->extradata = ps;
	ctx->extradata_size = ps_size;
	ctx->hw_device_ctx = av_buffer_ref(rd->av_hwdevice_ref);
	if (!ctx->hw_device_ctx) {
		avcodec_free_context(&ctx);
		sfd->video_hw_upgrade_failed = true;
		DIAG_VIDEO_LOG("[decode-upgrade] FAIL RID=%d hw_device_ctx; staying software\n",
				sfd->remote_id);
		setup_video_decode(sfd, rd);
		return;
	}
	ctx->get_format = get_decode_format;
	int open_err = avcodec_open2(ctx, codec, NULL);
	if (open_err < 0) {
		DIAG_VIDEO_LOG("[decode-upgrade] FAIL RID=%d err=%d (%s); staying software\n",
				sfd->remote_id, open_err,
				av_err2str(open_err));
		avcodec_free_context(&ctx);
		sfd->video_hw_upgrade_failed = true;
		setup_video_decode(sfd, rd);
		return;
	}
	struct AVFrame *yuv = av_frame_alloc();
	struct AVPacket *pkt = av_packet_alloc();
	if (!yuv || !pkt) {
		av_frame_free(&yuv);
		av_packet_free(&pkt);
		avcodec_free_context(&ctx);
		sfd->video_hw_upgrade_failed = true;
		DIAG_VIDEO_LOG("[decode-upgrade] FAIL RID=%d alloc; staying software\n",
				sfd->remote_id);
		setup_video_decode(sfd, rd);
		return;
	}
	sfd->video_context = ctx;
	sfd->video_yuv_frame = yuv;
	sfd->video_packet = pkt;
	sfd->video_yuv_frame_data = NULL;
	sfd->video_hw_upgrade_pending = false;
	DIAG_VIDEO_LOG("[decode-upgrade] ok RID=%d hw=1 extradata=%d bytes\n",
			sfd->remote_id, ps_size);
}
#endif

int setup_video_decode(struct shadow_fd *sfd, struct render_data *rd)
{
	/* Re-entry guard: a duplicate OPEN_DMAVID_DST must not leak or
	 * replace a live context. */
	/* waypipe.c main() never runs in the embedded GDExtension path, so
	 * av_log_set_callback never fires; decoder diagnostics (notably the
	 * avcodec_open2 MediaCodec failure) must reach logcat. Idempotent. */
	setup_video_logging();

	if (sfd->video_context) {
		wp_error("Video decode context already set up for RID=%d",
				sfd->remote_id);
		return -1;
	}

	bool has_hw = init_hwcontext(rd) == 0;

	enum AVPixelFormat avpixfmt = drm_to_av(sfd->dmabuf_info.format);
	if (avpixfmt == AV_PIX_FMT_NONE) {
		wp_error("Failed to find matching AvPixelFormat for %x",
				sfd->dmabuf_info.format);
		return -1;
	}
	enum AVPixelFormat videofmt = AV_PIX_FMT_YUV420P;

	if (sws_isSupportedInput(avpixfmt) == 0) {
		wp_error("source pixel format %x not supported", avpixfmt);
		return -1;
	}
	if (sws_isSupportedInput(videofmt) == 0) {
		wp_error("AV_PIX_FMT_YUV420P not supported");
		return -1;
	}

	const struct AVCodec *codec =
			get_video_decoder(sfd->video_fmt, true, has_hw);
	if (!codec) {
		return -1;
	}

	/* Determine whether this build's decoder actually registers a VAAPI
	 * hardware config. The vendored ffmpeg is configured
	 * --disable-hwaccels, so it will not, and the HW path must be
	 * skipped rather than attempted and failed. */
	bool hw_supported = false;
	if (has_hw) {
#ifdef __ANDROID__
		const enum AVHWDeviceType hwdev_type =
				AV_HWDEVICE_TYPE_MEDIACODEC;
#else
		const enum AVHWDeviceType hwdev_type = AV_HWDEVICE_TYPE_VAAPI;
#endif
		for (int i = 0;; i++) {
			const AVCodecHWConfig *cfg =
					avcodec_get_hw_config(codec, i);
			if (!cfg) {
				break;
			}
			if (cfg->device_type == hwdev_type &&
					(cfg->methods &
							AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
				hw_supported = true;
				break;
			}
		}
	}

#ifdef __ANDROID__
	if (has_hw && hw_supported && !av_android_sw_forced() &&
			sfd->video_fmt != VIDEO_H264 &&
			!sfd->video_hw_upgrade_failed) {
		/* Pool direct open (design §5.4/§7): vp9 needs no
		 * extradata, so acquire a surface-mode pool entry right
		 * away — av_android_pool_setup opens a fresh MediaCodec
		 * instance or rebinds + flushes an already-open one — and
		 * skip the local software open entirely. The returned
		 * context is pool-owned: never freed here; teardown goes
		 * through av_android_pool_release (destroy_video_data). */
		struct AVCodecContext *pool_ctx = NULL;
		int pool_err = av_android_pool_setup(sfd, rd, NULL, 0,
				&pool_ctx);
		if (pool_err == 0) {
			struct AVFrame *yuv_frame = av_frame_alloc();
			struct AVPacket *pkt =
					yuv_frame ? av_packet_alloc() : NULL;
			if (!yuv_frame || !pkt) {
				wp_error("Could not allocate video frame or packet");
				av_frame_free(&yuv_frame);
				av_packet_free(&pkt);
				/* Roll the just-created binding back; the
				 * entry itself stays IDLE for reuse. */
				av_android_pool_release(sfd, false);
			} else {
				/* Stream-change handling (design §5.3): when
				 * this acquire rebound an entry that served a
				 * previous stream, discard its stale in-flight
				 * buffers first; no-op on a fresh open. */
				av_android_pool_flush(sfd);
				sfd->video_context = pool_ctx;
				sfd->video_yuv_frame = yuv_frame;
				sfd->video_packet = pkt;
				sfd->video_yuv_frame_data = NULL;
				/* will be allocated on frame receipt (SW
				 * fallback only) */
				sfd->video_local_frame = NULL;
				sfd->video_color_context = NULL;
				DIAG_VIDEO_LOG("[decode-setup] ok RID=%d pool=1 %dx%d drm_fmt=0x%x\n",
						sfd->remote_id,
						(int)sfd->dmabuf_info.width,
						(int)sfd->dmabuf_info.height,
						sfd->dmabuf_info.format);
				return 0;
			}
		} else {
			DIAG_VIDEO_LOG("[decode-setup] FAIL RID=%d pool err=%d; software ladder\n",
					sfd->remote_id, pool_err);
		}
		/* Fallback ladder rung 4 (design §7): per-sfd software
		 * fallback — reselect the native software decoder and
		 * continue into the plain SW open below. */
		has_hw = false;
		hw_supported = false;
		codec = get_video_decoder(sfd->video_fmt, false, false);
		if (!codec) {
			return -1;
		}
	}
#endif

	struct AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) {
		wp_error("Failed to allocate context");
		return -1;
	}

	ctx->delay = 0;
	if (has_hw && hw_supported) {
#ifdef __ANDROID__
		if (sfd->video_fmt == VIDEO_H264 &&
				!sfd->video_hw_upgrade_failed) {
			/* h264_mediacodec requires SPS/PPS extradata at
			 * avcodec_open2, but the stream only delivers parameter
			 * sets in-band after the open. Pristine ffmpeg fails the
			 * no-extradata open with raw -1 (EPERM) — the previous
			 * builds only worked because of a since-reverted
			 * tolerance patch — so open the NATIVE software decoder
			 * for the pre-extradata phase; apply_video_packet
			 * upgrades to MediaCodec on the first SPS+PPS packet
			 * (try_hw_upgrade). */
			DIAG_VIDEO_LOG("[decode-setup] %dx%d hw deferred (no extradata yet); software open\n",
					(int)sfd->dmabuf_info.width,
					(int)sfd->dmabuf_info.height);
			sfd->video_hw_upgrade_pending = true;
			has_hw = false;
			hw_supported = false;
			codec = get_video_decoder(
					sfd->video_fmt, false, false);
			avcodec_free_context(&ctx);
			ctx = codec ? avcodec_alloc_context3(codec) : NULL;
			if (!ctx) {
				wp_error("Failed to allocate deferred SW context");
				return -1;
		}
		} else {
			/* MediaCodec accepts arbitrary frame sizes (alignment is
			 * internal to the codec) and the real dimensions come from
			 * the bitstream. The width%16 rejection in
			 * pad_hardware_size is a VAAPI driver workaround; applying
			 * it here silently forced EVERY streamed window (odd widths
			 * under fractional scale) to software decode — observed as
			 * hw=0 on all decode-setups in Quest 2 captures. */
			ctx->width = (int)sfd->dmabuf_info.width;
			ctx->height = (int)sfd->dmabuf_info.height;
		}
#else
		/* If alignment permits, use hardware decoding */
		if (!pad_hardware_size((int)sfd->dmabuf_info.width,
					(int)sfd->dmabuf_info.height,
					&ctx->width, &ctx->height)) {
			DIAG_VIDEO_LOG("[decode-setup] %ux%u rejected by hw alignment gate; software decode\n",
					sfd->dmabuf_info.width,
					sfd->dmabuf_info.height);
			has_hw = false;
			hw_supported = false;
		}
#endif
	} else {
		if (has_hw && !hw_supported) {
			DIAG_VIDEO_LOG("[decode-setup] codec %s advertises no HW_DEVICE_CTX config; software decode\n",
					codec->name);
		}
		has_hw = false;
		hw_supported = false;
	}

	if (has_hw && hw_supported) {
#ifdef __ANDROID__
		if (!av_android_sw_forced()) {
			/* Pool mode: rd->av_hwdevice_ref is intentionally
			 * NULL (no shared surface-less device, design §5);
			 * surface-mode contexts come from the pool. This spot
			 * is only reached when the pool path was skipped or
			 * failed — i.e. h264 with the video_hw_upgrade_failed
			 * latch — so decode software for this sfd. */
			has_hw = false;
			hw_supported = false;
			avcodec_free_context(&ctx);
			codec = get_video_decoder(sfd->video_fmt, false,
					false);
			if (!codec) {
				return -1;
			}
			ctx = avcodec_alloc_context3(codec);
			if (!ctx) {
				wp_error("Failed to allocate context");
				return -1;
			}
		} else
#endif
		{
			ctx->hw_device_ctx = av_buffer_ref(rd->av_hwdevice_ref);
			if (!ctx->hw_device_ctx) {
				wp_error("Failed to reference hardware device context");
				has_hw = false;
				hw_supported = false;
			} else {
				ctx->get_format = get_decode_format;
			}
		}
	}

	if (!has_hw || !hw_supported) {
		ctx->pix_fmt = videofmt;
		/* set context dimensions, and allocate buffer to write into */
		ctx->width = (int)sfd->dmabuf_info.width;
		ctx->height = (int)sfd->dmabuf_info.height;
		int linesize_align[AV_NUM_DATA_POINTERS];
		avcodec_align_dimensions2(
				ctx, &ctx->width, &ctx->height, linesize_align);
	}

	int open_err = avcodec_open2(ctx, codec, NULL);
	if (open_err < 0) {
		if (has_hw && hw_supported) {
			/* The HW-configured context failed to open (e.g. no
			 * registered hardware hw_config, or get_format could not
			 * obtain a surface). Retry once with a fresh software
			 * context instead of hard-failing. */
			DIAG_VIDEO_LOG("[decode-setup] HW open failed RID=%d err=%d (%s); retrying software\n",
					sfd->remote_id, open_err,
					av_err2str(open_err));
			avcodec_free_context(&ctx);
			/* The mediacodec decoders are hardware-only, so a fresh
			 * lookup of the native decoder is required for the
			 * software retry (on VAAPI this resolves to the same
			 * codec as before). */
			codec = get_video_decoder(sfd->video_fmt, false, false);
			if (!codec) {
				return -1;
			}
			ctx = avcodec_alloc_context3(codec);
			if (!ctx) {
				wp_error("Failed to allocate context");
				return -1;
			}
			ctx->delay = 0;
			ctx->pix_fmt = videofmt;
			ctx->width = (int)sfd->dmabuf_info.width;
			ctx->height = (int)sfd->dmabuf_info.height;
			int linesize_align[AV_NUM_DATA_POINTERS];
			avcodec_align_dimensions2(
					ctx, &ctx->width, &ctx->height,
					linesize_align);
			open_err = avcodec_open2(ctx, codec, NULL);
		}
		if (open_err < 0) {
			wp_error("Failed to open codec: %s",
					av_err2str(open_err));
			avcodec_free_context(&ctx);
			return -1;
		}
	}

	struct AVFrame *yuv_frame = av_frame_alloc();
	if (!yuv_frame) {
		wp_error("Could not allocate yuv frame");
		avcodec_free_context(&ctx);
		return -1;
	}
	struct AVPacket *pkt = av_packet_alloc();
	if (!pkt) {
		wp_error("Could not allocate video packet");
		av_frame_free(&yuv_frame);
		avcodec_free_context(&ctx);
		return -1;
	}

	if (ctx->hw_device_ctx) {
#ifdef HAS_VAAPI
		if (rd->av_vadisplay) {
			setup_vaapi_pipeline(sfd, rd, (uint32_t)ctx->width,
					(uint32_t)ctx->height);
		}
#endif
	}

	sfd->video_yuv_frame = yuv_frame;
	sfd->video_packet = pkt;
	sfd->video_context = ctx;
	/* yuv_frame not allocated by us */
	sfd->video_yuv_frame_data = NULL;
	/* will be allocated on frame receipt */
	sfd->video_local_frame = NULL;
	sfd->video_color_context = NULL;
	DIAG_VIDEO_LOG("[decode-setup] ok ctx=%dx%d drm_fmt=0x%x av_fmt=%d hw=%d\n",
			ctx->width, ctx->height, sfd->dmabuf_info.format,
			ctx->pix_fmt, ctx->hw_device_ctx != NULL);
	return 0;
}

void collect_video_from_mirror(
		struct shadow_fd *sfd, struct transfer_queue *transfers)
{
	if (sfd->video_color_context) {
		/* If using software encoding, need to convert to YUV */
		const char *data = NULL;
		uint32_t map_stride = 0;
		void *handle = NULL;
		if (sfd->cpu_mapped) {
			/* CPU fallback: the buffer is a plain mmap, not a
			 * GBM bo; feed the encoder from the mapped pixels. */
			if (!sfd->mem_local) {
				return;
			}
			data = sfd->mem_local;
			map_stride = sfd->dmabuf_map_stride;
			/* The mmap has length sfd->buffer_size; plane 0 begins
			 * at dmabuf_info.offsets[0], so the final source read
			 * is at offsets[0] + height*map_stride. Reading past
			 * the mapping faults (SIGBUS); bail if it will not fit. */
			size_t need = (size_t)sfd->dmabuf_info.offsets[0] +
					(size_t)sfd->dmabuf_info.height *
							(size_t)map_stride;
			if (need > sfd->buffer_size) {
				wp_error("CPU-mapped video buffer too small for RID=%d (need %zu, map %zu)",
						sfd->remote_id, need,
						sfd->buffer_size);
				return;
			}
			if (map_stride != sfd->dmabuf_info.strides[0]) {
				/* The mmap stride differs from the stride the
				 * encoder expects; stride-shift the mapped data
				 * into dmabuf_warped first (same approach as the
				 * FDC_DMABUF CPU path). */
				size_t tx_stride =
						(size_t)sfd->dmabuf_info.strides[0];
				if (!sfd->dmabuf_warped) {
					sfd->dmabuf_warped = zeroed_aligned_alloc(
							alignz(sfd->buffer_size, 64),
							64,
							&sfd->dmabuf_warped_handle);
					if (!sfd->dmabuf_warped) {
						return;
					}
				}
				stride_shifted_copy(sfd->dmabuf_warped,
						sfd->mem_local, 0,
						(size_t)sfd->dmabuf_info.height *
								(size_t)map_stride,
						(size_t)minu(map_stride, tx_stride),
						map_stride, tx_stride);
				data = sfd->dmabuf_warped;
				map_stride = (uint32_t)tx_stride;
			}
		} else {
			data = map_dmabuf(sfd->dmabuf_bo, false, &handle,
					&map_stride);
			if (!data) {
				return;
			}
		}
		copy_onto_video_mirror(data, map_stride, sfd->video_local_frame,
				&sfd->dmabuf_info);
		if (handle) {
			unmap_dmabuf(sfd->dmabuf_bo, handle);
		}

		if (sws_scale(sfd->video_color_context,
				    (const uint8_t *const *)sfd
						    ->video_local_frame->data,
				    sfd->video_local_frame->linesize, 0,
				    sfd->video_local_frame->height,
				    sfd->video_yuv_frame->data,
				    sfd->video_yuv_frame->linesize) < 0) {
			wp_error("Failed to perform color conversion");
			return;
		}
	}

	sfd->video_yuv_frame->pts = sfd->video_frameno++;
	int sendstat = avcodec_send_frame(
			sfd->video_context, sfd->video_yuv_frame);
	if (sendstat < 0) {
		wp_error("Failed to create frame: %s", av_err2str(sendstat));
		return;
	}
	/* A frame may yield zero, one, or several packets because of encoder
	 * delay/buffering. Drain until the encoder needs more input or hits
	 * EOF, emitting one transfer per produced packet. */
	while (true) {
		int recvstat = avcodec_receive_packet(
				sfd->video_context, sfd->video_packet);
		if (recvstat == AVERROR(EAGAIN) ||
				recvstat == AVERROR_EOF) {
			break;
		}
		if (recvstat < 0) {
			wp_error("Failed to receive packet for RID=%d: %s",
					sfd->remote_id, av_err2str(recvstat));
			break;
		}
		struct AVPacket *pkt = sfd->video_packet;
		size_t pktsz = (size_t)pkt->buf->size;
		size_t msgsz = sizeof(struct wmsg_basic) + pktsz;

		char *buf = malloc(alignz(msgsz, 4));

		struct wmsg_basic *header = (struct wmsg_basic *)buf;
		header->size_and_type =
				transfer_header(msgsz, WMSG_SEND_DMAVID_PACKET);
		header->remote_id = sfd->remote_id;

		memcpy(buf + sizeof(struct wmsg_basic), pkt->buf->data,
				pktsz);
		memset(buf + msgsz, 0, alignz(msgsz, 4) - msgsz);

		transfer_add(transfers, alignz(msgsz, 4), buf);

		av_packet_unref(pkt);
	}
}

static int setup_color_conv(struct shadow_fd *sfd, struct AVFrame *cpu_frame)
{
	struct AVCodecContext *ctx = sfd->video_context;

	enum AVPixelFormat avpixfmt = drm_to_av(sfd->dmabuf_info.format);

	struct AVFrame *local_frame = av_frame_alloc();
	if (!local_frame) {
		wp_error("Could not allocate video frame");
		return -1;
	}
	local_frame->format = avpixfmt;
	/* Adopt the DECODED frame's dimensions rather than the codec-context
	 * allocation hints (ctx->width/height): a 16-aligned remote encoder
	 * (e.g. the Rust waypipe) pads the coded frame past the surface's
	 * real size (e.g. 2064 for a 2058-wide window), and the decoder
	 * emits that padded size when the bitstream carries no crop. The
	 * intermediate must match what the decoder actually emits so the
	 * color conversion below is 1:1; the padded right/bottom margin is
	 * cropped afterwards when the pixels are copied into the real-size
	 * dmabuf target (copy_from_video_mirror walks dmabuf_info rows and
	 * strides only, so it never reaches the padding). */
	if (cpu_frame->width > 0 && cpu_frame->height > 0) {
		local_frame->width = cpu_frame->width;
		local_frame->height = cpu_frame->height;
	} else {
		/* A failed hardware-frame transfer leaves the temp frame
		 * zeroed; keep the context hints so the fixed-size mismatch
		 * guard in apply_video_packet drops the bad frame instead of
		 * converting from garbage dimensions. */
		local_frame->width = ctx->width;
		local_frame->height = ctx->height;
	}
	if (av_image_alloc(local_frame->data, local_frame->linesize,
			    local_frame->width, local_frame->height, avpixfmt,
			    64) < 0) {
		wp_error("Failed to allocate local image");
		av_frame_free(&local_frame);
		return -1;
	}

	struct SwsContext *sws = sws_getContext(cpu_frame->width,
			cpu_frame->height, cpu_frame->format,
			local_frame->width, local_frame->height, avpixfmt,
			SWS_BILINEAR, NULL, NULL, NULL);
	if (!sws) {
		wp_error("Could not create software color conversion context");
		av_freep(&local_frame->data[0]);
		av_frame_free(&local_frame);
		return -1;
	}

	sfd->video_local_frame = local_frame;

	sfd->video_local_frame_data = &local_frame->data[0];
	sfd->video_color_context = sws;
	return 0;
}
#ifdef __ANDROID__
/* A pool-bound codec died mid-stream (device evidence 2026-09-07: Venus
 * HW overload kills OMX instances; the sfd then floods errors and shows a
 * stale, never-blitted black AHB forever). Demote the sfd to the software
 * ladder — same teardown shape as the blit-failure path. */
static void android_demote_to_sw(struct shadow_fd *sfd,
		struct render_data *rd, const char *why)
{
	DIAG_VIDEO_LOG("[decode-upgrade] FAIL RID=%d %s; rebinding software\n",
			sfd->remote_id, why);
	sfd->video_context = NULL;
	av_android_pool_release(sfd, false);
	sfd->video_hw_upgrade_failed = true;
	av_frame_free(&sfd->video_yuv_frame);
	av_packet_free(&sfd->video_packet);
	sfd->video_yuv_frame_data = NULL;
	if (setup_video_decode(sfd, rd) < 0) {
		wp_error("Failed to rebuild software decoder after %s (RID=%d)",
				why, sfd->remote_id);
	}
}
#endif


void apply_video_packet(struct shadow_fd *sfd, struct render_data *rd,
		const struct bytebuf *msg)
{
	/* Latency telemetry: per-frame YUV->RGB + dmabuf-write cost (client
	 * decode), GDWAYPIPE_LATENCY=1. */
	static int g_lat_video_ready = 0;
	static struct wp_lat_video g_lat_video;
	if (wp_lat_enabled()) {
		if (!g_lat_video_ready) {
			wp_lat_video_init(&g_lat_video);
			g_lat_video_ready = 1;
		}
	}
	int lat_video_on = wp_lat_enabled();
	int64_t lat_f0 = 0;
#ifdef __ANDROID__
	/* First shot at the h264 hw decoder: upgrade to MediaCodec as soon
	 * as a packet carries SPS+PPS for the required extradata. Runs
	 * before the send so the next packet goes to the hw codec. */
	if (sfd->video_hw_upgrade_pending) {
		try_hw_upgrade(sfd, rd, (const uint8_t *)msg->data,
				(int)msg->size);
	}
#endif
	/* Packet payload MUST be set after try_hw_upgrade: the upgrade
	 * frees and reallocates video_packet, and avcodec_send_packet
	 * treats a size-0/data-NULL packet as end-of-stream, latching
	 * draining forever (hw decoder then never decodes a frame). */
	sfd->video_packet->data = (uint8_t *)msg->data;
	sfd->video_packet->size = (int)msg->size;

	/* MediaCodec needs a monotonic input PTS: with AV_NOPTS_VALUE it logs
	 * "Input packet is missing PTS" and stamps every frame pts=0.
	 * avctx->pkt_timebase is unset, so mediacodec passes the raw value
	 * through as microseconds. Process-global sequence keeps every
	 * decoder context monotonic across hw upgrades. */
	{
		static uint64_t video_pts_seq = 0;
		sfd->video_packet->pts =
				(int64_t)(++video_pts_seq * 1000);
	}

	int sendstat = avcodec_send_packet(
			sfd->video_context, sfd->video_packet);
	if (sendstat < 0) {
		wp_error("Failed to send packet: %s", av_err2str(sendstat));
#ifdef __ANDROID__
		if (sfd->video_android && sendstat != AVERROR(EAGAIN)) {
			/* Dead/failed pool codec (e.g. Venus HW overload);
			 * EAGAIN is mere backpressure — never demote on it. */
			android_demote_to_sw(sfd, rd, "codec send failed");
			return;
		}
#endif
	}

	/* Drain all produced frames; only the most recent one is rendered.
	 * Intermediate frames are stale by the time the compositor samples
	 * the dmabuf, and rendering each of them multiplies the sws+copy
	 * cost of a decoder burst (and flashes older content between
	 * commits).
	 *
	 * IMPORTANT: avcodec_receive_frame UNREFS video_yuv_frame on every
	 * call, INCLUDING the final EAGAIN return — so the newest software
	 * frame must be held by reference (video_last_frame) and true hw
	 * frames must be pulled out of their buffer inside the drain. */
	if (!sfd->video_last_frame &&
	    !(sfd->video_last_frame = av_frame_alloc())) {
		wp_error("Could not allocate last-frame holder");
		return;
	}
#if !HAS_VAAPI
	(void)rd;
#endif
	bool last_is_hw = false;
	bool last_is_vaapi = false;
	int have_frame = 0;
	while (true) {
		int recvstat = avcodec_receive_frame(
				sfd->video_context, sfd->video_yuv_frame);
		if (recvstat < 0) {
			if (recvstat != AVERROR(EAGAIN)) {
				wp_error("Failed to receive frame due to error: %s",
						av_err2str(recvstat));
			}
#ifdef __ANDROID__
			if (sfd->video_android) {
				android_demote_to_sw(sfd, rd,
						"codec receive failed");
				return;
			}
#endif
			break;
		}
		have_frame = 1;
#if HAS_VAAPI
		if (sfd->video_va_surface &&
				sfd->video_yuv_frame->format ==
						AV_PIX_FMT_VAAPI) {
			/* Converted to the dmabuf per-frame (hardware
			 * path); nothing to keep for the drain result. */
			run_vaapi_conversion(
					sfd, rd, sfd->video_yuv_frame);
			last_is_vaapi = true;
			last_is_hw = false;
			continue;
		}
#endif
#ifdef __ANDROID__
		if (sfd->video_android &&
				sfd->video_yuv_frame->format ==
						AV_PIX_FMT_MEDIACODEC) {
			/* Surface mode (pool bound, design §4.1): hold the
			 * newest MEDIACODEC frame by reference exactly like a
			 * software frame; its data[3] AVMediaCodecBuffer is
			 * consumed after the drain by av_android_blit_latest.
			 * NOTE: format alone identifies these frames — vendored
			 * mediacodecdec_common.c sets data[3] but NEVER
			 * hw_frames_ctx on surface-mode output, so a
			 * hw_frames_ctx requirement here silently routed every
			 * hw frame into the sws path (which cannot read
			 * MEDIACODEC frames), dropping all decoded output.
			 * A pool-bound surface-mode context emits ONLY
			 * MEDIACODEC frames, and the deferred-SW context never
			 * does, so format is an unambiguous discriminator.
			 * A previously held surface frame (from this drain or
			 * an earlier packet) is dropped with an explicit
			 * render=0 release before the ref swap: the
			 * free-callback backstop then no-ops because
			 * av_mediacodec_release_buffer latches
			 * buffer->released. The backstop always releases with
			 * render=0 and must never be the render path — every
			 * buffer is released explicitly here or by the blit,
			 * so MediaCodec's output queue can never deadlock. */
			AVMediaCodecBuffer *old_buf = NULL;
			if (sfd->video_last_frame->format ==
					AV_PIX_FMT_MEDIACODEC) {
				old_buf = (AVMediaCodecBuffer *)
						sfd->video_last_frame->data[3];
			}
			if (old_buf) {
				av_mediacodec_release_buffer(old_buf, 0);
			}
			/* av_frame_ref does not unref its destination (the
			 * vendored n6.1 implementation asserts dst is empty),
			 * so drop the previous hold first — otherwise its
			 * AVBufferRefs (including the MEDIACODEC buffer
			 * wrapper ref that drives the free-callback
			 * backstop) leak on every swap. */
			av_frame_unref(sfd->video_last_frame);
			if (av_frame_ref(sfd->video_last_frame,
					    sfd->video_yuv_frame) < 0) {
				wp_error("Failed to reference last frame");
				return;
			}
			last_is_hw = true;
			last_is_vaapi = false;
			continue;
		}
#endif
		if (sfd->video_yuv_frame->hw_frames_ctx != NULL &&
				sfd->video_yuv_frame->format ==
						AV_PIX_FMT_MEDIACODEC) {
			/* DEPRECATED: Android CPU decode path, remove after
			 * surface-mode soak (see android-hw-decode-design.md
			 * §13). Only reachable when GDWAYPIPE_ANDROID_SW=1
			 * forces the legacy ladder — in pool mode the branch
			 * above owns every MEDIACODEC frame. True hw frames do
			 * not survive the next receive; transfer the pixels
			 * out per-frame. Only true hw frames carry
			 * hw_frames_ctx: surface-less MediaCodec (ByteBuffer
			 * mode) emits plain software NV12 frames
			 * (hw_frames_ctx NULL), which must go through the
			 * normal path below. */
			if (!sfd->video_tmp_frame &&
			    !(sfd->video_tmp_frame = av_frame_alloc())) {
				wp_error("Failed to allocate temporary frame");
				return;
			}
			int tferr = av_hwframe_transfer_data(
					sfd->video_tmp_frame,
					sfd->video_yuv_frame, 0);
			if (tferr < 0) {
				wp_error("Failed to transfer hwframe data: %s",
						av_err2str(tferr));
				return;
			}
			last_is_hw = true;
			last_is_vaapi = false;
			continue;
		}
		/* Software frame: hold the newest by reference. av_frame_ref
		 * does not unref its destination (see the pool branch
		 * above), so drop the previous hold first — this also fixes
		 * the pre-existing ref leak on the Linux software path. */
		av_frame_unref(sfd->video_last_frame);
		if (av_frame_ref(sfd->video_last_frame,
				    sfd->video_yuv_frame) < 0) {
			wp_error("Failed to reference last frame");
			return;
		}
		last_is_hw = false;
		last_is_vaapi = false;
	}
	if (!have_frame || last_is_vaapi) {
		/* Nothing decoded, or the newest frame was already handled
		 * by the per-frame VAAPI conversion. */
		return;
	}
#ifdef __ANDROID__
	/* Format check only: surface-mode MEDIACODEC frames carry no
	 * hw_frames_ctx (see the drain comment above), so last_is_hw —
	 * which the drain sets from the same faulty test — must not gate
	 * the present path. A pool-bound context only ever produces
	 * MEDIACODEC frames. */
	if (sfd->video_android &&
			sfd->video_last_frame->format ==
					AV_PIX_FMT_MEDIACODEC) {
		/* Surface-mode present (design §4.1): instead of sws+copy,
		 * present the newest decoded frame by blitting it into the
		 * sfd's BGRA AHardwareBuffer (release render=1 →
		 * updateTexImage → OES crop-rect blit → glFinish). The
		 * bracket below keeps the existing telemetry shape: the
		 * recorded span now covers the whole present cost, so the
		 * pool's internal lat_us out-param stays unused (NULL). */
		if (lat_video_on) {
			lat_f0 = wp_lat_now_us();
		}
		int blit_err = av_android_blit_latest(sfd,
				sfd->video_last_frame, NULL);
		if (lat_video_on) {
			wp_lat_video_record(&g_lat_video,
					wp_lat_now_us() - lat_f0);
		}
		/* The blit consumed the frame's AVMediaCodecBuffer
		 * (render=1 on success, render=0 on failure — the failure
		 * release happens inside av_android_blit_latest), so
		 * dropping our reference cannot double-release it. */
		av_frame_unref(sfd->video_last_frame);
		if (blit_err < 0) {
			/* Fallback ladder rung 6 (design §7): per-sfd SW
			 * fallback. The pool latched the entry DEAD inside
			 * the failed blit; av_android_pool_release tears such
			 * an entry down even with destroy_entry=false. The
			 * video_hw_upgrade_failed latch stops any further
			 * pool acquire for this sfd. The frame is lost (its
			 * buffer was released render=0), and the packet that
			 * produced it was already consumed by the hw decoder;
			 * the demotion mirrors the blit-failure rung-6 path. */
			char why[48];
			snprintf(why, sizeof(why), "blit err=%d", blit_err);
			android_demote_to_sw(sfd, rd, why);
		}
		return;
	}
#endif

	struct AVFrame *cpu_frame = last_is_hw
			? sfd->video_tmp_frame
			: sfd->video_last_frame;
	if (!cpu_frame) {
		return;
	}

		if (!sfd->video_color_context) {
			if (setup_color_conv(sfd, cpu_frame) == -1) {
				return;
			}
		}
		/* Coded-size policy. The color-conversion context and
		 * intermediate were built for the first decoded
		 * frame's coded dimensions; the dmabuf copy below
		 * crops to dmabuf_info (the real surface size), so a
		 * remote encoder's 16-px coded-frame padding is
		 * absorbed there and never reaches the compositor.
		 * Drop only the cases that would make the fixed-size
		 * conversion write/read out of bounds: a mid-stream
		 * coded-size CHANGE after setup, or a decoded frame
		 * SMALLER than the real surface target. */
		if (cpu_frame->width !=
				    sfd->video_local_frame->width ||
				cpu_frame->height !=
				    sfd->video_local_frame->height) {
			wp_error("Decoded frame %dx%d mismatches setup %dx%d for RID=%d; dropping",
					cpu_frame->width,
					cpu_frame->height,
					sfd->video_local_frame->width,
					sfd->video_local_frame->height,
					sfd->remote_id);
			return;
		}
		if (cpu_frame->width <
					    (int)sfd->dmabuf_info.width ||
				cpu_frame->height <
					    (int)sfd->dmabuf_info.height) {
			wp_error("Decoded frame %dx%d smaller than surface %ux%u for RID=%d; dropping",
					cpu_frame->width,
					cpu_frame->height,
					sfd->dmabuf_info.width,
					sfd->dmabuf_info.height,
					sfd->remote_id);
			return;
		}


		if (lat_video_on) {
			lat_f0 = wp_lat_now_us();
		}
		/* Convert the held latest frame into the mirror. */
		if (sws_scale(sfd->video_color_context,
				    (const uint8_t *const *)
						    cpu_frame->data,
				    cpu_frame->linesize, 0,
				    cpu_frame->height,
				    sfd->video_local_frame->data,
				    sfd->video_local_frame->linesize) <
				0) {
			wp_error("Failed to perform color conversion");
		}

		if (!sfd->dmabuf_bo) {
			// ^ was not previously able to create buffer
			wp_error("DMABUF was not created");
			return;
		}
		/* Copy data onto DMABUF */
		uint32_t map_stride = 0;
		void *handle = NULL;
		/* B5: this is a CPU write into the dma-buf; bracket it
		 * with DMA_BUF_IOCTL_SYNC so the implicit fence orders
		 * the write against the compositor sampling the buffer
		 * (without it the surface can render stale/white). */
		dmabuf_sync_start(sfd->fd_local, true);
		void *data = map_dmabuf(sfd->dmabuf_bo, true, &handle,
				&map_stride);
		if (!data) {
			dmabuf_sync_end(sfd->fd_local, true);
			return;
		}
		copy_from_video_mirror(data, map_stride,
				sfd->video_local_frame,
				&sfd->dmabuf_info);
		unmap_dmabuf(sfd->dmabuf_bo, handle);
		dmabuf_sync_end(sfd->fd_local, true);
		if (lat_video_on) {
			wp_lat_video_record(&g_lat_video,
					wp_lat_now_us() - lat_f0);
		}
}


#endif /* HAS_VIDEO && HAS_DMABUF */
