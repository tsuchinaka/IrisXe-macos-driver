/* Force-included into every Mesa source (-include): mmap/munmap calls go through nmx_drm_mmap/nmx_drm_munmap
 * (nmx_mmap_impl.c), which map a /dev/dri node through NMXGPU's map ioctl. A kext cannot implement mmap(2) for a
 * device node, and the Metal plugin dlopen()s ANV into apps, where DYLD_INSERT_LIBRARIES (nmxmmap.dylib) is not
 * available. Everything that is not a DRM node passes straight to the system mmap. */
#ifndef NMX_MMAP_REDIRECT_H
#define NMX_MMAP_REDIRECT_H
/* not in assembly sources (blake3 .S files get c_args too) */
#if defined(__APPLE__) && !defined(NMX_MMAP_IMPL) && !defined(__ASSEMBLER__)
#include <sys/types.h>
#include <sys/mman.h>
#ifdef __cplusplus
extern "C" {
#endif
void *nmx_drm_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int nmx_drm_munmap(void *addr, size_t len);
#ifdef __cplusplus
}
#endif
#define mmap nmx_drm_mmap
#define munmap nmx_drm_munmap
#endif
#endif
