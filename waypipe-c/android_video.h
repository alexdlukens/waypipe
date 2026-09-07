/* android_video.h — Android MediaCodec surface-mode decode pathway.
 *
 * Frozen contract between video.c (call sites) and android_video.c
 * (implementation). Design: docs section 11 of the 2026-09-07 hw-decode
 * rewrite design (local://android-hw-decode-design.md):
 *   - Decoders run in MediaCodec surface mode, rendering onto a
 *     SurfaceTexture; each decoded frame is blitted (OES-external sample,
 *     crop rect) into the sfd's BGRA AHardwareBuffer via an EGLImage/FBO.
 *   - A process-global decoder pool reuses open MediaCodec instances
 *     across sfd teardown and resize.
 *   - Every failure falls back to the legacy software path; decoding must
 *     never stall (every received MEDIACODEC frame is released).
 *
 * Only compiled on Android; callers guard with __ANDROID__ && HAS_VIDEO.
 */
#ifndef WAYPIPE_ANDROID_VIDEO_H
#define WAYPIPE_ANDROID_VIDEO_H

#if defined(__ANDROID__) && defined(HAS_VIDEO)

#include <stdbool.h>
#include <stdint.h>

struct shadow_fd;
struct render_data;
struct AVCodecContext;
struct AVFrame;

/* True when the environment forces the legacy CPU decode path
 * (GDWAYPIPE_ANDROID_SW=1, read once and cached). */
bool av_android_sw_forced(void);

/* Acquire-or-create a pool entry for `sfd` and return its surface-mode
 * AVCodecContext via `*out_ctx` (pool retains ownership of the context;
 * callers must NOT free it — rebind/teardown goes through the pool API).
 *
 * h264 needs SPS/PPS extradata at avcodec_open2: for the FIRST entry open
 * of a codec, pass the start-code-prefixed blob here (video.c's
 * h264_extract_ps_extradata output). The blob is COPIED into the codec
 * context; ownership stays with the caller, who frees it after this call.
 * Pass NULL/0 when acquiring an already-open entry (flush-only rebind) or
 * for vp9. VP9 always opens with NULL extradata.
 *
 * Per-sfd state (sfd->video_android, EGLImage/FBO target) is created and
 * bound here. On failure (incl. tiny surfaces MediaCodec rejects): returns
 * <0 with no side effects on other sfds — caller falls back per-sfd. */
int av_android_pool_setup(struct shadow_fd *sfd, struct render_data *rd,
		const uint8_t *h264_extradata, int h264_extradata_size,
		struct AVCodecContext **out_ctx);

/* sfd teardown or explicit detach: flush the entry (frames discarded),
 * mark it IDLE for reuse, release the sfd's per-sfd target resources and
 * free sfd->video_android (set to NULL). With destroy_entry=true the
 * underlying MediaCodec/surface/GL resources are torn down instead of
 * pooled (DEAD/LRU paths). Safe on an sfd that never had a pool entry. */
void av_android_pool_release(struct shadow_fd *sfd, bool destroy_entry);

/* Flush the sfd's entry (avcodec_flush_buffers + serial bump) so stale
 * in-flight buffers are discarded before the next stream's packets. The
 * entry stays bound to sfd. No-op without a bound entry. */
void av_android_pool_flush(struct shadow_fd *sfd);

/* Present the newest decoded frame: release the AVMediaCodecBuffer with
 * render=1 onto the entry's SurfaceTexture, updateTexImage, OES blit of
 * the crop rect into the sfd's EGLImage/FBO (BGRA AHB), then glFinish()
 * (phase-1 sync). `hw_frame` is an AV_PIX_FMT_MEDIACODEC frame whose
 * data[3] carries the AVMediaCodecBuffer. Returns 0 on success; <0 on any
 * failure — the caller must fall back to the software ladder for this sfd
 * (the frame is still released render=0 inside on failure). *lat_us, when
 * non-NULL, receives the blit cost in microseconds for wp_lat telemetry. */
int av_android_blit_latest(struct shadow_fd *sfd, struct AVFrame *hw_frame,
		int64_t *lat_us);

/* The sfd's dmabuf target was (re)allocated: (re)build the EGLImage/FBO
 * from the current sfd->dmabuf_bo AHB. Called lazily-safe: no-op without
 * a bound entry, and idempotent when the target is unchanged. */
void av_android_sfd_target_changed(struct shadow_fd *sfd);

#endif /* __ANDROID__ && HAS_VIDEO */
#endif /* WAYPIPE_ANDROID_VIDEO_H */
