// NMIntelFB: framebuffer for the built-in panel of laptops whose panel is wired to an Intel GPU macOS has no driver for
// (display versions 12-20: Tiger Lake, Rocket Lake, Alder Lake, Raptor Lake, Meteor Lake, Arrow Lake, Lunar Lake).
//
// Phase 1 takes over the mode the firmware already lit: it finds the pipe whose primary plane is scanning out, checks
// that the plane is linear 32-bit XRGB, and publishes that surface (through the graphics aperture, BAR2 + the plane's GGTT
// offset) as an IOFramebuffer with the panel's real size and refresh. Nothing is written to the display engine, so a
// laptop it does not understand keeps exactly the screen it had: start() fails, every reason is logged, and the firmware
// framebuffer (IONDRVFramebuffer) takes the device as before.
//
// Register layout: Linux i915/xe display headers (MIT). Plane 1 of pipe P: PLANE_CTL 0x70180 + P*0x1000, STRIDE +0x08,
// SIZE +0x10, SURF +0x1C. Transcoder T timings at 0x60000 + T*0x1000, TRANSCONF at 0x70008 + P*0x1000, PIPESRC 0x6001C.

#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>
#include <libkern/OSAtomic.h>
#include "nvrm_vram_abi.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread_call.h>
#include <sys/sysctl.h>

#define IFBLOG(fmt, ...) IOLog("NMIntelFB: " fmt "\n", ##__VA_ARGS__)
#include <IOKit/IONVRAM.h>

/*
 * ifb_step(): breadcrumb that survives a forced power-off. Stored in NVRAM as nmintelfb-step; read it after the next
 * boot with:  nvram nmintelfb-step
 */
static void ifb_step_key(const char *key, const char *what);
static void ifb_step(const char *what) { ifb_step_key("nmintelfb-step", what); }
/* brightness steps: their own NVRAM entry, so a later boot with -nmintelfbnobl does not overwrite them */
static void bl_step(const char *what) { ifb_step_key("nmintelfb-bl-step", what); }
static void ifb_step_key(const char *key, const char *what)
{
    IFBLOG("step: %s", what);
    IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
    IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e);
    if (n == nullptr) { if (e) e->release(); return; }
    const OSSymbol *k = OSSymbol::withCString(key);
    OSData *d = OSData::withBytes(what, (unsigned)strlen(what));
    if (k && d) n->setProperty(k, d);
    if (d) d->release();
    if (k) k->release();
    n->sync();
    n->release();
}

/*
 * The IOFBDependentID/Index pair makes this panel head 4 of the NVIDIA accelerator's display group. On a laptop with no
 * NVIDIA card (no NVRM service) nothing ever answers for that group and WindowServer waits for it: the desktop never
 * came up on the Xiaomi Mi Notebook Pro (Tiger Lake, Sonoma, 10-14). Set the pair only when NVRM is present, or when
 * boot-arg nmintelfbdep=1 forces it (nmintelfbdep=0 forces it off).
 */
static bool ifb_join_nvidia_group()
{
    int force = -1;
    if (PE_parse_boot_argn("nmintelfbdep", &force, sizeof(force)) && (force == 0 || force == 1))
        return force == 1;
    bool found = false;
    if (OSDictionary *m = IOService::serviceMatching("NVRM")) {
        if (OSIterator *it = IOService::getMatchingServices(m)) {
            found = it->getNextObject() != nullptr;
            it->release();
        }
        m->release();
    }
    return found;
}

// kmutil places a kext in a kernel collection only with a _kmod_info symbol (Xcode generates it; this build does not)
#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
#ifdef NMINTELFB_VIRTUAL
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMIntelFBVirtual, "0.1", _start, _stop) }
#else
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMIntelFB, "0.1", _start, _stop) }
#endif
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

namespace {
// display version by PCI device ID, from Linux include/drm/intel/pciids.h (MIT); 0 = not ours
struct Id { uint16_t id; uint8_t ver; };
const Id kIds[] = {
#include "display_ids.inc"
};

uint8_t displayVersion(uint16_t dev)
{
    for (const Id &e : kIds) if (e.id == dev) return e.ver;
    return 0;
}

constexpr uint32_t PLANE_CTL(int p)    { return 0x70180u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_STRIDE(int p) { return 0x70188u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_SIZE(int p)   { return 0x70190u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_SURF(int p)   { return 0x7019Cu + (uint32_t)p * 0x1000u; }
constexpr uint32_t TRANSCONF(int p)    { return 0x70008u + (uint32_t)p * 0x1000u; }
constexpr uint32_t HTOTAL(int t)       { return 0x60000u + (uint32_t)t * 0x1000u; }
constexpr uint32_t VTOTAL(int t)       { return 0x6000Cu + (uint32_t)t * 0x1000u; }
constexpr uint32_t HBLANK(int t)       { return 0x60004u + (uint32_t)t * 0x1000u; }
constexpr uint32_t HSYNC(int t)        { return 0x60008u + (uint32_t)t * 0x1000u; }
constexpr uint32_t VBLANK(int t)       { return 0x60010u + (uint32_t)t * 0x1000u; }
constexpr uint32_t VSYNC(int t)        { return 0x60014u + (uint32_t)t * 0x1000u; }
// one count per vblank (Linux PIPE_FRMCOUNT_G4X, 0x70040 + pipe): counting it is how the real refresh is known without
// reading the PLL and M/N values back
constexpr uint32_t FRMCOUNT(int p)     { return 0x70040u + (uint32_t)p * 0x1000u; }

// PLANE_CTL (display 11+): enable bit 31, format bits 27:23 (8 = XRGB 8:8:8:8, Linux PLANE_CTL_FORMAT_XRGB_8888 on
// ICL+), tiling bits 12:10 (0 = linear)
constexpr uint32_t kEnable = 1u << 31;
// the two power states NVRMFB registers: off, and on + usable
IOPMPowerState kPowerStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};
constexpr uint32_t fmtOf(uint32_t ctl)  { return (ctl >> 23) & 0x1F; }
constexpr uint32_t tileOf(uint32_t ctl) { return (ctl >> 10) & 0x7; }
}

// Panel brightness (same scheme as NVRMFB's NVRMBacklightHandler). macOS gives the brightness keys and slider to an
// AppleBacklightDisplay, which IOGraphics creates for a built-in LCD connection when a service named "backlight"
// exists below the framebuffer before it is opened. This handler is that service: IODisplay offers "brightness" and
// "linear-brightness" (0-100) through it, and each change writes the panel PWM duty cycle the firmware already set up
// (display 11+/PCH: BLC_PWM_CTL 0xC8250 enable bit 31, BLC_PWM_FREQ 0xC8254, BLC_PWM_DUTY 0xC8258). The register write
// runs from a thread call, because IODisplay calls doIntegerSet() with the framebuffer lock held.
static const uint32_t kBlcCtl = 0xC8250, kBlcFreq = 0xC8254, kBlcDuty = 0xC8258;
static const SInt32 kMinBacklight = 5;   // never 0: some panels switch the backlight off completely

class NMIntelBacklight : public IODisplayParameterHandler {
    OSDeclareDefaultStructors(NMIntelBacklight)
    volatile uint8_t *fRegs = nullptr;
    uint32_t fFreq = 0;
    thread_call_t fApply = nullptr;
    volatile SInt32 fPending = -1;
    SInt32 fLevel = 100;
    thread_call_t fSave = nullptr;
    volatile bool fPowered = true;
    uint32_t dutyFor(SInt32 v) const {
        SInt32 pct = v < kMinBacklight ? kMinBacklight : (v > 100 ? 100 : v);
        return (uint32_t)(((uint64_t)fFreq * (uint64_t)pct) / 100);
    }
    // brightness survives a restart: kept in NVRAM as nmintelfb-brightness (one byte, 0-100), written 2 s after the last change
    static void saveFire(thread_call_param_t p, thread_call_param_t) {
        NMIntelBacklight *me = (NMIntelBacklight *)p;
        IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
        IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e);
        if (n == nullptr) { if (e) e->release(); return; }
        uint8_t b = (uint8_t)(me->fLevel < 0 ? 0 : (me->fLevel > 100 ? 100 : me->fLevel));
        const OSSymbol *k = OSSymbol::withCString("nmintelfb-brightness");
        OSData *d = OSData::withBytes(&b, 1);
        if (k && d) n->setProperty(k, d);
        if (d) d->release();
        if (k) k->release();
        n->sync();
        n->release();
    }
public:
    static SInt32 savedLevel() {
        SInt32 v = -1;
        IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e)) {
            if (OSData *d = OSDynamicCast(OSData, n->getProperty("nmintelfb-brightness")))
                if (d->getLength() == 1) v = *(const uint8_t *)d->getBytesNoCopy();
        }
        if (e) e->release();
        return (v >= kMinBacklight && v <= 100) ? v : -1;
    }
    // display/system sleep: backlight off; wake: the PWM set up again (sleep can reset it) at the current level
    void setPowered(bool on) {
        if (!fRegs || on == fPowered) return;
        fPowered = on;
        if (on) {
            *(volatile uint32_t *)(fRegs + kBlcFreq) = fFreq;
            *(volatile uint32_t *)(fRegs + kBlcDuty) = dutyFor(fLevel);
            uint32_t ctl = *(volatile uint32_t *)(fRegs + kBlcCtl);
            if (!(ctl & (1u << 31))) *(volatile uint32_t *)(fRegs + kBlcCtl) = ctl | (1u << 31);
            IFBLOG("backlight: on at %d%%", (int)fLevel);
        } else {
            *(volatile uint32_t *)(fRegs + kBlcDuty) = 0;
            IFBLOG("backlight: off (sleep)");
        }
    }
    void applyNow(SInt32 v) { if (fRegs && fPowered) *(volatile uint32_t *)(fRegs + kBlcDuty) = dutyFor(v); }
private:
    static void applyFire(thread_call_param_t p, thread_call_param_t) {
        NMIntelBacklight *me = (NMIntelBacklight *)p;
        SInt32 v;
        do { v = me->fPending; } while (!OSCompareAndSwap((UInt32)v, (UInt32)-1, (volatile UInt32 *)&me->fPending));
        if (v < 0 || !me->fRegs) return;
        uint32_t duty = me->dutyFor(v);
        me->fLevel = v;
        if (me->fPowered) *(volatile uint32_t *)(me->fRegs + kBlcDuty) = duty;
        IFBLOG("backlight: %d%% (duty %u of %u)%s", (int)v, duty, me->fFreq, me->fPowered ? "" : " (kept for wake)");
        if (me->fSave) { uint64_t d; clock_interval_to_deadline(2000, kMillisecondScale, &d); thread_call_enter_delayed(me->fSave, d); }
        static int applied;
        if (applied < 3) { applied++; bl_step("backlight: level applied"); }   /* first few only: spare the NVRAM */
    }
public:
    static NMIntelBacklight *with(volatile uint8_t *regs, uint32_t freq, SInt32 level) {
        NMIntelBacklight *h = OSTypeAlloc(NMIntelBacklight);
        if (!h || !h->init()) { OSSafeReleaseNULL(h); return nullptr; }
        h->fRegs = regs; h->fFreq = freq; h->fLevel = level < 0 ? 0 : (level > 100 ? 100 : level);
        h->fApply = thread_call_allocate(applyFire, h);
        h->fSave = thread_call_allocate(saveFire, h);
        if (!h->fApply) { h->release(); return nullptr; }
        return h;
    }
    void detachRegs() { fRegs = nullptr; }
    bool setDisplay(IODisplay *display) override {
        if (!display) return false;
        OSObject *old = display->copyProperty(gIODisplayParametersKey);
        OSDictionary *cur = OSDynamicCast(OSDictionary, old);
        OSDictionary *params = cur ? OSDictionary::withDictionary(cur) : OSDictionary::withCapacity(2);
        OSSafeReleaseNULL(old);
        if (!params) return false;
        IODisplay::addParameter(params, gIODisplayBrightnessKey, 0, 100);
        IODisplay::setParameter(params, gIODisplayBrightnessKey, fLevel);
        IODisplay::addParameter(params, gIODisplayLinearBrightnessKey, 0, 100);
        IODisplay::setParameter(params, gIODisplayLinearBrightnessKey, fLevel);
        display->setProperty(gIODisplayParametersKey, params);
        params->release();
        IFBLOG("backlight: brightness offered on %s, panel at %d%%", display->getName(), (int)fLevel);
        bl_step("backlight: brightness offered to macOS");
        return true;
    }
    bool doIntegerSet(OSDictionary *, const OSSymbol *paramName, UInt32 value) override {
        if (paramName != gIODisplayBrightnessKey && paramName != gIODisplayLinearBrightnessKey) return false;
        fPending = value > 100 ? 100 : (SInt32)value;
        thread_call_enter(fApply);
        return true;
    }
    bool doDataSet(const OSSymbol *, OSData *) override { return false; }
    bool doUpdate() override { return true; }
    void free() override {
        if (fApply) { thread_call_cancel(fApply); while (!thread_call_free(fApply)) { thread_call_cancel(fApply); IOSleep(1); } fApply = nullptr; }
        if (fSave) { thread_call_cancel(fSave); while (!thread_call_free(fSave)) { thread_call_cancel(fSave); IOSleep(1); } fSave = nullptr; }
        IODisplayParameterHandler::free();
    }
};
OSDefineMetaClassAndStructors(NMIntelBacklight, IODisplayParameterHandler)

class NMIntelFB : public IOFramebuffer {
    OSDeclareDefaultStructors(NMIntelFB)
    IOPCIDevice *fPCI = nullptr;
    IOMemoryMap *fMMIO = nullptr;
    IOMemoryMap *fSurfMap = nullptr;   // write-combined CPU view of the scan-out surface (NVIDIA frames land here)
    // WindowServer paces an accelerated display on its VBL interrupt: the NVIDIA display pipe completes each frame on it.
    // Without one the first frame never completed and WindowServer stalled every display (studio test 10-10, the
    // virtual panel froze the Mac). A 60 Hz timer is the VBL, as NVRMFB's fallback is.
    struct Irq { IOFBInterruptProc proc = nullptr; OSObject *target = nullptr; void *ref = nullptr; };
    Irq fVbl, fConnect;
    thread_call_t fVblTimer = nullptr;
    uint64_t fVblNext = 0, fVblCalls = 0;
    volatile bool fStopping = false;
    NMIntelBacklight *fBacklight = nullptr;
    void publishBacklight();
    thread_call_t fPmCall = nullptr;
    static void pmRegister(thread_call_param_t p0, thread_call_param_t) {
        NMIntelFB *me = (NMIntelFB *)p0;
        if (me->fStopping) return;
        ifb_step("power: registering (deferred)");
        IOReturn r = me->registerPowerDriver(me, kPowerStates, 2);
        ifb_step(r == kIOReturnSuccess ? "power: registered" : "power: registration FAILED");
    }
    static void vblFire(thread_call_param_t p0, thread_call_param_t) {
        NMIntelFB *me = (NMIntelFB *)p0;
        if (me->fStopping) return;
        if (me->fVbl.proc) { me->fVbl.proc(me->fVbl.target, me->fVbl.ref); me->fVblCalls++; }
        uint64_t period; nanoseconds_to_absolutetime(me->fFrameNs, &period);
        uint64_t now = mach_absolute_time();
        me->fVblNext = (me->fVblNext && me->fVblNext + period > now) ? me->fVblNext + period : now + period;
        thread_call_enter_delayed(me->fVblTimer, me->fVblNext);
    }
    void armVbl() {
        fVblTimer = thread_call_allocate(vblFire, this);
        if (fVblTimer) { uint64_t d; clock_interval_to_deadline((uint32_t)(fFrameNs / 1000), kMicrosecondScale, &d); fVblNext = d; thread_call_enter_delayed(fVblTimer, d); }
        IFBLOG("vbl timer %s", fVblTimer ? "armed (60 Hz)" : "ALLOCATE FAILED");
    }
#ifdef NMINTELFB_VIRTUAL
    IOBufferMemoryDescriptor *fVBuf = nullptr;
    IOTimerEventSource *fTimer = nullptr;
    unsigned fSamples = 0;
    // 64 pixels across the buffer: how many are non-black and how many differ from the first, published in the registry
public:
    // 64 pixels across the buffer, read on demand through sysctl debug.nmintelfb_sample (the registry property the
    // timer set never showed up on studio, 10-10)
    void sampleInto(char *b, size_t n) {
        if (!fSurfMap) { snprintf(b, n, "no surface"); return; }
        const volatile uint32_t *px = (const volatile uint32_t *)fSurfMap->getVirtualAddress();
        unsigned nz = 0, varied = 0; uint32_t first = px[0] & 0xFFFFFF;
        for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 8; j++) {
            uint32_t v = px[(fHeight * (2 * i + 1) / 16) * (fRowBytes / 4) + fWidth * (2 * j + 1) / 16] & 0xFFFFFF;
            if (v) nz++; if (v != first) varied++; }
        snprintf(b, n, "sample %u: nonzero %u/64 varied %u/64 first 0x%06x", ++fSamples, nz, varied, first);
    }
    void sample() { char b[96]; sampleInto(b, sizeof b); setProperty("NMIntelFBSample", b); }
private:
#endif   // write-combined CPU view of the scan-out surface (NVIDIA frames land here)
    volatile uint8_t *fRegs = nullptr;
    IOPhysicalAddress fApertureBase = 0;
    IOPhysicalLength fApertureLen = 0;
    uint32_t fWidth = 0, fHeight = 0, fRowBytes = 0, fSurf = 0, fRefresh16 = 60 << 16;
    uint64_t fFrameNs = 16666667ull;   // vblank period; measured from the pipe's frame counter on real hardware
    // transcoder timing (active, total, sync start/end) for getTimingInfoForDisplayMode
    uint32_t fHT = 0, fVT = 0, fHSS = 0, fHSE = 0, fVSS = 0, fVSE = 0;
    uint64_t fPclk = 0;
    int fPipe = -1;
    uint8_t fVer = 0;

    uint32_t rd(uint32_t off) const { return *(volatile uint32_t *)(fRegs + off); }
    bool adoptFirmwareMode();

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    IOReturn enableController() override { return kIOReturnSuccess; }
    bool isConsoleDevice() override { return true; }
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
    IOItemCount getDisplayModeCount() override { return 1; }
    IOReturn getDisplayModes(IODisplayModeID *modes) override { modes[0] = 1; return kIOReturnSuccess; }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { *mode = 1; *depth = 0; return kIOReturnSuccess; }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { return getCurrentDisplayMode(mode, depth); }
    // WindowServer showed the panel at 0.00 Hz without this (studio virtual panel, 10-10)
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
        if (mode != 1 || !info) return kIOReturnUnsupportedMode;
        bzero(info, sizeof(*info));
        info->appleTimingID = kIOTimingIDInvalid;
        if (!fPclk || fHT <= fWidth || fVT <= fHeight) return kIOReturnSuccess;
        IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
        d.pixelClock = d.minPixelClock = d.maxPixelClock = fPclk;
        d.horizontalActive = fWidth; d.horizontalBlanking = fHT - fWidth;
        d.horizontalSyncOffset = fHSS > fWidth ? fHSS - fWidth : 0; d.horizontalSyncPulseWidth = fHSE > fHSS ? fHSE - fHSS : 0;
        d.verticalActive = fHeight; d.verticalBlanking = fVT - fHeight;
        d.verticalSyncOffset = fVSS > fHeight ? fVSS - fHeight : 0; d.verticalSyncPulseWidth = fVSE > fVSS ? fVSE - fVSS : 0;
        info->flags = kIODetailedTimingValid;
        return kIOReturnSuccess;
    }
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override { return (mode == 1 && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupported; }
    IOItemCount getConnectionCount() override { return 1; }
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override;
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **iref) override {
        if (type == kIOFBVBLInterruptType) { fVbl = { proc, target, ref }; if (iref) *iref = &fVbl; return kIOReturnSuccess; }
        if (type == kIOFBConnectInterruptType) { fConnect = { proc, target, ref }; if (iref) *iref = &fConnect; return kIOReturnSuccess; }
        return kIOReturnUnsupported;
    }
    IOReturn unregisterInterrupt(void *) override { return kIOReturnSuccess; }
    IOReturn setInterruptState(void *, UInt32) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { return kIOReturnSuccess; }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    // Power and connection handling copied from NVRMFB, which shares this controller (same IOFBDependentID). Left to
    // IOFramebuffer's base class, a power change queues on the shared controller thread and waits for an acknowledge
    // (IOFramebuffer::setPowerState returns a 45 s timeout, checkPowerWork -> setAttribute(kIOPowerStateAttribute));
    // NVRMFB acknowledges at once and answers the power attributes itself. Studio 10-10: with NMIntelFB under NVRM the
    // first WindowServer restart froze every display.
    IOReturn setPowerState(unsigned long ordinal, IOService *) override {
        ifb_step(ordinal ? "setPowerState on" : "setPowerState off");
        dumpDisplay(ordinal ? "power on" : "power off");
        if (fBacklight && !ordinal) fBacklight->setPowered(false);
        handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        if (fBacklight && ordinal) fBacklight->setPowered(true);
        ifb_step("setPowerState done");
        return kIOPMAckImplied;
    }
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
        if (attribute == kIOWindowServerActiveAttribute) return kIOReturnSuccess;
        if (attribute == kIOPowerAttribute) {
            if (fBacklight) fBacklight->setPowered(value != 0);   // display sleep / wake
            if (value) { IOReturn r = IOFramebuffer::setAttribute(attribute, value); handleEvent(kIOFBNotifyDidPowerOn);
                         return r == kIOReturnUnsupported ? kIOReturnSuccess : r; }
            handleEvent(kIOFBNotifyWillPowerOff);
            IOReturn r = IOFramebuffer::setAttribute(attribute, value);
            return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
        }
        return IOFramebuffer::setAttribute(attribute, value);
    }
    IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override {
        switch (attribute) {
        case kConnectionPower: case kConnectionColorModesSupported: case kConnectionColorDepthsSupported:
        case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported: case kConnectionColorMode:
        case kConnectionDisplayFlags: case kConnectionFlags:
            return kIOReturnSuccess;
        default: return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
        }
    }
    // sleep/wake diagnosis: what the display engine looks like going into sleep and coming out of it (read only)
    void dumpDisplay(const char *when) {
        if (!fRegs) return;
        static const struct { const char *n; uint32_t o; } r[] = {
            { "PP_STATUS", 0xC7200 }, { "PP_CONTROL", 0xC7204 }, { "TRANSCONF_A", 0x70008 }, { "PLANE_CTL_1_A", 0x70180 },
            { "PLANE_SURF_1_A", 0x7019C }, { "PIPE_SRC_A", 0x6001C }, { "DDI_BUF_CTL_A", 0x64000 },
            { "TRANS_DDI_FUNC_CTL_A", 0x60400 }, { "DPLL0_ENABLE", 0x46010 }, { "PWR_WELL_CTL2", 0x45404 },
            { "DC_STATE_EN", 0x45504 }, { "BLC_PWM_CTL", 0xC8250 }, { "BLC_PWM_FREQ", 0xC8254 }, { "BLC_PWM_DUTY", 0xC8258 },
        };
        for (auto &e : r) IFBLOG("regs[%s] %s=0x%08x", when, e.n, rd(e.o));
    }
    // AppleBacklightDisplay::probe takes only an LCD connection; answer as NVRMFB does when the backlight is ours
    IOReturn getAppleSense(IOIndex connectIndex, UInt32 *senseType, UInt32 *primary, UInt32 *extended, UInt32 *displayType) override {
        if (connectIndex != 0 || !fBacklight) return kIOReturnUnsupported;
        if (senseType) *senseType = 0;
        if (primary) *primary = 0;
        if (extended) *extended = 0;
        if (displayType) *displayType = kGenericLCD;
        return kIOReturnSuccess;
    }
    IOReturn connectFlags(IOIndex, IODisplayModeID, IOOptionBits *flags) override {
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
        return kIOReturnSuccess;
    }
    IODeviceMemory *getVRAMRange() override { return getApertureRange(kIOFBSystemAperture); }
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override {
        if (attribute == kIOWindowServerActiveAttribute) { if (value) *value = 1; return kIOReturnSuccess; }
        return IOFramebuffer::getAttribute(attribute, value);
    }
    IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override;
};

OSDefineMetaClassAndStructors(NMIntelFB, IOFramebuffer)
#ifdef NMINTELFB_VIRTUAL
static NMIntelFB *gVirt;
static int nmintelfb_sample_sysctl SYSCTL_HANDLER_ARGS
{
    char b[128] = "no panel";
    if (gVirt) gVirt->sampleInto(b, sizeof b);
    return sysctl_handle_string(oidp, b, sizeof b, req);
}
SYSCTL_PROC(_debug, OID_AUTO, nmintelfb_sample, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
            nmintelfb_sample_sysctl, "A", "64 pixels of the virtual panel: non-black and varied counts");
#endif

IOService *NMIntelFB::probe(IOService *provider, SInt32 *score)
{
#ifdef NMINTELFB_VIRTUAL
    if (!provider->getProperty("nm-intel-panel")) return nullptr;   // only the nub the loader published
    // test build: a virtual 1920x1080 "panel" in system memory, so the NVIDIA -> Intel frame path can be proven on a
    // machine with no Intel GPU (frames composited on the NVIDIA card must arrive in this buffer)
    fVer = 0xFF; return IOFramebuffer::probe(provider, score);
#endif
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return nullptr;
    if (PE_parse_boot_argn("-nmintelfboff", nullptr, 0)) { IFBLOG("off by boot-arg"); return nullptr; }
    uint16_t dev = pci->configRead16(kIOPCIConfigDeviceID);
    fVer = displayVersion(dev);
    if (!fVer) return nullptr;   // an Intel GPU this driver does not know (older ones have Apple's own driver)
    IFBLOG("8086:%04x display version %u", dev, fVer);
    ifb_step("probe: Intel display matched");
    return IOFramebuffer::probe(provider, score);
}

bool NMIntelFB::adoptFirmwareMode()
{
    // the panel is on whichever pipe the firmware used; take the first pipe whose transcoder and plane 1 are both on
    for (int p = 0; p < 4; p++) {
        uint32_t conf = rd(TRANSCONF(p)), ctl = rd(PLANE_CTL(p));
        if (!(conf & kEnable) || !(ctl & kEnable)) continue;
        if (fmtOf(ctl) != 8) { IFBLOG("pipe %c: plane format %u is not XRGB8888 - leaving the firmware screen", 'A' + p, fmtOf(ctl)); return false; }
        if (tileOf(ctl) != 0) { IFBLOG("pipe %c: plane is tiled (%u) - leaving the firmware screen", 'A' + p, tileOf(ctl)); return false; }
        uint32_t size = rd(PLANE_SIZE(p)), stride = rd(PLANE_STRIDE(p)) & 0x7FF;
        fWidth = (size & 0xFFFF) + 1; fHeight = ((size >> 16) & 0xFFFF) + 1;
        fRowBytes = stride * 64;                     // linear planes count the stride in 64-byte units
        fSurf = rd(PLANE_SURF(p)) & ~0xFFFu;         // GGTT offset of the surface, 4 KiB aligned
        if (fWidth < 320 || fHeight < 200 || fRowBytes < fWidth * 4) {
            IFBLOG("pipe %c: plane %ux%u stride %u is not a usable surface", 'A' + p, fWidth, fHeight, fRowBytes); return false;
        }
        if ((uint64_t)fSurf + (uint64_t)fRowBytes * fHeight > fApertureLen) {
            IFBLOG("pipe %c: surface at 0x%x is past the %llu MiB aperture", 'A' + p, fSurf, (unsigned long long)(fApertureLen >> 20)); return false;
        }
        // the transcoder that feeds pipe p is p on these display versions (eDP on transcoder A in every recording)
        fHT = (rd(HTOTAL(p)) >> 16) + 1; fVT = (rd(VTOTAL(p)) >> 16) + 1;
        fHSS = (rd(HSYNC(p)) & 0xFFFF) + 1; fHSE = (rd(HSYNC(p)) >> 16) + 1;
        fVSS = (rd(VSYNC(p)) & 0xFFFF) + 1; fVSE = (rd(VSYNC(p)) >> 16) + 1;
        // refresh: count vblanks for 250 ms (a 30-240 Hz panel gives 7-60 counts); fall back to 60 Hz if it does not move
        uint32_t f0 = rd(FRMCOUNT(p)); uint64_t t0 = mach_absolute_time();
        IOSleep(250);
        uint32_t f1 = rd(FRMCOUNT(p)); uint64_t t1 = mach_absolute_time(), ns = 0;
        absolutetime_to_nanoseconds(t1 - t0, &ns);
        uint32_t frames = f1 - f0;
        if (frames >= 5 && frames <= 100 && ns) {
            fFrameNs = ns / frames;
            uint64_t mhz = (uint64_t)frames * 1000000000000ull / ns;   // refresh in milli-Hz
            fRefresh16 = (uint32_t)((mhz << 16) / 1000);
            fPclk = (uint64_t)fHT * fVT * mhz / 1000;
        } else fPclk = (uint64_t)fHT * fVT * 60;
        fPipe = p;
        IFBLOG("pipe %c: %ux%u stride %u surface 0x%x totals %ux%u, %u vblanks in %llu ms -> %u.%02u Hz, pclk %llu", 'A' + p,
               fWidth, fHeight, fRowBytes, fSurf, fHT, fVT, frames, (unsigned long long)(ns / 1000000),
               fRefresh16 >> 16, ((fRefresh16 & 0xFFFF) * 100) >> 16, (unsigned long long)fPclk);
        return true;
    }
    IFBLOG("no pipe is scanning out - the firmware left the panel off; leaving it to the firmware framebuffer");
    return false;
}

bool NMIntelFB::start(IOService *provider)
{
#ifdef NMINTELFB_VIRTUAL
    fWidth = 1920; fHeight = 1080; fRowBytes = 7680; fSurf = 0; fPipe = 0;
    fHT = 2200; fVT = 1125; fHSS = 2008; fHSE = 2052; fVSS = 1084; fVSE = 1089; fPclk = 148500000ull;   // CEA 1080p60
    fVBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                             (mach_vm_size_t)fRowBytes * fHeight, 0xFFFFFFFFFFFFF000ull);
    if (!fVBuf || fVBuf->prepare() != kIOReturnSuccess) { IFBLOG("virtual: no contiguous buffer"); OSSafeReleaseNULL(fVBuf); return false; }
    bzero(fVBuf->getBytesNoCopy(), fVBuf->getLength());
    fApertureBase = fVBuf->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone); fApertureLen = fVBuf->getLength();
    if (!IOFramebuffer::start(provider)) { fVBuf->complete(); OSSafeReleaseNULL(fVBuf); return false; }
    fSurfMap = fVBuf->map();
    setProperty("IOFBDependentID", 0x4E56524D00000001ull, 64);
    setProperty("IOFBDependentIndex", 4ull, 32);
    if (IOWorkLoop *wl = getWorkLoop()) {
        fTimer = IOTimerEventSource::timerEventSource(this, [](OSObject *o, IOTimerEventSource *t) {
            ((NMIntelFB *)o)->sample(); t->setTimeoutMS(2000); });
        if (fTimer && wl->addEventSource(fTimer) == kIOReturnSuccess) fTimer->setTimeoutMS(2000);
    }
    armVbl();
    registerPowerDriver(this, kPowerStates, 2);
    gVirt = this; sysctl_register_oid(&sysctl__debug_nmintelfb_sample);
    IFBLOG("virtual panel published (1920x1080 at phys 0x%llx)", (unsigned long long)fApertureBase);
    return true;
#endif
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) return false;
    fPCI->setMemoryEnable(true);
    fMMIO = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IODeviceMemory *ap = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!fMMIO || !ap) { IFBLOG("BAR0 or BAR2 is not mapped - leaving the firmware screen"); OSSafeReleaseNULL(fMMIO); return false; }
    fRegs = (volatile uint8_t *)fMMIO->getVirtualAddress();
    fApertureBase = ap->getPhysicalAddress(); fApertureLen = ap->getLength();
    if (!adoptFirmwareMode()) { ifb_step("start: firmware mode not usable, declined"); OSSafeReleaseNULL(fMMIO); fRegs = nullptr; return false; }
    publishBacklight();   // before IOFramebuffer::start(): IOFramebuffer::open looks for "backlight"
    ifb_step("start: firmware mode adopted, IOFramebuffer::start");
    if (!IOFramebuffer::start(provider)) { ifb_step("start: IOFramebuffer::start FAILED"); OSSafeReleaseNULL(fMMIO); fRegs = nullptr; return false; }
    ifb_step("start: IOFramebuffer::start returned");
    // Head 4 of the NVIDIA accelerator (NVAccel): WindowServer composites this panel on the NVIDIA card and the
    // display pipe copies each frame into fSurfMap. Without NVIDIA the second (write-combined) mapping of the scan-out
    // is not needed, and on the Xiaomi Tiger Lake laptop the boot froze right here (10-15): skip it.
    ifb_step("start: looking for an NVIDIA driver");
    bool joinNv = ifb_join_nvidia_group();
    ifb_step(joinNv ? "start: NVIDIA driver found" : "start: no NVIDIA driver");
    if (joinNv) {
        ifb_step("start: mapping the scan-out write-combined");
        if (IODeviceMemory *sm = IODeviceMemory::withRange(fApertureBase + fSurf, (IOPhysicalLength)fRowBytes * fHeight)) {
            fSurfMap = sm->map(kIOMapWriteCombineCache); sm->release();
        }
        ifb_step("start: scan-out mapped");
    }
    if (fSurfMap && joinNv) {
        setProperty("IOFBDependentID", 0x4E56524D00000001ull, 64);
        setProperty("IOFBDependentIndex", 4ull, 32);
        IFBLOG("joined the NVIDIA display group (head 4)");
    } else if (joinNv) IFBLOG("the surface would not map write-combined: NVIDIA rendering stays off for this panel");
    else IFBLOG("no NVIDIA card: standalone panel (no IOFBDependentID, no second mapping)");
    setProperty("NMIntelFBPipe", fPipe, 32);
    setProperty("NMIntelFBDisplayVersion", fVer, 32);
    setProperty("built-in", kOSBooleanTrue);   // the panel is the laptop's own (kConnectionFlags says so too)
    ifb_step("start: arming the 60 Hz vblank timer");
    armVbl();
    // registerPowerDriver() inside start() never returned on the Xiaomi Tiger Lake laptop (Sequoia, 10-15): the start
    // never finished, so IOGraphics never resumed the console or handed the panel to WindowServer. Register from a
    // thread call 2 s after start returns instead; boot-arg -nmintelfbnopm skips the registration altogether.
    if (PE_parse_boot_argn("-nmintelfbnopm", nullptr, 0)) {
        ifb_step("start: power registration skipped (-nmintelfbnopm)");
    } else if ((fPmCall = thread_call_allocate(pmRegister, this)) != nullptr) {
        uint64_t d; clock_interval_to_deadline(2000, kMillisecondScale, &d);
        thread_call_enter_delayed(fPmCall, d);
    } else {
        ifb_step("start: no thread call for power registration, skipped");
    }
    IFBLOG("built-in panel published (%ux%u)", fWidth, fHeight);
    ifb_step(joinNv ? "start: published (NVIDIA group)" : "start: published (standalone)");
    return true;
}

void NMIntelFB::publishBacklight()
{
    if (PE_parse_boot_argn("-nmintelfbnobl", nullptr, 0)) { IFBLOG("backlight: off by boot-arg"); return; }
    uint32_t ctl = rd(kBlcCtl), freq = rd(kBlcFreq), duty = rd(kBlcDuty);
    IFBLOG("backlight: BLC_PWM_CTL 0x%08x FREQ 0x%08x DUTY 0x%08x", ctl, freq, duty);
    if (!(ctl & kEnable) || freq == 0 || duty > freq) {
        bl_step("backlight: PWM not enabled by the firmware, no brightness control");
        return;
    }
    SInt32 level = (SInt32)(((uint64_t)duty * 100 + freq / 2) / freq);
    SInt32 saved = NMIntelBacklight::savedLevel();
    if (saved >= 0) { IFBLOG("backlight: restoring saved brightness %d%% (firmware left %d%%)", (int)saved, (int)level); level = saved; }
    fBacklight = NMIntelBacklight::with(fRegs, freq, level);
    if (!fBacklight) { bl_step("backlight: handler allocation failed"); return; }
    if (saved >= 0) fBacklight->applyNow(saved);
    fBacklight->setName("backlight");
    if (!fBacklight->attach(this)) { OSSafeReleaseNULL(fBacklight); bl_step("backlight: attach failed"); return; }
    bl_step("backlight: registering the handler (async)");
    fBacklight->registerService();
    bl_step("backlight: handler published");
}

void NMIntelFB::stop(IOService *provider)
{
    fStopping = true;
#ifdef NMINTELFB_VIRTUAL
    if (gVirt == this) { sysctl_unregister_oid(&sysctl__debug_nmintelfb_sample); gVirt = nullptr; }
#endif
    // thread_call_cancel_wait is not exported to kexts: cancel and retry the free until the callback is out (as NVRMFB)
    if (fBacklight) { fBacklight->detachRegs(); fBacklight->detach(this); OSSafeReleaseNULL(fBacklight); }
    if (fPmCall) { thread_call_cancel(fPmCall); while (!thread_call_free(fPmCall)) { thread_call_cancel(fPmCall); IOSleep(5); } fPmCall = nullptr; }
    if (fVblTimer) { thread_call_cancel(fVblTimer); while (!thread_call_free(fVblTimer)) { thread_call_cancel(fVblTimer); IOSleep(5); } fVblTimer = nullptr; }
    OSSafeReleaseNULL(fSurfMap); OSSafeReleaseNULL(fMMIO); fRegs = nullptr;
    IOFramebuffer::stop(provider);
}

IODeviceMemory *NMIntelFB::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture) return nullptr;
    return IODeviceMemory::withRange(fApertureBase + fSurf, (IOPhysicalLength)fRowBytes * fHeight);
}

IOReturn NMIntelFB::getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info)
{
    if (mode != 1 || !info) return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->maxDepthIndex = 0; info->nominalWidth = fWidth; info->nominalHeight = fHeight;
    info->refreshRate = fRefresh16; info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}

IOReturn NMIntelFB::getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi)
{
    if (mode != 1 || depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
    bzero(pi, sizeof(*pi));
    pi->bytesPerRow = fRowBytes; pi->bytesPerPlane = 0; pi->bitsPerPixel = 32; pi->pixelType = kIORGBDirectPixels;
    pi->componentCount = 3; pi->bitsPerComponent = 8;
    pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;
    strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
    pi->activeWidth = fWidth; pi->activeHeight = fHeight;
    return kIOReturnSuccess;
}

IOReturn NMIntelFB::getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value)
{
    if (idx != 0 || !value) return kIOReturnBadArgument;
    switch (attr) {
    case kConnectionEnable: *value = 1; return kIOReturnSuccess;
    case kConnectionCheckEnable: *value = 1; return kIOReturnSuccess;
    case kConnectionFlags: *value = kIOConnectionBuiltIn; return kIOReturnSuccess;
    // the colour answers NVRMFB gives (RGB, 8 bits per component), which WindowServer asks before compositing
    case kConnectionColorModesSupported: case kConnectionColorMode: *value = 0x00000001; return kIOReturnSuccess;
    case kConnectionColorDepthsSupported: case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported:
        *value = 0x00000002; return kIOReturnSuccess;
    default: return IOFramebuffer::getAttributeForConnection(idx, attr, value);
    }
}

// NVAccel's display pipe asks every head for its scan-out (NVRM_VRAM_FN_SCANOUT). This head answers with a CPU mapping and
// no VRAM address: phys 0 keeps NVAccel from treating Intel memory as NVIDIA VRAM (it is not in the NVIDIA card's BAR1),
// so frames reach the panel only through the copy into kva.
IOReturn NMIntelFB::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && fn->isEqualTo(NVRM_VRAM_FN_SCANOUT)) {
        NVRMVramRequest *r = (NVRMVramRequest *)p1;
        if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
        if (!fSurfMap) return kIOReturnNotReady;
        r->kva = (void *)fSurfMap->getVirtualAddress();
        r->phys = 0; r->actualSize = 0;
        r->width = fWidth; r->height = fHeight; r->pitch = fRowBytes;
        return kIOReturnSuccess;
    }
    return IOFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

#ifdef NMINTELFB_VIRTUAL
// Test loader: attaches a virtual panel under the NVIDIA PCI device on demand, the way the real driver will place the
// Intel panel so WindowServer composites it on the NVIDIA accelerator.
class NMIntelFBLoader : public IOService {
    OSDeclareDefaultStructors(NMIntelFBLoader)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};
OSDefineMetaClassAndStructors(NMIntelFBLoader, IOService)
static int gAttach = 0;
static int nmintelfb_attach_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gAttach, err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr || v != 1 || gAttach == 1) return err;
    // A spare NVRMDisplay nub under NVRM (NVRM's own class), marked as the Intel panel: NMIntelFB claims it ahead of
    // NVRMFB, so the panel sits where NVRMFB does (GPU -> NVRM -> NVRMDisplay -> framebuffer). Starting a framebuffer
    // directly on the NVIDIA PCI device hung the kernel (studio 10-10).
    IOService *nub0 = nullptr, *nvrm = nullptr;
    if (OSDictionary *m = IOService::serviceMatching("NVRMDisplay")) {
        if (OSIterator *it = IOService::getMatchingServices(m)) {
            while (OSObject *o = it->getNextObject()) {
                IOService *n = OSDynamicCast(IOService, o);
                OSNumber *fi = n ? OSDynamicCast(OSNumber, n->getProperty("fb-index")) : nullptr;
                if (fi && fi->unsigned32BitValue() == 0) { nub0 = n; nub0->retain(); break; }
            }
            it->release();
        }
        m->release();
    }
    if (nub0) { nvrm = nub0->getProvider(); if (nvrm) nvrm->retain(); }
    if (!nub0 || !nvrm) { OSSafeReleaseNULL(nub0); OSSafeReleaseNULL(nvrm); IFBLOG("attach: no NVRMDisplay 0 / NVRM"); return ENODEV; }
    IOService *nub = OSDynamicCast(IOService, OSMetaClass::allocClassWithName("NVRMDisplay"));
    if (!nub || !nub->init()) { OSSafeReleaseNULL(nub); nub0->release(); nvrm->release(); return ENOMEM; }
    static const char *const kCopy[] = {"nvkms-kapi", "gpu-id", "phys-for-va"};
    for (const char *k : kCopy) if (OSObject *v = nub0->getProperty(k)) nub->setProperty(k, v);
    nub->setProperty("fb-index", 4ull, 32);
    nub->setProperty("nm-intel-panel", kOSBooleanTrue);
    nub0->release();
    if (!nub->attach(nvrm)) { nub->release(); nvrm->release(); return EIO; }
    nvrm->release();
    nub->registerService();
    nub->release();
    gAttach = 1;
    IFBLOG("attach: Intel-panel NVRMDisplay nub published under NVRM");
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nmintelfb_attach, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, nullptr, 0,
            nmintelfb_attach_sysctl, "I", "1 = attach the virtual panel under the NVIDIA display device (test build)");
bool NMIntelFBLoader::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    sysctl_register_oid(&sysctl__debug_nmintelfb_attach);
    IFBLOG("loader idle until debug.nmintelfb_attach=1");
    return true;
}
void NMIntelFBLoader::stop(IOService *provider)
{
    sysctl_unregister_oid(&sysctl__debug_nmintelfb_attach);
    IOService::stop(provider);
}
#endif
