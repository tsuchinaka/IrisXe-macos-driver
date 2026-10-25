#!/bin/bash
# nmx-bos.sh - what the ~550 GPU memory objects WindowServer lists in every submission are. For 20 s it records every
# GPU memory allocation and free in WindowServer (kind, size, who asked) while you OPEN AND CLOSE a few apps/windows,
# and counts WindowServer's IOSurfaces (window buffers, which must stay separate). Read-only.
#   sudo ./nmx-bos.sh      Report: /Users/Shared/nmx-bos.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
O=/Users/Shared/nmx-bos.txt
W=$(pgrep -x WindowServer | head -1); [ -n "$W" ] || { echo "WindowServer not running"; exit 1; }
echo "for 20 s: OPEN and CLOSE a few apps and windows (Safari, Notes, System Settings), Mission Control once..."
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
  echo "--- WindowServer memory regions (IOSurface = window/layer buffers)"
  vmmap -summary "$W" 2>/dev/null | grep -E "IOSurface|IOAccelerator|IOKit|REGION TYPE|TOTAL" | head -12
  echo "--- GPU memory allocations in 20 s"
  dtrace -Z -q -x ustackframes=8 -p "$W" -n '
pid$target:libvulkan_intel.dylib:anv_AllocateMemory:entry
{ this->ai = (uint64_t *)copyin(arg1, 32); this->next = this->ai[1]; this->size = this->ai[2];
  this->st = this->next ? *(uint32_t *)copyin(this->next, 4) : 0;
  this->kind = this->st == 1000178000 ? "HOST-POINTER import" : this->st == 1000060000 ? "flags (device address)" :
               this->st == 1000127001 ? "dedicated (image/buffer)" : this->st == 0 ? "plain" : "other pNext";
  @k[this->kind] = count(); @sz[this->kind] = quantize(this->size); @bytes[this->kind] = sum(this->size);
  @who[this->kind, ustack(8)] = count(); }
pid$target:libvulkan_intel.dylib:anv_FreeMemory:entry { @free = count(); }
tick-20s {
  printf("allocations by kind (pNext sType 1000178000 = host pointer, 1000060000 = allocate-flags, 1000127001 = dedicated)\n"); printa("  %-28s %@d\n", @k);
  printf("frees: "); printa("%@d\n", @free);
  printf("\nbytes by kind\n"); printa("  %-28s %@d\n", @bytes);
  printf("\nsizes by kind\n"); printa("  %s%@d\n", @sz);
  trunc(@who, 20); printf("\ntop allocation sites\n"); printa("  [%s]%k\n   %@d\n\n", @who);
  exit(0); }' 2>&1 | grep -v "error on enabled probe"
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
