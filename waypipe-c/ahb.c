#include "ahb.h"
#include "dmabuf.h"
#include "shadow.h"

#ifdef __ANDROID__

#include <android/hardware_buffer.h>
#include <errno.h>

int ahb_readback_fallback(struct shadow_fd *sfd) {
    AHardwareBuffer *ahb = NULL;
    AHardwareBuffer_Desc desc = {
        .width = sfd->dmabuf_info.width,
        .height = sfd->dmabuf_info.height,
        .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
    };

    // Try to import the dmabuf into an AHardwareBuffer.
    // AHardwareBuffer_fromHandle or equivalent is not directly
    // available in the NDK; instead we create an AHB and blit.
    // For now this serves as a placeholder — real implementation
    // will use vendor-specific gralloc interop.
    //
    // Fallback: try mmap with PROT_READ|PROT_WRITE if available.
    errno = EOPNOTSUPP;
    (void)desc;
    return -1;
}

void ahb_cleanup(struct shadow_fd *sfd) {
    (void)sfd;
}

#else

#include <errno.h>

int ahb_readback_fallback(struct shadow_fd *sfd) {
    (void)sfd;
    errno = EOPNOTSUPP;
    return -1;
}

void ahb_cleanup(struct shadow_fd *sfd) {
    (void)sfd;
}

#endif /* __ANDROID__ */
