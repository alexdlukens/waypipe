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

#ifdef __ANDROID__
/* The GDExtension .so is loaded by Godot's Android Java layer via
 * System.loadLibrary, so exporting JNI_OnLoad is the reliable way to capture
 * the JavaVM for ffmpeg's MediaCodec decoders. JNI_GetCreatedJavaVMs is not
 * used: the NDK sysroot has no libnativehelper, so the call would be an
 * unresolved link. */
#include <jni.h>
static JavaVM *gdwaypipe_android_jvm = NULL;
JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
	(void)reserved;
	gdwaypipe_android_jvm = vm;
	return JNI_VERSION_1_6;
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
	if (!fn) {
		return;
	}
	char buf[1024];
	int len = vsnprintf(buf, 1023, fmt, args);
	while (buf[len - 1] == '\n' && len > 1) {
		buf[len - 1] = 0;
		len--;
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
		avcodec_free_context(&sfd->video_context);
		sws_freeContext(sfd->video_color_context);
		if (sfd->video_yuv_frame_data) {
			av_freep(sfd->video_yuv_frame_data);
		}
		if (sfd->video_local_frame_data) {
			av_freep(sfd->video_local_frame_data);
		}
		av_frame_free(&sfd->video_local_frame);
		av_frame_free(&sfd->video_tmp_frame);
		av_frame_free(&sfd->video_yuv_frame);
		av_packet_free(&sfd->video_packet);
	}
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
	/* The JavaVM captured by JNI_OnLoad must be handed to ffmpeg before
	 * any MediaCodec decoder is opened; without it decoder init fails.
	 * The JVM can only be set once, so a repeated registration with the
	 * same VM (AVERROR(EEXIST)) counts as success too. */
	if (gdwaypipe_android_jvm) {
		int jerr = av_jni_set_java_vm(gdwaypipe_android_jvm, NULL);
		if (jerr < 0 && jerr != AVERROR(EEXIST)) {
			wp_error("Failed to register Android JVM with ffmpeg: %s",
					av_err2str(jerr));
			rd->av_disabled = true;
			return -1;
		}
	} else {
		wp_debug("Android JVM not captured (JNI_OnLoad not called); "
			 "using software decode");
		rd->av_disabled = true;
		return -1;
	}
	/* A surface-less MediaCodec device runs the decoders in ByteBuffer
	 * mode, so decoded frames come out as plain CPU frames (NV12) and
	 * the existing sws_scale path applies directly. */
	if (av_hwdevice_ctx_create(&rd->av_hwdevice_ref,
			AV_HWDEVICE_TYPE_MEDIACODEC, NULL, NULL, 0) < 0) {
		wp_error("Failed to create MediaCodec hardware device");
		rd->av_disabled = true;
		return -1;
	}
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

int setup_video_decode(struct shadow_fd *sfd, struct render_data *rd)
{
	/* Re-entry guard: a duplicate OPEN_DMAVID_DST must not leak or
	 * replace a live context. */
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

	struct AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) {
		wp_error("Failed to allocate context");
		return -1;
	}

	ctx->delay = 0;
	if (has_hw && hw_supported) {
		/* If alignment permits, use hardware decoding */
		if (!pad_hardware_size((int)sfd->dmabuf_info.width,
					(int)sfd->dmabuf_info.height,
					&ctx->width, &ctx->height)) {
			has_hw = false;
			hw_supported = false;
		}
	} else {
		has_hw = false;
		hw_supported = false;
	}

	if (has_hw && hw_supported) {
		ctx->hw_device_ctx = av_buffer_ref(rd->av_hwdevice_ref);
		if (!ctx->hw_device_ctx) {
			wp_error("Failed to reference hardware device context");
			has_hw = false;
			hw_supported = false;
		} else {
			ctx->get_format = get_decode_format;
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
			wp_debug("HW decode open failed for RID=%d (%s); retrying in software",
					sfd->remote_id, av_err2str(open_err));
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
	/* adopt padded sizes */
	local_frame->width = ctx->width;
	local_frame->height = ctx->height;
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

void apply_video_packet(struct shadow_fd *sfd, struct render_data *rd,
		const struct bytebuf *msg)
{
	sfd->video_packet->data = (uint8_t *)msg->data;
	sfd->video_packet->size = (int)msg->size;

	int sendstat = avcodec_send_packet(
			sfd->video_context, sfd->video_packet);
	if (sendstat < 0) {
		wp_error("Failed to send packet: %s", av_err2str(sendstat));
	}

	/* Receive all produced frames, ignoring all but the most recent */
	while (true) {
		int recvstat = avcodec_receive_frame(
				sfd->video_context, sfd->video_yuv_frame);
		if (recvstat == 0) {
			struct AVFrame *cpu_frame = sfd->video_yuv_frame;
#if HAS_VAAPI
			if (sfd->video_va_surface &&
					sfd->video_yuv_frame->format ==
							AV_PIX_FMT_VAAPI) {
				run_vaapi_conversion(
						sfd, rd, sfd->video_yuv_frame);
				continue;
			}
#else
			(void)rd;
#endif

			if (sfd->video_yuv_frame->format == AV_PIX_FMT_VAAPI ||
					sfd->video_yuv_frame->format ==
							AV_PIX_FMT_MEDIACODEC) {
				if (!sfd->video_tmp_frame) {
					sfd->video_tmp_frame = av_frame_alloc();
					if (!sfd->video_tmp_frame) {
						wp_error("Failed to allocate temporary frame");
					}
				}

				int tferr = av_hwframe_transfer_data(
						sfd->video_tmp_frame,
						sfd->video_yuv_frame, 0);
				if (tferr < 0) {
					wp_error("Failed to transfer hwframe data: %s",
							av_err2str(tferr));
				}
				cpu_frame = sfd->video_tmp_frame;
			}
			if (!cpu_frame) {
				return;
			}

			if (!sfd->video_color_context) {
				if (setup_color_conv(sfd, cpu_frame) == -1) {
					return;
				}
			}
			/* The DMABUF-shaped local frame was sized once at setup;
			 * if the decoder emits a frame whose dimensions differ,
			 * the color conversion would write out of bounds. Drop
			 * the frame rather than corrupting the surface. */
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


			/* Handle frame immediately, since the next receive run
			 * will clear it again */
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
		} else {
			if (recvstat != AVERROR(EAGAIN)) {
				wp_error("Failed to receive frame due to error: %s",
						av_err2str(recvstat));
			}
			break;
		}
	}
}

#endif /* HAS_VIDEO && HAS_DMABUF */
