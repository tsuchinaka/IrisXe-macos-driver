/*
 * b80 (10-19): the DRM render node over IOKit.
 *
 * WindowServer and the browsers' GPU processes run in sandboxes that refuse open("/dev/dri/renderD128") with EPERM
 * (WindowServer, 10-19: "open(renderD128, O_RDWR) FAILED errno 1"), so Mesa ANV found no GPU there. Those sandboxes do
 * let a process open IOAccelerator user clients - the NVIDIA driver reaches its RM the same way (IOAccelNVRMBridge).
 * This user client is the render node behind such a connection: NMXAccel's newUserClient(0x4E56xxxx) asks NMXGPU
 * (callPlatformFunction "nmxNewDrmClient") for one, and each connection is one open of the render node, exactly as an
 * open() of /dev/dri/renderD128 would be (same per-open DRM file, same ioctls, same NMX mmap ioctl). The class name
 * starts with "IOAccel" because sandbox profiles allow user clients by that prefix.
 *
 * Selectors
 *   0 OPEN   -> scalar out[0] = errno (0 = the render node is open on this connection)
 *   1 IOCTL  scalar in[0] = the ioctl request, in[1] = the argument for IOC_VOID requests;
 *            struct in/out = the argument block (IOCPARM_LEN(request) bytes)  -> scalar out[0] = errno
 * As for ioctl(2) on xnu, the argument block is copied back only when the ioctl succeeded.
 */
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <string.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/OSAtomic.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/fcntl.h>

extern "C" {
void *nmx_xnu_devfs_render_cookie(void);
int nmx_cdev_open(void *cookie, int open_id, int fflags);
int nmx_cdev_close(int open_id, int fflags);
int nmx_cdev_ioctl(int open_id, unsigned long cmd, void *data, int fflag);
void nmx_critical_syscall_check(void);
volatile int g_nmx_bridge_opens = 0;
}

#define BRLOG(fmt, ...) IOLog("NullMoth bridge: " fmt "\n", ##__VA_ARGS__)

static volatile SInt32 g_next_id = 0x40000000;   /* open ids above every devfs clone minor (< 65536) */
static volatile SInt32 g_logged = 0;

class IOAccelNMXDrmBridge : public IOUserClient {
    OSDeclareDefaultStructors(IOAccelNMXDrmBridge)
    int fId = 0;
    bool fOpen = false;
    IOLock *fLock = nullptr;
    task_t fTask = nullptr;
public:
    bool initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) override {
        if (!IOUserClient::initWithTask(owningTask, securityToken, type, properties)) return false;
        fTask = owningTask;
        fLock = IOLockAlloc();
        return fLock != nullptr;
    }
    void closeNode() {
        if (fLock) IOLockLock(fLock);
        bool was = fOpen; fOpen = false;
        int id = fId;
        if (fLock) IOLockUnlock(fLock);
        if (was) {
            nmx_cdev_close(id, FREAD | FWRITE);
            nmx_critical_syscall_check();
            OSDecrementAtomic((volatile SInt32 *)&g_nmx_bridge_opens);
        }
    }
    IOReturn clientClose() override {
        closeNode();
        if (!isInactive()) terminate();
        return kIOReturnSuccess;
    }
    IOReturn clientDied() override { return clientClose(); }
    void free() override {
        closeNode();
        if (fLock) { IOLockFree(fLock); fLock = nullptr; }
        IOUserClient::free();
    }
    int doOpen() {
        if (fOpen) return 0;
        void *cookie = nmx_xnu_devfs_render_cookie();
        if (cookie == nullptr) return ENODEV;
        int id = (int)OSIncrementAtomic(&g_next_id);
        int err = nmx_cdev_open(cookie, id, FREAD | FWRITE);
        nmx_critical_syscall_check();
        if (err == 0) {
            fId = id; fOpen = true;
            OSIncrementAtomic((volatile SInt32 *)&g_nmx_bridge_opens);
        }
        if (OSIncrementAtomic(&g_logged) < 8)
            BRLOG("render node opened over IOKit (open %d) -> %d", id, err);
        return err;
    }
    int doIoctl(IOExternalMethodArguments *a) {
        if (!fOpen) return EBADF;
        if (a->scalarInputCount < 1) return EINVAL;
        const uint32_t cmd = (uint32_t)a->scalarInput[0];
        const size_t len = IOCPARM_LEN(cmd);
        uint8_t small[256];
        uint8_t *buf = small;
        const size_t blen = len < 8 ? 8 : len;
        if (blen > sizeof small) { buf = (uint8_t *)IOMalloc(blen); if (!buf) return ENOMEM; }
        bzero(buf, blen);
        if (len == 0 && (cmd & IOC_VOID) && a->scalarInputCount > 1)
            *(uint64_t *)buf = a->scalarInput[1];   /* xnu hands an IOC_VOID ioctl its argument value this way */
        if (len && (cmd & IOC_IN)) {
            if (a->structureInputDescriptor) {
                if (a->structureInputDescriptor->getLength() < len) { if (buf != small) IOFree(buf, blen); return EINVAL; }
                a->structureInputDescriptor->readBytes(0, buf, len);
            } else {
                if (a->structureInputSize < len) { if (buf != small) IOFree(buf, blen); return EINVAL; }
                bcopy(a->structureInput, buf, len);
            }
        }
        int err = nmx_cdev_ioctl(fId, cmd, buf, FREAD | FWRITE);
        nmx_critical_syscall_check();
        if (err == 0 && len && (cmd & IOC_OUT)) {
            if (a->structureOutputDescriptor) {
                if (a->structureOutputDescriptor->getLength() >= len) a->structureOutputDescriptor->writeBytes(0, buf, len);
            } else if (a->structureOutput && a->structureOutputSize >= len) {
                bcopy(buf, a->structureOutput, len);
                a->structureOutputSize = (uint32_t)len;
            }
        } else if (!a->structureOutputDescriptor) {
            a->structureOutputSize = 0;
        }
        if (buf != small) IOFree(buf, blen);
        return err;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *a, IOExternalMethodDispatch *, OSObject *,
                            void *) override {
        int err;
        switch (selector) {
        case 0: err = doOpen(); break;
        case 1: err = doIoctl(a); break;
        default: return kIOReturnUnsupported;
        }
        if (a->scalarOutputCount >= 1) a->scalarOutput[0] = (uint64_t)err;
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(IOAccelNMXDrmBridge, IOUserClient)

/* NMXGPU::callPlatformFunction("nmxNewDrmClient", task, type, &client) */
extern "C" IOReturn nmx_drm_bridge_new(void *task, uint32_t type, IOUserClient **out)
{
    if (!out) return kIOReturnBadArgument;
    *out = nullptr;
    if (nmx_xnu_devfs_render_cookie() == nullptr) return kIOReturnNotReady;
    IOAccelNMXDrmBridge *uc = OSTypeAlloc(IOAccelNMXDrmBridge);
    if (!uc) return kIOReturnNoMemory;
    if (!uc->initWithTask((task_t)task, nullptr, type, nullptr)) { uc->release(); return kIOReturnNoResources; }
    *out = uc;
    return kIOReturnSuccess;
}
