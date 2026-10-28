/*
 * b184: GPU firmware built into the kext. With NMXGPU loaded by OpenCore (boot kernel collection) the driver attaches
 * before the root file system exists, so /Library/NullMoth/firmware cannot be read yet. gen_fw_embedded.py (run by
 * ci-build-intel.sh before the runtime stage) writes nmx_fw_blobs.inc next to this file with the Tiger Lake files i915
 * asks for; without it the table is empty and the driver reads the folder as before.
 */
#include <IOKit/IOLib.h>
#include <string.h>

struct nmx_fw_blob { const char *name; const unsigned char *data; unsigned int size; };

#if __has_include("nmx_fw_blobs.inc")
#include "nmx_fw_blobs.inc"     /* static const nmx_fw_blob nmx_fw_blobs[]; static const unsigned nmx_fw_nblobs */
#else
static const nmx_fw_blob nmx_fw_blobs[1] = { { nullptr, nullptr, 0 } };
static const unsigned nmx_fw_nblobs = 0;
#endif

static const char *base_of(const char *n)
{
    const char *b = n;
    for (const char *p = n; *p; p++)
        if (*p == '/') b = p + 1;
    return b;
}

/* by Linux name ("i915/tgl_dmc_ver2_12.bin"); also the base name alone and the name with '/' written as '_' */
extern "C" int nmx_xnu_firmware_embedded(const char *name, const void **data, size_t *size)
{
    if (name == nullptr)
        return 0;
    for (unsigned i = 0; i < nmx_fw_nblobs; i++) {
        const char *e = nmx_fw_blobs[i].name;
        bool hit = strcmp(e, name) == 0 || strcmp(base_of(e), name) == 0 || strcmp(base_of(e), base_of(name)) == 0;
        if (!hit) {                                 /* "i915_tgl_dmc_ver2_12.bin" */
            size_t l = strlen(e);
            if (strlen(name) == l) {
                hit = true;
                for (size_t k = 0; k < l && hit; k++)
                    hit = name[k] == e[k] || (e[k] == '/' && name[k] == '_');
            }
        }
        if (hit) {
            *data = nmx_fw_blobs[i].data;
            *size = nmx_fw_blobs[i].size;
            return 1;
        }
    }
    return 0;
}

extern "C" unsigned nmx_xnu_firmware_embedded_count(void)
{
    return nmx_fw_nblobs;
}
