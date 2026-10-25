#!/usr/bin/env python3
# libdrm-darwin-etime.py xf86drm.c - NMXGPU's kernel side is FreeBSD's LinuxKPI, which spells Linux's ETIME as ETIMEDOUT
# (60). Mesa on macOS compares errno with ETIME (101), so a GPU wait that simply had not finished yet came back as
# "DRM_IOCTL_SYNCOBJ_WAIT failed" / VK_ERROR_UNKNOWN and the Metal plugin gave up (10-17). drmIoctl hands Mesa ETIME.
import re, sys
p = sys.argv[1]
s = open(p).read()
if 'NMX_ETIME' in s:
    print('libdrm: ETIME patch already applied'); sys.exit(0)
pat = re.compile(r'(drmIoctl\(int fd, unsigned long request, void \*arg\)\s*\{.*?while \(ret == -1 && \(errno == EINTR \|\| errno == EAGAIN\)\);\n)', re.S)
m = pat.search(s)
if not m:
    print('libdrm: drmIoctl loop not found'); sys.exit(1)
add = ('#if defined(__APPLE__) && defined(ETIME) /* NMX_ETIME */\n'
       '    if (ret == -1 && errno == ETIMEDOUT)\n'
       '        errno = ETIME;\n'
       '#endif\n')
s = s[:m.end()] + add + s[m.end():]
print('libdrm: drmIoctl turns ETIMEDOUT into ETIME for Mesa')

# Mesa builds absolute GPU-wait deadlines on CLOCK_MONOTONIC (mach_continuous_time on macOS); NMXGPU's kernel clock is
# mach_absolute_time (CLOCK_UPTIME_RAW). When the two differ, a "+200 ms" deadline was already in the kernel's past and
# every wait returned at once (10-17: 60 after 0.0 ms) - Mesa then spun the CPU. Re-base each deadline onto the kernel clock.
helper = '''
#if defined(__APPLE__) /* NMX_ABSCLK */
#include <time.h>
static int64_t nmx_abs_to_kernel(int64_t abs_ns)
{
    if (abs_ns <= 0 || abs_ns == INT64_MAX)
        return abs_ns;                       /* 0 = poll, negative/INT64_MAX = wait forever: clock-free */
    const uint64_t mono = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    const uint64_t up = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const int64_t rem = abs_ns - (int64_t)mono;
    if (rem <= 0)
        return 1;                            /* already due: an expired absolute deadline, the kernel polls once */
    if ((uint64_t)rem > (uint64_t)INT64_MAX - up)
        return INT64_MAX;
    return (int64_t)(up + (uint64_t)rem);
}
#else
#define nmx_abs_to_kernel(x) (x)
#endif
'''
anchor = s.find('drm_public int\ndrmIoctl(')
if anchor < 0:
    print('libdrm: drmIoctl definition not found for the clock helper'); sys.exit(1)
s = s[:anchor] + helper + '\n' + s[anchor:]
n = s.count('args.timeout_nsec = timeout_nsec;')
if n < 1:
    print('libdrm: no syncobj timeout assignment found'); sys.exit(1)
s = s.replace('args.timeout_nsec = timeout_nsec;', 'args.timeout_nsec = nmx_abs_to_kernel(timeout_nsec);')
open(p, 'w').write(s)
print('libdrm: %d syncobj wait deadline(s) re-based from CLOCK_MONOTONIC onto the kernel uptime clock' % n)
