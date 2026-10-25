/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "../nvrm_off.h"
#ifdef NMX_ACCEL
// NMXAccel: this accelerator built for the AMD and Intel GPUs that NMXGPU drives (see nmx_accel_names.h)
#include "nmx_accel_names.h"
#endif
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <string.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/thread_call.h>
#include "IOAccelerator.h"
#include "IOGraphicsAccelerator2.h"
#include "IOAccelEventMachineFast2.h"
#include "IOAccelLegacyDisplayMachine.h"
#include "IOAccelTask.h"
#include "IOAccelMemoryMap.h"
#include "../nvrm_gpuva_abi.h"
class NVMemoryMap;
static unsigned long long nvAccelGpuVaAlloc(IOAccelMemoryMap *map,
                                           unsigned long long len, unsigned long long align);
static void               nvAccelGpuVaFree(IOAccelMemoryMap *map);
static bool               nvAccelFlipTo(IOAccelResource2 *res);
static bool               nvAccelCopyToScanout(IOAccelResource2 *res);
void                      nvAccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2);
bool                      nvAccelResHasContent(IOAccelResource2 *res);
#ifdef NMX_ACCEL
bool                      nmxSwapPresent(IOAccelResource2 *a1, IOAccelResource2 *a2);
void                      nmxShadowForget(void *kva);
void                      nmxSwapStat(unsigned long long t0, unsigned long long t1, bool copied);
void                      nmxNoteFront(IOAccelResource2 *res);
void                      nmxSwapKeepFront(IOAccelResource2 *a1, IOAccelResource2 *shown);
bool                      nmxSwapIsStale(IOAccelResource2 *a2);
void                      nmxSwapNoteA1(IOAccelResource2 *a1);
IOReturn                  nmxCopyForward(unsigned int mode, IOAccelResource2 *a, IOAccelResource2 *b, const void *bounds, unsigned int n);
int                       nmxSwapSync(IOAccelResource2 *a1, IOAccelResource2 *a2);
void                      nmxSyncForget(void *kva);
#endif
static void               nvAccelWriteStamp(int index, unsigned int value);
#ifdef NMX_ACCEL
#define NV_LOG_TAG "NMXAccel: "
#else
#define NV_LOG_TAG "NVAccel: "
#endif
#define ALOG(fmt, ...) do { kprintf(NV_LOG_TAG fmt "\n", ##__VA_ARGS__); IOLog(NV_LOG_TAG fmt "\n", ##__VA_ARGS__); } while (0)
#include <libkern/c++/OSSymbol.h>
#include "nvrm_vram_abi.h"
#include <IOKit/IODeviceMemory.h>
#include "tahoe_fwd.h"   // macOS 26: inherited slots Apple no longer exports, forwarded through its vtables
#include <libkern/OSAtomic.h>
extern "C" uint64_t mach_absolute_time(void);
#include "nvaccel_stubs.h"
#include "nvkms-kapi.h"
#include <sys/sysctl.h>
#include <libkern/OSAtomic.h>
extern "C" {   // kern/thread_call.h and kern/clock.h clash with the family headers here, so the KPIs are declared by hand
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter_delayed(struct thread_call *call, uint64_t deadline);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result);
}
static volatile SInt64 gCntVmAlloc, gCntVmAllocBytes, gCntVmDealloc, gCntVmDeallocBytes, gCntVmRefused,
                       gCntResNew, gCntResFree, gCntMapCommit, gCntMapRelease, gCntMapFree;
static void nvaccelInstallSysctl(void);
static int gNvAccelArmed = 0;
#ifdef NMX_ACCEL
// b75: boot-arg nmxws=1 lets WindowServer composite on this accelerator (NMXGPUFB becomes head 0 of its display
// machine). Nothing changes until `sysctl debug.nvaccelfb=1` + `debug.nvaccel_iop=1` and a WindowServer restart; a
// reboot undoes the arm.
static int gNmxWs = 0;
#endif
// 1 once the headless check restored the plugin name without arming (no NVIDIA display head). The Metal plugin reads it:
// a headless GPU hands frames to another GPU's WindowServer through system memory, so IOSurfaces must be copied out.
static int gNvHeadless = 0;
#ifdef NMX_ACCEL
SYSCTL_INT(_debug, OID_AUTO, nmxaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadless, 0,
           "1 = AMD/Intel Metal device for apps; frames are copied to the framebuffer WindowServer draws");
#else
SYSCTL_INT(_debug, OID_AUTO, nvaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadless, 0,
           "1 = no NVIDIA display: Metal device for apps only, WindowServer on the GPU that owns the display");
#endif
// 4 NVIDIA heads (nvfbheads=4) + 1 for an Intel built-in panel (NMIntelFB, IOFBDependentIndex 4): WindowServer composites
// that panel on the NVIDIA card and the display-pipe copy path below writes each frame into the Intel scan-out surface.
static const unsigned kNvMaxFB = 5;
static unsigned gNvHeadsPublished = 0, gNvHeadsRegistered = 0;
static unsigned gNvHeadsPiped = 0;
// The Intel built-in panel (head 4) joins only when this is 1. Off by default while the path is proven: a write at run
// time (then debug.nvaccelfb=3 and a WindowServer restart) can be undone by a reboot, so a hang never needs hands
// (10-10: the first virtual-panel test froze studio at boot and needed a power cycle).
static int gNvIgpu = 0;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_igpu, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvIgpu, 0,
           "1 = give an Intel built-in panel (NMIntelFB, head 4) a display pipe on this accelerator");

extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
#ifdef NMX_ACCEL
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMXAccel, "0.1", _start, _stop) }
#else
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NVAccel, "0.1", _start, _stop) }
#endif
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

class NVEventMachine : public IOAccelEventMachineFast2 {
    OSDeclareDefaultStructors(NVEventMachine)
    NM_TAHOE_FWD(NVEventMachine)
public:
    void writeStamp(int s, vendevtCommandRec *r, unsigned int v) APPLE_KEXT_OVERRIDE {
        (void)r;
        nvAccelWriteStamp(s, v);
    }
    void prepareBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOG("EM prepareBarrier(%p)", b); }
    void completeBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOG("EM completeBarrier(%p)", b); }
    void writeBarrierElement(vendevtBarrierRec *b, int i, unsigned int v) APPLE_KEXT_OVERRIDE { ALOG("EM writeBarrierElement(%p, %d, %u)", b, i, v); }
    bool checkGPUProgress() APPLE_KEXT_OVERRIDE { return true; }
    bool checkChannelProgress(int ch) APPLE_KEXT_OVERRIDE { return true; }
    void *eventTimeout(int stamp) APPLE_KEXT_OVERRIDE { ALOG("EM eventTimeout(%d) — ignored, we do not use family channels", stamp); return nullptr; }
};
OSDefineMetaClassAndStructors(NVEventMachine, IOAccelEventMachineFast2)
static void (*gEnableAccel)(const char *why) = nullptr;
/* b132: 1 = after a system wake, finish a display-mode change the sleep left open (see NVAccel::wakeFire) */
static int gNmxWakeFix = 1;
/* b146: raise the transaction interrupt after each display-pipe transaction (see NMXDisplayPipeM::performTransaction) */
static int gIopTxSig = 1;
static SInt64 gCntIopTxSig;
static SInt64 gCntNmxWakeFixes;
static int gNvScanout = 0;
static int gNvAperture = 0;
class NVDisplayMachine : public IOAccelLegacyDisplayMachine {
    OSDeclareDefaultStructors(NVDisplayMachine)
    NM_TAHOE_FWD(NVDisplayMachine)
public:
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE { ALOG("DM displayModeWillChange"); return true; }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE {
        ALOG("DM displayModeDidChange -> true");
        if (gEnableAccel) gEnableAccel("displayModeDidChange");
        return true;
    }
};
OSDefineMetaClassAndStructors(NVDisplayMachine, IOAccelLegacyDisplayMachine)
#if defined(NMX_ACCEL) && !defined(NM_TAHOE)
/* b116: modern display machine (Apple Intel: IGAccelDisplayMachine : IOAccelDisplayMachine), picked by nmxmodern=1 */
class NMXDisplayMachineM : public IOAccelDisplayMachine {
    OSDeclareDefaultStructors(NMXDisplayMachineM)
public:
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE { ALOG("DM(modern) displayModeWillChange"); return true; }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE {
        ALOG("DM(modern) displayModeDidChange -> true");
        if (gEnableAccel) gEnableAccel("displayModeDidChange");
        return true;
    }
};
OSDefineMetaClassAndStructors(NMXDisplayMachineM, IOAccelDisplayMachine)
#endif
static int nmxModern()
{
#if defined(NMX_ACCEL) && !defined(NM_TAHOE)
    static int on = -1;
    if (on < 0) { int v = 0; on = (PE_parse_boot_argn("nmxmodern", &v, sizeof v) && v) ? 1 : 0;
                  IOLog("NMXAccel: display classes %s\n", on ? "MODERN (nmxmodern=1, b116: IOAccelSurface / IOAccelDisplayPipe / IOAccelDisplayMachine, as Apple's Intel driver)"
                                                          : "legacy (default; nmxmodern=1 = the modern ones)"); }
    return on;
#else
    return 0;
#endif
}
#define NV_PRIVATE_VA_BASE  0x800000000000ull
#define NV_PRIVATE_VA_SIZE  0x000010000000ull
class NVTask : public IOAccelTask {
    OSDeclareDefaultStructors(NVTask)
    NM_TAHOE_FWD(NVTask)
public:
    IORangeAllocator *fVa = nullptr;
    unsigned fAllocLogged = 0, fPrivate = 0;
    unsigned long long allocate(const IOAccelMemoryMap *map) APPLE_KEXT_OVERRIDE {
        const unsigned long long len = map ? ((IOAccelMemoryMap *)map)->getLength() : 0;
        const unsigned long long align = map ? *(const volatile unsigned long long *)((const unsigned char *)map + 0x80) : 0;
        unsigned long long got = nvAccelGpuVaAlloc((IOAccelMemoryMap *)map, len, align);
        if (!got && fAllocLogged < 12 && ++fAllocLogged)
            ALOG("NVTask::allocate: RM refused %llu bytes (align %llu) -- no GPU address", len, align);
        return got;
    }
    void deallocate(const IOAccelMemoryMap *map, unsigned long long addr) APPLE_KEXT_OVERRIDE {
        (void)addr;
        nvAccelGpuVaFree((IOAccelMemoryMap *)map);
    }
};
OSDefineMetaClassAndStructors(NVTask, IOAccelTask)
class NVMemoryMap : public IOAccelMemoryMap {
    OSDeclareDefaultStructors(NVMemoryMap)
    NM_TAHOE_FWD(NVMemoryMap)
public:
    IOAccelMemory     *fMem       = nullptr;
    unsigned int       fHVirt     = 0;
    unsigned int       fHRmMemory = 0;
    unsigned long long fGpuVa     = 0;
    unsigned long long fVaLen     = 0;
    bool               fMapped    = false;
    bool               fNmxPrivate = false;   // NMXAccel: a bookkeeping-only GPU VA (no NVIDIA RM behind it)
    unsigned int fCommits = 0;
    bool init(IOGraphicsAccelerator2 *a, IOAccelTask *t, IOAccelMemory *m,
              unsigned int o) APPLE_KEXT_OVERRIDE {
        if (!IOAccelMemoryMap::init(a, t, m, o)) return false;
        fMem = m;
        if (fMem) fMem->retain();
        return true;
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (fMem) { fMem->release(); fMem = nullptr; }
        IOAccelMemoryMap::free();
    }
    bool commitIntoGPUPageTable() APPLE_KEXT_OVERRIDE;
    void releaseFromGPUPageTable() APPLE_KEXT_OVERRIDE;
    bool updateGPUPageTable() APPLE_KEXT_OVERRIDE { return fMapped; }
};
OSDefineMetaClassAndStructors(NVMemoryMap, IOAccelMemoryMap)

#define NV_CARRIERS 32
class IOAccelNVRMCarrier : public IOService {
    OSDeclareDefaultStructors(IOAccelNVRMCarrier)
};
OSDefineMetaClassAndStructors(IOAccelNVRMCarrier, IOService)

class NVAccel : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(NVAccel)
    NM_TAHOE_FWD(NVAccel)
    IOAccelNVRMCarrier *fCarriers[NV_CARRIERS] = { nullptr };
    unsigned            fCarrierNext = 0;
    unsigned            fCarrierLogged = 0;
public:
    IOBufferMemoryDescriptor *fStamps = nullptr; IORangeAllocator *fRanges[32] = {};
    uint64_t  getStampMemory(unsigned int *p) APPLE_KEXT_OVERRIDE {
        if (!fStamps) { fStamps = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 16384, 4096); if (fStamps) { fStamps->prepare(); bzero(fStamps->getBytesNoCopy(), 16384); } }
        if (p) *p = fStamps ? 4096 : 0;
        ALOG("getStampMemory(%p) -> descriptor %p (%u stamps, 16 KB)", p, fStamps, p ? *p : 0); return (uint64_t)fStamps; }
    OSObject *newEventMachine() APPLE_KEXT_OVERRIDE { NVEventMachine *m = OSTypeAlloc(NVEventMachine); ALOG("newEventMachine -> %p (family inits it)", m); return m; }
    OSObject *newGPUTask(const char *who) {
        for (unsigned i = 0; i < 32; i++) if (!fRanges[i]) {
            fRanges[i] = IORangeAllocator::withRange(1ULL << 40, 4096);
            if (fRanges[i]) fRanges[i]->allocateRange(0, 0x100000);
        }
        NVTask *t = OSTypeAlloc(NVTask); bool ok = t && t->init(this, 1, fRanges);
        ALOG("%s -> %p init=%d", who, t, ok); if (t && !ok) { t->release(); t = nullptr; } return t; }
    OSObject *createUserGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createUserGPUTask"); }
    void populateAccelConfig(IOAccelConfig *c) APPLE_KEXT_OVERRIDE {
        uint8_t *p = reinterpret_cast<uint8_t *>(c);
        if (!p) { ALOG("populateAccelConfig(NULL) — nothing to do"); return; }
        if (((uintptr_t)p & 0xFFF) + 0x70 > 0x1000) {
            ALOG("populateAccelConfig(%p) — 0x70 bytes would cross out of this page; REFUSING to touch it", p);
            return;
        }
        ALOG("populateAccelConfig(%p) — what the FAMILY pre-filled, before we write a byte:", p);
        for (unsigned i = 0; i < 0x70; i += 8)
            ALOG("   cfg+0x%02x = 0x%016llx", i, *(const uint64_t *)(p + i));

        *(uint64_t *)(p + 0x00) = (uint64_t)(uintptr_t)"NVIDIAAccelerator";
        *(uint32_t *)(p + 0x08) = 0x30844300u;
        *(uint32_t *)(p + 0x0c) = 15u;
        *(uint32_t *)(p + 0x18) = 8u;
        *(uint32_t *)(p + 0x30) = 0x40004000u;
        *(uint32_t *)(p + 0x34) = 0x00008000u;
        *(uint32_t *)(p + 0x38) = 0x00008000u;
        *(uint8_t  *)(p + 0x44) = 0;
        *(uint8_t  *)(p + 0x46) = 1;
        *(uint8_t  *)(p + 0x47) = 1;
        *(uint64_t *)(p + 0x48) = 0x0000001F0000000FULL;
        *(uint64_t *)(p + 0x50) = 0x7FFFFFFF0000001FULL;
        *(uint64_t *)(p + 0x58) = 0x0000000C00000000ULL;
        *(uint32_t *)(p + 0x64) = 0x4Bu;
        *(uint32_t *)(p + 0x28) = kNvMaxFB;
        ALOG("populateAccelConfig(%p) — filled: AccelCaps=15, flags=0x30844300, maxtex=16384x16384, maxfb=%u written (cfg+0x2c left %u)",
             p, kNvMaxFB, *(const uint32_t *)(p + 0x2c));
    }
    bool      configureDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("configureDevice(%p) %04x:%04x — RM owns the card (NVRM.kext); nothing to do here yet", pci, pci ? pci->configRead16(0) : 0, pci ? pci->configRead16(2) : 0); return true; }
    void      teardownDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("teardownDevice(%p)", pci); }
    OSObject *createKernelGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createKernelGPUTask"); }
    OSObject *newDisplayPipe() APPLE_KEXT_OVERRIDE {
#if defined(NMX_ACCEL) && !defined(NM_TAHOE)
        if (nmxModern()) { NMXDisplayPipeM *mp = OSTypeAlloc(NMXDisplayPipeM); ALOG("newDisplayPipe -> %p (modern, b116)", mp); return mp; }
#endif
        NVDisplayPipe *p = OSTypeAlloc(NVDisplayPipe);
        if (!p) { ALOG("newDisplayPipe: OSTypeAlloc FAILED - the family will store this NULL unchecked"); return NULL; }
        ALOG("newDisplayPipe -> %p", p); return p; }
    OSObject *newDisplayMachine() APPLE_KEXT_OVERRIDE {
#if defined(NMX_ACCEL) && !defined(NM_TAHOE)
        if (nmxModern()) { NMXDisplayMachineM *md = OSTypeAlloc(NMXDisplayMachineM); fDM = md; ALOG("newDisplayMachine -> %p (modern, b116)", md); return md; }
#endif
        NVDisplayMachine *d = OSTypeAlloc(NVDisplayMachine);
        fDM = d;
        ALOG("newDisplayMachine -> %p", d); return d; }
    OSObject *newGLContext() APPLE_KEXT_OVERRIDE { NVGLContext *c = OSTypeAlloc(NVGLContext); ALOG("newGLContext -> %p", c); return c; }
    OSObject *newCLContext() APPLE_KEXT_OVERRIDE { NVCLContext *c = OSTypeAlloc(NVCLContext); ALOG("newCLContext -> %p", c); return c; }
    OSObject *newSurface() APPLE_KEXT_OVERRIDE {
#if defined(NMX_ACCEL) && !defined(NM_TAHOE)
        if (nmxModern()) { NMXSurfaceM *m = OSTypeAlloc(NMXSurfaceM); static SInt32 n; if (OSIncrementAtomic(&n) < 8) ALOG("newSurface -> %p (modern, b116)", m); return m; }
#endif
        NVSurface *o = OSTypeAlloc(NVSurface); ALOG("newSurface -> %p", o); return o; }
    OSObject *new2DContext() APPLE_KEXT_OVERRIDE { NV2DContext *c = OSTypeAlloc(NV2DContext); ALOG("new2DContext -> %p", c); return c; }
    OSObject *newVideoContext() APPLE_KEXT_OVERRIDE { NVVideoContext *c = OSTypeAlloc(NVVideoContext); ALOG("newVideoContext -> %p", c); return c; }
    OSObject *newSysMemory() APPLE_KEXT_OVERRIDE { NVSysMemory *m = OSTypeAlloc(NVSysMemory); ALOG("newSysMemory -> %p", m); return m; }
    OSObject *newVidMemory() APPLE_KEXT_OVERRIDE { NVVidMemory *m = OSTypeAlloc(NVVidMemory); ALOG("newVidMemory -> %p", m); return m; }
    OSObject *newResource() APPLE_KEXT_OVERRIDE { NVResource *r = OSTypeAlloc(NVResource); OSAddAtomic64(1, &gCntResNew); ALOG("newResource -> %p", r); return r; }
    OSObject *newMemoryMap() APPLE_KEXT_OVERRIDE { NVMemoryMap *m = OSTypeAlloc(NVMemoryMap); ALOG("newMemoryMap -> %p", m); return m; }

    IOAccelDisplayMachine *fDM = nullptr;   /* b116: legacy NMXDisplayMachine or modern NMXDisplayMachineM */
    IOPCIDevice *fPCI = nullptr;
    IONotifier *fFBNote = nullptr;
    /* b132 (wake): 10-22 the first real sleep (b131, nmx-fbpm=1) woke to a black desktop with only the cursor. At sleep the
       display machine got displayModeWillChange and after the wake never displayModeDidChange; WindowServer then logged
       "Fallback GPU Display Pipe transaction submission failed" and not one transaction reached performTransaction. Here
       the wake is watched and, 1.5 s later, a mode change still open (nest at display machine +0x10c) is finished. */
    IONotifier *fPmNote = nullptr;
    thread_call_t fWakeCall = nullptr;
    int fWakeTries = 0;
    static IOReturn pmMessage(void *target, void *, UInt32 type, IOService *, void *arg, vm_size_t) {
        NVAccel *me = (NVAccel *)target;
        if (type == kIOMessageCanSystemSleep || type == kIOMessageSystemWillSleep) {
            if (arg) ((IOPowerStateChangeNotification *)arg)->returnValue = 0;      /* acknowledged at once */
            if (type == kIOMessageSystemWillSleep && me && me->fDM)
                ALOG("pm: system will sleep (mode-change nest %u)", *(const volatile uint32_t *)((uint8_t *)me->fDM + 0x10c));
        } else if (type == kIOMessageSystemHasPoweredOn && me && me->fWakeCall) {
            me->fWakeTries = 3;
            uint64_t dl; clock_interval_to_deadline(1500, kMillisecondScale, &dl);
            thread_call_enter_delayed(me->fWakeCall, dl);
        }
        return kIOReturnSuccess;
    }
    static void wakeFire(thread_call_param_t p, thread_call_param_t) {
        NVAccel *me = (NVAccel *)p;
        if (!me || !me->fDM) return;
        if (me->fRegLock) IOLockLock(me->fRegLock);
        uint8_t *dm = (uint8_t *)me->fDM;
        const uint32_t nest = *(const volatile uint32_t *)(dm + 0x10c);
        ALOG("pm: woke - display mode-change nest %u, pipes %u%s", nest, *(const volatile uint32_t *)(dm + 0x108),
             gNmxWakeFix ? "" : " (fix off: sysctl debug.nvaccel_wakefix=0)");
        me->reportPipeGates("after wake");
        if (gNmxWakeFix && nest > 0 && nest < 8) {
            ALOG("pm: a display-mode change is still open after the wake - finishing it (display_mode_did_change)");
            me->fDM->display_mode_did_change(0);
            OSAddAtomic64(1, &gCntNmxWakeFixes);
            me->reportPipeGates("after the wake fix");
        }
        if (me->fRegLock) IOLockUnlock(me->fRegLock);
        if (--me->fWakeTries > 0) {
            uint64_t dl; clock_interval_to_deadline(2000, kMillisecondScale, &dl);
            thread_call_enter_delayed(me->fWakeCall, dl);
        }
    }
    IONotifier *fIntelNote = nullptr;
    static NVAccel *gAccel;
    IOService *fFB = nullptr;
    IOService *volatile fFBs[kNvMaxFB] = {};
    unsigned fFBRegistered = 0;
    IOLock *fRegLock = nullptr;
    OSObject *fMetalPlugin = nullptr;
    static bool fbPublished(void *target, void *ref, IOService *newService, IONotifier *note) {
        (void)ref; (void)note;
        NVAccel *me = (NVAccel *)target;
        if (!me || !newService) return true;
        unsigned idx = 0;
        OSNumber *num = OSDynamicCast(OSNumber, newService->getProperty("IOFBDependentIndex"));
        if (num) idx = num->unsigned32BitValue();
        const char *what = "kept";
        if (idx >= kNvMaxFB) what = "OUT OF RANGE -- this head will get no pipe";
        else {
            newService->retain();
            if (OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFBs[idx])) OSBitOrAtomic(1u << idx, &gNvHeadsPublished);
            else { newService->release(); what = "DUPLICATE -- the first one keeps the slot"; }
        }
        newService->retain();
        if (idx == 0 && me->fFBs[0] == newService) {
            IOService *old;
            do { old = me->fFB; } while (!OSCompareAndSwapPtr(old, newService, (void *volatile *)&me->fFB));
            if (old == newService) newService->release();
            else if (old) {
                bool slotted = false;
                for (unsigned j = 0; j < kNvMaxFB; j++) if (me->fFBs[j] == old) slotted = true;
                ALOG("fFB: head 0 (%s) displaced %s, which published first (%s)", newService->getName(), old->getName(),
                     slotted ? "its extra reference returned" : "NOT slotted: keeping its reference rather than risk a reader");
                if (slotted) old->release();
            }
        // only head 0 may become the primary framebuffer: an Intel panel (index 4) publishing first became fFB (10-10 log:
        // "head 0 displaced NMIntelFB, which published first"), and on a laptop with nothing on the NVIDIA ports it stays
        } else if (idx != 0 || !OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFB)) newService->release();
        unsigned before = me->fDM ? me->fDM->getFramebufferCount() : 0xFFFFFFFFu;
        ALOG("%s published -- display machine framebuffer count = %u (index %u %s)", newService->getName(), before, idx, what);
        if (gNvAccelArmed)
            ALOG("  index %u published AFTER the arm: it gets a pipe only when debug.nvaccelfb=3 is written again, "
                 "and WindowServer composites it only after a WindowServer restart", idx);
        else
            ALOG("  to register it:  sudo sysctl -w debug.nvaccelfb=1   (2 = re-run the family's own sweep)");
        return true;
    }
    // Headless GPU (laptops whose panel and outputs are all on the Intel or AMD GPU: issues #76, #106). NVKMS finds no
    // display, so no NVRMFB head ever publishes and the withheld MetalPluginName was never restored: Metal never offered
    // the card, although NVRM and NVK run it fine (#106: vkCreateInstance enumerates the GTX 1660 Ti). Apple's own
    // two-GPU MacBook Pros work this way: the integrated GPU drives every display and WindowServer, and the discrete GPU
    // is a second Metal device that apps choose and macOS copies their frames from. So when no head has published by the
    // deadline, the plugin name goes back on WITHOUT arming: apps get the NVIDIA device, and WindowServer (allow-listed
    // "!WindowServer" = only when armed) keeps the GPU that owns the display. Nothing here touches a display pipe or a
    // mode change. Boot-arg -nvaccelnoheadless keeps the old behaviour.
    static constexpr uint32_t kHeadlessDeadlineSec = 60;   // chosen: NVRMFB heads publish within ~20 s of start (#76 log)
    struct thread_call *fHeadlessCall = nullptr;
    void armHeadlessCheck() {
        int off = 0;
        if (PE_parse_boot_argn("-nvaccelnoheadless", &off, sizeof off)) { ALOG("headless check: off (-nvaccelnoheadless)"); return; }
        fHeadlessCall = thread_call_allocate(&NVAccel::headlessCheck, this);
        if (!fHeadlessCall) { ALOG("headless check: thread_call_allocate FAILED -- a GPU with no display stays hidden from Metal"); return; }
        uint64_t when = 0;
        clock_interval_to_deadline(kHeadlessDeadlineSec, kSecondScale, &when);
        thread_call_enter_delayed(fHeadlessCall, when);
        ALOG("headless check: armed for %u s from now", kHeadlessDeadlineSec);
    }
    static void headlessCheck(void *p0, void *) {
        NVAccel *me = (NVAccel *)p0;
        if (!me || me != gAccel) return;
        if (me->fRegLock) IOLockLock(me->fRegLock);
        const char *why = me->fFB ? "a display head published" : !me->fMetalPlugin ? "no plugin name to restore"
                        : me->getProperty("MetalPluginName") ? "the plugin name is already back" : nullptr;
        if (why) {
            ALOG("headless check: not headless (%s) -- nothing to do", why);
        } else {
            // name the GPU that owns the display, so a log says which driver WindowServer is on
            char owner[64] = "none found";
            if (OSIterator *it = getMatchingServices(serviceMatching("IOFramebuffer"))) {
                while (OSObject *o = it->getNextObject()) {
                    IOService *s = OSDynamicCast(IOService, o);
                    if (s && !s->metaCast("NVRMNVDAFramebuffer")) { strlcpy(owner, s->getMetaClass()->getClassName(), sizeof owner); break; }
                }
                it->release();
            }
            me->setProperty("MetalPluginName", me->fMetalPlugin);
            me->setProperty("NVAccelHeadless", kOSBooleanTrue);
            gNvHeadless = 1;
            ALOG("headless check: no NVIDIA display head after %u s -- MetalPluginName restored WITHOUT arming: apps get "
                 "the NVIDIA Metal device, WindowServer stays on the display's GPU (%s)", kHeadlessDeadlineSec, owner);
        }
        if (me->fRegLock) IOLockUnlock(me->fRegLock);
    }
    void deliverModeChange() {
        if (!fDM) return;
        uint8_t *dm = (uint8_t *)fDM;
        ALOG("  delivering display_mode_will_change/did_change (nest %u, pipes %u)",
             *(const volatile uint32_t *)(dm + 0x10c), *(const volatile uint32_t *)(dm + 0x108));
        fDM->display_mode_will_change(0);
        fDM->display_mode_did_change(0);
        reportPipeGates("after deliverModeChange");
    }
    void reportPipeGates(const char *when) {
        uint8_t accelFlags = *(const volatile uint8_t *)((const uint8_t *)this + 0xc78);
        ALOG("  GATES %s: accel+0xc78 = 0x%02x (enabled bit 0x2 %s)",
             when, accelFlags, (accelFlags & 0x2) ? "SET" : "CLEAR <- kIOReturnNotReady");
        if (!fDM) return;
        uint8_t *dm = (uint8_t *)fDM;
        uint32_t n = *(const volatile uint32_t *)(dm + 0x108);
        if (n > 8) { ALOG("  GATES %s: implausible pipe count %u -- not walking it", when, n); return; }
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *p = *(uint8_t **)(dm + 0x88 + 8 * i);
            if (!p) { ALOG("  GATES %s: pipe %u is NULL", when, i); continue; }
            ALOG("  GATES %s: pipe %u active(+0x298) = %u, wsaaDefer(+0x282) = %u",
                 when, i, *(const volatile uint8_t *)(p + 0x298), *(const volatile uint8_t *)(p + 0x282));
        }
    }
    unsigned registerPendingLocked(const char *why) {
        unsigned added = 0;
        for (unsigned i = 0; i < kNvMaxFB; i++) {
            IOService *fb = fFBs[i];
            if (!fb) {
                // The Intel panel (index 4, NMIntelFB) does not wait for NVIDIA heads that never publish: studio with one
                // monitor publishes only head 0, and the walk stopped there, so the Intel head got no pipe (measured 10-10,
                // published 0x11 registered 0x1). Pipes are found by their framebuffer pointer, not their number.
                if (gNvIgpu && i < 4 && ((gNvHeadsPublished >> 4) & 1)) { i = 3; continue; }
                if (gNvHeadsPublished >> i)
                    ALOG("  %s: index %u has not published (published 0x%x): stopping, so no head above it gets a pipe "
                         "this walk", why, i, gNvHeadsPublished);
                break;
            }
            if (fFBRegistered & (1u << i)) continue;
            if (i == 4 && !gNvIgpu) break;   // the Intel panel waits for debug.nvaccel_igpu=1
            unsigned b = fDM->getFramebufferCount();
            ALOG("  %s: calling found_framebuffer(%s index %u) -- count before %u", why, fb->getName(), i, b);
            fDM->found_framebuffer((IOFramebuffer *)fb);
            unsigned c = fDM->getFramebufferCount();
            fFBRegistered |= 1u << i;
            gNvHeadsRegistered = fFBRegistered;
            if (c != b + 1) {
                ALOG("  %s: index %u -- count went %u -> %u; the family did not add it (already known, or the cap "
                     "accel+0xcb0 = %u refused it). Stopping here so no later head takes this one's pipe number.",
                     why, i, b, c, *(const volatile uint32_t *)((const uint8_t *)this + 0xcb0));
                break;
            }
            uint8_t *dm = (uint8_t *)fDM;
            void *pipe = (b < 16) ? *(void *const volatile *)(dm + 0x88 + 8 * b) : nullptr;
            if (!pipe) {
                *(volatile uint32_t *)(dm + 0x108) = b;
                ALOG("  %s: index %u -- the family stored a NULL pipe in slot %u (0xe3b5, unchecked) and counted it; "
                     "count put back %u -> %u so no broadcast dereferences it (0xe320). This head gets no pipe this boot.",
                     why, i, b, c, fDM->getFramebufferCount());
                break;
            }
            IOService *pfb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
            if (pfb != fb) {
                ALOG("  %s: index %u -- pipe %u (%p) drives %p, not %s %p; keeping the pipe (it is real), stopping "
                     "the walk so no later head's pipe number is off by this one", why, i, b, pipe, pfb, fb->getName(), fb);
                break;
            }
            ALOG("  %s: index %u -> pipe %u (%p), count %u", why, i, b, pipe, c);
            gNvHeadsPiped |= 1u << i;
            added++;
        }
        return added;
    }
    void onCountGrewLocked() {
        deliverModeChange();
        gNvAccelArmed = 1;
        if (fMetalPlugin && !getProperty("MetalPluginName")) {
            setProperty("MetalPluginName", fMetalPlugin);
            ALOG("  MetalPluginName restored -- a NEW process that asks Metal for this accelerator "
                 "can now load the plugin (already-running ones will not)");
        }
        if (!getProperty("IOGLBundleName")) {
            setProperty("IOGLBundleName", "AppleMetalOpenGLRenderer");
            ALOG("  IOGLBundleName=AppleMetalOpenGLRenderer -- NEW GL processes get a hardware renderer over Metal");
        }
        ALOG("  debug.nvaccelfb now reads 1 -- armed-only Metal clients may have a device THIS BOOT ONLY "
             "(heads published 0x%x, registered 0x%x, piped 0x%x)", gNvHeadsPublished, gNvHeadsRegistered, gNvHeadsPiped);
    }
    void registerFramebuffer(int how) {
        if (fRegLock) IOLockLock(fRegLock);
        registerFramebufferLocked(how);
        if (fRegLock) IOLockUnlock(fRegLock);
    }
    bool registerFramebufferLocked(int how) {
        if (!fDM) { ALOG("registerFramebuffer(%d): no display machine", how); return false; }
        if (!fFB) { ALOG("registerFramebuffer(%d): no framebuffer has published yet", how); return false; }
        unsigned before = fDM->getFramebufferCount();
        ALOG("registerFramebuffer(%d): framebuffer count before = %u", how, before);
        if (how == 2) {
            if (before != 0) ALOG("  already %u -- refusing to re-run the family's sweep", before);
            else { ALOG("  re-running IOAccelDisplayMachine::start(pci) -- the family's own sweep"); fDM->start(fPCI); }
        } else {
            registerPendingLocked("sysctl");
        }
        unsigned after = fDM->getFramebufferCount();
        ALOG("  framebuffer count after = %u", after);
        if (after > before) { onCountGrewLocked(); return true; }
#ifdef NMX_ACCEL
        // b76: NMXGPUFB is on the same PCI device as this accelerator and published first, so the family's own sweep at
        // start() already gave it a pipe (10-19 log: "count before = 1", IOAccelIndex 0 on NMXGPUFB). found_framebuffer
        // then adds nothing. Adopt that pipe instead of reporting a failure.
        if (gNmxWs && adoptExistingPipesLocked()) { onCountGrewLocked(); return true; }
#endif
        return false;
    }
#ifdef NMX_ACCEL
    bool adoptExistingPipesLocked() {
        if (!fDM) return false;
        uint8_t *dm = (uint8_t *)fDM;
        const uint32_t n = *(const volatile uint32_t *)(dm + 0x108);
        if (n > 8) { ALOG("  adopt: implausible pipe count %u -- not walking it", n); return false; }
        bool any = false;
        for (uint32_t p = 0; p < n; p++) {
            void *pipe = *(void *const volatile *)(dm + 0x88 + 8 * p);
            if (!pipe) continue;
            IOService *pfb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
            for (unsigned i = 0; i < kNvMaxFB; i++) {
                if (!fFBs[i] || fFBs[i] != pfb) continue;
                fFBRegistered |= 1u << i; gNvHeadsRegistered = fFBRegistered;
                gNvHeadsPiped |= 1u << i; any = true;
                ALOG("  adopt: pipe %u (%p) already drives %s (index %u) -- the family's sweep made it; using it", p, pipe,
                     pfb->getName(), i);
            }
        }
        if (!any) ALOG("  adopt: none of the family's %u pipe(s) drives a framebuffer of ours", n);
        return any;
    }
#endif
    const OSSymbol *fSymAlloc = nullptr, *fSymFree = nullptr;
    IOReturn askFramebufferForVram(struct NVRMVramRequest *r, bool alloc) {
        if (!fFB) return kIOReturnNotAttached;
        if (!fSymAlloc) fSymAlloc = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_ALLOC);
        if (!fSymFree)  fSymFree  = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_FREE);
        const OSSymbol *fn = alloc ? fSymAlloc : fSymFree;
        if (!fn) return kIOReturnNoMemory;
        return fFB->callPlatformFunction(fn, false, r, nullptr, nullptr, nullptr);
    }
    IOReturn askFramebufferForScanout(struct NVRMVramRequest *r, IOService *fb = nullptr) {
        if (!fb) fb = fFB;
        if (!fb) return kIOReturnNotAttached;
        if (!fSymScanout) fSymScanout = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_SCANOUT);
        return fb->callPlatformFunction(fSymScanout, false, r, nullptr, nullptr, nullptr);
    }
    const OSSymbol *fSymScanout = nullptr;
    IOService *pipeFramebuffer(const void *pipe, unsigned *idx) {
        if (!pipe) return nullptr;
        IOService *fb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
        if (!fb) return nullptr;
        for (unsigned i = 0; i < kNvMaxFB; i++)
            if (fFBs[i] == fb) { if (idx) *idx = i; return fb; }
        return nullptr;
    }
    IOService *fNVRM = nullptr;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           IOUserClient **handler) APPLE_KEXT_OVERRIDE {
        if ((type & 0xFFFF0000u) != 0x4E560000u) {
            IOReturn fr = IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler);
            if (fUcLogged < 40) { fUcLogged++;
                ALOG("newUserClient(type %u / 0x%x) -> family returned 0x%x, client %s",
                     (unsigned)type, (unsigned)type, (unsigned)fr,
                     (handler && *handler) ? (*handler)->getMetaClass()->getClassName() : "(none)"); }
            return fr;
        }
        IOUserClient *uc = nullptr;
#ifdef NMX_ACCEL
        // b80: the DRM render node over IOKit, for processes whose sandbox refuses open("/dev/dri/...") (WindowServer,
        // browser GPU processes). NMXGPU makes the client (nmx_drm_bridge.cpp); it is parked on a carrier as NVIDIA's are.
        {
            if (!fNVRM) {
                OSDictionary *m = serviceMatching("NMXGPU");
                if (m) fNVRM = waitForMatchingService(m, 100ULL * 1000 * 1000);
            }
            if (!fNVRM) { ALOG("newUserClient(0x%x): no NMXGPU service", type); return kIOReturnNotAttached; }
            const OSSymbol *fnSym = OSSymbol::withCStringNoCopy("nmxNewDrmClient");
            if (!fnSym) return kIOReturnNoMemory;
            IOReturn r = fNVRM->callPlatformFunction(fnSym, false, (void *)owningTask,
                                                     (void *)(uintptr_t)(type & 0xFFFFu), &uc, nullptr);
            fnSym->release();
            if (r != kIOReturnSuccess || !uc) { ALOG("newUserClient(0x%x): NMXGPU refused the DRM bridge (0x%x)", type, r); return r ? r : kIOReturnNoResources; }
        }
#else
        if (!fNVRM) {
            OSDictionary *m = serviceMatching("NVRM");
            if (m) fNVRM = waitForMatchingService(m, 100ULL * 1000 * 1000);
        }
        if (!fNVRM) { ALOG("newUserClient(0x%x): no NVRM service", type); return kIOReturnNotAttached; }

        const OSSymbol *fnSym = OSSymbol::withCStringNoCopy("nvNewAccelClient");
        if (!fnSym) return kIOReturnNoMemory;
        IOReturn r = fNVRM->callPlatformFunction(fnSym, false, (void *)owningTask,
                                                 (void *)(uintptr_t)(type & 0xFFFFu), &uc, nullptr);
        fnSym->release();
        if (r != kIOReturnSuccess || !uc) { ALOG("newUserClient(0x%x): NVRM refused (0x%x)", type, r); return r ? r : kIOReturnNoResources; }
#endif

        IOService *carrier = nullptr;
        for (unsigned i = 0; i < NV_CARRIERS && !carrier; i++)
            carrier = fCarriers[(fCarrierNext + i) % NV_CARRIERS];
        fCarrierNext++;
        if (!carrier) {
            ALOG("newUserClient(0x%x): NO CARRIER available -- refusing rather than attaching to the "
                 "accelerator, which sleeps at ~1019 children", type);
            uc->release(); return kIOReturnNoResources;
        }
        if (!uc->attach(carrier))  { ALOG("newUserClient(0x%x): attach failed", type); uc->release(); return kIOReturnInternalError; }
        if (!uc->start(carrier))   { ALOG("newUserClient(0x%x): start failed", type); uc->detach(carrier); uc->release(); return kIOReturnInternalError; }
        ALOG("newUserClient(0x%x) -> %s on the ACCELERATOR (rm type %u)", type, uc->getMetaClass()->getClassName(), type & 0xFFFFu);
        *handler = uc;
        return kIOReturnSuccess;
    }
    static void triggerFromSysctl(int how) {
        if (!gAccel) { ALOG("sysctl debug.nvaccelfb=%d but no NVAccel is running", how); return; }
#ifdef NMX_ACCEL
        /* b112: the Phase 3 boot freezes were this panic (Kernel-2026-10-20-132943, uptime 10.9 s, nmxautoarm=1):
           nvaccelfb -> registerFramebufferLocked -> deliverModeChange -> IOAccelLegacyDisplayPipe::displayModeDidChange
           -> IOFramebuffer::getAttributeExt, NULL + 0x120. An IOFramebuffer has no controller until WindowServer opens
           it (nmx_fb.cpp), and the mode-change broadcast reads it. With debug=0x100 the Mac halts: black screen, no SSH.
           NMXGPUFB (b103+) sets NMXFBOpened when WindowServer opens it: until then the arm is refused, not crashed. */
        if (gNmxWs && how >= 1) {
            IOService *fb = gAccel->fFB;
            const bool nmxfb = fb && fb->metaCast("NMXGPUFB") != nullptr;
            if (!fb || (nmxfb && fb->getProperty("NMXFBOpened") != kOSBooleanTrue)) {
                ALOG("sysctl debug.nvaccelfb=%d REFUSED: the framebuffer %s - arming now would panic the Mac "
                     "(IOFramebuffer::getAttributeExt on a framebuffer WindowServer has not opened). Try again once the "
                     "login window is on screen. (Needs NMXGPU build 103 or newer for the NMXFBOpened signal.)",
                     how, !fb ? "has not published yet" : "is not opened by WindowServer yet");
                return;
            }
        }
#endif
        if (how >= 4) gNvAperture = 1;
        if (how >= 3) {
            gNvScanout = 1;
            ALOG("  level %d: registering any published head that has no pipe yet", how);
            if (gAccel->fRegLock) IOLockLock(gAccel->fRegLock);
            if (gAccel->registerFramebufferLocked(1))
                ALOG("sysctl debug.nvaccelfb=%d: scanout-backed display pipe ENABLED; the registration just delivered the mode "
                     "change -- not delivering it twice", how);
            else {
                ALOG("sysctl debug.nvaccelfb=%d: scanout-backed display pipe ENABLED, re-delivering the mode change", how);
                gAccel->deliverModeChange();
            }
            if (gAccel->fRegLock) IOLockUnlock(gAccel->fRegLock);
            return;
        }
        gAccel->registerFramebuffer(how);
    }
    static void enableFromHook(const char *why) {
        if (!gAccel) return;
        gAccel->enableAccelerator();
        gAccel->fEnableCount++;
        ALOG("enableAccelerator() from %s (call #%u)", why, gAccel->fEnableCount);
        gAccel->setProperty("NVAccelEnabledFrom", why);
        gAccel->setProperty("NVAccelEnableCount", gAccel->fEnableCount, 32);
    }
    unsigned fEnableCount = 0;
    unsigned fUcLogged = 0;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE {
        ALOG("probe(%s) score %d", provider ? provider->getName() : "?", score ? (int)*score : 0);
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            const unsigned cls = pci->configRead8(0x0B), sub = pci->configRead8(0x0A);
            const unsigned ven = pci->configRead16(0x00), dev = pci->configRead16(0x02);
#ifdef NMX_ACCEL
            if (cls != 0x03 || (ven != 0x1002 && ven != 0x8086)) {
#else
            if (cls != 0x03 || (sub != 0x00 && sub != 0x02) || ven != 0x10de || dev < 0x0020) {
#endif
                ALOG("%04x:%04x class %02x.%02x is not an NVIDIA display device we support - not claiming",
                     ven, dev, cls, sub);
                return NULL;
            }
            ALOG("NVIDIA display device %04x:%04x class %02x.%02x - claiming", ven, dev, cls, sub);
        }
        return IOGraphicsAccelerator2::probe(provider, score);
    }
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        { if (nvrm_driver_off()) return false; }
#ifdef NMX_ACCEL
        {   // boot-loop guard while the accelerator is new on Intel (as nvaccel=1 for NVIDIA): it starts only with nmxaccel=1
            uint32_t on = 0;
            if (!PE_parse_boot_argn("nmxaccel", &on, sizeof on) || !on) { ALOG("boot-arg nmxaccel=1 absent: NOT starting"); return false; }
        }
        // NMXGPU decides which AMD/Intel GPUs are driven (nmxamd=1 / nmxintel=1 / nullmoth-gpu); this accelerator only
        // starts on a device it has claimed, so a GPU macOS drives itself never gets a second accelerator
        // No waiting here: NMXGPU attaches from a thread once the disks with its firmware are mounted, and mounting waits
        // for matching to settle - a start() that blocked matching for the claim would hold the boot at the logo. NMXGPU
        // re-runs matching on the device (registerService) after it sets nmxgpu-claimed, which offers it here again.
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            if (pci->getProperty("nmxgpu-claimed") != kOSBooleanTrue) {
                ALOG("NMXGPU has not claimed %04x:%04x yet - not starting now; offered again when it does",
                     pci->configRead16(0x00), pci->configRead16(0x02));
                return false;
            }
            ALOG("NMXGPU drives %04x:%04x - starting", pci->configRead16(0x00), pci->configRead16(0x02));
        }
#else
        uint32_t gate = 0;
        if (!PE_parse_boot_argn("nvaccel", &gate, sizeof gate) || !gate) { ALOG("boot-arg nvaccel=1 absent: NOT starting (boot-loop guard)"); return false; }
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            unsigned waited = 0;
            while (pci->getProperty("nvrm-claimed") != kOSBooleanTrue && waited < 300) { IOSleep(100); waited++; }
            if (pci->getProperty("nvrm-claimed") != kOSBooleanTrue) {
                ALOG("NVRM never claimed %04x:%04x in 30 s - NOT starting the accelerator on a GPU with no RM",
                     pci->configRead16(0x00), pci->configRead16(0x02));
                return false;
            }
            ALOG("NVRM owns %04x:%04x (waited %u ms) - starting", pci->configRead16(0x00), pci->configRead16(0x02), waited * 100);
        }
#endif
        ALOG("start(%s): calling IOGraphicsAccelerator2::start — the family's own bring-up", provider ? provider->getName() : "?");
        bool r = IOGraphicsAccelerator2::start(provider);
        ALOG("IOGraphicsAccelerator2::start -> %d", r);
        if (r) {
            registerService(); ALOG("registerService() called — Metal can now find MetalPluginName=%s", "NVMTLDriver");
            gAccel = this; gEnableAccel = &NVAccel::enableFromHook;
            for (unsigned i = 0; i < NV_CARRIERS; i++) {
                IOAccelNVRMCarrier *c = OSTypeAlloc(IOAccelNVRMCarrier);
                if (!c) break;
                if (!c->init()) { c->release(); break; }
                if (!c->attach(this)) { c->release(); break; }
                if (!c->start(this)) { c->detach(this); c->release(); break; }
                fCarriers[i] = c;
            }
            { unsigned n = 0; for (unsigned i = 0; i < NV_CARRIERS; i++) if (fCarriers[i]) n++;
              ALOG("user-client carriers: %u of %u ready (each holds ~1019 clients before "
                   "IOService::attach starts sleeping)", n, (unsigned)NV_CARRIERS); }
            enableFromHook("start");
            fPCI = OSDynamicCast(IOPCIDevice, provider);
            ALOG("display machine framebuffer count after the family sweep = %u", fDM ? fDM->getFramebufferCount() : 0xFFFFFFFFu);
            fRegLock = IOLockAlloc();
#ifdef NMX_ACCEL
            fWakeCall = thread_call_allocate(&NVAccel::wakeFire, this);
            fPmNote = fWakeCall ? registerSleepWakeInterest(&NVAccel::pmMessage, this, nullptr) : nullptr;
            ALOG("pm: watching sleep/wake -> %p (b132 wake fix %s)", fPmNote, gNmxWakeFix ? "on" : "off");
#endif
            ALOG("multi-head: up to %u heads, registration lock %p%s", kNvMaxFB, fRegLock,
                 fRegLock ? "" : " -- NULL: two sysctl writers could interleave a registration");
#ifdef NMX_ACCEL
            // AMD/Intel: the Metal device is for apps; WindowServer draws into NMXGPU's framebuffer, and the plugin copies
            // finished frames into the IOSurfaces it reads (the headless path NVIDIA laptops use)
            nvaccelInstallSysctl();
            gNvHeadless = 1;
            setProperty("NMXAccelHeadless", kOSBooleanTrue);
            ALOG("AMD/Intel accelerator up: Metal device for apps, frames copied to the framebuffer WindowServer draws");
            OSDictionary *fbm = nullptr;
            { uint32_t ws = 0;
              if (PE_parse_boot_argn("nmxws", &ws, sizeof ws) && ws) {
                  gNmxWs = 1;
                  nvaccelInstallSysctl();
                  fbm = serviceMatching("NMXGPUFB");
                  ALOG("nmxws=1: watching for NMXGPUFB (head 0) -- WindowServer acceleration arms with "
                       "`sysctl -w debug.nvaccelfb=1 debug.nvaccel_iop=1` and a WindowServer restart");
              } }
#else
            OSDictionary *fbm = serviceMatching("NVRMNVDAFramebuffer");
#endif
            if (fbm) {
                fFBNote = addMatchingNotification(gIOFirstPublishNotification, fbm, &NVAccel::fbPublished, this, nullptr, 0);
                fbm->release();
                ALOG("watching for NVRMFramebuffer to publish -> notifier %p", fFBNote);
                // a laptop whose built-in panel is on the Intel GPU: NMIntelFB publishes as head 4 of this accelerator
                if (OSDictionary *im = serviceMatching("NMIntelFB")) {
                    fIntelNote = addMatchingNotification(gIOFirstPublishNotification, im, &NVAccel::fbPublished, this, nullptr, 0);
                    im->release();
                    ALOG("watching for an Intel built-in panel (NMIntelFB) -> notifier %p", fIntelNote);
                }
                nvaccelInstallSysctl();
#ifdef NMX_ACCEL
                if (false) {   // NMX: apps already use the Metal device; WindowServer is gated by the plugin's armed check
#else
                if (getProperty("MetalPluginName")) {
#endif
                    OSObject *mp = getProperty("MetalPluginName"); if (mp) mp->retain();
                    fMetalPlugin = mp;
                    removeProperty("MetalPluginName");
                    ALOG("MetalPluginName withheld -- nothing will load the Metal plugin until "
                         "`sysctl -w debug.nvaccelfb=1` puts it back, and a reboot takes it away again");
                    armHeadlessCheck();
                }
            } else ALOG("serviceMatching(NVRMFramebuffer) returned NULL -- cannot watch for the framebuffer");
        }
        return r;
    }
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE { ALOG("stop"); IOGraphicsAccelerator2::stop(provider); }
    virtual IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVAccel, IOGraphicsAccelerator2)
NVAccel *NVAccel::gAccel = nullptr;

template <class PipeT> static IOAccelMemory *nvpipe_initFramebufferResource(PipeT *self, unsigned int index, IOAccelResource2 *res)
{
    if (!gNvScanout) {
        ALOG("NVDisplayPipe::initFramebufferResource(index %u): scanout gate CLOSED -- returning NULL, "
             "which is the family's own default (sysctl debug.nvaccelfb=3 opens it)", index);
        return nullptr;
    }
    NVAccel *a = NVAccel::gAccel;
    if (!a || !res) { ALOG("NVDisplayPipe::initFramebufferResource: no accelerator or no resource"); return nullptr; }
    unsigned head = 0;
    IOService *fb = a->pipeFramebuffer(self, &head);
    if (!fb) {
        ALOG("NVDisplayPipe::initFramebufferResource(index %u): pipe %p drives framebuffer %p, which is none of ours -- "
             "refusing (never head 0's pages: that IS the bug)", index, self,
             *(void *const volatile *)((const uint8_t *)self + 0x98));
        return nullptr;
    }

    struct NVRMVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = NVRM_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&r, fb);
    if (ret != kIOReturnSuccess || !r.phys || !r.actualSize) {
        ALOG("NVDisplayPipe::initFramebufferResource: framebuffer would not describe its scanout (0x%x)", (unsigned)ret);
        return nullptr;
    }

    OSObject *o = a->createVidMemory(nullptr, res, r.actualSize, nullptr);
    NVVidMemory *m = OSDynamicCast(NVVidMemory, o);
    if (!m) {
        ALOG("NVDisplayPipe::initFramebufferResource: createVidMemory gave %p, not an NVVidMemory", o);
        if (o) o->release();
        return nullptr;
    }
#ifdef NMX_ACCEL
    // b92 (10-19): on Intel the framebuffer resource is a SHADOW in ordinary RAM (allocPhysical gives it nvAllocVram
    // memory), not the live scan-out. WindowServer reads it with the GPU (the login transition's CaptureSurfaceMetal made
    // a 2560x1600 texture over it); the scan-out sits in the GPU aperture, which Mesa can only import as a user pointer
    // of device memory, and EXECBUFFER2 failed with EFAULT -> VK_ERROR_DEVICE_LOST -> "Vulkan is not up" -> WindowServer
    // abort at every login (10-19 20:40). Frames still reach the panel through the swap copy, which copies whichever
    // swap argument holds the picture - self shadow too, when WindowServer draws into it directly.
    {
        static SInt32 gShadowLogs;
        if (OSIncrementAtomic(&gShadowLogs) < 6)
            ALOG("NVDisplayPipe::initFramebufferResource(head %u, index %u, res %p) -> vidmem %p, a SHADOW in system RAM "
                 "(%llu bytes; the scan-out at phys 0x%llx is fed by the swap copy)", head, index, res, m,
                 (unsigned long long)r.actualSize, (unsigned long long)r.phys);
        return (IOAccelMemory *)m;
    }
#endif
    m->fScanout = true;
    m->fPhys    = r.phys;
    m->fBytes   = r.actualSize;
    *(volatile unsigned long long *)((unsigned char *)m + 0xc8) = r.phys;
    ALOG("NVDisplayPipe::initFramebufferResource(head %u, index %u, res %p) -> vidmem %p over the LIVE SCANOUT "
         "phys 0x%llx %u x %u pitch %u (%llu bytes)",
         head, index, res, m, (unsigned long long)r.phys, r.width, r.height, r.pitch,
         (unsigned long long)r.actualSize);
    return (IOAccelMemory *)m;
}
IOAccelMemory *NVDisplayPipe::initFramebufferResource(unsigned int index, IOAccelResource2 *res) { return nvpipe_initFramebufferResource(this, index, res); }
#if defined(NMX_ACCEL) && !defined(NM_TAHOE)
IOAccelMemory *NMXDisplayPipeM::initFramebufferResource(unsigned int index, IOAccelResource2 *res) { return nvpipe_initFramebufferResource(this, index, res); }
#endif

static NVVidMemory *nvres_vidmem(IOAccelResource2 *r)
{
    if (!r) return nullptr;
    OSObject *o = *(OSObject **)((unsigned char *)r + 0x88);
    return OSDynamicCast(NVVidMemory, o);
}

bool NVResource::addToAperture()
{
    NVVidMemory *m = nvres_vidmem(this);
    if (fApLogged < 4) { fApLogged++;
        ALOG("NVResource::addToAperture -> true (vidmem %p phys 0x%llx %llu bytes, already in BAR1)",
             m, m ? m->fPhys : 0ull, m ? m->fBytes : 0ull); }
    return m && m->fPhys && m->fBytes;
}

void NVResource::removeFromAperture() { }

void *NVResource::getApertureMemoryDescriptor(unsigned long long *a0)
{
    if (a0) *a0 = 0;
    if (!gNvAperture) {
        if (fApLogged < 4) { fApLogged++;
            ALOG("NVResource::getApertureMemoryDescriptor: aperture gate CLOSED -- returning nullptr "
                 "(sysctl debug.nvaccelfb=4 opens it)"); }
        return nullptr;
    }
    if (fApDesc) return fApDesc;
    NVVidMemory *m = nvres_vidmem(this);
    if (!m || !m->fPhys || !m->fBytes) {
        if (fApLogged < 8) { fApLogged++;
            ALOG("NVResource::getApertureMemoryDescriptor: no BAR1-backed vidmem at +0x88 (mem %p)", m); }
        return nullptr;
    }
    if (m->fMd) {   // b90: ordinary RAM from the framebuffer: hand out its own descriptor (cached, any number of segments)
        m->fMd->retain(); fApDesc = m->fMd;
        if (fApLogged < 8) { fApLogged++;
            ALOG("NVResource::getApertureMemoryDescriptor -> %p, system memory (%llu bytes, first page 0x%llx)", fApDesc, m->fBytes, m->fPhys); }
        return fApDesc;
    }
    IODeviceMemory *d = IODeviceMemory::withRange((IOPhysicalAddress)m->fPhys, (IOPhysicalLength)m->fBytes);
    if (!d) { ALOG("NVResource::getApertureMemoryDescriptor: IODeviceMemory::withRange(0x%llx, %llu) FAILED",
                   m->fPhys, m->fBytes); return nullptr; }
    fApDesc = d;
    if (fApLogged < 8) { fApLogged++;
        ALOG("NVResource::getApertureMemoryDescriptor -> %p over BAR1 phys 0x%llx (%llu bytes), offset 0",
             d, m->fPhys, m->fBytes); }
    return fApDesc;
}

void NVResource::free()
{
    if (fApDesc) { fApDesc->release(); fApDesc = nullptr; }
    OSAddAtomic64(1, &gCntResFree);
    IOAccelResource2::free();
}

bool NVVidMemory::allocPhysical()
{
    if (fScanout) {
        if (!fPhys || !fBytes) { ALOG("NVVidMemory::allocPhysical(%p): scanout with no address", this); return false; }
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
        ALOG("NVVidMemory::allocPhysical(%p): SCANOUT phys 0x%llx (%llu bytes) -- already backed, nothing allocated",
             this, fPhys, fBytes);
        return true;
    }
    if (fRmHandle) {
        ALOG("NVVidMemory::allocPhysical(%p): already backed by 0x%llx (%llu bytes) -- false, as AMD's does",
             this, fPhys, fBytes);
        return false;
    }
    const unsigned long long want = *(const volatile unsigned long long *)((const unsigned char *)this + 0x40);
    if (!want) { ALOG("NVVidMemory::allocPhysical(%p): length at +0x40 is 0 -- nothing to back", this); return false; }
    if (!NVAccel::gAccel) { ALOG("NVVidMemory::allocPhysical(%p): no accelerator", this); return false; }

    struct NVRMVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = NVRM_VRAM_ABI_VERSION;
    r.size    = want;
    IOReturn ret = NVAccel::gAccel->askFramebufferForVram(&r, true);
    if (ret != kIOReturnSuccess || !r.handle || !r.phys) {
        OSAddAtomic64(1, &gCntVmRefused);
        ALOG("NVVidMemory::allocPhysical(%p): %llu bytes REFUSED by the framebuffer (0x%x)",
             this, want, (unsigned)ret);
        return false;
    }
    fRmHandle = r.handle; fKva = r.kva; fPhys = r.phys; fBytes = r.actualSize; fAllocationCookie = r.allocationCookie;
    fMd = (r.flags & NVRM_VRAM_FLAG_MD) ? (IOMemoryDescriptor *)r.handle : nullptr;
    OSAddAtomic64(1, &gCntVmAlloc); OSAddAtomic64((SInt64)fBytes, &gCntVmAllocBytes);
    *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
    ALOG("NVVidMemory::allocPhysical(%p): %llu bytes of REAL VRAM -- phys 0x%llx kva %p [BAR1 %llu]",
         this, fBytes, fPhys, fKva, r.mappedTotal);
    return true;
}

void NVVidMemory::deallocPhysical()
{
    if (fScanout) {
        ALOG("NVVidMemory::deallocPhysical(%p): SCANOUT -- not ours to free, only forgetting it", this);
        fPhys = 0; fBytes = 0; fScanout = false;
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
        return;
    }
    if (!fRmHandle) return;
#ifdef NMX_ACCEL
    nmxShadowForget(fKva);
    nmxSyncForget(fKva);
#endif
    OSAddAtomic64(1, &gCntVmDealloc); OSAddAtomic64((SInt64)fBytes, &gCntVmDeallocBytes);
    struct NVRMVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = NVRM_VRAM_ABI_VERSION;
    r.handle = fRmHandle; r.kva = fKva; r.actualSize = fBytes; r.allocationCookie = fAllocationCookie;
    if (NVAccel::gAccel) NVAccel::gAccel->askFramebufferForVram(&r, false);
    fRmHandle = nullptr; fKva = nullptr; fPhys = 0; fBytes = 0; fAllocationCookie = 0; fMd = nullptr;
    *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
}

class IOSurface {
public:
    IOMemoryDescriptor *getMemoryDescriptor() const;
    unsigned long long  getWidth() const;
    unsigned long long  getHeight() const;
    unsigned long long  getBytesPerRow() const;
    unsigned int        getPixelFormat() const;
    unsigned int        getSurfaceID() const;
};
class IOAccelDisplayPipeTransaction2 {
public:
    IOSurface *getPlaneIOSurface(unsigned int, unsigned int) const;
    OSObject  *getPlaneResource(unsigned int, unsigned int) const;
};

static int    gNvIop = 0;
static SInt64 gCntIopOk, gCntIopFail, gCntIopEmpty, gCntIopSrcRes, gCntIopSrcSurf, gCntIopBlank;
static SInt64 gCntIopCopyNs, gCntIopCopyMaxNs, gCntIopCopies;    /* b118: time spent copying frames to the panel */
static bool nvIopSlowCopy(void)
{
    static int v = -1;
    if (v < 0) { uint32_t b = 0; v = (PE_parse_boot_argn("nmxslowcopy", &b, sizeof b) && b) ? 1 : 0; }
    return v == 1;
}
static SInt64 gCntIopOkHead[kNvMaxFB];
#define NVIOP_LOG_MAX 40
static unsigned gIopLogs = 0;
#define NVIOPLOG(fmt, ...) do { if (gIopLogs < NVIOP_LOG_MAX) { gIopLogs++; ALOG("iop: " fmt, ##__VA_ARGS__); } } while (0)
#define NVIOP_FAIL(rc, fmt, ...) do { OSAddAtomic64(1, &gCntIopFail); NVIOPLOG(fmt, ##__VA_ARGS__); return (rc); } while (0)

/* b125: shared by the flip path below and the pipe teardown here */
static IOLock *gIopCopyLock;
static int gIopFlipDb = -1;
static SInt64 gCntIopDbFlips, gCntIopDbFail, gCntIopDbNs;
struct NmxFlipReq { uint32_t version, op; void *kva; uint32_t pitch, w, h; int32_t err; };
static void nvIopFlipHome(IOService *fb)
{
    static const OSSymbol *sym;
    if (!sym) sym = OSSymbol::withCStringNoCopy("nmxFlip");
    if (!fb || !sym || !fb->metaCast("NMXGPUFB")) return;
    NmxFlipReq r; bzero(&r, sizeof r); r.version = 1; r.op = 2;
    (void)fb->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr);
}
template <class PipeT> static void nvpipe_iopDropSources(PipeT *self)
{
    if (gIopFlipDb == 1 && NVAccel::gAccel) {      /* b125: the pipe is going away - the panel goes back to buffer 0 */
        unsigned head = 0; IOService *fb = NVAccel::gAccel->pipeFramebuffer(self, &head);
        IOLock *L = gIopCopyLock; if (L) IOLockLock(L);
        nvIopFlipHome(fb);
        if (L) IOLockUnlock(L);
    }
    for (unsigned i = 0; i < PipeT::kIopSrcSlots; i++) {
        typename PipeT::NVIopSrc *e = &self->fIopSrc[i];
        if (e->map) { e->map->release(); e->map = nullptr; }
        if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
        e->id = 0;
    }
}

void NVDisplayPipe::free()
{
    nvpipe_iopDropSources(this);
    IOAccelLegacyDisplayPipe::free();
}
#if defined(NMX_ACCEL) && !defined(NM_TAHOE)
void NMXDisplayPipeM::free()
{
    nvpipe_iopDropSources(this);
    IOAccelDisplayPipe::free();
}
#endif

template <class PipeT> static const void *nvpipe_iopMapSurface(PipeT *self, unsigned id, IOMemoryDescriptor *md, unsigned long long need)
{
    for (unsigned i = 0; i < PipeT::kIopSrcSlots; i++)
        if (self->fIopSrc[i].md == md && self->fIopSrc[i].id == id && self->fIopSrc[i].map)
            return (self->fIopSrc[i].map->getLength() >= need) ? (const void *)self->fIopSrc[i].map->getVirtualAddress() : nullptr;
    if (md->getLength() < need) return nullptr;
    typename PipeT::NVIopSrc *e = &self->fIopSrc[self->fIopNext % PipeT::kIopSrcSlots]; self->fIopNext++;
    if (e->map) { e->map->release(); e->map = nullptr; }
    if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
    e->id = 0;
    if (md->prepare() != kIOReturnSuccess) return nullptr;
    IOMemoryMap *m = md->map(kIOMapReadOnly);
    if (!m) { md->complete(); return nullptr; }
    md->retain(); e->md = md; e->map = m; e->id = id;
    NVIOPLOG("mapped source surface 0x%x (%llu bytes) into slot %u", id, (unsigned long long)m->getLength(), (self->fIopNext - 1) % PipeT::kIopSrcSlots);
    return (const void *)m->getVirtualAddress();
}

IOReturn NVDisplayPipe::validateTransaction(IOAccelDisplayPipeTransaction2 *)
{
    return kIOReturnSuccess;
}
#if defined(NMX_ACCEL) && !defined(NM_TAHOE)
IOReturn NMXDisplayPipeM::validateTransaction(IOAccelDisplayPipeTransaction2 *)
{
    return kIOReturnSuccess;
}
#endif

static bool nvAccelIopAsyncBusy(const void *pipe);
bool NVDisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2 *)
{
    return gNvIop != 0 && !nvAccelIopAsyncBusy(this);
}
#if defined(NMX_ACCEL) && !defined(NM_TAHOE)
bool NMXDisplayPipeM::isTransactionComplete(IOAccelDisplayPipeTransaction2 *)
{
    return gNvIop != 0 && !nvAccelIopAsyncBusy(this);
}
#endif

static void nvAccelSurfScanoutSync(IOSurface *s);
static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe);
static void nvAccelIopHome(IOService *fb, unsigned head);
/*
 * b123: settle copy. Each transaction copies WindowServer's composited surface to the panel at once. When the last
 * frame of a burst (a key press on the lock screen, the end of an animation) is copied before the GPU has finished
 * writing it, the panel keeps showing the older picture until something else changes - the 10-21 lock screen showed 2
 * or 3 password dots for 4 typed characters. After every frame the same surface is copied again 40 ms and 150 ms
 * later, unless a newer frame came in meanwhile (generation check under one lock shared with the transaction copy).
 * Costs ~1.3 ms per settle, only when the screen goes idle. Off: sysctl debug.nvaccel_iop_settle=0.
 */
static volatile UInt32 gIopGen;
static struct thread_call *gIopSettleCall;
static struct { IOSurface *s; IOService *fb; unsigned head, w, h, pitch; UInt32 gen; int pass; uint64_t sum; unsigned sumWords; } gIopSettle;   // under gIopCopyLock
static int gIopSettleOn = 1;
static SInt64 gCntIopSettle, gCntIopSettleSkip;
/* b129: late-change detector. A checksum of the surface is taken when the frame is copied and again at every settle pass
   (40, 190, 440, 1040 ms). A change means WindowServer's GPU wrote the frame AFTER performTransaction handed it to us -
   10-22: typed characters stayed invisible until the next key (copy and flip alike). The later passes also show it. */
static int gIopLateChk = 0;   /* b130: off by default (1.1 ms per frame); b129 showed no late changes */
static SInt64 gCntIopLate[4], gCntIopLateMs;
static uint64_t nvIopSum(const volatile unsigned int *src, unsigned spr, unsigned words, unsigned h)
{
    uint64_t a = 0, b = 0;
    const unsigned q = words / 2;
    for (unsigned y = 0; y < h; y++) {
        const uint64_t *r = (const uint64_t *)(const void *)(src + (unsigned long)y * spr);
        for (unsigned x = 0; x < q; x++) { const uint64_t v = r[x]; a += v; b ^= v + x + y; b = (b << 7) | (b >> 57); }
    }
    return a ^ (b * 0x9E3779B97F4A7C15ull);
}
static IOLock *nvIopCopyLock(void)
{
    if (!gIopCopyLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gIopCopyLock)) IOLockFree(l); }
    return gIopCopyLock;
}
static bool nvIopSlowCopy(void);
static void nvIopCopyRows(volatile unsigned int *d, unsigned dpr, const volatile unsigned int *src, unsigned spr, unsigned words,
                          unsigned h, unsigned pitch)
{
    if (nvIopSlowCopy()) {
        for (unsigned y = 0; y < h; y++) {
            const volatile unsigned int *sp = src + (unsigned long)y * spr;
            volatile unsigned int *dp = d + (unsigned long)y * dpr;
            for (unsigned x = 0; x < words; x++) dp[x] = sp[x];
        }
    } else if (spr == dpr && words == spr) {
        memcpy((void *)d, (const void *)src, (size_t)h * pitch);
    } else {
        for (unsigned y = 0; y < h; y++)
            memcpy((void *)(d + (unsigned long)y * dpr), (const void *)(src + (unsigned long)y * spr), (size_t)words * 4);
    }
}
static void nvIopSettleArm(uint32_t ms)
{
    if (!gIopSettleCall) return;
    uint64_t when = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &when);
    thread_call_enter_delayed(gIopSettleCall, when);
}
/*
 * b125 (R2): flip instead of copy-to-the-visible-buffer. With boot-arg nmxflip=1 (or sysctl debug.nvaccel_iop_flipdb=1)
 * each frame is copied into the scan-out buffer that is NOT on the panel and NMXGPU flips the primary plane to it at the
 * next vblank (atomic commit, blocking until it is on screen): no tearing, no half-copied frame, a frame cadence locked
 * to the panel. Turning it off flips back to the IOFramebuffer's own buffer. Caller holds gIopCopyLock.
 */
static bool nvIopFlipDbOn(void)
{
    /* b143: ON by default (10-22/23: stable, no lag, fine across sleep/wake). Boot-arg nmxflip=0 turns it off. */
    if (gIopFlipDb < 0) { uint32_t v = 1; PE_parse_boot_argn("nmxflip", &v, sizeof v); gIopFlipDb = v ? 1 : 0;
                          ALOG("iop: b125 double-buffered flip %s (boot-arg nmxflip=1, sysctl debug.nvaccel_iop_flipdb)", gIopFlipDb ? "ON" : "off"); }
    return gIopFlipDb == 1;
}
static bool nvIopPresentFlip(IOService *fb, const volatile unsigned int *src, unsigned w, unsigned h, unsigned pitch)
{
    static const OSSymbol *sym;
    if (!sym) sym = OSSymbol::withCStringNoCopy("nmxFlip");
    if (!fb || !sym || !fb->metaCast("NMXGPUFB")) return false;
    NmxFlipReq r; bzero(&r, sizeof r); r.version = 1; r.op = 0;
    if (fb->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr) != kIOReturnSuccess || !r.kva || r.w != w || r.h != h || r.pitch < 4) {
        if (OSAddAtomic64(1, &gCntIopDbFail) < 6) ALOG("iop: flip: no back buffer for %ux%u (err %d, back %ux%u) - copied as before", w, h, r.err, r.w, r.h);
        return false;
    }
    const uint64_t t0 = mach_absolute_time();
    const unsigned words = (((pitch < r.pitch) ? pitch : r.pitch) & ~3u) / 4;
    nvIopCopyRows((volatile unsigned int *)r.kva, r.pitch / 4, src, pitch / 4, words, h, pitch);
    r.op = 1;
    const IOReturn cr = fb->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr);
    OSAddAtomic64((SInt64)(mach_absolute_time() - t0), &gCntIopDbNs);
    if (cr != kIOReturnSuccess) { OSAddAtomic64(1, &gCntIopDbFail); return false; }
    OSAddAtomic64(1, &gCntIopDbFlips);
    return true;
}
static void nvIopSettleFire(void *, void *)
{
    IOLock *L = nvIopCopyLock();
    if (!L) return;
    /* b127: never block a thread-call thread. 10-21 panic: "thread_call group 'high' reached max thread cap (500):
       blocked 499" - each settle waited here while a frame's flip held the lock; a busy lock now just retries later. */
    if (!IOLockTryLock(L)) { OSAddAtomic64(1, &gCntIopSettleSkip); nvIopSettleArm(40); return; }
    IOSurface *s = gIopSettle.s;
    if (!gIopSettleOn || !s || gIopSettle.gen != gIopGen || !NVAccel::gAccel) { OSAddAtomic64(1, &gCntIopSettleSkip); IOLockUnlock(L); return; }
    IOMemoryDescriptor *md = s->getMemoryDescriptor();
    IOMemoryMap *m = md ? md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly) : nullptr;
    struct NVRMVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = NVRM_VRAM_ABI_VERSION;
    const IOReturn ret = m ? NVAccel::gAccel->askFramebufferForScanout(&q, gIopSettle.fb) : kIOReturnNoMemory;
    const unsigned w = gIopSettle.w, h = gIopSettle.h, pitch = gIopSettle.pitch;
    const int pass = gIopSettle.pass;
    bool changed = true;
    if (m && gIopLateChk && m->getLength() >= (IOByteCount)h * pitch) {
        const uint64_t sum = nvIopSum((const volatile unsigned int *)m->getVirtualAddress(), pitch / 4, gIopSettle.sumWords, h);
        changed = sum != gIopSettle.sum;
        if (changed) {
            OSAddAtomic64(1, &gCntIopLate[pass & 3]);
            static SInt32 lateLogs;
            if (OSIncrementAtomic(&lateLogs) < 12)
                ALOG("iop: b129 the frame CHANGED after it was shown (settle pass %d) - the GPU finished it late; copied again", pass);
        }
        gIopSettle.sum = sum;
    }
    if (pass >= 2 && !changed) { if (m) m->release(); if (pass == 2) { gIopSettle.pass = 3; nvIopSettleArm(600); } IOLockUnlock(L); return; }
    if (m && nvIopFlipDbOn() && m->getLength() >= (IOByteCount)h * pitch
        && nvIopPresentFlip(gIopSettle.fb, (const volatile unsigned int *)m->getVirtualAddress(), w, h, pitch)) {
        OSAddAtomic64(1, &gCntIopSettle);
    } else if (ret == kIOReturnSuccess && q.kva && q.pitch >= 4 && w == q.width && h == q.height
        && m->getLength() >= (IOByteCount)h * pitch) {
        const unsigned words = (((pitch < q.pitch) ? pitch : q.pitch) & ~3u) / 4;
        nvIopCopyRows((volatile unsigned int *)q.kva, q.pitch / 4, (const volatile unsigned int *)m->getVirtualAddress(), pitch / 4, words, h, pitch);
        OSAddAtomic64(1, &gCntIopSettle);
    } else OSAddAtomic64(1, &gCntIopSettleSkip);
    if (m) m->release();
    if (gIopSettle.pass == 0)      { gIopSettle.pass = 1; nvIopSettleArm(150); }
    else if (gIopSettle.pass == 1) { gIopSettle.pass = 2; nvIopSettleArm(250); }
    else if (gIopSettle.pass == 2) { gIopSettle.pass = 3; nvIopSettleArm(600); }
    IOLockUnlock(L);
}
template <class PipeT> static IOReturn nvpipe_performTransaction(PipeT *self, IOAccelDisplayPipeTransaction2 *txn)
{
    if (!gNvIop) return (IOReturn)0xe00002c7;
    if (!txn) NVIOP_FAIL(kIOReturnBadArgument, "performTransaction(NULL)");
    NVAccel *a = NVAccel::gAccel;
    unsigned head = 0;
    IOService *fb = a ? a->pipeFramebuffer(self, &head) : nullptr;
    if (!fb) NVIOP_FAIL(kIOReturnNotAttached, "no accelerator, or pipe %p drives a framebuffer that is none of ours", self);

    IOSurface *s  = txn->getPlaneIOSurface(0, 0);
    OSObject  *ro = txn->getPlaneResource(0, 0);
    if (!s && !ro) { OSAddAtomic64(1, &gCntIopEmpty); return kIOReturnSuccess; }

    const volatile unsigned int *src = nullptr; unsigned w = 0, h = 0, pitch = 0; bool fromRes = false;
    NVResource *res = OSDynamicCast(NVResource, ro);
    NVVidMemory *vm = res ? nvres_vidmem(res) : nullptr;
    static SInt32 gIopK6Logs;
    if (gIopK6Logs < 6) {
        OSIncrementAtomic(&gIopK6Logs);
        const unsigned char *kr = (const unsigned char *)res;
        ALOG("iop: K6 plane 0: surface %s id 0x%x %ux%u row %u fmt 0x%08x | resource %s w %u h %u +0xb8 %u +0xc0 %u vidmem %llu bytes kva %d scanout %d",
             s ? "yes" : "NO", s ? (unsigned)s->getSurfaceID() : 0u, s ? (unsigned)s->getWidth() : 0u, s ? (unsigned)s->getHeight() : 0u,
             s ? (unsigned)s->getBytesPerRow() : 0u, s ? (unsigned)s->getPixelFormat() : 0u,
             ro ? ro->getMetaClass()->getClassName() : "(none)",
             res ? (unsigned)*(const volatile unsigned short *)(kr + 0xb0) : 0u, res ? (unsigned)*(const volatile unsigned short *)(kr + 0xb2) : 0u,
             res ? *(const volatile unsigned int *)(kr + 0xb8) : 0u, res ? *(const volatile unsigned int *)(kr + 0xc0) : 0u,
             vm ? (unsigned long long)vm->fBytes : 0ull, (vm && vm->fKva) ? 1 : 0, vm ? (int)vm->fScanout : -1);
    }
    if (!s && vm && vm->fKva) {
        const unsigned char *rr = (const unsigned char *)res;
        w     = *(const volatile unsigned short *)(rr + 0xb0);
        h     = *(const volatile unsigned short *)(rr + 0xb2);
        pitch = *(const volatile unsigned int   *)(rr + 0xb8);
        if ((unsigned long long)pitch < 4ull * w) NVIOP_FAIL(kIOReturnBadArgument, "resource %ux%u: +0xb8 reads %u, which is not a row pitch (need >= %u) — not a surface buffer", w, h, pitch, 4u * w);
        if ((unsigned long long)h * pitch > vm->fBytes) NVIOP_FAIL(kIOReturnOverrun, "resource %ux%u pitch %u exceeds its %llu bytes", w, h, pitch, (unsigned long long)vm->fBytes);
        src = (const volatile unsigned int *)vm->fKva; fromRes = true;
    } else if (s && nvAccelIopFlip(fb, head, s, self)) {
        OSAddAtomic64(1, &gCntIopSrcSurf); OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[head]);
        return kIOReturnSuccess;
    } else if (s) {
        nvAccelSurfScanoutSync(s);
        const unsigned fmt = s->getPixelFormat();
        if (fmt != 0x42475241u) NVIOP_FAIL(kIOReturnUnsupported, "surface 0x%x format 0x%08x is not BGRA", s->getSurfaceID(), fmt);
        const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
        if (!W || !H || P < 4 || W > 16384 || H > 16384 || P > 16384ull * 8) NVIOP_FAIL(kIOReturnBadArgument, "surface geometry %llux%llu pitch %llu", W, H, P);
        if (P < 4 * W || (P & 3)) NVIOP_FAIL(kIOReturnBadArgument, "surface %llux%llu: row pitch %llu is under 4*W or not word-aligned", W, H, P);
        IOMemoryDescriptor *md = s->getMemoryDescriptor();
        if (!md) NVIOP_FAIL(kIOReturnNoMemory, "surface 0x%x has no memory descriptor", s->getSurfaceID());
        src = (const volatile unsigned int *)nvpipe_iopMapSurface(self, s->getSurfaceID(), md, H * P);
        if (!src) NVIOP_FAIL(kIOReturnVMError, "surface 0x%x would not prepare/map %llu bytes", s->getSurfaceID(), H * P);
        w = (unsigned)W; h = (unsigned)H; pitch = (unsigned)P;
    } else {
        NVIOP_FAIL(kIOReturnUnsupported, "plane resource %p is not an NVResource with a CPU mapping, and there is no IOSurface", ro);
    }
    if (!w || !h || pitch < 4) NVIOP_FAIL(kIOReturnBadArgument, "source geometry %ux%u pitch %u", w, h, pitch);

    struct NVRMVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = NVRM_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&q, fb);
    if (ret != kIOReturnSuccess || !q.kva || q.pitch < 4 || !q.height) NVIOP_FAIL(kIOReturnNotReady, "framebuffer would not describe its scanout (0x%x kva %p pitch %u)", (unsigned)ret, q.kva, q.pitch);
    if (w != q.width || h != q.height) NVIOP_FAIL(kIOReturnBadArgument, "source %ux%u is not the scanout's %ux%u", w, h, q.width, q.height);

    nvAccelIopHome(fb, head);
    const unsigned words = (((pitch < q.pitch) ? pitch : q.pitch) & ~3u) / 4;
    const unsigned spr = pitch / 4, dpr = q.pitch / 4;
    volatile unsigned int *d = (volatile unsigned int *)q.kva;
    /* b118: the copy is the whole cost of a frame on Intel (no flip yet, R2). It was a volatile 32-bit load/store loop
       (16 MB per 2560x1600 frame, one word at a time into write-combined memory); now one memcpy per row (or one for the
       frame when the pitches match), which uses the CPU's fast string copy. Boot-arg nmxslowcopy=1 restores the loop.
       The time of every copy is counted: debug.nvaccel_iop_copy_ns (total), _copy_max_ns, _copies. */
    IOLock *copyLock = nvIopCopyLock();
    if (copyLock) IOLockLock(copyLock);
    const uint64_t tc0 = mach_absolute_time();
    if (!(nvIopFlipDbOn() && !fromRes && nvIopPresentFlip(fb, src, w, h, pitch)))
        nvIopCopyRows(d, dpr, src, spr, words, h, pitch);
    OSIncrementAtomic((volatile SInt32 *)&gIopGen);
    IOSurface *settleOld = nullptr;
    if (copyLock && gIopSettleOn && s && !fromRes) {          /* b123: copy this frame again once the screen is idle */
        if (!gIopSettleCall) { struct thread_call *c = thread_call_allocate(&nvIopSettleFire, nullptr);
                               if (c && !OSCompareAndSwapPtr(nullptr, c, (void *volatile *)&gIopSettleCall)) { /* lost a race: keep the first */ } }
        if (gIopSettle.s != s) { settleOld = gIopSettle.s; ((OSObject *)s)->retain(); gIopSettle.s = s; }
        gIopSettle.fb = fb; gIopSettle.head = head; gIopSettle.w = w; gIopSettle.h = h; gIopSettle.pitch = pitch;
        gIopSettle.gen = gIopGen; gIopSettle.pass = 0;
        if (gIopLateChk) {
            const uint64_t tl = mach_absolute_time();
            gIopSettle.sum = nvIopSum(src, spr, words, h); gIopSettle.sumWords = words;
            OSAddAtomic64((SInt64)(mach_absolute_time() - tl), &gCntIopLateMs);
        }
        nvIopSettleArm(40);
    }
    if (copyLock) IOLockUnlock(copyLock);
    if (settleOld) ((OSObject *)settleOld)->release();
    {
        const SInt64 dt = (SInt64)(mach_absolute_time() - tc0);
        OSAddAtomic64(dt, &gCntIopCopyNs); OSAddAtomic64(1, &gCntIopCopies);
        SInt64 m = gCntIopCopyMaxNs;
        while (dt > m && !OSCompareAndSwap64((UInt64)m, (UInt64)dt, (volatile UInt64 *)&gCntIopCopyMaxNs)) m = gCntIopCopyMaxNs;
    }
    OSAddAtomic64(1, fromRes ? &gCntIopSrcRes : &gCntIopSrcSurf);
    const SInt64 n = OSAddAtomic64(1, &gCntIopOk);
    const SInt64 nh = OSAddAtomic64(1, &gCntIopOkHead[head]);
    unsigned nz = 0, varied = 0; const unsigned first = src[0] & 0xffffffu;
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 8; j++) {
        unsigned sx = (w * (2 * j + 1)) / 16; if (sx >= spr) sx = spr - 1;
        const unsigned v = src[(unsigned long)((h * (2 * i + 1)) / 16) * spr + sx] & 0xffffffu;
        if (v) nz++;
        if (v != first) varied++;
    }
    if (!varied) OSAddAtomic64(1, &gCntIopBlank);
    if ((n <= 3 || n == 600 || nh == 0) && gIopK6Logs < 12) {
        OSIncrementAtomic(&gIopK6Logs);
        ALOG("iop: K6 transaction #%lld (head %u #%lld) on the panel: %ux%u pitch %u from %s — 64 samples: nonzero %u varied %u", (long long)n, head, (long long)nh, w, h, pitch,
             fromRes ? "our VRAM resource" : "the IOSurface's memory", nz, varied);
    }
    return kIOReturnSuccess;
}

IOReturn NVDisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2 *txn) { return nvpipe_performTransaction(this, txn); }
#if defined(NMX_ACCEL) && !defined(NM_TAHOE)
/*
 * b146: tell the family the transaction is done. The copy (and the flip) finish inside performTransaction, but nothing
 * raised the transaction interrupt afterwards, so IOAccelDisplayPipe only re-checked isTransactionComplete when something
 * else woke it - the next transaction or event. WindowServer keeps one transaction in flight: the frame it composited
 * next (the one with the newest typed character) waited for that check. That is "one update behind until the next key"
 * (10-22/23: frames never change after they are shown; it happens only on the accelerated path). Off: sysctl
 * debug.nvaccel_iop_txsig=0.
 */
extern "C" void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);
static struct thread_call *gIopTxSigCall;
static void *volatile gIopTxSigPipe;
static void nvIopTxSigFire(void *, void *)
{
    void *pipe = gIopTxSigPipe;
    if (!pipe) return;
    OSAddAtomic64(1, &gCntIopTxSig);
    _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(pipe, nullptr);
}
IOReturn NMXDisplayPipeM::performTransaction(IOAccelDisplayPipeTransaction2 *txn)
{
    static SInt32 n; if (OSIncrementAtomic(&n) < 6) ALOG("iop: performTransaction on the MODERN pipe %p (b116)", this);
    const IOReturn r = nvpipe_performTransaction(this, txn);
    if (gIopTxSig && r == kIOReturnSuccess) {
        if (!gIopTxSigCall) { struct thread_call *c = thread_call_allocate(&nvIopTxSigFire, nullptr);
                              if (c && !OSCompareAndSwapPtr(nullptr, c, (void *volatile *)&gIopTxSigCall)) thread_call_free(c); }
        gIopTxSigPipe = this;
        if (gIopTxSigCall) thread_call_enter(gIopTxSigCall);   /* after we return: the family holds its gate in here */
    }
    return r;
}
#endif

static int nvacceliop_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNvIop;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    NVAccel *a = NVAccel::gAccel;
    if (!a) return ENXIO;
    if (v) {
        OSDictionary *caps = OSDictionary::withCapacity(2);
        if (!caps) return ENOMEM;
        caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
        caps->setObject("TransactionsSupported", kOSBooleanTrue);
        a->setProperty("IOAccelDisplayPipeCapabilities", caps);
        caps->release();
        gNvIop = 1;
        ALOG("iop: debug.nvaccel_iop=1 -- capabilities published, transaction hooks OPEN");
    } else {
        gNvIop = 0;
        a->removeProperty("IOAccelDisplayPipeCapabilities");
        ALOG("iop: debug.nvaccel_iop=0 -- capabilities removed, transaction hooks answer as the base class");
    }
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_iop, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvacceliop_sysctl, "I",
            "K3: 1 = publish IOAccelDisplayPipeCapabilities and perform display-pipe transactions, 0 = behave as the base class (default)");

static int nvaccelfb_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNvAccelArmed;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    NVAccel::triggerFromSysctl(v);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccelfb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvaccelfb_sysctl, "I",
            "register the NVIDIA framebuffer with the accelerator display machine (1 = found_framebuffer, 2 = family sweep)");
#define NVACCEL_CNT(nm, var, desc) SYSCTL_QUAD(_debug, OID_AUTO, nm, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&var, desc)
NVACCEL_CNT(nvaccel_vm_alloc,         gCntVmAlloc,        "NVVidMemory::allocPhysical successes");
NVACCEL_CNT(nvaccel_vm_alloc_bytes,   gCntVmAllocBytes,   "bytes of VRAM granted to NVVidMemory");
NVACCEL_CNT(nvaccel_vm_dealloc,       gCntVmDealloc,      "NVVidMemory::deallocPhysical calls with a handle");
NVACCEL_CNT(nvaccel_vm_dealloc_bytes, gCntVmDeallocBytes, "bytes of VRAM given back by NVVidMemory");
NVACCEL_CNT(nvaccel_vm_refused,       gCntVmRefused,      "allocPhysical refused by the framebuffer");
NVACCEL_CNT(nvaccel_res_new,          gCntResNew,         "NVResource objects created");
NVACCEL_CNT(nvaccel_res_free,         gCntResFree,        "NVResource objects freed");
NVACCEL_CNT(nvaccel_map_commit,       gCntMapCommit,      "GPU page-table commits (MAP_MEMORY_DMA ok)");
NVACCEL_CNT(nvaccel_map_release,      gCntMapRelease,     "GPU page-table releases (UNMAP_ONLY)");
NVACCEL_CNT(nvaccel_map_free,         gCntMapFree,        "GPU VA ranges freed (nvAccelGpuVaFree)");
NVACCEL_CNT(nvaccel_iop_ok,           gCntIopOk,          "K3: display-pipe transactions copied to the panel");
NVACCEL_CNT(nvaccel_iop_ok_h0,        gCntIopOkHead[0],   "frames copied to head 0's panel");
NVACCEL_CNT(nvaccel_iop_ok_h1,        gCntIopOkHead[1],   "frames copied to head 1's panel");
NVACCEL_CNT(nvaccel_iop_ok_h2,        gCntIopOkHead[2],   "frames copied to head 2's panel");
NVACCEL_CNT(nvaccel_iop_ok_h3,        gCntIopOkHead[3],   "frames copied to head 3's panel");
NVACCEL_CNT(nvaccel_iop_ok_h4,        gCntIopOkHead[4],   "frames copied to head 4 (an Intel built-in panel, NMIntelFB)");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_published, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadsPublished, 0,
            "bit i = head i's framebuffer has published to the accelerator");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_registered, CTLFLAG_RD | CTLFLAG_LOCKED, &gNvHeadsRegistered, 0,
            "bit i = head i's framebuffer was handed to the display machine (found_framebuffer)");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_piped, CTLFLAG_RD | CTLFLAG_LOCKED, &gNvHeadsPiped, 0,
            "bit i = head i has a display pipe that drives ITS OWN framebuffer (pipe+0x98 read back)");
static_assert(kNvMaxFB == 5, "kNvMaxFB changed: add debug.nvaccel_iop_ok_h<i> for every new head");
NVACCEL_CNT(nvaccel_iop_fail,         gCntIopFail,        "K3: transactions refused (see the capped iop: log lines)");
NVACCEL_CNT(nvaccel_iop_empty,        gCntIopEmpty,       "K3: transactions with no plane (gamma/options only)");
NVACCEL_CNT(nvaccel_iop_src_res,      gCntIopSrcRes,      "K3: frames sourced from our VRAM resource");
NVACCEL_CNT(nvaccel_iop_src_surf,     gCntIopSrcSurf,     "K3: frames sourced from the IOSurface memory");
NVACCEL_CNT(nvaccel_iop_blank,        gCntIopBlank,       "K6: frames copied whose 64 samples were all ONE colour (a blank frame)");
NVACCEL_CNT(nvaccel_iop_copy_ns,      gCntIopCopyNs,      "b118: total ns spent copying frames to the panel");
NVACCEL_CNT(nvaccel_iop_copy_max_ns,  gCntIopCopyMaxNs,   "b118: slowest single frame copy (ns)");
NVACCEL_CNT(nvaccel_iop_copies,       gCntIopCopies,      "b118: frames copied (timed)");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_settle, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopSettleOn, 0, "b123: 1 = copy the last frame again 40 and 150 ms later (idle screen)");
NVACCEL_CNT(nvaccel_iop_settles,      gCntIopSettle,      "b123: settle copies made");
static int nvaccel_flipdb_sysctl SYSCTL_HANDLER_ARGS
{
    int v = nvIopFlipDbOn() ? 1 : 0;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    const int was = gIopFlipDb; gIopFlipDb = v ? 1 : 0;
    if (was == 1 && !gIopFlipDb && NVAccel::gAccel) {
        IOLock *L = nvIopCopyLock(); if (L) IOLockLock(L);
        for (unsigned i = 0; i < kNvMaxFB; i++) nvIopFlipHome(NVAccel::gAccel->fFBs[i]);
        nvIopFlipHome(NVAccel::gAccel->fFB);
        if (L) IOLockUnlock(L);
    }
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_iop_flipdb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, nvaccel_flipdb_sysctl, "I",
            "b125: 1 = present frames by a page flip between two scan-out buffers at vblank, 0 = copy into the visible one");
NVACCEL_CNT(nvaccel_iop_dbflips,      gCntIopDbFlips,     "b125: frames presented by a vblank page flip");
NVACCEL_CNT(nvaccel_iop_dbflip_fail,  gCntIopDbFail,      "b125: flips refused (that frame was copied instead)");
NVACCEL_CNT(nvaccel_iop_dbflip_ns,    gCntIopDbNs,        "b125: total ns in copy + flip (includes the wait for vblank)");
NVACCEL_CNT(nvaccel_iop_settle_skip,  gCntIopSettleSkip,  "b123: settle copies skipped (a newer frame came, or no surface)");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_txsig, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopTxSig, 0, "b146: 1 = raise the transaction interrupt after every transaction");
NVACCEL_CNT(nvaccel_iop_txsigs, gCntIopTxSig, "b146: transaction interrupts raised");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_wakefix, CTLFLAG_RW | CTLFLAG_LOCKED, &gNmxWakeFix, 0, "b132: 1 = finish a display-mode change left open by sleep, after the wake");
NVACCEL_CNT(nvaccel_wakefixes, gCntNmxWakeFixes, "b132: display-mode changes finished after a wake");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_latechk, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopLateChk, 0, "b129: 1 = checksum each frame and count frames that change after they were shown");
NVACCEL_CNT(nvaccel_iop_late40,   gCntIopLate[0], "b129: frames that changed within 40 ms after they were shown");
NVACCEL_CNT(nvaccel_iop_late190,  gCntIopLate[1], "b129: ... between 40 and 190 ms");
NVACCEL_CNT(nvaccel_iop_late440,  gCntIopLate[2], "b129: ... between 190 and 440 ms");
NVACCEL_CNT(nvaccel_iop_late1040, gCntIopLate[3], "b129: ... between 440 and 1040 ms");
NVACCEL_CNT(nvaccel_iop_latechk_ns, gCntIopLateMs, "b129: total time spent on the per-frame checksum");
static int    gIopFlip = 0;
static SInt64 gCntIopFlip, gCntIopFlipHome, gCntIopFlipNo[8];
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_flip, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gIopFlip, 0, "1 = scan the composited IOSurface out of its own VRAM (no copy)");
NVACCEL_CNT(nvaccel_iop_flips,        gCntIopFlip,        "frames scanned out of the surface's own VRAM (zero copy)");
NVACCEL_CNT(nvaccel_iop_flip_home,    gCntIopFlipHome,    "flips back to the boot scanout before a copied frame");
NVACCEL_CNT(nvaccel_iop_flip_lock,    gCntIopFlipNo[3],   "copied because the accelerator lock was busy");
NVACCEL_CNT(nvaccel_iop_flip_novram,  gCntIopFlipNo[4],   "copied because the surface has no RM VRAM");
NVACCEL_CNT(nvaccel_iop_flip_stale,   gCntIopFlipNo[5],   "copied because system memory holds the newest copy");
NVACCEL_CNT(nvaccel_iop_flip_refused, gCntIopFlipNo[7],   "copied because the framebuffer refused the flip");
static SInt64 gCntIopFlipHit, gCntIopFlipMiss;
NVACCEL_CNT(nvaccel_iop_flip_hit,     gCntIopFlipHit,     "swaps resolved from the cache (no accelerator lock)");
NVACCEL_CNT(nvaccel_iop_flip_miss,    gCntIopFlipMiss,    "swaps that resolved under the accelerator lock");
static int    gIopAsync = 1;
static SInt64 gCntIopAsync, gCntIopAsyncRefused, gCntIopAsyncDrop;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_async, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopAsync, 0, "1 = the swap runs on a thread call and completes through signalTransactionInterrupt (Apple's shape); 0 = the earlier synchronous flip");
NVACCEL_CNT(nvaccel_iop_async_flips,   gCntIopAsync,        "swaps completed asynchronously");
NVACCEL_CNT(nvaccel_iop_async_refused, gCntIopAsyncRefused, "async swaps NVKMS refused (that frame stayed off the panel)");
NVACCEL_CNT(nvaccel_iop_async_drop,    gCntIopAsyncDrop,    "frames dropped because a flip was still in flight (never expected)");
static int gCrcHead = 0;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_crc_head, CTLFLAG_RW | CTLFLAG_LOCKED, &gCrcHead, 0, "b85k-crc: which head debug.nvaccel_crc reads");
static int nvaccel_crc_sysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_crc, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, nvaccel_crc_sysctl, "I",
            "b85k-crc: display CRC32 of what head nvaccel_crc_head is showing (0 = unavailable)");
static bool gSysctlInstalled = false;
#ifdef NMX_ACCEL
static bool gSysctlHeadlessInstalled = false;
#endif
static void nvaccelInstallSysctl(void)
{
    if (gSysctlInstalled) return;
#ifdef NMX_ACCEL
    // only this one: the NVIDIA accelerator may be loaded too, and owns the debug.nvaccel* names
    if (!gSysctlHeadlessInstalled) { sysctl_register_oid(&sysctl__debug_nmxaccel_headless); gSysctlHeadlessInstalled = true; }
    // b75 (WindowServer acceleration on Intel, boot-arg nmxws=1): this accelerator drives the display, so it takes the
    // display controls (debug.nvaccelfb, debug.nvaccel_iop, ...) - only safe with no NVIDIA accelerator loaded
    if (!gNmxWs) return;
    ALOG("nmxws=1: display controls installed (debug.nvaccelfb=1 registers NMXGPUFB, debug.nvaccel_iop=1 opens the pipe)");
#else
    sysctl_register_oid(&sysctl__debug_nvaccel_headless);
#endif
    sysctl_register_oid(&sysctl__debug_nvaccelfb);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_alloc);       sysctl_register_oid(&sysctl__debug_nvaccel_vm_alloc_bytes);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_dealloc);     sysctl_register_oid(&sysctl__debug_nvaccel_vm_dealloc_bytes);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_refused);
    sysctl_register_oid(&sysctl__debug_nvaccel_res_new);        sysctl_register_oid(&sysctl__debug_nvaccel_res_free);
    sysctl_register_oid(&sysctl__debug_nvaccel_map_commit);     sysctl_register_oid(&sysctl__debug_nvaccel_map_release);
    sysctl_register_oid(&sysctl__debug_nvaccel_map_free);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok);         sysctl_register_oid(&sysctl__debug_nvaccel_iop_fail);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_empty);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_src_res);    sysctl_register_oid(&sysctl__debug_nvaccel_iop_src_surf);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_blank);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_ns);    sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_max_ns);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_copies);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_settle);     sysctl_register_oid(&sysctl__debug_nvaccel_iop_settles);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_settle_skip);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_txsig); sysctl_register_oid(&sysctl__debug_nvaccel_iop_txsigs);
    sysctl_register_oid(&sysctl__debug_nvaccel_wakefix);  sysctl_register_oid(&sysctl__debug_nvaccel_wakefixes);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_latechk);  sysctl_register_oid(&sysctl__debug_nvaccel_iop_late40);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_late190);  sysctl_register_oid(&sysctl__debug_nvaccel_iop_late440);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_late1040); sysctl_register_oid(&sysctl__debug_nvaccel_iop_latechk_ns);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flipdb);     sysctl_register_oid(&sysctl__debug_nvaccel_iop_dbflips);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_dbflip_fail); sysctl_register_oid(&sysctl__debug_nvaccel_iop_dbflip_ns);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip);       sysctl_register_oid(&sysctl__debug_nvaccel_iop_flips);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_home);  sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_lock);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_novram); sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_stale);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_refused);
    sysctl_register_oid(&sysctl__debug_nvaccel_crc_head);       sysctl_register_oid(&sysctl__debug_nvaccel_crc);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_hit);   sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_miss);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_async);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_flips);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_refused); sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_drop);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h0);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h1);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h2);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h3);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h4);      sysctl_register_oid(&sysctl__debug_nvaccel_igpu);
    sysctl_register_oid(&sysctl__debug_nvaccel_heads_published); sysctl_register_oid(&sysctl__debug_nvaccel_heads_registered);
    sysctl_register_oid(&sysctl__debug_nvaccel_heads_piped);
    gSysctlInstalled = true;
    ALOG("sysctl debug.nvaccelfb installed -- `sudo sysctl -w debug.nvaccelfb=1` registers the framebuffer");
}

static inline void nvAccelZero(void *p, unsigned long n)
{ volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static struct NVRMRmHandles gRmHandles;
static bool                 gRmHandlesValid;

static unsigned nvRmHandlesLogged;

static bool nvAccelRmHandles(struct NVRMRmHandles *out)
{
    if (gRmHandlesValid) { *out = gRmHandles; return true; }
    NVAccel *a = NVAccel::gAccel;
    if (!a) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: no NVAccel instance yet -- too early"); }
        return false;
    }
    if (!a->fFB) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: no framebuffer has published yet -- will retry"); }
        return false;
    }
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_GPUVA_FN_HANDLES);
    if (!sym) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: OSSymbol allocation failed"); }
        return false;
    }
    struct NVRMRmHandles h;
    nvAccelZero(&h, sizeof h);
    h.version = NVRM_GPUVA_ABI_VERSION;
    IOReturn r = a->fFB->callPlatformFunction(sym, false, &h, nullptr, nullptr, nullptr);
    sym->release();
    if (r != kIOReturnSuccess || !h.kapiDevice) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: framebuffer gave no kapi device (ret 0x%x, kapiDevice %p)",
                 (unsigned)r, h.kapiDevice); }
        return false;
    }
    gRmHandles = h; gRmHandlesValid = true;
    ALOG("RM handles: kapiDevice %p (hClient/hDevice resolved inside NVRM by name) "
         "*** the accelerator can now build GPU page tables ***", h.kapiDevice);
    *out = h;
    return true;
}

static IOService *nvAccelNVRM()
{
    NVAccel *a = NVAccel::gAccel;
    if (!a) return nullptr;
    if (!a->fNVRM) {
        OSDictionary *m = IOService::serviceMatching("NVRM");
        if (m) a->fNVRM = IOService::waitForMatchingService(m, 100ULL * 1000 * 1000);
    }
    return a->fNVRM;
}

static IOReturn nvAccelGpuVaCall(const char *fn, struct NVRMGpuVaRequest *g)
{
    IOService *nvrm = nvAccelNVRM();
    if (!nvrm) return kIOReturnNotAttached;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(fn);
    if (!sym) return kIOReturnNoMemory;
    IOReturn r = nvrm->callPlatformFunction(sym, false, g, nullptr, nullptr, nullptr);
    sym->release();
    return r;
}

#define NVRM_SS_KERNEL_ABI 1
#include "../nvrm_surfshare_abi.h"
extern "C" {
void *_ZN13IOSurfaceRoot13lookupSurfaceEjP4task(void *root, unsigned int id, task_t task);
void *_ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej(void *accel, void *surface, unsigned int plane);
void  _ZN16IOAccelVidMemory4wireEv(void *vidmem);
void  _ZN20IOSurfaceDeviceCache10setCurrentEv(void *cache);
void *_ZNK9IOSurface19getMemoryDescriptorEv(void *surface);
unsigned int _ZNK9IOSurface12getSurfaceIDEv(void *surface);
void  _ZN22IOGraphicsAccelerator29lock_busyEv(void *accel);
void  _ZN22IOGraphicsAccelerator211unlock_busyEv(void *accel);
}
static bool nvFamLock(NVAccel *a, bool tryOnly)
{
    unsigned char *b = (unsigned char *)a;
    IOLock *lk = *(IOLock *volatile *)(b + 0x88);
    if (!lk) return false;
    if (tryOnly) { if (!IOLockTryLock(lk)) return false; }
    else {
        OSIncrementAtomic((volatile SInt32 *)(b + 0x90));
        IOLockLock(lk);
        OSDecrementAtomic((volatile SInt32 *)(b + 0x90));
    }
    _ZN22IOGraphicsAccelerator29lock_busyEv(a);
    a->acceleratorDidLock("", 0);
    return true;
}
static void nvFamUnlock(NVAccel *a)
{
    a->acceleratorWillUnlock("", 0);
    _ZN22IOGraphicsAccelerator211unlock_busyEv(a);
    IOLockUnlock(*(IOLock *volatile *)((unsigned char *)a + 0x88));
}
static unsigned gNVSVLockMiss;
enum { kNVSVPend = 512 };
static unsigned gNVSVPendId[kNVSVPend];
static IOSimpleLock *gNVSVPendLock;
static bool nvSVPend(unsigned id, int op)
{
    if (!id) return op == 1 ? false : false;
    if (!gNVSVPendLock) return op == 1 ? false : false;
    bool r = false; int freeSlot = -1;
    IOSimpleLockLock(gNVSVPendLock);
    for (int i = 0; i < kNVSVPend; i++) {
        if (gNVSVPendId[i] == id) { r = true; if (op == 0) gNVSVPendId[i] = 0; break; }
        if (!gNVSVPendId[i] && freeSlot < 0) freeSlot = i;
    }
    if (op == 1 && !r) { if (freeSlot >= 0) { gNVSVPendId[freeSlot] = id; r = true; } }
    else if (op == 1) r = true;
    IOSimpleLockUnlock(gNVSVPendLock);
    return r;
}
static void    *gNVSVRoot;
static unsigned gNVSVOk, gNVSVRefused;
static inline void nvSVZero(void *p, unsigned long n) { volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static unsigned gNVSVDirty, gNVSVDirtyRefused, gNVSVPageoff, gNVSVPageoffFail;
static unsigned nvAccelSurfDirty(void *cache, NVResource *res, unsigned plane)
{
    volatile unsigned char *r = (volatile unsigned char *)res;
    unsigned char type = r[0x14];
    unsigned int fl = *(volatile unsigned int *)(r + 0xc);
    volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
    if (type != 0xc0 || (fl & 0x8000) || !valid || plane > 15) return NVRM_SS_NO_CACHE;
    if (!gNVSVPendLock) gNVSVPendLock = IOSimpleLockAlloc();
    if (!nvSVPend(_ZNK9IOSurface12getSurfaceIDEv(*(void *volatile *)((unsigned char *)cache + 0x10)), 1)) return NVRM_SS_NO_CACHE;
    _ZN20IOSurfaceDeviceCache10setCurrentEv(cache);
    valid[6] |= (unsigned short)(1u << plane);
    valid[0] &= (unsigned short)~(1u << plane);
    return 0;
}
static IOReturn nvAccelSurfVram(struct NVRMSurfShareEsc *p, task_t task, unsigned cmd)
{
    p->hSrcClient = p->hSrcMemory = 0;
    if (p->version != NVRM_SURFSHARE_VERSION) { p->status = NVRM_SS_BAD_VERSION; return kIOReturnSuccess; }
    NVAccel *a = NVAccel::gAccel;
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    if (!task || !a || !nvAccelRmHandles(&h)) { p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    if (!gNVSVRoot) {
        OSDictionary *m = IOService::serviceMatching("IOSurfaceRoot");
        if (m) { gNVSVRoot = IOService::waitForMatchingService(m, 5ULL * 1000 * 1000 * 1000); m->release(); }
        if (!gNVSVRoot) { p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    }
    void *surf = _ZN13IOSurfaceRoot13lookupSurfaceEjP4task(gNVSVRoot, p->surfaceID, task);
    if (!surf) { p->status = NVRM_SS_NO_SURFACE; gNVSVRefused++; return kIOReturnSuccess; }
    if (!nvFamLock(a, false)) { ((OSObject *)surf)->release(); p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, surf, p->plane);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    NVResource *res = OSDynamicCast(NVResource, ctx);
    NVVidMemory *vm = res ? nvres_vidmem(res) : nullptr;
    if (cmd == NVRM_ESC_SURF_DIRTY) {
        p->status = (cache && res && vm && vm->fRmHandle) ? nvAccelSurfDirty(cache, res, p->plane) : NVRM_SS_NO_CACHE;
        if (p->status == 0) gNVSVDirty++; else gNVSVDirtyRefused++;
        if (gNVSVDirty + gNVSVDirtyRefused <= 6 || ((gNVSVDirty + gNVSVDirtyRefused) & 4095) == 0)
            ALOG("surfvram dirty: surface %u plane %u type 0x%x -> %#x (dirty %u, refused %u)", p->surfaceID, p->plane,
                 res ? (unsigned)((volatile unsigned char *)res)[0x14] : 0u, p->status, gNVSVDirty, gNVSVDirtyRefused);
        if (cache) ((OSObject *)cache)->release();
        nvFamUnlock(a);
        ((OSObject *)surf)->release();
        return kIOReturnSuccess;
    }
    if (vm && !vm->fRmHandle && !vm->fScanout) _ZN16IOAccelVidMemory4wireEv(vm);
    if (!cache || !res) p->status = NVRM_SS_NO_CACHE;
    else if (!vm || !vm->fRmHandle || !vm->fBytes) p->status = NVRM_SS_NO_VRAM;
    else if (p->size > vm->fBytes) p->status = NVRM_SS_TOO_SMALL;
    else {
        struct NVRMSurfShareRm g; nvSVZero(&g, sizeof g);
        g.version = NVRM_SS_RM_VERSION; g.op = NVRM_SS_RM_SHARE_KAPI;
        g.kapiDevice = h.kapiDevice; g.kapiMemory = vm->fRmHandle; g.target = p->hClient; g.status = NVRM_SS_OFF;
        IOService *nvrm = nvAccelNVRM();
        const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_RM);
        if (nvrm && sym && nvrm->callPlatformFunction(sym, false, &g, nullptr, nullptr, nullptr) == kIOReturnSuccess) {}
        if (sym) sym->release();
        p->status = g.status; p->hSrcClient = g.hClient; p->hSrcMemory = g.hObject; p->size = vm->fBytes;
    }
    if (p->status == 0) gNVSVOk++; else gNVSVRefused++;
    if (gNVSVOk + gNVSVRefused <= 8 || ((gNVSVOk + gNVSVRefused) & 1023) == 0)
        ALOG("surfvram: surface %u plane %u -> cache %p res %p vidmem %p (%llu B) -> client %#x mem %#x status %#x (ok %u, refused %u)",
             p->surfaceID, p->plane, cache, res, vm, vm ? vm->fBytes : 0ull, p->hSrcClient, p->hSrcMemory, p->status, gNVSVOk, gNVSVRefused);
    if (cache) ((OSObject *)cache)->release();
    nvFamUnlock(a);
    ((OSObject *)surf)->release();
    return kIOReturnSuccess;
}

#include <mach/mach_time.h>
static IOLock *volatile gNVSVStageLock;
static IOBufferMemoryDescriptor *gNVSVStageBuf;
static unsigned gNVSVStaged, gNVSVStageNoBuf, gNVSVStageBackoff;
static IOLock *nvSVStageLock(void)
{
    if (!gNVSVStageLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNVSVStageLock)) IOLockFree(l);
    }
    return gNVSVStageLock;
}
static IOBufferMemoryDescriptor *nvSVStage(unsigned long long len)
{
    if (gNVSVStageBuf && gNVSVStageBuf->getLength() >= len) return gNVSVStageBuf;
    if (len > (64ull << 20) || gNVSVStageBackoff) { if (gNVSVStageBackoff) gNVSVStageBackoff--; return nullptr; }
    const unsigned long long want = (len + 0xFFFFFull) & ~0xFFFFFull;
    IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                                      (vm_size_t)want, PAGE_SIZE);
    if (b && b->prepare() != kIOReturnSuccess) { b->release(); b = nullptr; }
    if (!b) {
        gNVSVStageNoBuf++; gNVSVStageBackoff = 1024;
        ALOG("surfvram pageoff: no %llu B physically contiguous staging buffer (miss %u) -- per-page copies for the next 1024", want, gNVSVStageNoBuf);
        return nullptr;
    }
    if (gNVSVStageBuf) { gNVSVStageBuf->complete(); gNVSVStageBuf->release(); }
    gNVSVStageBuf = b;
    ALOG("surfvram pageoff: staging buffer %llu B, physically contiguous", want);
    return b;
}

void NVResource::pageoff(IOAccelEvent *ev, bool a1, bool *out, unsigned long long a3)
{
    volatile unsigned char *r = (volatile unsigned char *)this;
    void *cache = *(void *volatile *)(r + 0xe0);
    NVVidMemory *vm = nvres_vidmem(this);
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    void *surf = (r[0x14] == 0xc0 && cache) ? *(void *volatile *)((unsigned char *)cache + 0x10) : nullptr;
    if (!surf || !vm || !vm->fRmHandle || vm->fScanout || !vm->fBytes || !nvAccelRmHandles(&h)) {
        IOAccelResource2::pageoff(ev, a1, out, a3);
        return;
    }
    IOMemoryDescriptor *md = (IOMemoryDescriptor *)_ZNK9IOSurface19getMemoryDescriptorEv(surf);
    if (!md || md->prepare(kIODirectionNone) != kIOReturnSuccess) {
        gNVSVPageoffFail++;
        ALOG("surfvram pageoff: res %p surface pages could not be wired (md %p) -- family pageoff instead", this, md);
        IOAccelResource2::pageoff(ev, a1, out, a3);
        return;
    }
    bool mayUnwire = true;
    unsigned long long len = md->getLength();
    if (len > vm->fBytes) len = vm->fBytes;
    unsigned long long pages = (len + 4095) >> 12;
    bool ok = false; unsigned st = NVRM_SS_OFF; unsigned long long usec = 0, cpuUs = 0;
    bool staged = false;
    if (IOLock *lk = nvSVStageLock()) {
        IOLockLock(lk);
        IOBufferMemoryDescriptor *sb = nvSVStage(len);
        IOByteCount sl = 0;
        addr64_t sp = sb ? sb->getPhysicalSegment(0, &sl, kIOMemoryMapperNone) : 0;
        unsigned long long *spa = (sp && sl >= len && pages) ? (unsigned long long *)IOMalloc(pages * sizeof *spa) : nullptr;
        if (spa) {
            for (unsigned long long k = 0; k < pages; k++) spa[k] = sp + k * 4096;
            struct NVRMSurfPageoff q; nvSVZero(&q, sizeof q);
            q.version = NVRM_SS_RM_VERSION; q.status = NVRM_SS_OFF; q.kapiDevice = h.kapiDevice; q.kapiMemory = vm->fRmHandle;
            q.pages = spa; q.pageCount = pages; q.length = len;
            IOService *nvrm = nvAccelNVRM();
            const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_PAGEOFF);
            if (nvrm && sym) nvrm->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
            if (sym) sym->release();
            st = q.status; usec = q.usec;
            if (st == 0) {
                const uint64_t t0 = mach_absolute_time();
                const IOByteCount w = md->writeBytes(0, sb->getBytesNoCopy(), (IOByteCount)len);
                cpuUs = (mach_absolute_time() - t0) / 1000;
                ok = (w == (IOByteCount)len);
                if (!ok) st = NVRM_SS_TOO_SMALL;
                else gNVSVStaged++;
            } else if (st == 0x65u) {
                ALOG("surfvram pageoff: CE copy TIMED OUT into the staging buffer -- abandoning it (%llu B)", (unsigned long long)sb->getLength());
                gNVSVStageBuf = nullptr;
            }
            staged = true;
            IOFree(spa, pages * sizeof *spa);
        }
        IOLockUnlock(lk);
    }
    unsigned long long *pa = (!staged && pages) ? (unsigned long long *)IOMalloc(pages * sizeof *pa) : nullptr;
    if (pa) {
        unsigned long long i = 0;
        for (IOByteCount off = 0; i < pages; ) {
            IOByteCount seglen = 0;
            addr64_t phys = md->getPhysicalSegment(off, &seglen, kIOMemoryMapperNone);
            if (!phys || !seglen) break;
            for (IOByteCount k = 0; k < seglen && i < pages; k += 4096, off += 4096) pa[i++] = phys + k;
        }
        if (i == pages) {
            struct NVRMSurfPageoff q; nvSVZero(&q, sizeof q);
            q.version = NVRM_SS_RM_VERSION; q.status = NVRM_SS_OFF; q.kapiDevice = h.kapiDevice; q.kapiMemory = vm->fRmHandle;
            q.pages = pa; q.pageCount = pages; q.length = len;
            IOService *nvrm = nvAccelNVRM();
            const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_PAGEOFF);
            if (nvrm && sym) nvrm->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
            if (sym) sym->release();
            st = q.status; usec = q.usec; ok = st == 0;
            if (st == 0x65u) mayUnwire = false;
        }
        IOFree(pa, pages * sizeof *pa);
    }
    if (mayUnwire) md->complete(kIODirectionNone);
    else ALOG("surfvram pageoff: res %p CE copy TIMED OUT -- surface pages stay wired (%llu B)", this, len);
    if (ok) {
        nvSVPend(_ZNK9IOSurface12getSurfaceIDEv(surf), 0);
        volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
        if (valid) valid[0] |= 1;
        gNVSVPageoff++;
    } else gNVSVPageoffFail++;
    if (out) *out = ok;
    if (gNVSVPageoff + gNVSVPageoffFail <= 8 || ((gNVSVPageoff + gNVSVPageoffFail) & 1023) == 0)
        ALOG("surfvram pageoff: res %p %llu B on the copy engine -> %#x in %llu us%s + cpu %llu us (ok %u, failed %u, staged %u)",
             this, len, st, usec, staged ? " (staged)" : " (per page)", cpuUs, gNVSVPageoff, gNVSVPageoffFail, gNVSVStaged);
}

static IOSurface *gIopFlipFront[kNvMaxFB], *gIopFlipPrev[kNvMaxFB];
static bool gIopFlipped[kNvMaxFB];
static unsigned gIopFlipLogs;
static bool nvAccelIopFlipNo(int why) { OSAddAtomic64(1, &gCntIopFlipNo[why & 7]); return false; }
struct NVIopFlipCache { IOSurface *s; NVResource *res; NVVidMemory *vm; void *kmem; unsigned long long bytes; unsigned long long use; };
static NVIopFlipCache gIopFlipCache[8];
static IOSimpleLock *gIopFlipCacheLock;
static unsigned long long gIopFlipCacheTick;
static bool nvAccelIopCacheGet(IOSurface *s, NVResource **res, NVVidMemory **vm, void **kmem, unsigned long long *bytes)
{
    if (!gIopFlipCacheLock) return false;
    bool hit = false;
    IOSimpleLockLock(gIopFlipCacheLock);
    for (unsigned i = 0; i < 8; i++) if (gIopFlipCache[i].s == s) {
        NVVidMemory *v = gIopFlipCache[i].vm;
        if (v && v->fRmHandle == gIopFlipCache[i].kmem && v->fBytes == gIopFlipCache[i].bytes) {
            *res = gIopFlipCache[i].res; *vm = v; *kmem = gIopFlipCache[i].kmem; *bytes = gIopFlipCache[i].bytes;
            gIopFlipCache[i].use = ++gIopFlipCacheTick; hit = true;
        }
        break;
    }
    IOSimpleLockUnlock(gIopFlipCacheLock);
    return hit;
}
static void nvAccelIopCachePut(IOSurface *s, NVResource *res, NVVidMemory *vm, void *kmem, unsigned long long bytes)
{
    if (!gIopFlipCacheLock) { IOSimpleLock *l = IOSimpleLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gIopFlipCacheLock)) IOSimpleLockFree(l); }
    if (!gIopFlipCacheLock) return;
    IOSurface *drop = nullptr;
    ((OSObject *)s)->retain();
    IOSimpleLockLock(gIopFlipCacheLock);
    unsigned k = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (gIopFlipCache[i].s == s) { k = i; break; }
        if (!gIopFlipCache[i].s) { k = i; break; }
        if (gIopFlipCache[i].use < gIopFlipCache[k].use) k = i;
    }
    drop = gIopFlipCache[k].s;
    gIopFlipCache[k].s = s; gIopFlipCache[k].res = res; gIopFlipCache[k].vm = vm; gIopFlipCache[k].kmem = kmem;
    gIopFlipCache[k].bytes = bytes; gIopFlipCache[k].use = ++gIopFlipCacheTick;
    IOSimpleLockUnlock(gIopFlipCacheLock);
    if (drop) ((OSObject *)drop)->release();
}
extern "C" {
void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter(struct thread_call *call);
}
struct NVIopAsync { void *pipe; IOService *fb; IOSurface *s; struct NVRMFlipRequest f; volatile SInt32 busy; struct thread_call *call; };
static NVIopAsync gIopAs[kNvMaxFB];
static SInt32 gIopAsyncRefuseRun;
static bool nvAccelIopAsyncBusy(const void *pipe)
{
    for (unsigned h = 0; h < kNvMaxFB; h++)
        if (gIopAs[h].pipe == pipe && __atomic_load_n(&gIopAs[h].busy, __ATOMIC_ACQUIRE)) return true;
    return false;
}
static void nvAccelIopAsyncRun(void *p0, void *)
{
    NVIopAsync *e = (NVIopAsync *)p0;
    const unsigned head = (unsigned)(e - gIopAs);
    IOSurface *s = e->s; e->s = nullptr;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? e->fb->callPlatformFunction(sym, false, &e->f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    if (rr == kIOReturnSuccess) {
        gIopAsyncRefuseRun = 0;
        if (gIopFlipPrev[head]) ((OSObject *)gIopFlipPrev[head])->release();
        gIopFlipPrev[head] = gIopFlipFront[head]; gIopFlipFront[head] = s; gIopFlipped[head] = true;
        const SInt64 n = OSAddAtomic64(1, &gCntIopFlip); OSAddAtomic64(1, &gCntIopAsync);
        if (n < 4 || n == 600 || (n & 0xffff) == 0)
            ALOG("iop: ASYNC swap #%lld head %u: surface 0x%x %ux%u pitch %u scanned out of its own VRAM (flipResult %d)",
                 (long long)n + 1, head, s->getSurfaceID(), e->f.width, e->f.height, e->f.pitch, e->f.flipResult);
    } else {
        OSAddAtomic64(1, &gCntIopAsyncRefused); OSAddAtomic64(1, &gCntIopFlipNo[7]);
        if (gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: async flip REFUSED (0x%x, flipResult %d) head %u -- that frame is not on the panel",
                                                     (unsigned)rr, e->f.flipResult, head); }
        if (++gIopAsyncRefuseRun >= 3 && gIopAsync) {
            gIopAsync = 0;
            ALOG("iop: b89k 3 async refusals in a row -- debug.nvaccel_iop_async=0 (synchronous flip, which copies on refusal)");
        }
        ((OSObject *)s)->release();
    }
    __atomic_store_n(&e->busy, 0, __ATOMIC_RELEASE);
    _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(e->pipe, nullptr);
}
static bool nvAccelIopAsyncStart(void *pipe, IOService *fb, unsigned head, IOSurface *s, const struct NVRMFlipRequest *f)
{
    NVIopAsync *e = &gIopAs[head];
    for (unsigned i = 0; i < 50 && __atomic_load_n(&e->busy, __ATOMIC_ACQUIRE); i++) IOSleep(1);
    if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE)) {
        OSAddAtomic64(1, &gCntIopAsyncDrop);
        return true;
    }
    if (!e->call) e->call = thread_call_allocate(nvAccelIopAsyncRun, e);
    if (!e->call) return false;
    ((OSObject *)s)->retain();
    e->pipe = pipe; e->fb = fb; e->s = s;
    for (unsigned i = 0; i < sizeof *f; i++) ((volatile unsigned char *)&e->f)[i] = ((const unsigned char *)f)[i];
    __atomic_store_n(&e->busy, 1, __ATOMIC_RELEASE);
    thread_call_enter(e->call);
    return true;
}
static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe)
{
    if (!gIopFlip || !fb || !s || head >= kNvMaxFB) return false;
    // zero-copy scan-out is an NVIDIA display-engine flip; another vendor's panel (NMIntelFB) always takes the copy
    if (!fb->metaCast("NVRMNVDAFramebuffer")) return false;
    NVAccel *a = NVAccel::gAccel;
    if (!a) return nvAccelIopFlipNo(1);
    if (s->getPixelFormat() != 0x42475241u) return nvAccelIopFlipNo(2);
    NVResource *res = nullptr; NVVidMemory *vm = nullptr; void *kmem = nullptr; unsigned long long bytes = 0;
    if (nvAccelIopCacheGet(s, &res, &vm, &kmem, &bytes)) {
        OSAddAtomic64(1, &gCntIopFlipHit);
    } else {
        if (!nvFamLock(a, true)) return nvAccelIopFlipNo(3);
        void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
        OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
        res = OSDynamicCast(NVResource, ctx);
        vm = res ? nvres_vidmem(res) : nullptr;
        kmem = (vm && vm->fRmHandle && vm->fBytes && !vm->fScanout) ? vm->fRmHandle : nullptr;
        bytes = vm ? vm->fBytes : 0;
        if (cache) ((OSObject *)cache)->release();
        nvFamUnlock(a);
        OSAddAtomic64(1, &gCntIopFlipMiss);
        if (kmem) nvAccelIopCachePut(s, res, vm, kmem, bytes);
    }
    volatile unsigned short *valid = res ? *(unsigned short *volatile *)((volatile unsigned char *)res + 0x30) : nullptr;
    const bool vramNewest = valid && (valid[6] & 1u);
    if (!kmem) return nvAccelIopFlipNo(4);
    if (!vramNewest) return nvAccelIopFlipNo(5);
    const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
    if (!W || !H || P < 4 * W || H * P > bytes) return nvAccelIopFlipNo(6);
    struct NVRMFlipRequest f; nvSVZero(&f, sizeof f);
    f.version = NVRM_FLIP_ABI_VERSION; f.kapiMemory = kmem; f.allocationCookie = vm->fAllocationCookie; f.width = (unsigned)W; f.height = (unsigned)H; f.pitch = (unsigned)P;
    if (gIopAsync && pipe) return nvAccelIopAsyncStart(pipe, fb, head, s, &f);
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    if (rr != kIOReturnSuccess) {
        if (gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: flip REFUSED (0x%x, flipResult %d) head %u %llux%llu pitch %llu -- copying",
                                                     (unsigned)rr, f.flipResult, head, W, H, P); }
        return nvAccelIopFlipNo(7);
    }
    ((OSObject *)s)->retain();
    if (gIopFlipPrev[head]) ((OSObject *)gIopFlipPrev[head])->release();
    gIopFlipPrev[head] = gIopFlipFront[head]; gIopFlipFront[head] = s; gIopFlipped[head] = true;
    const SInt64 n = OSAddAtomic64(1, &gCntIopFlip);
    if (n < 4 || n == 600 || (n & 0xffff) == 0)
        ALOG("iop: zero-copy swap #%lld head %u: surface 0x%x %llux%llu pitch %llu scanned out of its own VRAM (flipResult %d)",
             (long long)n + 1, head, s->getSurfaceID(), W, H, P, f.flipResult);
    return true;
}
static int nvaccel_crc_sysctl SYSCTL_HANDLER_ARGS
{
    unsigned v = 0;
    NVAccel *a = NVAccel::gAccel;
    IOService *fb = a ? a->fFB : nullptr;
    IOService *nub = fb ? fb->getProvider() : nullptr;
    OSNumber *n = nub ? OSDynamicCast(OSNumber, nub->getProperty("nvkms-kapi")) : nullptr;
    struct NvKmsKapiFunctionsTable *k = n ? (struct NvKmsKapiFunctionsTable *)(uintptr_t)n->unsigned64BitValue() : nullptr;
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    if (k && k->getCRC32 && gCrcHead >= 0 && gCrcHead < 8 && nvAccelRmHandles(&h) && h.kapiDevice) {
        struct NvKmsKapiCrcs c; nvSVZero(&c, sizeof c);
        if (k->getCRC32((struct NvKmsKapiDevice *)h.kapiDevice, (NvU32)gCrcHead, &c))
            v = c.compositorCrc32.supported ? c.compositorCrc32.value : (c.outputCrc32.supported ? c.outputCrc32.value : 0);
    }
    int out = (int)v;
    return sysctl_handle_int(oidp, &out, 0, req);
}
static void nvAccelIopHome(IOService *fb, unsigned head)
{
    if (!fb || head >= kNvMaxFB || !gIopFlipped[head] || !fb->metaCast("NVRMNVDAFramebuffer")) return;
    struct NVRMFlipRequest f; nvSVZero(&f, sizeof f);
    f.version = NVRM_FLIP_ABI_VERSION; f.flags = 1u;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    OSAddAtomic64(1, &gCntIopFlipHome);
    if (rr != kIOReturnSuccess && gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: HOME flip refused (0x%x) head %u", (unsigned)rr, head); }
    gIopFlipped[head] = false;
}
static void nvAccelSurfScanoutSync(IOSurface *s)
{
    if (!s || !NVAccel::gAccel || !nvSVPend(_ZNK9IOSurface12getSurfaceIDEv((void *)s), 2)) return;
    NVAccel *a = NVAccel::gAccel;
    if (!nvFamLock(a, true)) {
        if (++gNVSVLockMiss <= 8 || (gNVSVLockMiss & 1023) == 0)
            ALOG("surfvram scanout: accelerator lock busy, surface %u stays pending (misses %u)",
                 _ZNK9IOSurface12getSurfaceIDEv((void *)s), gNVSVLockMiss);
        return;
    }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    NVResource *res = OSDynamicCast(NVResource, ctx);
    bool out = false;
    if (res) res->pageoff(nullptr, true, &out, 0);
    if (!out) nvSVPend(_ZNK9IOSurface12getSurfaceIDEv((void *)s), 0);
    if (cache) ((OSObject *)cache)->release();
    nvFamUnlock(a);
}

static unsigned nvVaAllocLogged, nvVaMapLogged;
#ifdef NMX_ACCEL
// AMD/Intel have no NVIDIA RM to hand out GPU addresses. The IOAccel family still wants one for every memory map it
// makes (WindowServer's 2560x1600 surfaces were refused and the screen stayed black), but nothing ever runs NVIDIA
// commands against it: Vulkan/ANV gets its own addresses from i915 through NMXGPU. So these are private, unique
// placeholder addresses from one locked range allocator.
#define NMX_VA_BASE  0x0000100000000000ull
#define NMX_VA_SIZE  0x0000010000000000ull   // 1 TB of placeholder space
static IORangeAllocator *gNmxVa;
static unsigned gNmxVaLogged;
#endif

static unsigned long long nvAccelGpuVaAlloc(IOAccelMemoryMap *map,
                                            unsigned long long len, unsigned long long align)
{
    NVMemoryMap *mm = OSDynamicCast(NVMemoryMap, map);
    if (!mm || !len) return 0;
    if (mm->fGpuVa) return mm->fGpuVa;
#ifdef NMX_ACCEL
    {
        if (!gNmxVa) {
            IORangeAllocator *ra = IORangeAllocator::withRange(NMX_VA_SIZE, 0x10000, 0, IORangeAllocator::kLocking);
            if (ra && !OSCompareAndSwapPtr(nullptr, ra, (void * volatile *)&gNmxVa)) ra->release();
            if (!gNmxVa) { ALOG("gpuVaAlloc: no placeholder VA allocator"); return 0; }
        }
        const unsigned long long a = align > 0x10000 ? align : 0x10000;
        const unsigned long long sz = (len + 0xffffull) & ~0xffffull;
        IORangeScalar off = 0;
        if (!gNmxVa->allocate(sz, &off, a)) {
            if (gNmxVaLogged < 8) { gNmxVaLogged++; ALOG("gpuVaAlloc: placeholder VA space full (%llu bytes)", len); }
            return 0;
        }
        mm->fNmxPrivate = true; mm->fGpuVa = NMX_VA_BASE + off; mm->fVaLen = sz;
        if (gNmxVaLogged < 8) { gNmxVaLogged++;
            ALOG("gpuVaAlloc: %llu bytes -> placeholder VA 0x%llx (AMD/Intel: no RM page tables)", len, mm->fGpuVa); }
        return mm->fGpuVa;
    }
#endif

    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return 0;

    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.size = len; g.align = align;
    if (nvAccelGpuVaCall(NVRM_GPUVA_FN_ALLOC, &g) != kIOReturnSuccess || !g.gpuva) {
        if (nvVaAllocLogged < 8) { nvVaAllocLogged++;
            ALOG("gpuVaAlloc: RM refused %llu bytes (align %llu) -> status 0x%x", len, align, g.status); }
        return 0;
    }
    mm->fHVirt = g.hVirt; mm->fGpuVa = g.gpuva; mm->fVaLen = len;
    if (nvVaAllocLogged < 8) { nvVaAllocLogged++;
        ALOG("gpuVaAlloc: %llu bytes -> gpuVA 0x%llx (hVirt 0x%x)", len, g.gpuva, g.hVirt); }
    return g.gpuva;
}

static void nvAccelGpuVaFree(IOAccelMemoryMap *map)
{
    NVMemoryMap *mm = OSDynamicCast(NVMemoryMap, map);
#ifdef NMX_ACCEL
    if (mm && mm->fNmxPrivate) {
        if (gNmxVa && mm->fGpuVa >= NMX_VA_BASE) gNmxVa->deallocate(mm->fGpuVa - NMX_VA_BASE, mm->fVaLen);
        OSAddAtomic64(1, &gCntMapFree);
        mm->fNmxPrivate = false; mm->fGpuVa = 0; mm->fVaLen = 0; mm->fMapped = false;
        return;
    }
#endif
    if (!mm || !mm->fHVirt) return;
    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return;
    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.hVirt = mm->fHVirt; g.hMemory = mm->fMapped ? mm->fHRmMemory : 0; g.size = mm->fVaLen;
    g.gpuva = mm->fGpuVa;
    nvAccelGpuVaCall(NVRM_GPUVA_FN_FREE, &g);
    OSAddAtomic64(1, &gCntMapFree);
    mm->fHVirt = 0; mm->fGpuVa = 0; mm->fVaLen = 0; mm->fMapped = false;
}

bool NVMemoryMap::commitIntoGPUPageTable()
{
    if (fMapped) return true;
#ifdef NMX_ACCEL
    if (fNmxPrivate) { fMapped = true; OSAddAtomic64(1, &gCntMapCommit); return true; }   // nothing to map: no RM
#endif
    NVVidMemory *vm = OSDynamicCast(NVVidMemory, fMem);
    void *kapiMem = vm ? vm->fRmHandle : nullptr;
    unsigned long long len = fVaLen ? fVaLen : getLength();
    if (!fHVirt || !kapiMem || !len) {
        if (fCommits < 8) { fCommits++;
            ALOG("MM commit: NOT MAPPED -- hVirt 0x%x kapiMem %p len %llu (mem %p%s)",
                 fHVirt, kapiMem, len, fMem, vm ? "" : ", not an NVVidMemory"); }
        return false;
    }
    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return false;
    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice; g.kapiMemory = kapiMem;
    g.hVirt = fHVirt; g.size = len;
    g.isVidmem = 1;
    if (nvAccelGpuVaCall(NVRM_GPUVA_FN_MAP, &g) != kIOReturnSuccess) {
        if (fCommits < 8) { fCommits++;
            ALOG("MM commit: MAP_MEMORY_DMA refused gpuVA 0x%llx len %llu -> status 0x%x",
                 fGpuVa, len, g.status); }
        return false;
    }
    fHRmMemory = g.hMemory; fMapped = true;
    OSAddAtomic64(1, &gCntMapCommit);
    if (g.gpuva) fGpuVa = g.gpuva;
    if (fCommits < 8) { fCommits++;
        ALOG("MM commit: MAPPED gpuVA 0x%llx len %llu <- hMem 0x%x  *** the GPU can reach these bytes ***",
             fGpuVa, len, g.hMemory); }
    return true;
}

void NVMemoryMap::releaseFromGPUPageTable()
{
#ifdef NMX_ACCEL
    if (fNmxPrivate) { fMapped = false; return; }
#endif
    if (!fMapped || !fHVirt) return;
    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return;
    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.hVirt = fHVirt; g.hMemory = fHRmMemory; g.size = fVaLen;
    g.gpuva = fGpuVa;
    g.flags = NVRM_GPUVA_FLAG_UNMAP_ONLY;
    nvAccelGpuVaCall(NVRM_GPUVA_FN_FREE, &g);
    fMapped = false;
    OSAddAtomic64(1, &gCntMapRelease);
}

static void nvAccelWriteStamp(int index, unsigned int value)
{
    NVAccel *a = NVAccel::gAccel;
    if (!a || !a->fStamps || index < 0 || index >= 4096) return;
    volatile unsigned int *base = (volatile unsigned int *)a->fStamps->getBytesNoCopy();
    if (!base) return;
    base[index] = value;
}

static unsigned nvFrontSamples, nvFrontFlips;
static unsigned nvFrontLastSum;
static void nvAccelSampleFront(NVVidMemory *vm, unsigned w, unsigned h, unsigned pitch)
{
    if (!vm || !vm->fKva || !pitch || !w || !h) return;
#define NVFRONT_EVERY 10u
    if (++nvFrontFlips % NVFRONT_EVERY) return;
    if (nvFrontSamples >= 40) return;
    nvFrontSamples++;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    unsigned n = 0, nonzero = 0, sum = 0;
    for (unsigned y = 0; y < h; y += 8)
        for (unsigned x = 0; x < w; x += 8) {
            unsigned v = px[y * ppr + x];
            sum = sum * 33u + v; if (v) nonzero++; n++;
        }
    const unsigned p1 = (w > 100  && h > 100)  ? px[100  * ppr + 100]  : 0u;
    const unsigned p2 = (w > 960  && h > 540)  ? px[540  * ppr + 960]  : 0u;
    const unsigned p3 = (w > 1800 && h > 1000) ? px[1000 * ppr + 1800] : 0u;
    const bool moved = (nvFrontSamples > 1) && (sum != nvFrontLastSum);
    ALOG("FRONT SURFACE sample %u: sum 0x%08x %s nonzero %u/%u "
         "px(100,100)=0x%08x px(960,540)=0x%08x px(1800,1000)=0x%08x %ux%u pitch %u",
         nvFrontSamples, sum,
         moved ? "*** MOVED -- A LIVE WRITER IS PAINTING THE SCANNED-OUT SURFACE ***"
               : (nvFrontSamples > 1 ? "(UNCHANGED since last sample -- frozen, not proof of a picture)"
                                     : "(first sample -- motion is the proof, wait for the next)"),
         nonzero, n, p1, p2, p3, w, h, pitch);
    nvFrontLastSum = sum;
}

static unsigned nvSwapProbes;
static void nvAccelResStats(IOAccelResource2 *res, const char *tag)
{
    if (!res) { ALOG("  swap %s: (null)", tag); return; }
    NVVidMemory *vm = nvres_vidmem(res);
    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!vm || !vm->fKva || !pitch || !w || !h) {
        ALOG("  swap %s: %ux%u pitch %u vm %p kva %p scanout %d -- NOT SAMPLEABLE",
             tag, w, h, pitch, vm, vm ? vm->fKva : nullptr, vm ? (int)vm->fScanout : -1);
        return;
    }
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    unsigned n = 0, nonzero = 0, sum = 0;
    for (unsigned y = 0; y < h; y += 8)
        for (unsigned x = 0; x < w; x += 8) {
            unsigned v = px[y * ppr + x];
            sum = sum * 33u + v; if (v) nonzero++; n++;
        }
    ALOG("  swap %s: %ux%u pitch %u scanout %d  sum 0x%08x  nonzero %u/%u  %s",
         tag, w, h, pitch, (int)vm->fScanout, sum, nonzero, n,
         nonzero ? "*** HAS CONTENT -- THIS IS THE SOURCE ***" : "(empty)");
}
bool nvAccelResHasContent(IOAccelResource2 *res)
{
    if (!res) return false;
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm || !vm->fKva) return false;
    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!w || !h || pitch < 4) return false;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    for (unsigned iy = 1; iy <= 4; iy++)
        for (unsigned ix = 1; ix <= 4; ix++) {
            const unsigned x = (w * ix) / 5, y = (h * iy) / 5;
            if (x < w && y < h && px[y * ppr + x]) return true;
        }
    return false;
}

void nvAccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2)
{
    if (nvSwapProbes >= 6) return;
    nvSwapProbes++;
    ALOG("SWAP PROBE %u -- which argument holds the composited frame?", nvSwapProbes);
    nvAccelResStats(a1, "a1");
    nvAccelResStats(a2, "a2");
}

static unsigned nvFlipWhy = 0;
#define NVFLIPWHY(...) do { if (nvFlipWhy < 20) { nvFlipWhy++; ALOG(__VA_ARGS__); } } while (0)

#define NVSCANOUT_COPY_MAX 120
static unsigned nvScanoutCopies = 0;
static bool nvAccelCopyToScanout(IOAccelResource2 *res)
{
#ifndef NMX_ACCEL
    if (nvScanoutCopies >= NVSCANOUT_COPY_MAX) return false;
#endif
    // b91 (10-19): no cap on Intel. This copy IS the desktop there (WindowServer's frame -> the panel), and the 120-frame
    // debugging cap froze the screen ~7 s after the armed lock screen came up (WindowServer kept drawing: 7642
    // copy-forward attempts while the panel held frame #120).
    if (!res) return false;
    NVAccel *a = NVAccel::gAccel;
    if (!a || !a->fFB) { NVFLIPWHY("copyToScanout: no accelerator/framebuffer"); return false; }
    if (gNvHeadsPiped & (gNvHeadsPiped - 1)) { NVFLIPWHY("copyToScanout: refused -- heads piped 0x%x, no way to tell whose frame this is", gNvHeadsPiped); return false; }
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm || !vm->fKva) { NVFLIPWHY("copyToScanout: source has no kva"); return false; }

    const unsigned char *rr = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(rr + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(rr + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(rr + 0xb8);
    if (!w || !h || pitch < 4) { NVFLIPWHY("copyToScanout: source geometry %ux%u pitch %u", w, h, pitch); return false; }

    struct NVRMVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = NVRM_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&q);
    if (ret != kIOReturnSuccess || !q.kva || !q.pitch) {
        NVFLIPWHY("copyToScanout: framebuffer would not describe its scanout (0x%x kva %p pitch %u)",
                  (unsigned)ret, q.kva, q.pitch);
        return false;
    }

    const unsigned rows = (h < q.height) ? h : q.height;
    const unsigned rowb = ((pitch < q.pitch) ? pitch : q.pitch) & ~3u;
    const unsigned words = rowb / 4;
    const unsigned spr = pitch / 4, dpr = q.pitch / 4;
    const volatile unsigned int *s = (const volatile unsigned int *)vm->fKva;
    volatile unsigned int *d = (volatile unsigned int *)q.kva;
#ifdef NMX_ACCEL
    for (unsigned y = 0; y < rows; y++)   // b91: whole rows at once (cached RAM -> write-combined scan-out)
        memcpy((void *)(d + (unsigned long)y * dpr), (const void *)(s + (unsigned long)y * spr), (size_t)words * 4);
#else
    for (unsigned y = 0; y < rows; y++) {
        const volatile unsigned int *sp = s + (unsigned long)y * spr;
        volatile unsigned int *dp = d + (unsigned long)y * dpr;
        for (unsigned x = 0; x < words; x++) dp[x] = sp[x];
    }
#endif
    nvScanoutCopies++;
#ifdef NMX_ACCEL
    if (nvScanoutCopies <= 3)
#else
    if (nvScanoutCopies <= 3 || nvScanoutCopies == NVSCANOUT_COPY_MAX)
#endif
        ALOG("*** COPIED THE COMPOSITED FRAME ONTO THE PANEL *** #%u: %ux%u, %u rows x %u bytes, "
             "src kva %p pitch %u -> scanout kva %p phys 0x%llx pitch %u%s",
             nvScanoutCopies, w, h, rows, rowb, vm->fKva, pitch, q.kva,
             (unsigned long long)q.phys, q.pitch,
             nvScanoutCopies == NVSCANOUT_COPY_MAX ? "  [CAP REACHED -- stopping, panel keeps this frame]" : "");
    return true;
}

#ifdef NMX_ACCEL
// b93 (10-19): the Intel desktop's real front buffer is the framebuffer resource - since b92 a shadow in RAM (swap
// argument a1). WindowServer composites into a back buffer (a2), "swaps" it into a1, and then draws the SOFTWARE CURSOR
// straight into a1 (NMXGPUFB has no hardware cursor), saving and restoring the pixels under it. b91/b92 copied a2 to the
// panel and never showed a1: the cursor's save-under/restore fought the copy - multiple ghost cursors and flicker
// (10-19 photos). Now: a2 -> a1 on every swap, and a1 -> panel on every swap AND ~60 times a second (cursor moves need no
// swap), so the panel shows exactly what WindowServer believes the screen holds.
static IOLock *gNmxPresLock = nullptr;
static struct { void *kva; unsigned w, h, pitch; } gNmxShadow;
static struct thread_call *gNmxPresCall = nullptr;
static volatile SInt64 gNmxPresSwaps, gNmxPresTicks;
static void nmxCopyRows(void *d, unsigned dp, const void *s, unsigned sp, unsigned rowBytes, unsigned rows)
{
    for (unsigned y = 0; y < rows; y++)
        memcpy((uint8_t *)d + (size_t)y * dp, (const uint8_t *)s + (size_t)y * sp, rowBytes);
}
static void nmxShadowToPanelLocked()
{
    NVAccel *a = NVAccel::gAccel;
    if (!a || !gNmxShadow.kva) return;
    struct NVRMVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = NVRM_VRAM_ABI_VERSION;
    if (a->askFramebufferForScanout(&q) != kIOReturnSuccess || !q.kva || !q.pitch) return;
    const unsigned rows = gNmxShadow.h < q.height ? gNmxShadow.h : q.height;
    const unsigned w = gNmxShadow.w < q.width ? gNmxShadow.w : q.width;
    unsigned rb = w * 4; if (rb > gNmxShadow.pitch) rb = gNmxShadow.pitch; if (rb > q.pitch) rb = q.pitch;
    nmxCopyRows(q.kva, q.pitch, gNmxShadow.kva, gNmxShadow.pitch, rb & ~3u, rows);
}
static void nmxPresentTick(void *, void *)
{
    if (gNmxPresLock && gNvIop) {
        IOLockLock(gNmxPresLock);
        if (gNmxShadow.kva) { nmxShadowToPanelLocked(); OSAddAtomic64(1, &gNmxPresTicks); }
        IOLockUnlock(gNmxPresLock);
    }
    uint64_t when = 0; clock_interval_to_deadline(16, kMillisecondScale, &when);
    if (gNmxPresCall) thread_call_enter_delayed(gNmxPresCall, when);
}
// returns true when the frame was presented through the shadow (else the caller keeps the old copy path)
bool nmxSwapPresent(IOAccelResource2 *a1, IOAccelResource2 *a2)
{
    /* b94: the b93 front-buffer present path is opt-in (boot-arg nmxpresent=1). b94 gives NMXGPUFB a hardware cursor,
       so WindowServer no longer draws the pointer into the front buffer and the b92 copy path is the default again. */
    static int sOn = -1;
    if (sOn < 0) { int v = 0; sOn = (PE_parse_boot_argn("nmxpresent", &v, sizeof v) && v) ? 1 : 0;
                   IOLog("NMXAccel: present path %s\n", sOn ? "ON (b93 front buffer, nmxpresent=1)" : "off (b92 copy; nmxpresent=1 turns it on)"); }
    if (!sOn) return false;
    if (!gNmxPresLock) { IOLock *l = IOLockAlloc(); if (!l) return false; if (!OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNmxPresLock)) IOLockFree(l); }
    NVVidMemory *vm1 = nvres_vidmem(a1);
    if (!vm1 || !vm1->fKva || vm1->fScanout) return false;
    const unsigned char *r1 = (const unsigned char *)a1;
    const unsigned w1 = *(const volatile unsigned short *)(r1 + 0xb0), h1 = *(const volatile unsigned short *)(r1 + 0xb2);
    const unsigned p1 = *(const volatile unsigned int *)(r1 + 0xb8);
    if (!w1 || !h1 || p1 < 4 * w1 || (unsigned long long)p1 * h1 > vm1->fBytes) return false;
    IOLockLock(gNmxPresLock);
    NVVidMemory *vm2 = a2 ? nvres_vidmem(a2) : nullptr;
    if (vm2 && vm2->fKva && vm2 != vm1) {
        const unsigned char *r2 = (const unsigned char *)a2;
        const unsigned w2 = *(const volatile unsigned short *)(r2 + 0xb0), h2 = *(const volatile unsigned short *)(r2 + 0xb2);
        const unsigned p2 = *(const volatile unsigned int *)(r2 + 0xb8);
        if (w2 && h2 && p2 >= 4 * w2 && (unsigned long long)p2 * h2 <= vm2->fBytes) {
            const unsigned w = w2 < w1 ? w2 : w1, h = h2 < h1 ? h2 : h1;
            nmxCopyRows(vm1->fKva, p1, vm2->fKva, p2, w * 4, h);
        }
    }
    gNmxShadow.kva = vm1->fKva; gNmxShadow.w = w1; gNmxShadow.h = h1; gNmxShadow.pitch = p1;
    nmxShadowToPanelLocked();
    IOLockUnlock(gNmxPresLock);
    const SInt64 n = OSAddAtomic64(1, &gNmxPresSwaps);
    if (n < 3) ALOG("present: swap #%lld through the RAM shadow %ux%u pitch %u (a2 %s) -> panel; refresh timer %s", (long long)n + 1,
                    w1, h1, p1, (vm2 && vm2->fKva) ? "copied in" : "none", gNmxPresCall ? "running" : "starting");
    if (!gNmxPresCall) {
        struct thread_call *c = thread_call_allocate(&nmxPresentTick, nullptr);
        if (c && OSCompareAndSwapPtr(nullptr, c, (void *volatile *)&gNmxPresCall)) nmxPresentTick(nullptr, nullptr);
    }
    return true;
}
// called before a vidmem's memory goes away: the timer must never copy from freed pages
void nmxShadowForget(void *kva)
{
    if (!gNmxPresLock || !kva) return;
    IOLockLock(gNmxPresLock);
    if (gNmxShadow.kva == kva) gNmxShadow.kva = nullptr;
    IOLockUnlock(gNmxPresLock);
}
#endif

static bool nvAccelFlipTo(IOAccelResource2 *res)
{
    if (!res) { NVFLIPWHY("nvAccelFlipTo: no resource"); return false; }
    NVAccel *a = NVAccel::gAccel;
    if (!a || !a->fFB) { NVFLIPWHY("nvAccelFlipTo: accel %p fFB %p", a, a ? a->fFB : nullptr); return false; }
    if (gNvHeadsPiped & (gNvHeadsPiped - 1)) { NVFLIPWHY("nvAccelFlipTo: refused -- heads piped 0x%x, no way to tell whose surface this is", gNvHeadsPiped); return false; }
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm || !vm->fRmHandle) {
        NVFLIPWHY("nvAccelFlipTo: vm %p fRmHandle %p -- the composited surface has no RM memory to flip to",
                  vm, vm ? vm->fRmHandle : nullptr);
        return false;
    }
    if (vm->fScanout) { NVFLIPWHY("nvAccelFlipTo: already the scanout"); return false; }

    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!w || !h || !pitch) { NVFLIPWHY("nvAccelFlipTo: geometry %ux%u pitch %u", w, h, pitch); return false; }

    struct NVRMFlipRequest f;
    volatile unsigned char *z = (volatile unsigned char *)&f;
    for (unsigned i = 0; i < sizeof f; i++) z[i] = 0;
    f.version = NVRM_FLIP_ABI_VERSION;
    f.kapiMemory = vm->fRmHandle; f.allocationCookie = vm->fAllocationCookie;
    f.width = w; f.height = h; f.pitch = pitch;

    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_FLIP_FN);
    if (!sym) { NVFLIPWHY("nvAccelFlipTo: OSSymbol allocation failed"); return false; }
    IOReturn rr = a->fFB->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr);
    sym->release();
    if (rr != kIOReturnSuccess) {
        NVFLIPWHY("nvAccelFlipTo: framebuffer refused the flip (0x%x, flipResult %d) for %ux%u pitch %u kapiMemory %p",
                  (unsigned)rr, f.flipResult, w, h, pitch, vm->fRmHandle);
        return false;
    }
    NVFLIPWHY("nvAccelFlipTo: *** FLIPPED *** to %ux%u pitch %u kapiMemory %p (flipResult %d)",
              w, h, pitch, vm->fRmHandle, f.flipResult);
    nvAccelSampleFront(vm, w, h, pitch);
    return true;
}

IOReturn NVAccel::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && p1 && fn->isEqualTo(NVACCEL_SS_FN))
        return nvAccelSurfVram((struct NVRMSurfShareEsc *)p1, (task_t)p2, (unsigned)(uintptr_t)p3);
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

NM_TAHOE_FWD_DEFS

#ifdef NMX_ACCEL
/* b98: frame timing, one log line per second while WindowServer swaps (no line in an idle second). The lag of 10-20 had
   no busy process in two spindumps, so this measures what reaches the panel: swaps per second, the CPU copy's time,
   and the longest gap between two swaps in that second. */
static IOSimpleLock *gNmxStatLock;
static unsigned long long gNmxStatWin, gNmxStatLast, gNmxStatMaxGap, gNmxStatSum, gNmxStatMax;
static unsigned gNmxStatN, gNmxStatCopied, gNmxStatLines;
static volatile SInt64 gCntCfOk, gCntCfFail, gCntCfPx, gCntStaleSkip;
void nmxSwapStat(unsigned long long t0, unsigned long long t1, bool copied)
{
    if (!gNmxStatLock) { IOSimpleLock *l = IOSimpleLockAlloc(); if (!l) return; if (!OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNmxStatLock)) IOSimpleLockFree(l); }
    char line[400]; line[0] = 0;
    IOSimpleLockLock(gNmxStatLock);
    if (gNmxStatWin && t0 - gNmxStatWin >= 1000000000ull && gNmxStatN) {
        snprintf(line, sizeof line, "frames: %u in %llu ms, copied %u, copy avg %llu us max %llu us, longest gap %llu ms | pipe vbl %d/%d tx %d/%d begin %d signal %d submit %d TIMEOUTS %d | copyfwd ok %lld fail %lld | b113 back buffers synced %lld",
                 gNmxStatN, (t0 - gNmxStatWin) / 1000000ull, gNmxStatCopied, gNmxStatSum / gNmxStatN / 1000ull,
                 gNmxStatMax / 1000ull, gNmxStatMaxGap / 1000000ull,
                 (int)gNmxPipeCnt[kPipeVblOn], (int)gNmxPipeCnt[kPipeVblOff], (int)gNmxPipeCnt[kPipeTxOn], (int)gNmxPipeCnt[kPipeTxOff],
                 (int)gNmxPipeCnt[kPipeBegin], (int)gNmxPipeCnt[kPipeSignal], (int)gNmxPipeCnt[kPipeSubmit], (int)gNmxPipeCnt[kPipeTimeout],
                 (long long)gCntCfOk, (long long)gCntCfFail, (long long)gCntStaleSkip);
        gNmxStatN = gNmxStatCopied = 0; gNmxStatSum = gNmxStatMax = gNmxStatMaxGap = 0;
        gNmxStatWin = t0;
    }
    if (!gNmxStatWin || t0 - gNmxStatWin >= 5000000000ull) { gNmxStatWin = t0; gNmxStatN = gNmxStatCopied = 0; gNmxStatSum = gNmxStatMax = gNmxStatMaxGap = 0; }
    if (gNmxStatLast && t0 > gNmxStatLast && gNmxStatN && t0 - gNmxStatLast > gNmxStatMaxGap) gNmxStatMaxGap = t0 - gNmxStatLast;
    gNmxStatLast = t0;
    const unsigned long long d = t1 > t0 ? t1 - t0 : 0;
    gNmxStatN++; if (copied) gNmxStatCopied++; gNmxStatSum += d; if (d > gNmxStatMax) gNmxStatMax = d;
    IOSimpleLockUnlock(gNmxStatLock);
    if (line[0] && gNmxStatLines < 3000) { gNmxStatLines++; ALOG("%s", line); }
}

/* b107: copy forward (see NVSurface::submitCopyForward). The resource geometry is read where the swap code reads it
   (width +0xb0, height +0xb2, pitch +0xb8); the bounds are IOAccelBounds {int16 x, y, w, h} in pixels, 4 bytes each. */
static IOAccelResource2 *volatile gNmxFront = nullptr;     // the buffer last copied to the panel (swap a2 normally)
static IOAccelResource2 *volatile gNmxSwapA1 = nullptr;    // the swap's a1: the family's FRONT buffer
static volatile SInt32 gNmxCfLogs = 0;
static int nmxCopyFwdOff()
{
    /* b108: OPT-IN while it is proven (b107 had it on by default and froze the login screen with stale pixels).
       boot-arg nmxcopyfwd=1 turns it on; without it (or with nmxnocopyfwd=1) the family default answers unsupported,
       exactly as b106 and earlier. */
    static int off = -1;
    if (off < 0) { int on = 0, no = 0;
                   PE_parse_boot_argn("nmxcopyfwd", &on, sizeof on); PE_parse_boot_argn("nmxnocopyfwd", &no, sizeof no);
                   off = (on && !no) ? 0 : 1;
                   ALOG("copyfwd: %s", off ? "OFF (default; boot-arg nmxcopyfwd=1 turns it on) - unsupported, as b106"
                                           : "ON (nmxcopyfwd=1, b108)"); }
    return off;
}
static bool nmxResGeom(IOAccelResource2 *r, NVVidMemory **vm, unsigned *w, unsigned *h, unsigned *pitch);
/* b108: b107 froze the login screen with old pixels around the cursor (10-20 photo). In the family's model the swap's a1
   is the FRONT buffer and copy forward copies FROM it; our swap copied a2 to the panel and never into a1, so a1 held a
   stale picture, and b107 copied from it when neither argument was the buffer last shown. Now the swap also keeps a1
   equal to what the panel shows (a2 -> a1, one more ~1 ms copy, only while copy forward is on), so the front buffer
   is current whichever argument copy forward names. */
void nmxNoteFront(IOAccelResource2 *res) { gNmxFront = res; }
/* b109: the swap trace (10-20 18:49, 186 swaps on one surface) showed every update as
       copyfwd a1 -> B1, swap(a1, B1), and 2 ms later swap(a1, B2)
   B2 never receives a copy forward and is never drawn into: it still held the LOCK SCREEN from before login, and the
   panel showed it 2 ms after every real frame - the flicker between the desktop and the old lock screen. (b106 skipped
   B2 only because it was all zero then: nvAccelResHasContent said "empty".) A back buffer is therefore shown only if it
   is NEW: it was the target of the last copy forward, or its pixels changed since it was last shown (a whole-screen
   redraw needs no copy forward). Otherwise the panel keeps the last real frame. Only while copy forward is on. */
/* b110: b109 also compared a 48 x 48 pixel sample to spot buffers that changed without a copy forward. That missed small
   changes: a Dock label going away is redrawn into B1 WITHOUT a copy forward (its old area is covered by the new
   damage), the sample did not change, the frame was skipped, a1 kept the label and copy forward kept copying it back -
   every label the pointer passed over stayed on screen (10-20 19:07 photo). The trace's real rule is simpler: B2 is
   NEVER a copy-forward target, B1 is one all the time. So: once copy forward is running, a back buffer that has never
   been its target is the unused second buffer and is not shown; every buffer that has been a target is always shown,
   however small its change. If no copy forward has happened for 1 s (whole-screen redraws only), everything is shown. */
enum { kNmxCfSlots = 8 };
static IOAccelResource2 *volatile gNmxCfSeen[kNmxCfSlots];
static unsigned gNmxCfSeenNext;
static volatile uint64_t gNmxCfLastAbs;
static void nmxNoteCfTarget(IOAccelResource2 *res)
{
    gNmxCfLastAbs = mach_absolute_time();
    for (unsigned i = 0; i < kNmxCfSlots; i++) if (gNmxCfSeen[i] == res) return;
    gNmxCfSeen[gNmxCfSeenNext++ % kNmxCfSlots] = res;
}
/* b111: b110 showed every buffer again after 1 s without a copy forward - sitting still was enough for the unused
   buffer (the old lock screen) to reach the panel, and the lazy a1 sync then copied that picture into the real buffer:
   the lock screen came back with the desktop showing through only where something was redrawn (10-21 03:33). No timer
   now: once copy forward has run, a buffer that was never its target is never shown and never becomes a copy source.
   The list starts over only when the surface gets a different front buffer (a1) - the buffers were rebuilt. */
static IOAccelResource2 *volatile gNmxCfSurfA1 = nullptr;
static bool nmxIsCfTarget(IOAccelResource2 *r)
{
    for (unsigned i = 0; i < kNmxCfSlots; i++) if (gNmxCfSeen[i] == r) return true;
    return false;
}
void nmxSwapNoteA1(IOAccelResource2 *a1)
{
    if (!a1 || a1 == gNmxCfSurfA1) return;
    if (gNmxCfSurfA1) {
        for (unsigned i = 0; i < kNmxCfSlots; i++) gNmxCfSeen[i] = nullptr;
        gNmxCfLastAbs = 0;
        ALOG("swap: new front buffer %p (was %p) - the buffers were rebuilt, copy-forward targets start over (b111)", a1, gNmxCfSurfA1);
    }
    gNmxCfSurfA1 = a1;
}
bool nmxSwapIsStale(IOAccelResource2 *a2)
{
    if (!a2 || nmxCopyFwdOff() || !gNmxCfLastAbs) return false;
    if (nmxIsCfTarget(a2)) return false;
    const SInt64 n = OSAddAtomic64(1, &gCntStaleSkip);
    if (n < 6) ALOG("swap: back buffer %p has never received a copy forward (the unused second buffer) - NOT shown, "
                    "the panel keeps the last real frame (b111)", a2);
    return true;
}
/* b110 (lag): the a2 -> a1 copy is made only when copy forward actually needs a1 as its source for a buffer that is not
   already the picture on the panel. On this Mac every copy forward goes into the buffer shown last (the trace: B1 each
   time), which already holds everything outside the new damage - so neither copy is needed and a frame costs one full
   copy (to the panel) instead of three. */
static IOAccelResource2 *volatile gNmxA1Pending = nullptr;
static void nmxSyncFront(IOAccelResource2 *a1, IOAccelResource2 *shown);
void nmxSwapKeepFront(IOAccelResource2 *a1, IOAccelResource2 *shown)
{
    if (!a1) return;
    gNmxSwapA1 = a1;
    if (nmxCopyFwdOff() || !shown || shown == a1) return;
    if (nmxIsCfTarget(shown) || !gNmxCfLastAbs) gNmxA1Pending = shown;   /* a1 is behind the panel by this buffer; synced lazily */
}
static void nmxSyncFront(IOAccelResource2 *a1, IOAccelResource2 *shown)
{
    if (!a1 || !shown || shown == a1) return;
    NVVidMemory *vf = nullptr, *vs = nullptr; unsigned wf, hf, pf, ws, hs, ps;
    if (!nmxResGeom(a1, &vf, &wf, &hf, &pf) || !nmxResGeom(shown, &vs, &ws, &hs, &ps) || vf == vs) return;
    const unsigned w = wf < ws ? wf : ws, h = hf < hs ? hf : hs;
    for (unsigned y = 0; y < h; y++)
        memcpy((uint8_t *)vf->fKva + (size_t)y * pf, (const uint8_t *)vs->fKva + (size_t)y * ps, (size_t)w * 4);
}
struct NmxBounds { int16_t x, y, w, h; };
static bool nmxResGeom(IOAccelResource2 *r, NVVidMemory **vm, unsigned *w, unsigned *h, unsigned *pitch)
{
    NVVidMemory *m = r ? nvres_vidmem(r) : nullptr;
    if (!m || !m->fKva) return false;
    const unsigned char *rr = (const unsigned char *)r;
    *w = *(const volatile unsigned short *)(rr + 0xb0); *h = *(const volatile unsigned short *)(rr + 0xb2);
    *pitch = *(const volatile unsigned int *)(rr + 0xb8);
    if (!*w || !*h || *pitch < 4 * *w || (unsigned long long)*pitch * *h > m->fBytes) return false;
    *vm = m; return true;
}
static IOReturn nmxCopyForwardSync(unsigned int mode, IOAccelResource2 *a, IOAccelResource2 *b, const void *bounds, unsigned int n);
static int nmxSyncModeOff();
IOReturn nmxCopyForward(unsigned int mode, IOAccelResource2 *a, IOAccelResource2 *b, const void *bounds, unsigned int n)
{
    if (nmxCopyFwdOff()) return kIOReturnUnsupported;
    if (!nmxSyncModeOff()) return nmxCopyForwardSync(mode, a, b, bounds, n);   /* b113 */
    NVVidMemory *va = nullptr, *vb = nullptr; unsigned wa = 0, ha = 0, pa = 0, wb = 0, hb = 0, pb = 0;
    const bool ga = nmxResGeom(a, &va, &wa, &ha, &pa), gb = nmxResGeom(b, &vb, &wb, &hb, &pb);
    if (!ga || !gb) {
        OSAddAtomic64(1, &gCntCfFail);
        if (OSIncrementAtomic(&gNmxCfLogs) < 12)
            ALOG("copyfwd: mode 0x%x a %p (%s) b %p (%s) - a buffer without memory/geometry, answering unsupported as before",
                 mode, a, ga ? "ok" : "NO", b, gb ? "ok" : "NO");
        return kIOReturnUnsupported;
    }
    if (va == vb) return kIOReturnSuccess;
    /* source = a buffer known to hold the picture on the panel: the family's front buffer (swap a1, kept current by
       nmxSwapKeepFront) or the buffer last copied to the panel. Neither argument known: no guess (b107 guessed and
       pasted a stale picture) - answer unsupported as before, CoreDisplay then redraws that region itself. */
    IOAccelResource2 *front = gNmxFront, *fa1 = gNmxSwapA1; const char *why;
    bool aSrc;
    if (a == fa1 && b != fa1)        { aSrc = true;  why = "a = front buffer (swap a1)"; }
    else if (b == fa1 && a != fa1)   { aSrc = false; why = "b = front buffer (swap a1)"; }
    else if (a == front && b != front) { aSrc = true;  why = "a = last on the panel"; }
    else if (b == front && a != front) { aSrc = false; why = "b = last on the panel"; }
    else {
        OSAddAtomic64(1, &gCntCfFail);
        if (OSIncrementAtomic(&gNmxCfLogs) < 24)
            ALOG("copyfwd: mode 0x%x a %p b %p - neither is the front buffer (a1 %p) nor the last shown (%p): unsupported",
                 mode, a, b, fa1, front);
        return kIOReturnUnsupported;
    }
    IOAccelResource2 *dstRes = aSrc ? b : a, *srcRes = aSrc ? a : b;
    if (!gNmxCfLastAbs) {
        /* b111: the FIRST copy forward (also the first after the buffers were rebuilt). Which back buffer is real is
           known only from now on, and a1 may still hold whatever the unused buffer showed (the lock screen): copying
           from it would plant that picture in the real buffer for good. The destination already holds WindowServer's
           own last frame, so nothing is copied, and anything queued for a1 is dropped. */
        gNmxA1Pending = nullptr;
        OSAddAtomic64(1, &gCntCfOk); nmxNoteCfTarget(dstRes);
        ALOG("copyfwd: first one into %p - nothing copied, it becomes the real back buffer (b111)", dstRes);
        return kIOReturnSuccess;
    }
    if (dstRes == gNmxFront) {   /* b110: the destination IS the picture on the panel - nothing to copy */
        OSAddAtomic64(1, &gCntCfOk); nmxNoteCfTarget(dstRes);
        return kIOReturnSuccess;
    }
    if (srcRes == fa1 && gNmxA1Pending) { nmxSyncFront(fa1, gNmxA1Pending); gNmxA1Pending = nullptr; }
    NVVidMemory *vs = aSrc ? va : vb, *vd = aSrc ? vb : va;
    const unsigned ps = aSrc ? pa : pb, pd = aSrc ? pb : pa;
    const unsigned W = wa < wb ? wa : wb, H = ha < hb ? ha : hb;
    const NmxBounds whole = { 0, 0, (int16_t)(W > 32767 ? 32767 : W), (int16_t)(H > 32767 ? 32767 : H) };
    const NmxBounds *bb = (bounds && n) ? (const NmxBounds *)bounds : &whole;
    const unsigned cnt = (bounds && n) ? (n > 4096 ? 4096 : n) : 1;
    unsigned long long px = 0;
    for (unsigned i = 0; i < cnt; i++) {
        int x0 = bb[i].x, y0 = bb[i].y, x1 = x0 + bb[i].w, y1 = y0 + bb[i].h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int)W) x1 = (int)W;
        if (y1 > (int)H) y1 = (int)H;
        if (x1 <= x0 || y1 <= y0) continue;
        const size_t rb = (size_t)(x1 - x0) * 4;
        for (int y = y0; y < y1; y++)
            memcpy((uint8_t *)vd->fKva + (size_t)y * pd + (size_t)x0 * 4, (const uint8_t *)vs->fKva + (size_t)y * ps + (size_t)x0 * 4, rb);
        px += (unsigned long long)(x1 - x0) * (unsigned long long)(y1 - y0);
    }
    OSAddAtomic64(1, &gCntCfOk); OSAddAtomic64((SInt64)px, &gCntCfPx);
    nmxNoteCfTarget(aSrc ? b : a);
    if (OSIncrementAtomic(&gNmxCfLogs) < 24)
        ALOG("copyfwd: mode 0x%x %u rect(s)%s, first %d,%d %dx%d - %s -> %s (%s; a %p b %p a1 %p shown %p), %ux%u, %llu px",
             mode, cnt, (bounds && n) ? "" : " (none given: whole buffer)", bb[0].x, bb[0].y, bb[0].w, bb[0].h,
             aSrc ? "a" : "b", aSrc ? "b" : "a", why, a, b, fa1, front, W, H, px);
    return kIOReturnSuccess;
}
/* b113 - the lock screen (10-21 photo: clock missing, the date cut off at the baseline, the unlock spinner a black
   square; armed only, also with nmx-ws-arm.sh). b109-b111 decided which back buffer is "real" by a rule (a buffer that
   never received a copy forward is the unused one and is never shown). That rule came from a desktop trace. On the lock
   screen WindowServer draws into the OTHER buffer (b108: B2 held the lock screen) - with b111 those frames never reached
   the panel, and after a buffer rebuild the first copy forward copied nothing, so a new (all-zero) buffer showed black
   wherever it was not redrawn.
   Now no guessing at all: every swap shows its back buffer, and right after that the same picture is copied into every
   other back buffer of the desktop. So whichever buffer WindowServer draws into next, everything it does not redraw
   is already the latest picture: a buffer it leaves untouched shows the same frame again (no flicker), a buffer it
   draws into shows its drawing (the lock screen clock, the spinner). Copy forward copies its rectangles from the last
   shown frame. Buffers are tracked by their memory (kva) and forgotten when that memory is freed, so a rebuild can
   never copy from freed pages. Cost: one more full-frame RAM copy per swap (~1 ms). boot-arg nmxsyncoff=1 = b111. */
struct NmxBuf { uint8_t *kva; unsigned w, h, pitch; };
enum { kNmxSyncSlots = 6 };
static IOLock *gNmxSyncLock = nullptr;
static NmxBuf gNmxLast;                       /* the frame on the panel (memory of the back buffer last shown) */
static NmxBuf gNmxBacks[kNmxSyncSlots];       /* back buffers seen in swaps */
static unsigned gNmxBackNext;
static volatile SInt32 gNmxSyncLogs;
static int nmxSyncModeOff()
{
    static int off = -1;
    /* b115: OFF by default again. b113 did not fix the lock screen (that was framebuffer fetch, b114) and cost up to
       four full-frame copies per screen update (show B1, sync B2, show B2, sync B1) - more typing delay for nothing.
       boot-arg nmxsync=1 turns it back on. */
    if (off < 0) { int v = 0; off = (PE_parse_boot_argn("nmxsync", &v, sizeof v) && v) ? 0 : 1;
                   ALOG("swap: %s", off ? "b111 buffer rule (default; nmxsync=1 = b113 sync)" : "b113 every back buffer kept equal to the panel (nmxsync=1)"); }
    return off;
}
static bool nmxSyncLockReady()
{
    if (gNmxSyncLock) return true;
    IOLock *l = IOLockAlloc(); if (!l) return false;
    if (!OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNmxSyncLock)) IOLockFree(l);
    return true;
}
static bool nmxBufOf(IOAccelResource2 *r, NmxBuf *o)
{
    NVVidMemory *vm = nullptr; unsigned w, h, p;
    if (!nmxResGeom(r, &vm, &w, &h, &p) || vm->fScanout) return false;
    o->kva = (uint8_t *)vm->fKva; o->w = w; o->h = h; o->pitch = p; return true;
}
static void nmxBufCopy(const NmxBuf &d, const NmxBuf &s)
{
    const unsigned w = d.w < s.w ? d.w : s.w, h = d.h < s.h ? d.h : s.h;
    for (unsigned y = 0; y < h; y++) memcpy(d.kva + (size_t)y * d.pitch, s.kva + (size_t)y * s.pitch, (size_t)w * 4);
}
void nmxSyncForget(void *kva)
{
    if (!gNmxSyncLock || !kva) return;
    IOLockLock(gNmxSyncLock);
    if (gNmxLast.kva == kva) gNmxLast.kva = nullptr;
    for (unsigned i = 0; i < kNmxSyncSlots; i++) if (gNmxBacks[i].kva == kva) gNmxBacks[i].kva = nullptr;
    IOLockUnlock(gNmxSyncLock);
}
/* -1: not handled (copy forward off, nmxsyncoff=1, or a2 has no memory) - the caller keeps the old path; 0/1: copied */
int nmxSwapSync(IOAccelResource2 *a1, IOAccelResource2 *a2)
{
    if (nmxCopyFwdOff() || nmxSyncModeOff() || !a2 || a2 == a1 || !nmxSyncLockReady()) return -1;
    NmxBuf b;
    if (!nmxBufOf(a2, &b)) return -1;
    IOLockLock(gNmxSyncLock);
    const bool copied = nvAccelCopyToScanout(a2);
    unsigned synced = 0; bool known = false;
    if (copied) {
        for (unsigned i = 0; i < kNmxSyncSlots; i++) {
            NmxBuf &o = gNmxBacks[i];
            if (!o.kva) continue;
            if (o.kva == b.kva) { known = true; continue; }
            if (o.w != b.w || o.h != b.h) continue;      /* another size: an old desktop mode, left alone */
            nmxBufCopy(o, b); synced++;
        }
        if (!known) {   /* a new back buffer: take a free slot, else the oldest */
            unsigned slot = kNmxSyncSlots;
            for (unsigned i = 0; i < kNmxSyncSlots; i++) if (!gNmxBacks[i].kva) { slot = i; break; }
            if (slot == kNmxSyncSlots) slot = gNmxBackNext++ % kNmxSyncSlots;
            gNmxBacks[slot] = b;
        }
        gNmxLast = b;
    }
    IOLockUnlock(gNmxSyncLock);
    if (copied) { nmxNoteFront(a2); gNmxSwapA1 = a1; OSAddAtomic64((SInt64)synced, &gCntStaleSkip); }
    if (!known && OSIncrementAtomic(&gNmxSyncLogs) < 16)
        ALOG("swap: new back buffer %p kva %p %ux%u pitch %u (front %p) - shown, and from now on kept equal to the panel (b113)",
             a2, b.kva, b.w, b.h, b.pitch, a1);
    return copied ? 1 : 0;
}
static IOReturn nmxCopyForwardSync(unsigned int mode, IOAccelResource2 *a, IOAccelResource2 *b, const void *bounds, unsigned int n)
{
    /* the destination is the argument that is not the front buffer (swap a1); the source is always the last shown frame */
    IOAccelResource2 *fa1 = gNmxSwapA1;
    IOAccelResource2 *dstRes = (a == fa1) ? b : (b == fa1) ? a : (a == gNmxFront) ? b : (b == gNmxFront) ? a : nullptr;
    NmxBuf d;
    if (!dstRes || !nmxBufOf(dstRes, &d) || !nmxSyncLockReady()) {
        OSAddAtomic64(1, &gCntCfFail);
        if (OSIncrementAtomic(&gNmxCfLogs) < 24)
            ALOG("copyfwd: mode 0x%x a %p b %p (front %p, shown %p) - no destination known: unsupported (b113)", mode, a, b, fa1, gNmxFront);
        return kIOReturnUnsupported;
    }
    IOLockLock(gNmxSyncLock);
    const NmxBuf s = gNmxLast;
    if (!s.kva || s.w != d.w || s.h != d.h) {
        IOLockUnlock(gNmxSyncLock);
        OSAddAtomic64(1, &gCntCfFail);
        if (OSIncrementAtomic(&gNmxCfLogs) < 24)
            ALOG("copyfwd: mode 0x%x into %p - no shown frame of the same size yet: unsupported, WindowServer redraws (b113)", mode, dstRes);
        return kIOReturnUnsupported;
    }
    unsigned long long px = 0;
    if (s.kva != d.kva) {
        const unsigned W = d.w, H = d.h;
        const NmxBounds whole = { 0, 0, (int16_t)(W > 32767 ? 32767 : W), (int16_t)(H > 32767 ? 32767 : H) };
        const NmxBounds *bb = (bounds && n) ? (const NmxBounds *)bounds : &whole;
        const unsigned cnt = (bounds && n) ? (n > 4096 ? 4096 : n) : 1;
        for (unsigned i = 0; i < cnt; i++) {
            int x0 = bb[i].x, y0 = bb[i].y, x1 = x0 + bb[i].w, y1 = y0 + bb[i].h;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > (int)W) x1 = (int)W;
            if (y1 > (int)H) y1 = (int)H;
            if (x1 <= x0 || y1 <= y0) continue;
            const size_t rb = (size_t)(x1 - x0) * 4;
            for (int y = y0; y < y1; y++)
                memcpy(d.kva + (size_t)y * d.pitch + (size_t)x0 * 4, s.kva + (size_t)y * s.pitch + (size_t)x0 * 4, rb);
            px += (unsigned long long)(x1 - x0) * (unsigned long long)(y1 - y0);
        }
    }
    IOLockUnlock(gNmxSyncLock);
    OSAddAtomic64(1, &gCntCfOk); OSAddAtomic64((SInt64)px, &gCntCfPx);
    if (OSIncrementAtomic(&gNmxCfLogs) < 12)
        ALOG("copyfwd: mode 0x%x %u rect(s) from the shown frame into %p, %llu px (b113)", mode, (bounds && n) ? n : 1u, dstRes, px);
    return kIOReturnSuccess;
}
#endif
