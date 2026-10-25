/*
 * b80 (10-19): the DRM render node through IOKit for sandboxed processes.
 *
 * WindowServer's and the browsers' GPU sandboxes refuse open("/dev/dri/renderD128") with EPERM, so Mesa ANV found no
 * GPU there (WindowServer log 10-19: "open(renderD128, O_RDWR) FAILED errno 1"). Those sandboxes do allow IOAccelerator
 * user clients: NMXAccel opens the render node for us behind one (NMXGPU's IOAccelNMXDrmBridge).
 *
 * The Vulkan driver (libvulkan_intel.dylib) and libdrm are not changed. After the plugin loads them, their imports of
 * open/close/ioctl/fstat/dup/fcntl are pointed at the functions below (the indirect-symbol tables of those two images
 * only - the rest of the process keeps libc). Every call goes straight to libc first; only an open() of /dev/dri that
 * libc refuses with EPERM/EACCES becomes a bridge connection, represented to Mesa by a placeholder descriptor on
 * /dev/null (readable in every sandbox). ioctls on a placeholder become IOConnectCallMethod; everything else is libc.
 * NVMTL_DRM_BRIDGE=0 turns the hook off; =force uses the bridge even where /dev/dri opens.
 */
#include <IOKit/IOKitLib.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>

#define NMXBR_MAX_FD 4096
static io_connect_t g_br_conn[NMXBR_MAX_FD];
static char *g_br_path[NMXBR_MAX_FD];
static pthread_mutex_t g_br_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_br_mode = -1;            /* 0 off, 1 on refusal, 2 always */
static unsigned long g_br_opens, g_br_ioctls, g_br_fail;

static int nmxbr_mode(void)
{
    if (g_br_mode < 0) {
        const char *e = getenv("NVMTL_DRM_BRIDGE");
        g_br_mode = !e ? 1 : (e[0] == '0' ? 0 : (!strcmp(e, "force") ? 2 : 1));
    }
    return g_br_mode;
}

/* connection refcounts: dup()ed placeholders share one connection (one DRM file, as a dup of a real fd would) */
static io_connect_t g_br_refc_conn[64]; static int g_br_refc[64];
static void nmxbr_ref(io_connect_t c, int d)
{
    for (int i = 0; i < 64; i++) if (g_br_refc_conn[i] == c) {
        g_br_refc[i] += d;
        if (g_br_refc[i] <= 0) { g_br_refc_conn[i] = 0; g_br_refc[i] = 0; IOServiceClose(c); }
        return; }
    if (d > 0) for (int i = 0; i < 64; i++) if (!g_br_refc_conn[i]) { g_br_refc_conn[i] = c; g_br_refc[i] = d; return; }
    if (d < 0) IOServiceClose(c);
}

static io_connect_t nmxbr_conn(int fd)
{
    if (fd < 0 || fd >= NMXBR_MAX_FD) return 0;
    return g_br_conn[fd];
}

static int nmxbr_open(const char *path)
{
    io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching("NMXAccel"));
    if (!svc) { errno = ENODEV; return -1; }
    io_connect_t c = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0x4E560000u | 2u, &c);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        g_br_fail++;
        nvlog("drm bridge: IOServiceOpen(NMXAccel, bridge) -> 0x%x in %s - the new NMXGPU/NMXAccel kexts are needed", kr, getprogname());
        errno = EPERM; return -1;
    }
    uint64_t out = 0; uint32_t n = 1;
    kr = IOConnectCallScalarMethod(c, 0, NULL, 0, &out, &n);
    if (kr != KERN_SUCCESS || out) {
        nvlog("drm bridge: opening the render node over IOKit failed (kr 0x%x, errno %llu)", kr, (unsigned long long)out);
        IOServiceClose(c); errno = kr ? EIO : (int)out; return -1;
    }
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fd >= NMXBR_MAX_FD) { if (fd >= 0) close(fd); IOServiceClose(c); errno = EMFILE; return -1; }
    pthread_mutex_lock(&g_br_lock);
    g_br_conn[fd] = c; free(g_br_path[fd]); g_br_path[fd] = strdup(path);
    nmxbr_ref(c, 1);
    pthread_mutex_unlock(&g_br_lock);
    if (g_br_opens++ < 16)
        nvlog("drm bridge: %s opened through NMXAccel (IOKit) in %s - placeholder fd %d", path, getprogname(), fd);
    return fd;
}

static int nmxbr_hook_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    const int dri = path && !strncmp(path, "/dev/dri/", 9);
    if (dri && nmxbr_mode() == 2) return nmxbr_open(path);
    int fd = open(path, flags, mode);
    if (fd < 0 && dri && (errno == EPERM || errno == EACCES) && nmxbr_mode() == 1) {
        static int said; if (!said++) nvlog("drm bridge: open(%s) refused here (%s) - using the IOKit bridge", path, strerror(errno));
        return nmxbr_open(path);
    }
    return fd;
}

static int nmxbr_hook_ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    io_connect_t c = nmxbr_conn(fd);
    if (!c) return ioctl(fd, req, arg);
    const uint32_t cmd = (uint32_t)req;
    const size_t len = IOCPARM_LEN(cmd);
    uint64_t in[2] = { cmd, (uint64_t)(uintptr_t)arg }, out = 0; uint32_t nout = 1;
    size_t olen = (cmd & IOC_OUT) ? len : 0;
    kern_return_t kr = IOConnectCallMethod(c, 1, in, 2, (cmd & IOC_IN) && len ? arg : NULL, (cmd & IOC_IN) ? len : 0,
                                           &out, &nout, olen ? arg : NULL, olen ? &olen : NULL);
    g_br_ioctls++;
    if (kr != KERN_SUCCESS) {
        static int said; if (said++ < 8) nvlog("drm bridge: ioctl 0x%08x over IOKit -> kr 0x%x", cmd, kr);
        errno = EIO; return -1;
    }
    if (out) {
        /* b81: say which ioctl the kernel refused (vkCreateDevice -> -3 in WindowServer, 10-19) */
        static int said; if (said++ < 40) nvlog("drm bridge: ioctl 0x%08x (nr 0x%02x, %zu B) -> errno %d", cmd, cmd & 0xff, len, (int)out);
        errno = (int)out; return -1;
    }
    return 0;
}

/* b81: Mesa writes why a call failed to stderr, which goes nowhere in WindowServer. Around a call, stderr is a pipe and
   what arrives is logged here. Only in a process that uses the bridge. */
static int nmxbr_err_begin(int *saved, int p[2])
{
    if (!g_br_opens || pipe(p)) return 0;
    fcntl(p[0], F_SETFL, O_NONBLOCK); fcntl(p[1], F_SETFL, O_NONBLOCK);
    fflush(stderr);
    *saved = dup(2);
    if (*saved < 0) { close(p[0]); close(p[1]); return 0; }
    dup2(p[1], 2);
    return 1;
}
static void nmxbr_err_end(int saved, int p[2], const char *what)
{
    fflush(stderr);
    dup2(saved, 2); close(saved); close(p[1]);
    char buf[8192]; ssize_t n = read(p[0], buf, sizeof buf - 1);
    close(p[0]);
    if (n <= 0) { nvlog("drm bridge: %s: Mesa wrote nothing to stderr", what); return; }
    buf[n] = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) nvlog("drm bridge: %s: mesa: %s", what, line);
}

static int nmxbr_hook_close(int fd)
{
    io_connect_t c = 0;
    pthread_mutex_lock(&g_br_lock);
    if (fd >= 0 && fd < NMXBR_MAX_FD && g_br_conn[fd]) {
        c = g_br_conn[fd]; g_br_conn[fd] = 0; free(g_br_path[fd]); g_br_path[fd] = NULL;
        nmxbr_ref(c, -1);
    }
    pthread_mutex_unlock(&g_br_lock);
    return close(fd);
}

static int nmxbr_hook_fstat(int fd, struct stat *st)
{
    const char *p = NULL;
    pthread_mutex_lock(&g_br_lock);
    if (fd >= 0 && fd < NMXBR_MAX_FD && g_br_conn[fd]) p = g_br_path[fd];
    char path[256]; if (p) strlcpy(path, p, sizeof path);
    pthread_mutex_unlock(&g_br_lock);
    if (p) return stat(path, st);      /* the node itself, as libdrm expects of a DRM fd */
    return fstat(fd, st);
}

static int nmxbr_track_dup(int from, int to)
{
    if (to < 0) return to;
    pthread_mutex_lock(&g_br_lock);
    io_connect_t c = nmxbr_conn(from);
    if (c && to < NMXBR_MAX_FD) {
        g_br_conn[to] = c; free(g_br_path[to]); g_br_path[to] = g_br_path[from] ? strdup(g_br_path[from]) : NULL;
        nmxbr_ref(c, 1);
    }
    pthread_mutex_unlock(&g_br_lock);
    return to;
}
static int nmxbr_hook_dup(int fd) { return nmxbr_track_dup(fd, dup(fd)); }
static int nmxbr_hook_fcntl(int fd, int cmd, ...)
{
    va_list ap; va_start(ap, cmd); intptr_t a = va_arg(ap, intptr_t); va_end(ap);
    int r = fcntl(fd, cmd, a);
    if ((cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) && r >= 0) return nmxbr_track_dup(fd, r);
    return r;
}

/* b104 (Phase 4): app sandboxes (TextEdit 10-20: "opendir(/dev/dri) FAILED errno 1") refuse even LISTING /dev/dri, so
   libdrm's drmGetDevices2 found no node and never reached the open() above: zero Vulkan devices, the app stayed on the
   CPU. WindowServer's sandbox lists the folder, so it never hit this. stat() of the nodes is allowed in those sandboxes,
   so the listing is rebuilt: an empty file per node that stats as a character device, in a folder of the process's own
   temp space, and opendir returns that folder. libdrm reads only the names, then stats and opens the real
   /dev/dri/<name> - the open is refused and becomes a bridge connection as before. */
static DIR *nmxbr_hook_opendir(const char *name)
{
    DIR *d = opendir(name);
    if (d || !name || nmxbr_mode() == 0) return d;
    const int e = errno;
    if ((strcmp(name, "/dev/dri") && strcmp(name, "/dev/dri/")) || (e != EPERM && e != EACCES)) return d;
    char tmp[900] = ""; const char *base = getenv("XDG_RUNTIME_DIR");
    if (!base || !*base) { size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp); base = (n > 0 && n <= sizeof tmp) ? tmp : getenv("TMPDIR"); }
    if (!base || !*base) { errno = e; return NULL; }
    char dir[1024]; const size_t bl = strlen(base);
    snprintf(dir, sizeof dir, "%s%snvmtl-dri", base, base[bl - 1] == '/' ? "" : "/");
    mkdir(dir, 0700);
    static const char *const nodes[] = { "card0", "card1", "renderD128", "renderD129", "renderD130", "renderD131" };
    int listed = 0; char seen[160] = "";
    for (size_t i = 0; i < sizeof nodes / sizeof nodes[0]; i++) {
        char real[64], fake[1100]; struct stat st;
        snprintf(real, sizeof real, "/dev/dri/%s", nodes[i]); snprintf(fake, sizeof fake, "%s/%s", dir, nodes[i]);
        if (stat(real, &st) == 0 && S_ISCHR(st.st_mode)) {
            int fd = open(fake, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
            if (fd >= 0) { close(fd); listed++; strlcat(seen, " ", sizeof seen); strlcat(seen, nodes[i], sizeof seen); }
        } else unlink(fake);
    }
    if (listed) d = opendir(dir);
    static int said; if (said++ < 4)
        nvlog("drm bridge: opendir(/dev/dri) refused here (%s) - listing rebuilt in %s:%s -> %s", strerror(e), dir,
              listed ? seen : " (no node stats)", d ? "ok" : "FAILED");
    if (!d) errno = e;
    return d;
}

/* point one image's imports of the names below at the hooks (its indirect symbol tables, as dyld left them) */
static const struct { const char *name; void *fn; } g_br_hooks[] = {
    { "_open", (void *)nmxbr_hook_open }, { "_ioctl", (void *)nmxbr_hook_ioctl }, { "_close", (void *)nmxbr_hook_close },
    { "_fstat$INODE64", (void *)nmxbr_hook_fstat },
    { "_dup", (void *)nmxbr_hook_dup }, { "_fcntl", (void *)nmxbr_hook_fcntl },
    { "_opendir$INODE64", (void *)nmxbr_hook_opendir }, { "_opendir", (void *)nmxbr_hook_opendir },   /* b104 */
};
static int nmxbr_rebind_image(const struct mach_header_64 *mh, intptr_t slide)
{
    const struct segment_command_64 *linkedit = NULL;
    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysym = NULL;
    const struct load_command *lc = (const struct load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, SEG_LINKEDIT)) linkedit = (const void *)lc;
        else if (lc->cmd == LC_SYMTAB) symtab = (const void *)lc;
        else if (lc->cmd == LC_DYSYMTAB) dysym = (const void *)lc;
    }
    if (!linkedit || !symtab || !dysym || !dysym->nindirectsyms) return 0;
    const uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
    const struct nlist_64 *syms = (const struct nlist_64 *)(base + symtab->symoff);
    const char *strs = (const char *)(base + symtab->stroff);
    const uint32_t *ind = (const uint32_t *)(base + dysym->indirectsymoff);
    int done = 0;
    lc = (const struct load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *seg = (const void *)lc;
        if (strcmp(seg->segname, "__DATA") && strcmp(seg->segname, "__DATA_CONST") && strcmp(seg->segname, "__AUTH_CONST")) continue;
        const struct section_64 *sect = (const struct section_64 *)(seg + 1);
        for (uint32_t s = 0; s < seg->nsects; s++, sect++) {
            const uint32_t type = sect->flags & SECTION_TYPE;
            if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) continue;
            const uint32_t *idx = ind + sect->reserved1;
            void **ptrs = (void **)((uintptr_t)slide + sect->addr);
            const uint64_t n = sect->size / sizeof(void *);
            for (uint64_t j = 0; j < n; j++) {
                const uint32_t si = idx[j];
                if (si == INDIRECT_SYMBOL_ABS || si == INDIRECT_SYMBOL_LOCAL || si == (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) continue;
                if (si >= symtab->nsyms) continue;
                const char *name = strs + syms[si].n_un.n_strx;
                for (size_t k = 0; k < sizeof g_br_hooks / sizeof g_br_hooks[0]; k++) {
                    if (strcmp(name, g_br_hooks[k].name)) continue;
                    if (ptrs[j] == g_br_hooks[k].fn) break;
                    vm_address_t page = (vm_address_t)&ptrs[j] & ~(vm_address_t)(vm_page_size - 1);
                    kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
                    if (kr != KERN_SUCCESS) { nvlog("drm bridge: cannot write the import table (%s, 0x%x)", name, kr); break; }
                    ptrs[j] = g_br_hooks[k].fn; done++;
                    if (!strcmp(seg->segname, "__DATA_CONST") || !strcmp(seg->segname, "__AUTH_CONST"))
                        vm_protect(mach_task_self(), page, vm_page_size, 0, VM_PROT_READ);
                    break;
                }
            }
        }
    }
    return done;
}

/* after the plugin dlopen()ed the Vulkan driver: hook libvulkan_intel and libdrm from our own folder */
static void nmxbr_install(void)
{
    if (nmxbr_mode() == 0) { nvlog("drm bridge: off (NVMTL_DRM_BRIDGE=0)"); return; }
    int images = 0, slots = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *n = _dyld_get_image_name(i);
        if (!n || !strstr(n, "/GPUBundles/nvmtl/")) continue;
        if (!strstr(n, "libvulkan_intel") && !strstr(n, "libdrm")) continue;
        const struct mach_header *mh = _dyld_get_image_header(i);
        if (!mh || mh->magic != MH_MAGIC_64) continue;
        slots += nmxbr_rebind_image((const struct mach_header_64 *)mh, _dyld_get_image_vmaddr_slide(i));
        images++;
    }
    nvlog("drm bridge: %d import slot(s) hooked in %d image(s) in %s (mode %s)", slots, images, getprogname(),
          nmxbr_mode() == 2 ? "always" : "when /dev/dri is refused");
}
