#!/bin/bash
set -euo pipefail

usage()
{
    cat >&2 <<'EOF'
usage:
  build-rauc-bundle.sh \
    VERSION \
    BUILD_ID \
    ROOTFS_EXT4 \
    BOOT_IMG \
    OUTPUT_RAUC_BUNDLE

example:
  ./build-rauc-bundle.sh \
    1.2.0 \
    rk3588-1.2.0-001 \
    /path/rootfs.ext4 \
    /path/boot.img \
    /tmp/update-1.2.0.raucb
EOF
    exit 2
}

[ "$#" -eq 5 ] || usage

VERSION="$1"
BUILD_ID="$2"
ROOTFS="$3"
BOOT="$4"
OUTPUT="$5"

COMPATIBLE='EdgeGuard-ATK-DLRK3588-RK3588'

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO="$SCRIPT_DIR"

PKI="${EDGEGUARD_RAUC_PKI:-$HOME/.local/share/edgeguard-ota/pki/r3-test-v3}"

RAUC="${RAUC:-/usr/bin/rauc}"

DEFAULT_HOST_CONF="$REPO/buildroot-external/package/edgeguard-remote-ota/system.conf"
HOST_CONF="${EDGEGUARD_RAUC_HOST_CONF:-$DEFAULT_HOST_CONF}"

ROOT_CA="$PKI/certs/root-ca.cert.pem"
INTERMEDIATE_CA="$PKI/certs/intermediate-ca.cert.pem"
SIGNER_CERT="$PKI/certs/bundle-signer.cert.pem"
SIGNER_KEY="$PKI/private/bundle-signer.key.pem"


fail()
{
    echo "bundle-build: $*" >&2
    exit 1
}


#
# Input validation
#

[ -x "$RAUC" ] \
    || fail "RAUC executable missing"

case "$RAUC" in
    /*) ;;
    *) fail "RAUC must be an absolute executable path" ;;
esac

[ -f "$ROOTFS" ] \
    || fail "rootfs image missing: $ROOTFS"

[ -f "$BOOT" ] \
    || fail "boot image missing: $BOOT"

[ -f "$HOST_CONF" ] \
    || fail "system.conf missing: $HOST_CONF"

case "$HOST_CONF" in
    /*) ;;
    *) fail "host system.conf override must be absolute" ;;
esac

# An override may relocate the production profile, not substitute a different
# compatibility/slot policy for bundle creation.
cmp -s "$HOST_CONF" "$DEFAULT_HOST_CONF" \
    || fail "host system.conf is not the production R3 profile"

[ -f "$ROOT_CA" ] \
    || fail "Root CA missing"

[ -f "$INTERMEDIATE_CA" ] \
    || fail "Intermediate CA missing"

[ -f "$SIGNER_CERT" ] \
    || fail "signer certificate missing"

[ -f "$SIGNER_KEY" ] \
    || fail "signer private key missing"


#
# Use the same canonical R3 version grammar as the Agent.
#

[[ "$VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] \
    || fail "VERSION must be canonical MAJOR.MINOR.PATCH"
IFS=. read -r version_major version_minor version_patch <<<"$VERSION"
for component in "$version_major" "$version_minor" "$version_patch"; do
    [ "${#component}" -le 10 ] && ((10#$component <= 4294967295)) \
        || fail "VERSION component exceeds uint32"
done

case "$BUILD_ID" in
    *[!A-Za-z0-9._+-]*|'')
        fail "unsafe BUILD_ID syntax"
        ;;
esac


#
# Matching host RAUC is mandatory for R3.
#

RAUC_VERSION="$("$RAUC" --version 2>&1)"

echo "$RAUC_VERSION"

[[ "$RAUC_VERSION" =~ (^|[[:space:]])1\.5\.1($|[[:space:]]) ]] \
    || fail "host RAUC is not exactly 1.5.1"


#
# Prove PKI chain before spending time bundling a large rootfs.
#

openssl verify \
    -CAfile "$ROOT_CA" \
    "$INTERMEDIATE_CA" \
    >/dev/null \
    || fail "Intermediate CA verification failed"

openssl verify \
    -CAfile "$ROOT_CA" \
    -untrusted "$INTERMEDIATE_CA" \
    "$SIGNER_CERT" \
    >/dev/null \
    || fail "bundle signer verification failed"


#
# Build staging.
#

STAGE="$(mktemp -d /tmp/edgeguard-rauc-stage.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

cp "$ROOTFS" \
    "$STAGE/rootfs.ext4"

cp "$BOOT" \
    "$STAGE/boot.img"


#
# Generate the bundle manifest.
#

cat > "$STAGE/manifest.raucm" <<EOF
[update]
compatible=$COMPATIBLE
version=$VERSION
description=EdgeGuard Remote OTA signed update
build=$BUILD_ID

[bundle]
format=plain

[image.rootfs]
filename=rootfs.ext4

[image.boot]
filename=boot.img
EOF


#
# Record inputs.
#

echo
echo "=== INPUT ==="

stat -c \
    'rootfs.ext4 size=%s' \
    "$STAGE/rootfs.ext4"

stat -c \
    'boot.img size=%s' \
    "$STAGE/boot.img"

sha256sum \
    "$STAGE/rootfs.ext4" \
    "$STAGE/boot.img"

echo
cat "$STAGE/manifest.raucm"
echo


#
# Sign bundle.
#

mkdir -p "$(dirname "$OUTPUT")"

rm -f "$OUTPUT"

"$RAUC" \
    -c "$HOST_CONF" \
    --keyring="$ROOT_CA" \
    --intermediate="$INTERMEDIATE_CA" \
    --cert="$SIGNER_CERT" \
    --key="$SIGNER_KEY" \
    bundle \
    "$STAGE" \
    "$OUTPUT"


#
# Mandatory post-sign verification.
#
# Do NOT pass --intermediate here.
# The whole point is proving the intermediate was embedded
# into the bundle signature.
#

echo
echo "=== VERIFY SIGNED BUNDLE ==="

"$RAUC" \
    -c "$HOST_CONF" \
    --keyring="$ROOT_CA" \
    info \
    "$OUTPUT"


#
# Machine-readable evidence.
#

"$RAUC" \
    -c "$HOST_CONF" \
    --keyring="$ROOT_CA" \
    info \
    --output-format=shell \
    "$OUTPUT" \
    > "${OUTPUT}.info"


# Parse RAUC shell-format output strictly as data. The requested identities and
# image classes cannot contain apostrophes, so reject exotic quoting outright.
decode_rauc_value()
{
    case "$1" in
        \'*\') ;;
        *) fail "malformed RAUC info assignment" ;;
    esac
    DECODED=${1#\'}
    DECODED=${DECODED%\'}
    case "$DECODED" in
        *\'*|*$'\r'*|*$'\n'*) fail "unsupported RAUC info quoting" ;;
    esac
    [ -n "$DECODED" ] || fail "empty RAUC info value"
}

info_compatible=
info_version=
info_build=
compatible_count=0
version_count=0
build_count=0
class_count=0
rootfs_count=0
boot_count=0

while IFS= read -r line || [ -n "$line" ]; do
    key=${line%%=*}
    [ "$key" != "$line" ] || continue
    raw=${line#*=}
    case "$key" in
        RAUC_MF_COMPATIBLE)
            compatible_count=$((compatible_count + 1))
            decode_rauc_value "$raw"
            info_compatible=$DECODED
            ;;
        RAUC_MF_VERSION)
            version_count=$((version_count + 1))
            decode_rauc_value "$raw"
            info_version=$DECODED
            ;;
        RAUC_MF_BUILD)
            build_count=$((build_count + 1))
            decode_rauc_value "$raw"
            info_build=$DECODED
            ;;
        RAUC_IMAGE_CLASS_[0-9]*)
            index=${key#RAUC_IMAGE_CLASS_}
            case "$index" in ''|*[!0-9]*) fail "malformed RAUC image class index" ;; esac
            decode_rauc_value "$raw"
            class_count=$((class_count + 1))
            case "$DECODED" in
                rootfs) rootfs_count=$((rootfs_count + 1)) ;;
                boot) boot_count=$((boot_count + 1)) ;;
                *) fail "unexpected signed image class: $DECODED" ;;
            esac
            ;;
    esac
done < "${OUTPUT}.info"

[ "$compatible_count" -eq 1 ] && [ "$info_compatible" = "$COMPATIBLE" ] \
    || fail "signed compatible mismatch or duplicate"
[ "$version_count" -eq 1 ] && [ "$info_version" = "$VERSION" ] \
    || fail "signed version mismatch or duplicate"
[ "$build_count" -eq 1 ] && [ "$info_build" = "$BUILD_ID" ] \
    || fail "signed build mismatch or duplicate"
[ "$class_count" -eq 2 ] && [ "$rootfs_count" -eq 1 ] && [ "$boot_count" -eq 1 ] \
    || fail "signed bundle must contain exactly rootfs and boot image classes"

sha256sum "$OUTPUT" \
    > "${OUTPUT}.sha256"

stat -c '%s' "$OUTPUT" \
    > "${OUTPUT}.size"



#
# Final result.
#

echo
echo "=== BUNDLE READY ==="
echo "bundle: $OUTPUT"
cat "${OUTPUT}.sha256"
read -r bundle_size < "${OUTPUT}.size"
echo "size:   $bundle_size"
echo "info:   ${OUTPUT}.info"
