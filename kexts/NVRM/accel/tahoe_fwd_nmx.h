// b185: macOS 26 (Tahoe) forwards for the Intel MODERN display classes (NMXSurfaceM : IOAccelSurface,
// NMXDisplayPipeM : IOAccelDisplayPipe, NMXDisplayMachineM : IOAccelDisplayMachine), written by hand from tahoe_fwd.h.
// Only IOAccelSurface's 4 private slots apply (the others tahoe_fwd.h forwards are declared by the legacy classes).
// Tahoe made these IOAcceleratorFamily2 methods private. They are the same slots tahoe_fwd.h forwards for the legacy
// NV classes (IOAccelLegacySurface : IOAccelSurface etc. only append slots, so the offsets are the same); here each jumps
// through the MODERN base class's exported vtable to Apple's implementation. If Tahoe reports more unresolved symbols
// for these classes (kmutil print-diagnostics), they get added here the same way.
#pragma once
#if defined(NM_TAHOE) && defined(NMX_ACCEL)
#define NMX_TAHOE_FWD_NMXSurfaceM \
    void set_shape(eIOAccelSurfaceShapeBits a0, unsigned int a1, IOAccelDeviceRegion* a2, unsigned long long a3) override; \
    void set_shape_backing_length_ext(eIOAccelSurfaceShapeBits a0, unsigned int a1, unsigned long long a2, unsigned int a3, unsigned long long a4, IOAccelDeviceRegion* a5, unsigned long long a6) override; \
    void surface_lock_options(eLockType a0, unsigned int a1, IOAccelSurfaceInformation* a2, unsigned long long a3) override; \
    void surface_unlock_options(eLockType a0, unsigned int a1) override;
/* setStereo, submitCopyForward and submitUpdate are LEGACY-class slots (IOAccelLegacy*): the modern classes do not have them */
#define NMX_TAHOE_FWD_NMXDisplayPipeM
#define NMX_TAHOE_FWD_NMXDisplayMachineM
#define NMX_TAHOE_FWD_DEFS \
__attribute__((naked)) void NMXSurfaceM::set_shape(eIOAccelSurfaceShapeBits a0, unsigned int a1, IOAccelDeviceRegion* a2, unsigned long long a3) { __asm__("movq __ZTV14IOAccelSurface@GOTPCREL(%rip), %r11\n\tjmpq *2592(%r11)"); } \
__attribute__((naked)) void NMXSurfaceM::set_shape_backing_length_ext(eIOAccelSurfaceShapeBits a0, unsigned int a1, unsigned long long a2, unsigned int a3, unsigned long long a4, IOAccelDeviceRegion* a5, unsigned long long a6) { __asm__("movq __ZTV14IOAccelSurface@GOTPCREL(%rip), %r11\n\tjmpq *2600(%r11)"); } \
__attribute__((naked)) void NMXSurfaceM::surface_lock_options(eLockType a0, unsigned int a1, IOAccelSurfaceInformation* a2, unsigned long long a3) { __asm__("movq __ZTV14IOAccelSurface@GOTPCREL(%rip), %r11\n\tjmpq *2608(%r11)"); } \
__attribute__((naked)) void NMXSurfaceM::surface_unlock_options(eLockType a0, unsigned int a1) { __asm__("movq __ZTV14IOAccelSurface@GOTPCREL(%rip), %r11\n\tjmpq *2616(%r11)"); }
#else
#define NMX_TAHOE_FWD_NMXSurfaceM
#define NMX_TAHOE_FWD_NMXDisplayPipeM
#define NMX_TAHOE_FWD_NMXDisplayMachineM
#define NMX_TAHOE_FWD_DEFS
#endif
