/*
 * drmprobe - first acceleration check for NMXGPU on Iris Xe: talks to the i915 render node the way Mesa ANV does,
 * one step at a time, and says which step fails. No libdrm needed. Read-mostly: it creates one 4 KiB buffer and one
 * GPU context and frees them again.
 *   ./drmprobe            (uses /dev/dri/renderD128)
 *   ./drmprobe /dev/dri/card0
 * ioctl numbers are BSD-encoded (sys/ioccom.h), as FreeBSD's drm-kmod and NMXGPU use.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ioccom.h>
#include <unistd.h>

struct drm_version { int major, minor, patch; size_t name_len; char *name; size_t date_len; char *date; size_t desc_len; char *desc; };
struct i915_getparam { int32_t param; int *value; };
struct i915_gem_create { uint64_t size; uint32_t handle; uint32_t pad; };
struct gem_close { uint32_t handle; uint32_t pad; };
struct i915_ctx_create { uint32_t ctx_id; uint32_t pad; };
struct i915_ctx_destroy { uint32_t ctx_id; uint32_t pad; };
struct i915_gem_mmap_offset { uint32_t handle; uint32_t pad; uint64_t offset; uint64_t flags; uint64_t extensions; };

#define CMD_BASE 0x40
#define IOC_VERSION        _IOWR('d', 0x00, struct drm_version)
#define IOC_GEM_CLOSE      _IOW('d', 0x09, struct gem_close)
#define IOC_GETPARAM       _IOWR('d', CMD_BASE + 0x06, struct i915_getparam)
#define IOC_GEM_CREATE     _IOWR('d', CMD_BASE + 0x1b, struct i915_gem_create)
#define IOC_CTX_CREATE     _IOWR('d', CMD_BASE + 0x2d, struct i915_ctx_create)
#define IOC_CTX_DESTROY    _IOW('d', CMD_BASE + 0x2e, struct i915_ctx_destroy)
#define IOC_MMAP_OFFSET    _IOWR('d', CMD_BASE + 0x24, struct i915_gem_mmap_offset)

static int fails;
static void res(const char *what, int ok, const char *detail)
{
    printf("[%s] %-28s %s\n", ok ? " OK " : "FAIL", what, detail);
    if (!ok) fails++;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    char buf[256];
    int fd = open(path, O_RDWR);
    snprintf(buf, sizeof buf, "%s%s%s", path, fd < 0 ? ": " : "", fd < 0 ? strerror(errno) : "");
    res("open render node", fd >= 0, buf);
    if (fd < 0) return 1;

    char name[64] = "", date[64] = "", desc[128] = "";
    struct drm_version v = { 0 };
    v.name = name; v.name_len = sizeof name - 1; v.date = date; v.date_len = sizeof date - 1; v.desc = desc; v.desc_len = sizeof desc - 1;
    int r = ioctl(fd, IOC_VERSION, &v);
    if (r) snprintf(buf, sizeof buf, "%s", strerror(errno));
    else snprintf(buf, sizeof buf, "%s %d.%d.%d (%s)", name, v.major, v.minor, v.patch, desc);
    res("DRM version", r == 0 && strcmp(name, "i915") == 0, buf);

    static const struct { int id; const char *n; } params[] = {
        { 4, "chipset id" }, { 32, "revision" }, { 33, "subslice total" }, { 34, "EU total" },
        { 41, "has exec timeline" }, { 54, "CS timestamp freq" }, { 20, "has scheduler" },
    };
    for (unsigned i = 0; i < sizeof params / sizeof params[0]; i++) {
        int val = -1;
        struct i915_getparam gp = { params[i].id, &val };
        r = ioctl(fd, IOC_GETPARAM, &gp);
        if (r) snprintf(buf, sizeof buf, "%s", strerror(errno));
        else snprintf(buf, sizeof buf, "%d (0x%x)", val, val);
        res(params[i].n, r == 0 && (params[i].id != 4 || val == 0x9a49), buf);
    }

    struct i915_gem_create gc = { 4096, 0, 0 };
    r = ioctl(fd, IOC_GEM_CREATE, &gc);
    if (r) snprintf(buf, sizeof buf, "%s", strerror(errno));
    else snprintf(buf, sizeof buf, "handle %u", gc.handle);
    res("GPU buffer create (4 KiB)", r == 0 && gc.handle != 0, buf);
    if (r == 0) {
        struct i915_gem_mmap_offset mo = { gc.handle, 0, 0, 2 /* WC */, 0 };
        int r2 = ioctl(fd, IOC_MMAP_OFFSET, &mo);
        if (r2) { mo.flags = 4; /* FIXED (discrete-style) */ r2 = ioctl(fd, IOC_MMAP_OFFSET, &mo); }
        snprintf(buf, sizeof buf, "%s", r2 ? strerror(errno) : "offset ok");
        res("buffer CPU mapping offset", r2 == 0, buf);
        struct gem_close cl = { gc.handle, 0 };
        r2 = ioctl(fd, IOC_GEM_CLOSE, &cl);
        res("buffer free", r2 == 0, r2 ? strerror(errno) : "");
    }

    struct i915_ctx_create cc = { 0, 0 };
    r = ioctl(fd, IOC_CTX_CREATE, &cc);
    snprintf(buf, sizeof buf, "%s", r ? strerror(errno) : "context created");
    res("GPU context create", r == 0, buf);
    if (r == 0) {
        struct i915_ctx_destroy cd = { cc.ctx_id, 0 };
        r = ioctl(fd, IOC_CTX_DESTROY, &cd);
        res("GPU context destroy", r == 0, r ? strerror(errno) : "");
    }
    close(fd);
    printf("\n%s: %d step(s) failed\n", fails ? "RESULT" : "RESULT: all steps passed", fails);
    return fails ? 2 : 0;
}
