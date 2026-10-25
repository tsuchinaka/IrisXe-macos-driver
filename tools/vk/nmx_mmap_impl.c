/* nmx_drm_mmap / nmx_drm_munmap: see nmx_mmap_redirect.h. Same logic as tools/vk/nmxmmap.c, linked into Mesa. */
#define NMX_MMAP_IMPL 1
/* nmx_mmap_redirect.h is force-included before this line: the real calls below must not be redirected */
#undef mmap
#undef munmap
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

struct nmx_drm_mmap_req { uint64_t offset, size; uint32_t prot, flags; uint64_t addr; };
struct nmx_drm_munmap_req { uint64_t addr, size; };
#define NMX_DRM_IOCTL_MMAP _IOWR('N', 0x40, struct nmx_drm_mmap_req)
#define NMX_DRM_IOCTL_MUNMAP _IOW('N', 0x41, struct nmx_drm_munmap_req)

struct nmx_map_ent { uintptr_t addr; size_t len; int fd; struct nmx_map_ent *next; };
static struct nmx_map_ent *nmx_maps;
static pthread_mutex_t nmx_maps_lk = PTHREAD_MUTEX_INITIALIZER;

static int nmx_is_dri(int fd)
{
    static int major_dri = -2;
    if (major_dri == -2) {
        struct stat st;
        major_dri = stat("/dev/dri/renderD128", &st) == 0 ? (int)major(st.st_rdev) : -1;
    }
    struct stat fs;
    return major_dri >= 0 && fd >= 0 && fstat(fd, &fs) == 0 && S_ISCHR(fs.st_mode) && (int)major(fs.st_rdev) == major_dri;
}

void *nmx_drm_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (!nmx_is_dri(fd))
        return mmap(addr, len, prot, flags, fd, off);
    struct nmx_drm_mmap_req rq = { (uint64_t)off, (uint64_t)len, (uint32_t)(prot & (PROT_READ | PROT_WRITE)),
                                   (flags & MAP_FIXED) ? 1u : 0u, (uint64_t)(uintptr_t)addr };
    if (ioctl(fd, NMX_DRM_IOCTL_MMAP, &rq) != 0) {
        if (getenv("NMXMMAP_DEBUG"))
            fprintf(stderr, "nmx_drm_mmap: off 0x%llx len 0x%zx failed: %s\n", (unsigned long long)off, len, strerror(errno));
        return MAP_FAILED;
    }
    struct nmx_map_ent *e = malloc(sizeof *e);
    if (e) {
        e->addr = (uintptr_t)rq.addr; e->len = len; e->fd = fd;
        pthread_mutex_lock(&nmx_maps_lk); e->next = nmx_maps; nmx_maps = e; pthread_mutex_unlock(&nmx_maps_lk);
    }
    return (void *)(uintptr_t)rq.addr;
}

int nmx_drm_munmap(void *addr, size_t len)
{
    struct nmx_map_ent **pp, *e = NULL;
    pthread_mutex_lock(&nmx_maps_lk);
    for (pp = &nmx_maps; *pp; pp = &(*pp)->next)
        if ((*pp)->addr == (uintptr_t)addr) { e = *pp; *pp = e->next; break; }
    pthread_mutex_unlock(&nmx_maps_lk);
    if (e == NULL)
        return munmap(addr, len);
    struct nmx_drm_munmap_req rq = { (uint64_t)e->addr, (uint64_t)e->len };
    int r = ioctl(e->fd, NMX_DRM_IOCTL_MUNMAP, &rq);
    free(e);
    return r;
}
