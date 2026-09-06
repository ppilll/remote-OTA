#!/bin/sh
# Append LAST to BR2_ROOTFS_POST_BUILD_SCRIPT; Buildroot supplies TARGET_DIR as $1.
set -eu
[ "$#" -ge 1 ] || exit 1
: "${HOST_DIR:?Buildroot HOST_DIR required}"
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$HOST_DIR/bin/python3" "$here/prepare-release.py" --check-target "$1"
