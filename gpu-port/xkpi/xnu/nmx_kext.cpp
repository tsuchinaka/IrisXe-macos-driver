/*
 * The kext: one IOService per AMD or Intel GPU it is allowed to drive (amdgpu and i915 share one DRM core and one
 * port layer here). Compiled with Apple's headers only; everything else is reached through nmx_port.h.
 *
 * Opt-in only, per vendor. The service matches every AMD and Intel display-class device but declines in probe()
 * unless the user asked for that vendor: boot argument nmxamd=1 / nmxintel=1, or the device property
 * "nullmoth-gpu" (set through the bootloader's DeviceProperties). Without that, the device keeps whatever driver macOS
 * gives it. Taking a GPU that is lighting the boot display resets its display engine, so the opt-in is per user,
 * never a default.
 *
 * Intel only: nmxnofb=1 attaches headless (render path without the display bridge); see headless() below.
 *
 * The port (the FreeBSD/Linux layer and the AMD driver) starts with the first device and stops when the kext
 * unloads; unloading is refused while any process still has a GPU device node open.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IONVRAM.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include "nmx_port.h"
#include "nmx_xnu.h"

#define AMD_VENDOR 0x1002
#define INTEL_VENDOR 0x8086

/* the display bridge (nmx_fb.cpp) */
IOService *nmx_fb_publish(IOService *gpu, void *dev);
void nmx_fb_unpublish(IOService *fb, IOService *gpu, void *dev);

class NMXGPU : public IOService {
    OSDeclareDefaultStructors(NMXGPU)
    void *dev_;
    IOService *fb_;
    IOPCIDevice *pci_;
    volatile int attach_;   /* 0 = attach pending on its thread, 1 = attached, -1 = not attached */
    bool pm_;
    bool powered_ = true; /* the device is on from the moment it attaches; IOKit's first state-0 call must not suspend it */
    static void attachThread(void *arg, wait_result_t);
    void attachNow();
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn setPowerState(unsigned long state, IOService *device) override;
    unsigned long initialPowerStateForDomainState(IOPMPowerFlags) override { return 1; }
    void systemWillShutdown(IOOptionBits spec) override;
};
OSDefineMetaClassAndStructors(NMXGPU, IOService)

static IOLock *g_lock;
static bool g_started;
static int g_devices;

static bool opted_in(IOPCIDevice *pci, uint16_t vendor)
{
    int v = 0;
    /* the boot picker's "Start once without NVIDIA driver" entry (-nvoff) parks every NullMoth GPU driver for that start */
    if (PE_parse_boot_argn("-nvoff", &v, sizeof(v)))
        return false;
    if (PE_parse_boot_argn(vendor == AMD_VENDOR ? "nmxamd" : "nmxintel", &v, sizeof(v)) && v != 0)
        return true;
    OSObject *p = pci->getProperty("nullmoth-gpu");
    if (OSBoolean *b = OSDynamicCast(OSBoolean, p))
        return b->isTrue();
    if (OSData *d = OSDynamicCast(OSData, p))
        return d->getLength() > 0 && ((const uint8_t *)d->getBytesNoCopy())[0] != 0;
    return false;
}

/*
 * Headless bring-up (boot argument nmxnofb=1): the driver attaches and the accelerator starts, but the display bridge is
 * not published, so no DRM client lights the outputs and no IOFramebuffer is added. On a laptop whose internal panel is
 * wired to this same Intel GPU, that leaves the screen exactly as the firmware (or NMIntelFB) set it up: the first test
 * of the render path cannot take the display away. Applies to Intel only; AMD behaves as before.
 */
static bool headless(uint16_t vendor)
{
    int v = 0;
    return vendor == INTEL_VENDOR && PE_parse_boot_argn("nmxnofb", &v, sizeof(v)) && v != 0;
}

IOService *NMXGPU::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    uint16_t vendor = pci ? pci->configRead16(kIOPCIConfigVendorID) : 0;
    if (vendor != AMD_VENDOR && vendor != INTEL_VENDOR)
        return nullptr;
    if (!opted_in(pci, vendor))
        return nullptr;     /* not asked for: leave the device to macOS */
    return IOService::probe(provider, score);
}

bool NMXGPU::start(IOService *provider)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci == nullptr || g_lock == nullptr || !IOService::start(provider))
        return false;
    IOLockLock(g_lock);
    if (!g_started) {
        int err = nmx_port_start();
        if (err) {
            IOLockUnlock(g_lock);
            IOLog("NullMoth GPU: the driver layer did not start (%d)\n", err);
            IOService::stop(provider);
            return false;
        }
        g_started = true;
    }
    g_devices++;
    IOLockUnlock(g_lock);

    /*
     * amdgpu, i915 and xe load firmware while they attach. During boot matching that read would come before the root and
     * Data volumes are mounted, and mounting waits for matching to settle: the Mac would stop at the logo (seen with the
     * NPU kext on the same layer, 10-11). The attach runs on its own thread once the firmware folder exists; the firmware
     * framebuffer keeps the screen until then.
     */
    pci_ = pci;
    dev_ = nullptr;
    fb_ = nullptr;
    pm_ = false;
    attach_ = 0;
    retain();
    thread_t t = THREAD_NULL;
    if (kernel_thread_start(&NMXGPU::attachThread, this, &t) != KERN_SUCCESS) {
        IOLog("NullMoth GPU: could not start the attach thread\n");
        attach_ = -1;
        release();
        return true;
    }
    thread_deallocate(t);
    return true;
}

void NMXGPU::attachThread(void *arg, wait_result_t)
{
    NMXGPU *self = (NMXGPU *)arg;
    self->attachNow();
    self->release();
}

/*
 * Boot guard, as in NMXNPU: the NPU kext on this layer panicked 7 s into every start until its boot argument was
 * removed from Linux (10-11). The attach, the modeset and the hand-over to macOS run between setting an NVRAM marker
 * and clearing it; a start that finds the marker still set follows an attach that never returned (a panic or a hang),
 * so this GPU stays with macOS and the driver says so. Turning the test driver off and on in the app clears it.
 */
static const char *const kAttachMarker = "nullmoth-gpu-attaching";

static IODTNVRAM *nmx_nvram()
{
    IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
    IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e);
    if (n == nullptr && e != nullptr)
        e->release();
    return n;
}

static bool nmx_marker_set()
{
    IODTNVRAM *n = nmx_nvram();
    if (n == nullptr)
        return false;
    OSObject *v = n->copyProperty(kAttachMarker);
    bool set = v != nullptr;
    if (v)
        v->release();
    n->release();
    return set;
}

static void nmx_marker_write(bool on)
{
    IODTNVRAM *n = nmx_nvram();
    if (n == nullptr) {
        IOLog("NullMoth GPU: no NVRAM; the boot guard is off\n");
        return;
    }
    const OSSymbol *k = OSSymbol::withCString(kAttachMarker);
    if (k != nullptr) {
        if (on) {
            OSData *d = OSData::withBytes("1", 1);
            if (d == nullptr || !n->setProperty(k, d))
                IOLog("NullMoth GPU: could not set the boot guard\n");
            if (d)
                d->release();
        } else {
            n->removeProperty(k);
        }
        k->release();
    }
    n->sync();
    n->release();
}


/* read-only snapshot of the display engine (eDP panel power, backlight, pipe A, plane 1, DDI A, power wells) for black-screen
   diagnosis; reads only, safe at any time the device is powered */
extern "C" unsigned long __drm_debug;

static void nmx_dump_display(IOPCIDevice *pci, const char *tag)
{
    IOMemoryMap *m = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (m == nullptr) {
        IOLog("NullMoth DISP[%s] cannot map BAR0\n", tag);
        return;
    }
    volatile uint32_t *r = (volatile uint32_t *)m->getVirtualAddress();
    static const struct { const char *n; uint32_t o; } regs[] = {
        { "PP_STATUS", 0xc7200 }, { "PP_CONTROL", 0xc7204 },
        { "BLC_PWM_CTL1", 0xc8250 }, { "BLC_PWM_FREQ1", 0xc8254 }, { "BLC_PWM_DUTY1", 0xc8258 },
        { "PIPECONF_A", 0x70008 }, { "PIPE_SRC_A", 0x6001c }, { "TRANS_DDI_FUNC_CTL_A", 0x60400 },
        { "TRANS_CLK_SEL_A", 0x46140 }, { "DDI_BUF_CTL_A", 0x64000 }, { "DP_TP_CTL_A", 0x64040 },
        { "PLANE_CTL_1_A", 0x70180 }, { "PLANE_SURF_1_A", 0x7019c }, { "CDCLK_CTL", 0x46000 },
        { "PWR_WELL_CTL1", 0x45400 }, { "PWR_WELL_CTL2", 0x45404 }, { "DC_STATE_EN", 0x45504 },
        { "PLANE_STRIDE_1_A", 0x70188 }, { "PLANE_SIZE_1_A", 0x70190 }, { "PLANE_OFFSET_1_A", 0x701a4 },
        { "PSR1_CTL_A", 0x60800 }, { "PSR2_CTL_A", 0x60900 },
    };
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
        IOLog("NullMoth DISP[%s] %s=0x%08x\n", tag, regs[i].n, r[regs[i].o / 4]);
    /* the firmware framebuffer is scanned out through the GGTT: log a few page-table entries and the first pixels of the
       pages they point at, to tell "the screen is blank" from "the plane no longer reads the framebuffer" */
    if (m->getLength() >= 0x800000 + 8 * 4001) {
        volatile uint64_t *gsm = (volatile uint64_t *)((uintptr_t)r + 0x800000);
        static const uint32_t pages[] = { 0, 1, 100, 1000, 2000, 3999, 4000 };
        for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
            uint64_t pte = gsm[pages[i]];
            IOLog("NullMoth DISP[%s] GGTT[%u]=0x%016llx\n", tag, pages[i], pte);
        }
    }
    m->release();

    /* what the plane scans out: count non-zero words in six 64 KiB windows of the framebuffer through the GTT aperture (BAR2),
       so an emptied picture shows up as 0 and a drawn one does not */
    IOMemoryMap *ap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2, kIOMapInhibitCache);
    if (ap != nullptr) {
        if (ap->getLength() >= 0x1000000) {
            volatile uint32_t *fbp = (volatile uint32_t *)ap->getVirtualAddress();
            static const uint32_t win[] = { 0, 0x100000, 0x400000, 0x800000, 0xc00000, 0xf00000 };
            for (unsigned i = 0; i < sizeof(win) / sizeof(win[0]); i++) {
                uint32_t nz = 0, first = 0;
                for (uint32_t j = 0; j < 16384; j++) {
                    uint32_t v = fbp[win[i] / 4 + j];
                    if (v != 0) {
                        if (nz == 0)
                            first = v;
                        nz++;
                    }
                }
                IOLog("NullMoth DISP[%s] FB[0x%x] nonzero=%u/16384 first=0x%08x\n", tag, win[i], nz, first);
            }
        }
        ap->release();
    }

    /* which framebuffer drivers macOS has right now */
    OSDictionary *match = IOService::serviceMatching("IOFramebuffer");
    if (match != nullptr) {
        OSIterator *it = IOService::getMatchingServices(match);
        unsigned n = 0;
        if (it != nullptr) {
            while (OSObject *o = it->getNextObject()) {
                IOService *svc = OSDynamicCast(IOService, o);
                if (svc != nullptr) {
                    IOLog("NullMoth DISP[%s] IOFramebuffer: %s (%s)\n", tag, svc->getName(), svc->getMetaClass()->getClassName());
                    n++;
                }
            }
            it->release();
        }
        IOLog("NullMoth DISP[%s] IOFramebuffer count=%u\n", tag, n);
        match->release();
    }
}

void NMXGPU::attachNow()
{
    int err = nmx_xnu_wait_firmware_root(300);
    if (err) {
        IOLog("NullMoth GPU: the firmware folder never appeared (%d); this GPU stays with macOS\n", err);
        attach_ = -1;
        return;
    }
    if (nmx_marker_set()) {
        IOLog("NullMoth GPU: the last attach of %04x:%04x never returned (the Mac stopped or restarted); this GPU stays "
              "with macOS. Send logs, then turn the test driver off and on to try again\n",
              pci_->configRead16(kIOPCIConfigVendorID), pci_->configRead16(kIOPCIConfigDeviceID));
        setProperty("nullmoth-gpu-state", "parked: the last attach never returned");
        attach_ = -1;
        return;
    }
    nmx_marker_write(true);
    {
        /* diagnostic build: DRM debug messages are opt-in (boot-arg nmxdrm=<mask>, e.g. 22 = driver+KMS+atomic) */
        uint32_t dbg = 0;
        PE_parse_boot_argn("nmxdrm", &dbg, sizeof(dbg));
        __drm_debug = dbg;
    }
    nmx_dump_display(pci_, "before-attach");
    dev_ = nmx_port_attach(pci_);
    if (dev_ == nullptr) {
        nmx_marker_write(false);
        IOLog("NullMoth GPU: no driver took %04x:%04x\n", pci_->configRead16(kIOPCIConfigVendorID),
              pci_->configRead16(kIOPCIConfigDeviceID));
        attach_ = -1;
        return;
    }

    static IOPMPowerState states[2] = {
        { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { kIOPMPowerStateVersion1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    PMinit();
    pci_->joinPMtree(this);
    registerPowerDriver(this, states, 2);
    pm_ = true;
    makeUsable(); /* hold the device at full power: a bare PM registration can leave it at state 0, which suspends the display */
    registerService();
    /* NMXAccel (the Metal accelerator for this GPU) starts only on a device this driver has claimed; it declined while
       the claim was pending, so matching on the device runs again now that it is set */
    pci_->setProperty("nmxgpu-claimed", kOSBooleanTrue);
    pci_->registerService();
    /* light the outputs and hand them to macOS; without it the device still renders, with no screen of its own */
    if (headless(pci_->configRead16(kIOPCIConfigVendorID))) {
        IOLog("NullMoth GPU: nmxnofb=1: display bridge not published; the screen stays as the firmware left it\n");
        setProperty("nullmoth-gpu-display", "headless (nmxnofb)");
        fb_ = nullptr;
    } else {
        fb_ = nmx_fb_publish(this, dev_);
    }
    nmx_marker_write(false);
    IOLog("NullMoth GPU: driving %04x:%04x\n", pci_->configRead16(kIOPCIConfigVendorID),
          pci_->configRead16(kIOPCIConfigDeviceID));
    OSSynchronizeIO();
    attach_ = 1;
    nmx_dump_display(pci_, "attach+0s");
    IOSleep(3000);
    nmx_dump_display(pci_, "attach+3s");
    IOSleep(10000);
    nmx_dump_display(pci_, "attach+13s");
}

void NMXGPU::stop(IOService *provider)
{
    /* an attach still running on its thread finishes first (it holds a reference on this object meanwhile) */
    for (int i = 0; attach_ == 0 && i < 6000; i++)
        IOSleep(100);
    provider->removeProperty("nmxgpu-claimed");
    if (fb_ != nullptr)
        nmx_fb_unpublish(fb_, this, dev_);
    fb_ = nullptr;
    if (pm_)
        PMstop();
    pm_ = false;
    nmx_port_detach(dev_);
    dev_ = nullptr;
    IOLockLock(g_lock);
    g_devices--;
    IOLockUnlock(g_lock);
    IOService::stop(provider);
}

/* state 0 = sleep, 1 = on */
IOReturn NMXGPU::setPowerState(unsigned long state, IOService *device)
{
    IOLog("NullMoth GPU: power state %lu requested (device is %s)\n", state, powered_ ? "on" : "off");
    if (dev_ != nullptr && (state != 0) != powered_) {
        int err = nmx_port_power(dev_, state != 0);
        powered_ = state != 0;
        if (err)
            IOLog("NullMoth GPU: %s returned %d\n", state ? "resume" : "suspend", err);
    }
    return kIOPMAckImplied;
}

void NMXGPU::systemWillShutdown(IOOptionBits spec)
{
    if (dev_ != nullptr)
        nmx_port_shutdown(dev_);
    IOService::systemWillShutdown(spec);
}

/* ---- module start/stop: the stop refuses while device nodes are open ------------------------------------------------- */

extern "C" {
kern_return_t nmx_kmod_start(kmod_info_t *ki, void *data);
kern_return_t nmx_kmod_stop(kmod_info_t *ki, void *data);
}

kern_return_t nmx_kmod_start(kmod_info_t *ki, void *data)
{
    g_lock = IOLockAlloc();
    return g_lock ? KERN_SUCCESS : KERN_FAILURE;
}

kern_return_t nmx_kmod_stop(kmod_info_t *ki, void *data)
{
    IOLockLock(g_lock);
    if (g_started) {
        if (g_devices > 0 || nmx_port_busy() > 0) {
            IOLockUnlock(g_lock);
            IOLog("NullMoth GPU: still in use; not unloading\n");
            return KERN_FAILURE;
        }
        nmx_port_stop();
        g_started = false;
    }
    IOLockUnlock(g_lock);
    IOLockFree(g_lock);
    g_lock = nullptr;
    return KERN_SUCCESS;
}

/* libkmod's entry points; they call _realmain/_antimain below */
extern "C" {
kern_return_t _start(kmod_info_t *ki, void *data);
kern_return_t _stop(kmod_info_t *ki, void *data);
KMOD_EXPLICIT_DECL(com.nullmoth.NMXGPU, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = nmx_kmod_start;
__private_extern__ kmod_stop_func_t *_antimain = nmx_kmod_stop;
}
