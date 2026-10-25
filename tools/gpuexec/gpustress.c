/*
 * gpustress - finds which NMXGPU kernel path wears out under repetition (10-17: Metal froze the whole Mac after ~3000
 * GPU submissions, with and without a window). It talks to the i915 render node directly - no Mesa, no Metal - and runs
 * ONE path in a loop. Progress is written to ~/gpustress.txt with fsync every 100 iterations, so after a hard freeze
 * the file shows how far it got.
 *   ./gpustress submit  [N]   execbuf + GEM_WAIT on the same two buffers, N times              (default 10000)
 *   ./gpustress burst   [N]   execbuf without waiting, a wait every 16 (like Mesa's pipelining)
 *   ./gpustress syncobj [N]   execbuf that signals a syncobj + SYNCOBJ_WAIT + reset (Mesa's fence path)
 *   ./gpustress alloc   [N]   create + map + unmap + close a buffer (no GPU work)
 */
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

struct i915_gem_create { uint64_t size; uint32_t handle; uint32_t pad; };
struct gem_close { uint32_t handle; uint32_t pad; };
struct i915_gem_mmap_offset { uint32_t handle; uint32_t pad; uint64_t offset; uint64_t flags; uint64_t extensions; };
struct exec_object2 { uint32_t handle, relocation_count; uint64_t relocs_ptr, alignment, offset, flags, rsvd1, rsvd2; };
struct execbuffer2 {
    uint64_t buffers_ptr; uint32_t buffer_count, batch_start_offset, batch_len, DR1, DR4, num_cliprects;
    uint64_t cliprects_ptr, flags, rsvd1, rsvd2;
};
struct gem_wait { uint32_t bo_handle, flags; int64_t timeout_ns; };
struct exec_fence { uint32_t handle, flags; };
struct syncobj_create { uint32_t handle, flags; };
struct syncobj_destroy { uint32_t handle, pad; };
struct syncobj_wait { uint64_t handles; int64_t timeout_nsec; uint32_t count_handles, flags, first_signaled, pad; };
struct syncobj_array { uint64_t handles; uint32_t count_handles, pad; };
struct nmx_drm_mmap { uint64_t offset, size; uint32_t prot, flags; uint64_t addr; };
struct nmx_drm_munmap { uint64_t addr, size; };

#define CMD_BASE 0x40
#define IOC_GEM_CLOSE        _IOW('d', 0x09, struct gem_close)
#define IOC_GEM_CREATE       _IOWR('d', CMD_BASE + 0x1b, struct i915_gem_create)
#define IOC_MMAP_OFFSET      _IOWR('d', CMD_BASE + 0x24, struct i915_gem_mmap_offset)
#define IOC_EXECBUF2         _IOWR('d', CMD_BASE + 0x29, struct execbuffer2)
#define IOC_GEM_WAIT         _IOWR('d', CMD_BASE + 0x2c, struct gem_wait)
#define IOC_SYNCOBJ_CREATE   _IOWR('d', 0xBF, struct syncobj_create)
#define IOC_SYNCOBJ_DESTROY  _IOWR('d', 0xC0, struct syncobj_destroy)
#define IOC_SYNCOBJ_WAIT     _IOWR('d', 0xC3, struct syncobj_wait)
#define IOC_SYNCOBJ_RESET    _IOWR('d', 0xC4, struct syncobj_array)
#define NMX_MMAP             _IOWR('N', 0x40, struct nmx_drm_mmap)
#define NMX_MUNMAP           _IOW('N', 0x41, struct nmx_drm_munmap)

#define EXEC_OBJECT_WRITE   (1u << 2)
#define EXEC_OBJECT_48B     (1u << 3)
#define EXEC_OBJECT_PINNED  (1u << 4)
#define EXEC_NO_RELOC       (1u << 11)
#define EXEC_FENCE_ARRAY    (1u << 19)
#define EXEC_FENCE_SIGNAL   (1u << 1)

static int fd, logfd = -1;
static const char *mode;

static double now_s(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }

static void mark(const char *fmt, ...)
{
    char b[300]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    printf("%s", b); fflush(stdout);
    if (logfd >= 0) { write(logfd, b, n); fsync(logfd); }
}

static uint32_t bo_new(uint64_t size)
{
    struct i915_gem_create gc = { size, 0, 0 };
    return ioctl(fd, IOC_GEM_CREATE, &gc) ? 0 : gc.handle;
}
static void bo_close(uint32_t h) { struct gem_close c = { h, 0 }; ioctl(fd, IOC_GEM_CLOSE, &c); }
static volatile uint32_t *bo_map(uint32_t h, uint64_t size)
{
    static const uint64_t modes[] = { 2, 1, 3, 0 };
    for (unsigned i = 0; i < 4; i++) {
        struct i915_gem_mmap_offset mo = { h, 0, 0, modes[i], 0 };
        if (ioctl(fd, IOC_MMAP_OFFSET, &mo)) continue;
        struct nmx_drm_mmap m = { mo.offset, size, PROT_READ | PROT_WRITE, 0, 0 };
        if (ioctl(fd, NMX_MMAP, &m) == 0 && m.addr) return (volatile uint32_t *)(uintptr_t)m.addr;
    }
    return NULL;
}
static void bo_unmap(volatile uint32_t *p, uint64_t size)
{
    struct nmx_drm_munmap u = { (uint64_t)(uintptr_t)p, size };
    ioctl(fd, NMX_MUNMAP, &u);
}

int main(int argc, char **argv)
{
    mode = argc > 1 ? argv[1] : "submit";
    long n = argc > 2 ? atol(argv[2]) : 10000;
    // optional: [pace ms] sleeps between submissions (16 = a 60 fps app: the GPU idles between frames, like mtlwindow);
    //           [work] makes each batch do that many extra GPU stores first, so every wait really sleeps on the interrupt
    const long pace_ms = argc > 3 ? atol(argv[3]) : 0, work = argc > 4 ? atol(argv[4]) : 0;
    const uint64_t bbsize = work ? (((uint64_t)work * 16 + 64 + 4095) & ~4095ull) : 4096;
    fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) { printf("open renderD128: %s\n", strerror(errno)); return 1; }
    char lp[512]; snprintf(lp, sizeof lp, "%s/gpustress.txt", getenv("HOME") ? getenv("HOME") : "/tmp");
    logfd = open(lp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    mark("gpustress %s x %ld, pace %ld ms, work %ld stores (progress also in %s)\n", mode, n, pace_ms, work, lp);

    const int is_alloc = !strcmp(mode, "alloc"), is_burst = !strcmp(mode, "burst"), is_sync = !strcmp(mode, "syncobj");
    if (!is_alloc && !is_burst && !is_sync && strcmp(mode, "submit")) { printf("unknown mode %s\n", mode); return 1; }

    uint32_t tgt = 0, bb = 0, so = 0;
    volatile uint32_t *t = NULL, *b = NULL;
    const uint64_t tgt_gpu = 0x100000, bb_gpu = 0x200000;
    if (!is_alloc) {
        tgt = bo_new(4096); bb = bo_new(bbsize);
        t = tgt ? bo_map(tgt, 4096) : NULL; b = bb ? bo_map(bb, bbsize) : NULL;
        if (b && work) {     // the busy work: `work` stores into the target's second cache line, written once
            for (long w = 0; w < work; w++) {
                b[w * 4] = (0x20u << 23) | 2; b[w * 4 + 1] = (uint32_t)(tgt_gpu + 64); b[w * 4 + 2] = 0; b[w * 4 + 3] = (uint32_t)w;
            }
        }
        if (!t || !b) { mark("FAIL: buffer create/map\n"); return 2; }
        if (is_sync) {
            struct syncobj_create sc = { 0, 0 };
            if (ioctl(fd, IOC_SYNCOBJ_CREATE, &sc)) { mark("FAIL: syncobj create: %s\n", strerror(errno)); return 2; }
            so = sc.handle;
        }
    }

    double t0 = now_s(), tl = t0;
    long fails = 0;
    for (long i = 1; i <= n; i++) {
        if (is_alloc) {
            uint32_t h = bo_new(65536);
            volatile uint32_t *p = h ? bo_map(h, 65536) : NULL;
            if (!p) { mark("FAIL at %ld: create/map (%s)\n", i, strerror(errno)); return 3; }
            p[0] = (uint32_t)i;
            bo_unmap(p, 65536);
            bo_close(h);
        } else {
            const uint32_t value = 0xA5000000u | (uint32_t)(i & 0xFFFFFF);
            int k = (int)(work * 4);
            b[k++] = (0x20u << 23) | 2; b[k++] = (uint32_t)tgt_gpu; b[k++] = (uint32_t)(tgt_gpu >> 32); b[k++] = value;
            b[k++] = 0x0Au << 23; b[k++] = 0;
            __sync_synchronize();
            struct exec_object2 obj[2];
            memset(obj, 0, sizeof obj);
            obj[0].handle = tgt; obj[0].offset = tgt_gpu; obj[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_48B | EXEC_OBJECT_WRITE;
            obj[1].handle = bb;  obj[1].offset = bb_gpu;  obj[1].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_48B;
            struct exec_fence ef = { so, EXEC_FENCE_SIGNAL };
            struct execbuffer2 eb;
            memset(&eb, 0, sizeof eb);
            eb.buffers_ptr = (uint64_t)(uintptr_t)obj; eb.buffer_count = 2; eb.batch_len = (uint32_t)(k * 4);
            eb.flags = 1 /* render */ | EXEC_NO_RELOC;
            if (is_sync) { eb.flags |= EXEC_FENCE_ARRAY; eb.cliprects_ptr = (uint64_t)(uintptr_t)&ef; eb.num_cliprects = 1; }
            if (ioctl(fd, IOC_EXECBUF2, &eb)) { mark("FAIL at %ld: execbuf (%s)\n", i, strerror(errno)); return 3; }
            int waited = 0;
            if (is_sync) {
                struct syncobj_wait w = { (uint64_t)(uintptr_t)&so,
                                          (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 2000000000LL, 1, 0, 0, 0 };
                if (ioctl(fd, IOC_SYNCOBJ_WAIT, &w)) { fails++; if (fails < 5) mark("  syncobj wait at %ld: %s\n", i, strerror(errno)); }
                struct syncobj_array r = { (uint64_t)(uintptr_t)&so, 1, 0 };
                ioctl(fd, IOC_SYNCOBJ_RESET, &r);
                waited = 1;
            } else if (!is_burst || (i % 16) == 0) {
                struct gem_wait w = { tgt, 0, 2000000000LL };
                if (ioctl(fd, IOC_GEM_WAIT, &w)) { fails++; if (fails < 5) mark("  gem wait at %ld: %s\n", i, strerror(errno)); }
                waited = 1;
            }
            if (waited && t[0] != value) { fails++; if (fails < 5) mark("  value at %ld: 0x%08x, expected 0x%08x\n", i, t[0], value); }
        }
        if (pace_ms) usleep((useconds_t)pace_ms * 1000);
        if (i % 100 == 0) {
            double nw = now_s();
            struct rusage ru; getrusage(RUSAGE_SELF, &ru);
            mark("%6ld done  %.0f per s  fails %ld  rss %ld KB\n", i, 100 / (nw - tl), fails, (long)(ru.ru_maxrss / 1024));
            tl = nw;
        }
    }
    mark("RESULT: %s %ld iterations in %.1f s, %ld failures - no freeze\n", mode, n, now_s() - t0, fails);
    return fails ? 4 : 0;
}
