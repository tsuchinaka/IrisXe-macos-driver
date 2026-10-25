/*
 * nmxmmap.dylib - lets Mesa map GPU memory on macOS. A kext cannot implement mmap(2) for a device node, so NMXGPU maps
 * GPU buffers into a process through an ioctl (NMX_DRM_IOCTL_MMAP, gpu-port/xkpi/bsdcompat/include/nmx_drm_uapi.h).
 * Mesa's Intel Vulkan driver calls mmap(2) on the DRM node directly; this library, loaded with DYLD_INSERT_LIBRARIES,
 * interposes mmap/munmap and turns a mapping of a /dev/dri node into that ioctl. Everything else passes through.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

struct nmx_drm_mmap { uint64_t offset, size; uint32_t prot, flags; uint64_t addr; };
struct nmx_drm_munmap { uint64_t addr, size; };
#define NMX_DRM_IOCTL_MMAP _IOWR('N', 0x40, struct nmx_drm_mmap)
#define NMX_DRM_IOCTL_MUNMAP _IOW('N', 0x41, struct nmx_drm_munmap)
#define NMX_DRM_MMAP_FIXED 0x1

struct m { uintptr_t addr; size_t len; int fd; struct m *next; };
static struct m *maps;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

static int is_dri(int fd)
{
    static int major_dri = -2;
    if (major_dri == -2) {
        struct stat st;
        major_dri = stat("/dev/dri/renderD128", &st) == 0 ? (int)major(st.st_rdev) : -1;
    }
    struct stat fs;
    return major_dri >= 0 && fd >= 0 && fstat(fd, &fs) == 0 && S_ISCHR(fs.st_mode) && (int)major(fs.st_rdev) == major_dri;
}

static int dbg = -1;
#define LOG(...) do { if (dbg < 0) dbg = getenv("NMXMMAP_DEBUG") != NULL; if (dbg) fprintf(stderr, "nmxmmap: " __VA_ARGS__); } while (0)

static void *nmx_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (!is_dri(fd))
        return mmap(addr, len, prot, flags, fd, off);
    int e0 = 0;
    struct nmx_drm_mmap rq = { (uint64_t)off, (uint64_t)len, (uint32_t)(prot & (PROT_READ | PROT_WRITE)),
                               (flags & MAP_FIXED) ? NMX_DRM_MMAP_FIXED : 0, (uint64_t)(uintptr_t)addr };
    if (ioctl(fd, NMX_DRM_IOCTL_MMAP, &rq) != 0) {
        e0 = errno;
        LOG("mmap fd %d off 0x%llx len 0x%zx prot %d flags 0x%x addr %p -> FAILED %s\n", fd, (unsigned long long)off, len,
            prot, flags, addr, strerror(e0));
        errno = e0;
        return MAP_FAILED;
    }
    LOG("mmap fd %d off 0x%llx len 0x%zx -> %p\n", fd, (unsigned long long)off, len, (void *)(uintptr_t)rq.addr);
    struct m *e = malloc(sizeof *e);
    if (e) {
        e->addr = (uintptr_t)rq.addr; e->len = len; e->fd = fd;
        pthread_mutex_lock(&lk); e->next = maps; maps = e; pthread_mutex_unlock(&lk);
    }
    return (void *)(uintptr_t)rq.addr;
}

static int nmx_munmap(void *addr, size_t len)
{
    struct m **pp, *e = NULL;
    pthread_mutex_lock(&lk);
    for (pp = &maps; *pp; pp = &(*pp)->next)
        if ((*pp)->addr == (uintptr_t)addr) { e = *pp; *pp = e->next; break; }
    pthread_mutex_unlock(&lk);
    if (e == NULL)
        return munmap(addr, len);
    struct nmx_drm_munmap rq = { (uint64_t)e->addr, (uint64_t)e->len };
    int r = ioctl(e->fd, NMX_DRM_IOCTL_MUNMAP, &rq);
    free(e);
    return r;
}

#define INTERPOSE(n, o) \
    __attribute__((used)) static struct { const void *a, *b; } _interpose_##o \
    __attribute__((section("__DATA,__interpose"))) = { (const void *)(unsigned long)&n, (const void *)(unsigned long)&o }
INTERPOSE(nmx_mmap, mmap);
INTERPOSE(nmx_munmap, munmap);
