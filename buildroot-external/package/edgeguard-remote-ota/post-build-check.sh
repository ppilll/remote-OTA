#!/bin/sh
# Append LAST to BR2_ROOTFS_POST_BUILD_SCRIPT; Buildroot supplies TARGET_DIR as $1.
set -eu
[ "$#" -ge 1 ] || exit 1
: "${HOST_DIR:?Buildroot HOST_DIR required}"
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"$HOST_DIR/bin/python3" "$here/prepare-release.py" --check-target "$1"

target_hook=$1/usr/libexec/edgeguard/edgeguard-health-check
target_config=$1/etc/edgeguard-ota/agent.conf
[ -x "$target_hook" ] || {
    echo "edgeguard-remote-ota: missing executable health hook" >&2
    exit 1
}
cmp -s "$here/edgeguard-health-check" "$target_hook" || {
    echo "edgeguard-remote-ota: target health hook differs from package source" >&2
    exit 1
}
cmp -s "$here/agent.conf" "$target_config" || {
    echo "edgeguard-remote-ota: target Agent configuration differs from R4 candidate configuration" >&2
    exit 1
}
