#!/usr/bin/env python3
"""Second libdrm fix for the NullMoth nodes, applied after gpu-port/patches/libdrm-macos.patch.

The nodes in /dev/dri are clone nodes: stat() returns a fresh minor every time (0x110, 0x111, ...), so the minor
process_device() reads never names a DRM minor and drmNodeIsDRM() rejected every node: drmGetDevices2() found no
device and Mesa ANV reported no GPU (Iris Xe, 10-16). The patch's own comment says the minor is taken from the node
name; this puts that in: renderD128 -> 128, card0 -> 0."""
import re, sys
p = sys.argv[1]
s = open(p).read()
m = re.search(r'\nprocess_device\s*\([^;{]*\)\s*\{', s)   # libdrm writes 'static int' on the line above
if not m:
    sys.exit('process_device not found')
start = m.end()
mm = re.compile(r'min\s*=\s*minor\(\s*sbuf\.st_rdev\s*\)\s*;').search(s, start)
if not mm or mm.start() - start > 3000:
    sys.exit('minor() read in process_device not found')
k = mm.end()
inject = '''
#ifdef __APPLE__
    {   /* clone nodes: the DRM minor is the number in the node name */
        const char *dg = d_name + strlen(d_name);
        while (dg > d_name && dg[-1] >= '0' && dg[-1] <= '9')
            dg--;
        if (*dg)
            min = (unsigned int)strtoul(dg, NULL, 10);
    }
#endif'''
if 'clone nodes: the DRM minor is the number' not in s:
    s = s[:k] + inject + s[k:]
open(p, 'w').write(s)
print('libdrm: process_device takes the DRM minor from the node name')

# --- second fix: finding the drmDevice of an open fd ---------------------------------------------------------------
# drmGetDevice2(fd) and drmGet{Primary,Render}DeviceNameFromFd fstat() the fd and look for a node with the same
# st_rdev; a clone node's fd never has one, so Mesa's intel_get_device_info_from_fd() failed with "Failed to query
# drm device" (Iris Xe, 10-16). On Darwin the fd's device is found by its PCI identity (DRM_IOCTL_GET_PCIINFO on the
# fd) among the devices drmGetDevices2() lists.
s = open(p).read()
if 'drm_darwin_device_for_fd' not in s:
    proto = '''#ifdef __APPLE__
static int drm_darwin_device_for_fd(int fd, uint32_t flags, drmDevicePtr *device);
#endif
'''
    m = re.search(r'\nstatic char \*drmGetMinorNameForFD\(int fd, int type\)\s*\{', s)
    if not m:
        sys.exit('drmGetMinorNameForFD definition not found')
    body = '''
#ifdef __APPLE__
    {
        drmDevicePtr dd = NULL;
        char *name = NULL;
        if (drm_darwin_device_for_fd(fd, 0, &dd) == 0) {
            if (type >= 0 && type < DRM_NODE_MAX && (dd->available_nodes & (1 << type)))
                name = strdup(dd->nodes[type]);
            drmFreeDevice(&dd);
        }
        return name;
    }
#endif'''
    s = s[:m.start()] + '\n' + proto + s[m.start():m.end()] + body + s[m.end():]
    m = re.search(r'drmGetDevice2\(int fd, uint32_t flags, drmDevicePtr \*device\)\s*\{', s)
    if not m:
        sys.exit('drmGetDevice2 definition not found')
    s = s[:m.end()] + '''
#ifdef __APPLE__
    return drm_darwin_device_for_fd(fd, flags, device);
#endif''' + s[m.end():]
    s += '''
#ifdef __APPLE__
/* the drmDevice of an open DRM fd: the listed device whose PCI address the fd's node reports */
static int drm_darwin_device_for_fd(int fd, uint32_t flags, drmDevicePtr *device)
{
    struct drm_pciinfo pinfo;
    drmDevicePtr devs[64];
    int n, i, found = -1;

    if (device == NULL)
        return -EINVAL;
    memset(&pinfo, 0, sizeof(pinfo));
    if (drmIoctl(fd, DRM_IOCTL_GET_PCIINFO, &pinfo))
        return -errno;
    n = drmGetDevices2(flags, devs, 64);
    if (n < 0)
        return n;
    for (i = 0; i < n; i++) {
        if (found < 0 && devs[i]->bustype == DRM_BUS_PCI && devs[i]->businfo.pci->domain == pinfo.domain &&
            devs[i]->businfo.pci->bus == pinfo.bus && devs[i]->businfo.pci->dev == pinfo.dev &&
            devs[i]->businfo.pci->func == pinfo.func)
            found = i;
    }
    for (i = 0; i < n; i++)
        if (i != found)
            drmFreeDevice(&devs[i]);
    if (found < 0)
        return -ENODEV;
    *device = devs[found];
    return 0;
}
#endif
'''
    open(p, 'w').write(s)
    print('libdrm: drmGetDevice2(fd) and the FromFd names find the device by PCI identity')
