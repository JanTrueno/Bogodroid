#!/bin/bash
set -e

# Running under QEMU: mount /proc if it's missing (README notes loaders need it).
if ! mountpoint -q /proc 2>/dev/null; then
    mount -t proc none /proc 2>/dev/null || true
fi

if [ "$#" -gt 0 ]; then
    exec "./${PROJ}" "$@"
fi

default_config="../configs/${PROJ%loader}.toml"
exec "./${PROJ}" "$default_config"
