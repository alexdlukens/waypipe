#ifndef WAYPIPE_AHB_H
#define WAYPIPE_AHB_H

struct shadow_fd;

/**
 * ahb_readback_fallback - On Android, when direct mmap of a dmabuf FD fails
 * (EACCES), attempt to read back pixel data using AHardwareBuffer_lock().
 * Stores the mapped pointer in sfd->mem_local and the AHB handle in
 * sfd->ahb_handle for later cleanup.
 * Returns 0 on success, -1 on failure.
 */
int ahb_readback_fallback(struct shadow_fd *sfd);

/**
 * ahb_cleanup - Unlock and release the AHardwareBuffer, if one was created.
 */
void ahb_cleanup(struct shadow_fd *sfd);

#endif /* WAYPIPE_AHB_H */
