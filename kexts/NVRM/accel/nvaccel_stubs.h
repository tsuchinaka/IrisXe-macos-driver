/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once
#include "IOAccelResource2.h"
#include "IOAccelSysMemory.h"
#include "IOAccelVidMemory.h"
#include "IOAccelLegacySurface.h"
#include "IOAccelLegacyDisplayPipe.h"
#include "IOAccelSurface.h"
#include "IOAccelDisplayPipe.h"
#include "IOAccelGLContext2.h"
#include "IOAccel2DContext2.h"
#include "IOAccelCLContext2.h"
#include "IOAccelVideoContext2.h"
class NVResource : public IOAccelResource2 {
    OSDeclareDefaultStructors(NVResource)
    NM_TAHOE_FWD(NVResource)
public:
    unsigned int fPbLogged = 0;
    void rebuildPagingBuffer() APPLE_KEXT_OVERRIDE { if (fPbLogged < 4) { fPbLogged++; ALOG("NVResource::rebuildPagingBuffer -- nothing to rebuild until there are page tables (rung 5)"); } }
    uint64_t getLevelOffset(unsigned char a0, unsigned char a1, int* a2) APPLE_KEXT_OVERRIDE {
        const unsigned char *r = (const unsigned char *)this;
        if (a2) {
            unsigned long long pitch = *(const volatile unsigned long long *)(r + 0xb8);
            unsigned base = r[0xae];
            *a2 = (int)(pitch >> ((a1 >= base) ? (unsigned)(a1 - base) : 0u));
        }
        return (unsigned long long)a0 * *(const volatile unsigned long long *)(r + 0xd0);
    }
    uint64_t getBackingLevelOffset(unsigned char a0, unsigned char a1, int* a2) APPLE_KEXT_OVERRIDE {
        const unsigned char *r = (const unsigned char *)this;
        if (a2) {
            unsigned long long pitch = *(const volatile unsigned long long *)(r + 0xc0);
            unsigned base = r[0xae];
            *a2 = (int)(pitch >> ((a1 >= base) ? (unsigned)(a1 - base) : 0u));
        }
        return (unsigned long long)a0 * *(const volatile unsigned long long *)(r + 0xd0);
    }
    uint64_t calculateIOSurfaceDeviceCacheVRAMBytes(unsigned long long* a0, unsigned long long* a1) APPLE_KEXT_OVERRIDE {
        const unsigned char *r = (const unsigned char *)this;
        unsigned long long size = *(const volatile unsigned long long *)(r + 0xd0);
        if (a1) *a1 = 1;
        if (a0) *a0 = size;
        return size;
    }
    bool addToAperture() APPLE_KEXT_OVERRIDE;
    void removeFromAperture() APPLE_KEXT_OVERRIDE;
    void *getApertureMemoryDescriptor(unsigned long long *a0) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    void pageoff(IOAccelEvent *, bool, bool *, unsigned long long) APPLE_KEXT_OVERRIDE;
    unsigned int fApLogged = 0;
    IOMemoryDescriptor *fApDesc = nullptr;
};
OSDefineMetaClassAndStructors(NVResource, IOAccelResource2)
class NVSysMemory : public IOAccelSysMemory {
    OSDeclareDefaultStructors(NVSysMemory)
    NM_TAHOE_FWD(NVSysMemory)
public:
};
OSDefineMetaClassAndStructors(NVSysMemory, IOAccelSysMemory)
class NVVidMemory : public IOAccelVidMemory {
    OSDeclareDefaultStructors(NVVidMemory)
    NM_TAHOE_FWD(NVVidMemory)
public:
    void              *fRmHandle = nullptr;
    unsigned long long fAllocationCookie = 0;
    void              *fKva      = nullptr;
    unsigned long long fPhys     = 0;
    unsigned long long fBytes    = 0;
    bool fScanout = false;
    // b90: memory the framebuffer handed out as an IOMemoryDescriptor (Intel: ordinary, cached, possibly scattered RAM -
    // there is no VRAM; NVRM_VRAM_FLAG_MD). nullptr for NVIDIA VRAM and the scan-out, which are one physical run.
    IOMemoryDescriptor *fMd = nullptr;

    bool allocPhysical() APPLE_KEXT_OVERRIDE;
    void deallocPhysical() APPLE_KEXT_OVERRIDE;

    uint64_t getPhysicalSegment(unsigned long long a0, unsigned long long* a1) APPLE_KEXT_OVERRIDE {
        const unsigned long long total = *(const volatile unsigned long long *)((const unsigned char *)this + 0x40);
        if (fMd) {
            if (a0 >= total || a0 >= fMd->getLength()) { if (a1) *a1 = 0; return 0; }
            IOByteCount len = 0;
            const addr64_t p = fMd->getPhysicalSegment((IOByteCount)a0, &len, kIOMemoryMapperNone);
            if (a1) *a1 = len;
            return p;
        }
        if (!fPhys || a0 >= total) { if (a1) *a1 = 0; return 0; }
        if (a1) *a1 = total - a0;
        return fPhys + a0;
    }
};
OSDefineMetaClassAndStructors(NVVidMemory, IOAccelVidMemory)
static inline void nv_surface_swap(IOAccelResource2* a1, IOAccelResource2* a2)
{
        nvAccelSwapProbe(a1, a2);
#ifdef NMX_ACCEL
        if (nmxSwapPresent(a1, a2)) return;   // b93: a2 -> RAM front buffer a1 -> panel (software cursor lives in a1)
#endif
#ifdef NMX_ACCEL
        const unsigned long long t0 = mach_absolute_time();
#endif
#ifdef NMX_ACCEL
        {   /* b113: show a2, then keep every other back buffer equal to the panel (lock screen black boxes) */
            const int r = nmxSwapSync(a1, a2);
            if (r >= 0) { nmxSwapStat(t0, mach_absolute_time(), r > 0); return; }
        }
        nmxSwapNoteA1(a1);
        if (nmxSwapIsStale(a2)) { nmxSwapStat(t0, mach_absolute_time(), false); return; }   /* b109-b111: the unused 2nd swap */
#endif
        IOAccelResource2 *src = nvAccelResHasContent(a2) ? a2
                              : nvAccelResHasContent(a1) ? a1 : nullptr;
        bool copied = false;
        if (src && !nvAccelFlipTo(src)) copied = nvAccelCopyToScanout(src);
#ifdef NMX_ACCEL
        if (copied) { nmxNoteFront(src); nmxSwapKeepFront(a1, src); }   /* b107/b108: copy forward's source stays current */
        nmxSwapStat(t0, mach_absolute_time(), copied);
#else
        (void)copied;
#endif
    }
static inline void nv_surface_shape(IOAccelSurface *self, unsigned int a0, unsigned short a1, unsigned short a2)
{
        const unsigned w = a1, h = a2;
        uint32_t smode = *(const volatile uint32_t *)((const uint8_t *)self + 0x11B0);
        unsigned bpp = ((smode >> 16) & 0xf) + 1;
        if (bpp < 4 || bpp > 16) bpp = 4;
        unsigned pitch = (unsigned)((w * bpp + 255u) & ~255u);
        unsigned size  = pitch * h;
        IOAccelResource2 **bufs = (IOAccelResource2 **)((uint8_t *)self + 0x11C0);
        unsigned present = 0, shaped = 0;
        for (unsigned i = 0; i < 32; i++) {
            if (!(a0 & (1u << i))) continue;
            IOAccelResource2 *r = OSDynamicCast(IOAccelResource2, bufs[i]);
            if (!r) continue;
            present++;
            if (r->initSurfaceBuffer(self, w, h, size, pitch, 0)) shaped++;
        }
        static SInt32 sShapeLogs;   /* b97: every frame calls this - 3 kernel log lines a frame cost CPU; first 8 only */
        if (OSIncrementAtomic(&sShapeLogs) < 8)
            ALOG("NVSurface::shapeSurface(mode 0x%x, %u x %u) smode 0x%x bpp %u pitch %u size %u -> %u present, %u shaped",
                 a0, w, h, smode, bpp, pitch, size, present, shaped);
        static int gShapeDumps = 0;
        if (gShapeDumps < 4) { gShapeDumps++;
            const volatile unsigned long long *qq = (const volatile unsigned long long *)((const unsigned char *)self + 0x1160);
            for (unsigned o = 0; o < 0x60; o += 16)
                ALOG("   surf+0x%04x: %016llx %016llx", 0x1160 + o, qq[o / 8], qq[o / 8 + 1]);
        }
    }
class NVSurface : public IOAccelLegacySurface {
    OSDeclareDefaultStructors(NVSurface)
    NM_TAHOE_FWD(NVSurface)
public:
    void setSyncCommand(unsigned int* a0, unsigned int& a1, unsigned int& a2, _AMDSurfaceSwapSyncOptions const& a3) const APPLE_KEXT_OVERRIDE { static SInt32 n; if (OSIncrementAtomic(&n) < 5) ALOG("NVSurface::setSyncCommand (b97: logged 5 times only - it runs every frame)"); }
    void _pure335() APPLE_KEXT_OVERRIDE { ALOG("NVSurface::_pure335"); }
    bool copyFromBuffer(int a0, int a1, int a2, int a3, unsigned int a4, unsigned int a5, IOAccelEvent* a6, IOAccelResource2* a7, IOAccelSysMemory* a8, unsigned int a9, unsigned long long a10, unsigned int a11) APPLE_KEXT_OVERRIDE { ALOG("NVSurface::copyFromBuffer"); return false; }
    bool copyToBuffer(int a0, int a1, int a2, int a3, unsigned int a4, unsigned int a5, IOAccelEvent* a6, IOAccelResource2* a7, IOAccelSysMemory* a8, unsigned int a9, unsigned long long a10, unsigned int a11) APPLE_KEXT_OVERRIDE { ALOG("NVSurface::copyToBuffer"); return false; }
    void submitSwapCopy(IOAccelEvent* a0, IOAccelResource2* a1, IOAccelResource2* a2) APPLE_KEXT_OVERRIDE { (void)a0; nv_surface_swap(a1, a2); }
#if defined(NMX_ACCEL) && !defined(NM_TAHOE)
    /* b107 (Intel): CoreDisplay's copy forward - the unchanged pixels of the last frame into the next back buffer - was
       the family's default, kIOReturnUnsupported (dtrace 10-20: 242 of 242 calls in 8 s, selector 9 -> s_set_shape ->
       copy_forward -> submitCopyForward). The second back buffer then held only what was drawn into it (the rest of it
       zero): nvAccelResHasContent called it empty and every other frame never reached the panel ("frames 66, copied
       33"), and a frame that did pass could show garbage bands. Here: a CPU copy of the given rectangles, from the
       buffer last copied to the panel into the other one (both are cached system memory on Intel). */
    IOReturn submitCopyForward(IOAccelEvent* a0, unsigned int a1, IOAccelResource2* a2, IOAccelResource2* a3,
                               IOAccelBounds const* a4, unsigned int a5) APPLE_KEXT_OVERRIDE {
        (void)a0;
        return nmxCopyForward(a1, a2, a3, (const void *)a4, a5);
    }
#endif
    void shapeSurface(unsigned int a0, unsigned short a1, unsigned short a2) APPLE_KEXT_OVERRIDE { nv_surface_shape(this, a0, a1, a2); }
    uint32_t getDirtyBufferBitsFromPrivateModeBits(unsigned long long a0) APPLE_KEXT_OVERRIDE { static SInt32 n; if (OSIncrementAtomic(&n) < 5) ALOG("NVSurface::getDirtyBufferBitsFromPrivateModeBits(0x%llx)", (unsigned long long)a0); return 0; }
};
OSDefineMetaClassAndStructors(NVSurface, IOAccelLegacySurface)
/* b102: which display-pipe services the family asks of us. NVDisplayPipe never answered the VBL / transaction
   interrupt calls (the family's own versions ran), and frames reached the panel in bursts with 0.3-0.9 s gaps (10-20)
   while nothing was busy. These overrides only count and log, then do what the family did before. */
enum { kPipeVblOn, kPipeVblOff, kPipeTxOn, kPipeTxOff, kPipeBegin, kPipeSignal, kPipeSubmit, kPipeTimeout, kPipeKinds };
static volatile SInt32 gNmxPipeCnt[kPipeKinds];
static const char *const kNmxPipeName[kPipeKinds] = { "enableVBLInterrupt", "disableVBLInterrupt", "enableTransactionInterrupt",
    "disableTransactionInterrupt", "beginTransaction", "signalTransactionComplete", "submitTransaction",
    "logTransactionTimeoutDiagnosisReport" };
static inline void nmxPipeNote(int k, const void *pipe, const void *arg)
{
    const SInt32 n = OSIncrementAtomic(&gNmxPipeCnt[k]);
    if (n < 6 || (k == kPipeTimeout && n < 40)) ALOG("pipe %p: %s(%p) call #%d", pipe, kNmxPipeName[k], arg, (int)n + 1);
}
class NVDisplayPipe : public IOAccelLegacyDisplayPipe {
    OSDeclareDefaultStructors(NVDisplayPipe)
    NM_TAHOE_FWD(NVDisplayPipe)
public:
    void enableVBLInterrupt() APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeVblOn, this, nullptr); IOAccelLegacyDisplayPipe::enableVBLInterrupt(); }
    void disableVBLInterrupt() APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeVblOff, this, nullptr); IOAccelLegacyDisplayPipe::disableVBLInterrupt(); }
    void enableTransactionInterrupt() APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeTxOn, this, nullptr); IOAccelLegacyDisplayPipe::enableTransactionInterrupt(); }
    void disableTransactionInterrupt() APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeTxOff, this, nullptr); IOAccelLegacyDisplayPipe::disableTransactionInterrupt(); }
    void beginTransaction(IOAccelEvent *e) APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeBegin, this, e); IOAccelLegacyDisplayPipe::beginTransaction(e); }
    void signalTransactionComplete(IOAccelEvent *e) APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeSignal, this, e); IOAccelLegacyDisplayPipe::signalTransactionComplete(e); }
    void submitTransaction(IOAccelDisplayPipeTransaction2 *t) APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeSubmit, this, t); IOAccelLegacyDisplayPipe::submitTransaction(t); }
    void logTransactionTimeoutDiagnosisReport() APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeTimeout, this, nullptr); IOAccelLegacyDisplayPipe::logTransactionTimeoutDiagnosisReport(); }
    IOAccelMemory *initFramebufferResource(unsigned int index, IOAccelResource2 *res) APPLE_KEXT_OVERRIDE;
    IOReturn validateTransaction(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE;
    IOReturn performTransaction(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE;
    bool     isTransactionComplete(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE;
    void     free() APPLE_KEXT_OVERRIDE;
    enum { kIopSrcSlots = 4 };
    struct NVIopSrc { unsigned id; IOMemoryDescriptor *md; IOMemoryMap *map; } fIopSrc[kIopSrcSlots];
    unsigned fIopNext;
};
OSDefineMetaClassAndStructors(NVDisplayPipe, IOAccelLegacyDisplayPipe)
#if defined(NMX_ACCEL) && !defined(NM_TAHOE)
/* b116 (DESIGN.md R4): the MODERN family classes, as Apple's own Intel driver uses them (AppleIntelICLGraphics:
   IGAccelSurface : IOAccelSurface, IGAccelDisplayPipe : IOAccelDisplayPipe, IGAccelDisplayMachine : IOAccelDisplayMachine).
   NMXSurface / NMXDisplayPipe / NMXDisplayMachine are the LEGACY (NVIDIA-era) ones. Boot-arg nmxmodern=1 picks these. */
class NMXSurfaceM : public IOAccelSurface {
    OSDeclareDefaultStructors(NMXSurfaceM)
public:
    bool copyFromBuffer(int, int, int, int, unsigned int, unsigned int, IOAccelEvent*, IOAccelResource2*, IOAccelSysMemory*, unsigned int, unsigned long long, unsigned int) APPLE_KEXT_OVERRIDE { static SInt32 n; if (OSIncrementAtomic(&n) < 4) ALOG("NMXSurfaceM::copyFromBuffer (b116)"); return false; }
    bool copyToBuffer(int, int, int, int, unsigned int, unsigned int, IOAccelEvent*, IOAccelResource2*, IOAccelSysMemory*, unsigned int, unsigned long long, unsigned int) APPLE_KEXT_OVERRIDE { static SInt32 n; if (OSIncrementAtomic(&n) < 4) ALOG("NMXSurfaceM::copyToBuffer (b116)"); return false; }
    void submitSwapCopy(IOAccelEvent* a0, IOAccelResource2* a1, IOAccelResource2* a2) APPLE_KEXT_OVERRIDE {
        (void)a0; static SInt32 n; if (OSIncrementAtomic(&n) < 4) ALOG("NMXSurfaceM::submitSwapCopy (b116 modern surface)");
        nv_surface_swap(a1, a2);
    }
    void shapeSurface(unsigned int a0, unsigned short a1, unsigned short a2) APPLE_KEXT_OVERRIDE { nv_surface_shape(this, a0, a1, a2); }
    uint32_t getDirtyBufferBitsFromPrivateModeBits(unsigned long long) APPLE_KEXT_OVERRIDE { return 0; }
};
OSDefineMetaClassAndStructors(NMXSurfaceM, IOAccelSurface)
class NMXDisplayPipeM : public IOAccelDisplayPipe {
    OSDeclareDefaultStructors(NMXDisplayPipeM)
public:
    void enableVBLInterrupt() APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeVblOn, this, nullptr); IOAccelDisplayPipe::enableVBLInterrupt(); }
    void disableVBLInterrupt() APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeVblOff, this, nullptr); IOAccelDisplayPipe::disableVBLInterrupt(); }
    void enableTransactionInterrupt() APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeTxOn, this, nullptr); IOAccelDisplayPipe::enableTransactionInterrupt(); }
    void disableTransactionInterrupt() APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeTxOff, this, nullptr); IOAccelDisplayPipe::disableTransactionInterrupt(); }
    void beginTransaction(IOAccelEvent *e) APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeBegin, this, e); IOAccelDisplayPipe::beginTransaction(e); }
    void signalTransactionComplete(IOAccelEvent *e) APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeSignal, this, e); IOAccelDisplayPipe::signalTransactionComplete(e); }
    void submitTransaction(IOAccelDisplayPipeTransaction2 *t) APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeSubmit, this, t); IOAccelDisplayPipe::submitTransaction(t); }
    void logTransactionTimeoutDiagnosisReport() APPLE_KEXT_OVERRIDE { nmxPipeNote(kPipeTimeout, this, nullptr); IOAccelDisplayPipe::logTransactionTimeoutDiagnosisReport(); }
    IOAccelMemory *initFramebufferResource(unsigned int index, IOAccelResource2 *res) APPLE_KEXT_OVERRIDE;
    IOReturn validateTransaction(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE;
    IOReturn performTransaction(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE;
    bool     isTransactionComplete(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE;
    void     free() APPLE_KEXT_OVERRIDE;
    enum { kIopSrcSlots = 4 };
    struct NVIopSrc { unsigned id; IOMemoryDescriptor *md; IOMemoryMap *map; } fIopSrc[kIopSrcSlots];
    unsigned fIopNext;
};
OSDefineMetaClassAndStructors(NMXDisplayPipeM, IOAccelDisplayPipe)
#endif
class NVGLContext : public IOAccelGLContext2 {
    OSDeclareDefaultStructors(NVGLContext)
    NM_TAHOE_FWD(NVGLContext)
public:
    void _pure362() APPLE_KEXT_OVERRIDE { ALOG("NVGLContext::_pure362"); }
    void _pure363() APPLE_KEXT_OVERRIDE { ALOG("NVGLContext::_pure363"); }
    void _pure364() APPLE_KEXT_OVERRIDE { ALOG("NVGLContext::_pure364"); }
    void _pure367() APPLE_KEXT_OVERRIDE { ALOG("NVGLContext::_pure367"); }
};
OSDefineMetaClassAndStructors(NVGLContext, IOAccelGLContext2)
class NV2DContext : public IOAccel2DContext2 {
    OSDeclareDefaultStructors(NV2DContext)
    NM_TAHOE_FWD(NV2DContext)
public:
    void _pure362() APPLE_KEXT_OVERRIDE { ALOG("NV2DContext::_pure362"); }
    void _pure363() APPLE_KEXT_OVERRIDE { ALOG("NV2DContext::_pure363"); }
};
OSDefineMetaClassAndStructors(NV2DContext, IOAccel2DContext2)
class NVCLContext : public IOAccelCLContext2 {
    OSDeclareDefaultStructors(NVCLContext)
    NM_TAHOE_FWD(NVCLContext)
public:
};
OSDefineMetaClassAndStructors(NVCLContext, IOAccelCLContext2)
class NVVideoContext : public IOAccelVideoContext2 {
    OSDeclareDefaultStructors(NVVideoContext)
    NM_TAHOE_FWD(NVVideoContext)
public:
};
OSDefineMetaClassAndStructors(NVVideoContext, IOAccelVideoContext2)
