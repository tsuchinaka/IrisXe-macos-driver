#!/bin/bash
# nmx-execbuf.sh - the GPU submissions WindowServer makes while animating: how many per second, how many buffers each
# lists (every listed buffer is looked up, reserved and tracked by i915 = the kernel time in nmx-wsprof), and how long
# each submission takes in the kernel. Read-only, 15 s.   sudo ./nmx-execbuf.sh  then ANIMATE
#   Report: /Users/Shared/nmx-execbuf.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
O=/Users/Shared/nmx-execbuf.txt
echo "ANIMATE NOW for 15 s: Mission Control on/off, scroll, drag windows..."
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
dtrace -Z -q -n '
fbt:com.nullmoth.NMXGPU:i915_gem_execbuffer2_ioctl:entry
{ self->t = timestamp; this->n = *(uint32_t *)(arg1 + 8);
  @n[execname] = count(); @bos[execname] = quantize(this->n); @avg[execname] = avg(this->n); @mx[execname] = max(this->n); }
fbt:com.nullmoth.NMXGPU:i915_gem_execbuffer2_ioctl:return /self->t/
{ @us[execname] = quantize((timestamp - self->t) / 1000); @tot[execname] = sum((timestamp - self->t) / 1000); self->t = 0; }
fbt:com.nullmoth.NMXGPU:i915_gem_userptr_ioctl:entry { @up[execname] = count(); }
fbt:com.nullmoth.NMXGPU:i915_gem_create_ext_ioctl:entry, fbt:com.nullmoth.NMXGPU:i915_gem_create_ioctl:entry { @cr[execname] = count(); }
tick-15s {
  printf("--- submissions in 15 s\n"); printa("  %-20s %@d\n", @n);
  printf("\n--- buffers per submission: average / max\n"); printa("  %-20s %@d\n", @avg); printa("  %-20s max %@d\n", @mx);
  printf("\n--- buffers per submission (distribution)\n"); printa("  %s%@d\n", @bos);
  printf("\n--- kernel time per submission (us)\n"); printa("  %s%@d\n", @us);
  printf("\n--- total kernel time in submissions (us, of 15000000)\n"); printa("  %-20s %@d\n", @tot);
  printf("\n--- new host-pointer (userptr) buffers / new GPU buffers created in 15 s\n"); printa("  userptr %-20s %@d\n", @up); printa("  create  %-20s %@d\n", @cr);
  exit(0); }' 2>&1 | grep -v "error on enabled probe"
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
