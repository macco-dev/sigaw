#!/bin/sh
set -eu

lib="$1"

if objdump -p "$lib" | grep -F "NEEDED               libvulkan.so.1" >/dev/null; then
    echo "libSigaw.so should not link directly against libvulkan.so.1"
    exit 1
fi

undefined_vk_symbols="$(nm -D "$lib" | awk '/ U vk/ { print $NF }')"
if [ -n "$undefined_vk_symbols" ]; then
    echo "libSigaw.so should not import global vk* symbols"
    echo "$undefined_vk_symbols"
    exit 1
fi
