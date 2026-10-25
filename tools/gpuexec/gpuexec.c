/*
 * gpuexec - second acceleration check for NMXGPU on Iris Xe: makes the GPU itself run a tiny command buffer.
 * The batch holds one MI_STORE_DWORD_IMM: "write 0xC0FFEE42 into this buffer", then MI_BATCH_BUFFER_END. If the value
 * appears in the buffer afterwards, the GPU fetched and executed our commands (the path every Vulkan/Metal draw
 * takes). Buffers are soft-pinned at fixed GPU addresses (no relocations). It runs on the render, copy and video
 * engines in turn. Nothing is drawn on screen.
 *   ./gpuexec [/dev/dri/renderD128]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
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
struct nmx_drm_mmap { uint64_t offset, size; uint32_t prot, flags; uint64_t addr; };
struct nmx_drm_munmap { uint64_t addr, size; };

#define CMD_BASE 0x40
#define IOC_GEM_CLOSE   _IOW('d', 0x09, struct gem_close)
#define IOC_GEM_CREATE  _IOWR('d', CMD_BASE + 0x1b, struct i915_gem_create)
#define IOC_MMAP_OFFSET _IOWR('d', CMD_BASE + 0x24, struct i915_gem_mmap_offset)
#define IOC_EXECBUF2    _IOWR('d', CMD_BASE + 0x29, struct execbuffer2)
#define IOC_GEM_WAIT    _IOWR('d', CMD_BASE + 0x2c, struct gem_wait)
#define NMX_MMAP        _IOWR('N', 0x40, struct nmx_drm_mmap)
#define NMX_MUNMAP      _IOW('N', 0x41, struct nmx_drm_munmap)

#define EXEC_OBJECT_WRITE      (1u << 2)
#define EXEC_OBJECT_48B        (1u << 3)
#define EXEC_OBJECT_PINNED     (1u << 4)
#define EXEC_NO_RELOC          (1u << 11)

static int fd;

static uint32_t bo_new(void)
{
    struct i915_gem_create gc = { 4096, 0, 0 };
    if (ioctl(fd, IOC_GEM_CREATE, &gc)) { printf("[FAIL] buffer create: %s\n", strerror(errno)); return 0; }
    return gc.handle;
}

static volatile uint32_t *bo_map(uint32_t h)
{
    static const uint64_t modes[] = { 2 /* WC */, 1 /* WB */, 3 /* UC */, 0 /* GTT */ };
    for (unsigned i = 0; i < 4; i++) {
        struct i915_gem_mmap_offset mo = { h, 0, 0, modes[i], 0 };
        if (ioctl(fd, IOC_MMAP_OFFSET, &mo)) continue;
        struct nmx_drm_mmap m = { mo.offset, 4096, PROT_READ | PROT_WRITE, 0, 0 };
        if (ioctl(fd, NMX_MMAP, &m) == 0 && m.addr) return (volatile uint32_t *)(uintptr_t)m.addr;
        printf("       map mode %llu: %s\n", (unsigned long long)modes[i], strerror(errno));
    }
    printf("[FAIL] buffer CPU map: no mapping mode worked\n");
    return NULL;
}

static int run(const char *name, uint64_t engine, uint32_t value)
{
    uint32_t tgt = bo_new(), bb = bo_new();
    if (!tgt || !bb) return 1;
    volatile uint32_t *t = bo_map(tgt), *b = bo_map(bb);
    if (!t || !b) return 1;
    const uint64_t tgt_gpu = 0x100000, bb_gpu = 0x200000;    /* soft-pinned GPU virtual addresses */
    t[0] = 0x11111111;
    int i = 0;
    b[i++] = (0x20u << 23) | 2;                              /* MI_STORE_DWORD_IMM, 64-bit address, 4 dwords */
    b[i++] = (uint32_t)tgt_gpu;
    b[i++] = (uint32_t)(tgt_gpu >> 32);
    b[i++] = value;
    b[i++] = 0x0Au << 23;                                    /* MI_BATCH_BUFFER_END */
    b[i++] = 0;                                              /* MI_NOOP: keep the length a multiple of 8 bytes */
    __sync_synchronize();

    struct exec_object2 obj[2];
    memset(obj, 0, sizeof obj);
    obj[0].handle = tgt; obj[0].offset = tgt_gpu; obj[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_48B | EXEC_OBJECT_WRITE;
    obj[1].handle = bb;  obj[1].offset = bb_gpu;  obj[1].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_48B;
    struct execbuffer2 eb;
    memset(&eb, 0, sizeof eb);
    eb.buffers_ptr = (uint64_t)(uintptr_t)obj;
    eb.buffer_count = 2;
    eb.batch_len = (uint32_t)(i * 4);
    eb.flags = engine | EXEC_NO_RELOC;
    if (ioctl(fd, IOC_EXECBUF2, &eb)) { printf("[FAIL] %-6s submit: %s\n", name, strerror(errno)); return 1; }
    struct gem_wait w = { tgt, 0, 3000000000LL };
    int wr = ioctl(fd, IOC_GEM_WAIT, &w);
    uint32_t got = t[0];
    int ok = wr == 0 && got == value;
    printf("[%s] %-6s engine: wait %s, buffer reads 0x%08x (expected 0x%08x)\n", ok ? " OK " : "FAIL", name,
           wr ? strerror(errno) : "done", got, value);
    struct gem_close c1 = { tgt, 0 }, c2 = { bb, 0 };
    ioctl(fd, IOC_GEM_CLOSE, &c1);
    ioctl(fd, IOC_GEM_CLOSE, &c2);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    fd = open(path, O_RDWR);
    if (fd < 0) { printf("[FAIL] open %s: %s\n", path, strerror(errno)); return 1; }
    int fails = 0;
    fails += run("render", 1 /* I915_EXEC_RENDER */, 0xC0FFEE42);
    fails += run("copy",   3 /* I915_EXEC_BLT */,    0xB17B17B1);
    fails += run("video",  2 /* I915_EXEC_BSD */,    0x0DEC0DE5);
    close(fd);
    printf("\nRESULT: %s (%d of 3 engines failed)\n", fails ? "some engines failed" : "the GPU executed our commands", fails);
    return fails ? 2 : 0;
}
