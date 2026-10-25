#!/bin/bash
# run-vk.sh - runs vkfill on the real Iris Xe through Mesa ANV (needs NMXGPU driving the GPU: the quiet/option-1 config).
# Do NOT use sudo (macOS drops DYLD_* variables for sudo). Writes vkfill-log.txt next to this script.
cd "$(dirname "$0")" || exit 1
xattr -dr com.apple.quarantine . 2>/dev/null
export VK_DRIVER_FILES="$PWD/intel_icd.json" VK_ICD_FILENAMES="$PWD/intel_icd.json"
export DYLD_INSERT_LIBRARIES="$PWD/nmxmmap.dylib" DYLD_LIBRARY_PATH="$PWD"
export NMXMMAP_DEBUG=1 MESA_DEBUG=1 MESA_LOG_LEVEL=debug MESA_SHADER_CACHE_DISABLE=1 VK_LOADER_DEBUG="${VK_LOADER_DEBUG:-error,warn}"
ls -l /dev/dri 2>&1
[ -x ./drmlist ] && ./drmlist 2>&1 | tee drmlist-log.txt
./vkfill 2>&1 | tee vkfill-log.txt
echo "(saved to $PWD/vkfill-log.txt)"
