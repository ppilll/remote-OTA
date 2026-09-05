#!/usr/bin/env bash
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
  ./scripts/build-rauc-bundle.sh \
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

REPO="$HOME/work/EG_OTA"

PKI="${EDGEGUARD_RAUC_PKI:-$HOME/.local/share/edgeguard-ota/pki/r3-test-v3}"

RAUC="${RAUC:-$(command -v rauc)}"

HOST_CONF="$REPO/buildroot-external/board/r3-c/rootfs-overlay/etc/rauc/system.conf"

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

[ -f "$ROOTFS" ] \
    || fail "rootfs image missing: $ROOTFS"

[ -f "$BOOT" ] \
    || fail "boot image missing: $BOOT"

[ -f "$HOST_CONF" ] \
    || fail "system.conf missing: $HOST_CONF"

[ -f "$ROOT_CA" ] \
    || fail "Root CA missing"

[ -f "$INTERMEDIATE_CA" ] \
    || fail "Intermediate CA missing"

[ -f "$SIGNER_CERT" ] \
    || fail "signer certificate missing"

[ -f "$SIGNER_KEY" ] \
    || fail "signer private key missing"


#
# Hygiene only.
# This is not the Agent's eventual version-policy grammar.
#

case "$VERSION" in
    *[!A-Za-z0-9._+-]*|'')
        fail "unsafe VERSION syntax"
        ;;
esac

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

printf '%s\n' "$RAUC_VERSION" |
grep -q '1\.5\.1' \
    || fail "host RAUC is not 1.5.1"


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

sha256sum "$OUTPUT" \
    > "${OUTPUT}.sha256"


#
# Final result.
#

echo
echo "=== BUNDLE READY ==="
echo "bundle: $OUTPUT"
cat "${OUTPUT}.sha256"
echo "info:   ${OUTPUT}.info"