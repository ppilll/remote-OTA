#!/usr/bin/env bash
set -Eeuo pipefail

usage()
{
    cat >&2 <<'USAGE'
Usage:
  ./build-r3-candidate.sh VERSION BUILD_ID MODE

MODE:
  rootfs-only
      Generate release identity, rebuild the OTA package,
      run vendor ./build.sh buildroot, and verify the final rootfs.

  bundle-only
      Do not rebuild rootfs. Verify that the existing rootfs
      contains exactly VERSION/BUILD_ID, then build the signed RAUC bundle.

  all
      Build and verify rootfs, then build and verify the signed RAUC bundle.

Examples:

  ./build-r3-candidate.sh \
      1.2.1 \
      rk3588-r3g-1.2.1-001 \
      rootfs-only

  ./build-r3-candidate.sh \
      1.2.1 \
      rk3588-r3g-1.2.1-001 \
      bundle-only

  ./build-r3-candidate.sh \
      1.2.1 \
      rk3588-r3g-1.2.1-001 \
      all
USAGE
    exit 2
}

fail()
{
    echo "R3 candidate: $*" >&2
    exit 1
}

[ "$#" -eq 3 ] || usage

VERSION="$1"
BUILD_ID="$2"
MODE="$3"

case "$MODE" in
    rootfs-only|bundle-only|all)
        ;;
    *)
        usage
        ;;
esac


###############################################################################
# Fixed project paths
###############################################################################

REPO="${EDGEGUARD_REPO:-/home/liu2004/work/EG_OTA}"

SDK="${EDGEGUARD_SDK:-/home/liu2004/work/Linux_SDK/atk_dlrk3588_linux5.10}"

BR_DIR="$SDK/buildroot"
BR_OUT="$BR_DIR/output/alientek_rk3588"

EXT="$REPO/buildroot-external"

R3_BOARD="$EXT/board/r3-g"

# This file is generated for every rootfs candidate build.
RELEASE="$R3_BOARD/release.json"

POST_CHECK="$EXT/package/edgeguard-remote-ota/post-build-check.sh"
PREPARE_RELEASE="$EXT/package/edgeguard-remote-ota/prepare-release.py"

# Buildroot currently creates:
#   rootfs.ext2
#   rootfs.ext4 -> rootfs.ext2
ROOTFS_LINK="${EDGEGUARD_ROOTFS:-$BR_OUT/images/rootfs.ext4}"

BOOT_IMG="${EDGEGUARD_BOOT_IMG:-$SDK/kernel/boot.img}"

# Exact host RAUC used for R3.
RAUC_HOST="${EDGEGUARD_RAUC:-/home/liu2004/.local/rauc-1.5.1/bin/rauc}"

# Persistent build evidence. Override with EDGEGUARD_ARTIFACT_ROOT if desired.
ARTIFACT_ROOT="${EDGEGUARD_ARTIFACT_ROOT:-$HOME/work/EG_OTA-artifacts}"
ARTIFACT_DIR="$ARTIFACT_ROOT/${VERSION}__${BUILD_ID}"

BUNDLE="$ARTIFACT_DIR/update-${VERSION}.raucb"


###############################################################################
# Argument validation
###############################################################################

[[ "$VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] ||
    fail "VERSION must be canonical MAJOR.MINOR.PATCH"

IFS=. read -r VER_MAJOR VER_MINOR VER_PATCH <<<"$VERSION"

for component in "$VER_MAJOR" "$VER_MINOR" "$VER_PATCH"; do
    [ "${#component}" -le 10 ] &&
        ((10#$component <= 4294967295)) ||
        fail "VERSION component exceeds uint32"
done

[[ "$BUILD_ID" =~ ^[A-Za-z0-9._+-]+$ ]] ||
    fail "invalid BUILD_ID syntax"


###############################################################################
# Static environment validation
###############################################################################

[ -d "$REPO" ] ||
    fail "repository missing: $REPO"

[ -x "$SDK/build.sh" ] ||
    fail "vendor build.sh missing or not executable: $SDK/build.sh"

[ -f "$BR_DIR/Makefile" ] ||
    fail "Buildroot tree missing: $BR_DIR"

[ -f "$BR_OUT/.config" ] ||
    fail "Buildroot output .config missing: $BR_OUT/.config"

[ -f "$PREPARE_RELEASE" ] ||
    fail "prepare-release.py missing: $PREPARE_RELEASE"

[ -f "$POST_CHECK" ] ||
    fail "post-build checker missing: $POST_CHECK"

[ -f "$REPO/build-rauc-bundle.sh" ] ||
    fail "build-rauc-bundle.sh missing"

# Buildroot invokes this directly.
chmod 0755 "$POST_CHECK"

# CRLF here is a functional Buildroot failure, not a style check.
if grep -q $'\r' "$EXT/external.desc"; then
    fail "external.desc contains CRLF; Buildroot will reject the external name"
fi

export BR2_EXTERNAL="$EXT"

mkdir -p "$ARTIFACT_DIR"


###############################################################################
# release.json helpers
###############################################################################

write_release()
{
    local output="$1"
    local check_output

    mkdir -p "$(dirname "$output")"

    cat > "$output" <<JSON
{
  "schema_version": 1,
  "device_compatible": "atk-dlrk3588",
  "rauc_compatible": "EdgeGuard-ATK-DLRK3588-RK3588",
  "version": "$VERSION",
  "build_id": "$BUILD_ID"
}
JSON

    check_output="$(mktemp /tmp/edgeguard-release-check.XXXXXX.json)"

    python3 \
        "$PREPARE_RELEASE" \
        --input "$output" \
        --output "$check_output"

    rm -f "$check_output"
}


###############################################################################
# Verify permanent Buildroot integration
###############################################################################

validate_buildroot_config()
{
    python3 - "$BR_OUT/.config" <<'PY'
import ast
import sys

path = sys.argv[1]

with open(path, "r", encoding="utf-8") as f:
    lines = f.read().splitlines()

line_set = set(lines)

required_y = [
    "BR2_PACKAGE_RAUC",
    "BR2_PACKAGE_EDGEGUARD_RK_AB",
    "BR2_PACKAGE_EDGEGUARD_REMOTE_OTA",
    "BR2_PACKAGE_LIBCURL",
    "BR2_PACKAGE_LIBGLIB2",
    "BR2_PACKAGE_JSON_GLIB",
]

for name in required_y:
    if f"{name}=y" not in line_set:
        raise SystemExit(f"Buildroot config missing {name}=y")


def get_string(name):
    prefix = name + "="
    for line in lines:
        if line.startswith(prefix):
            raw = line[len(prefix):]
            try:
                return ast.literal_eval(raw)
            except Exception:
                raise SystemExit(f"Malformed Buildroot string: {name}")
    raise SystemExit(f"Buildroot config missing {name}")


release = get_string("BR2_PACKAGE_EDGEGUARD_REMOTE_OTA_RELEASE_FILE")

if "board/r3-g/release.json" not in release.replace("\\", "/"):
    raise SystemExit(
        "release input is not the permanent board/r3-g/release.json path"
    )

if "EG_OTA-r3g-build" in release:
    raise SystemExit(
        "temporary R3-G release path is still present in Buildroot config"
    )


overlay = get_string("BR2_ROOTFS_OVERLAY").replace("\\", "/")

if "board/r3-g/rootfs-overlay" not in overlay:
    raise SystemExit(
        "permanent R3-G rootfs overlay is not configured"
    )

if "board/r3-c/rootfs-overlay" in overlay:
    raise SystemExit(
        "old R3-C rootfs overlay is still active"
    )


post_build = get_string("BR2_ROOTFS_POST_BUILD_SCRIPT").replace("\\", "/")
scripts = post_build.split()

if not scripts:
    raise SystemExit("BR2_ROOTFS_POST_BUILD_SCRIPT is empty")

if not scripts[-1].endswith(
    "package/edgeguard-remote-ota/post-build-check.sh"
):
    raise SystemExit(
        "EdgeGuard final checker is not the last post-build script"
    )

print("Buildroot permanent OTA integration: PASS")
PY
}


###############################################################################
# Resolve final rootfs
###############################################################################

resolve_rootfs()
{
    [ -e "$ROOTFS_LINK" ] ||
        fail "rootfs image missing: $ROOTFS_LINK"

    ROOTFS_REAL="$(readlink -f "$ROOTFS_LINK")"

    [ -n "$ROOTFS_REAL" ] ||
        fail "cannot resolve rootfs image"

    [ -f "$ROOTFS_REAL" ] ||
        fail "resolved rootfs image missing: $ROOTFS_REAL"
}


###############################################################################
# Verify identity inside the filesystem image itself
###############################################################################

verify_rootfs_identity()
{
    local expected_release="$1"
    local actual_release
    local actual_rauc

    command -v debugfs >/dev/null 2>&1 ||
        fail "debugfs is required to inspect the ext2/ext4 candidate image"

    resolve_rootfs

    actual_release="$(mktemp /tmp/edgeguard-image-release.XXXXXX.json)"
    actual_rauc="$(mktemp /tmp/edgeguard-image-rauc.XXXXXX.conf)"

    if ! debugfs \
        -R 'cat /etc/edgeguard-ota/release.json' \
        "$ROOTFS_REAL" \
        >"$actual_release" 2>/dev/null
    then
        rm -f "$actual_release" "$actual_rauc"
        fail "cannot read release.json from candidate rootfs"
    fi

    if ! debugfs \
        -R 'cat /etc/rauc/system.conf' \
        "$ROOTFS_REAL" \
        >"$actual_rauc" 2>/dev/null
    then
        rm -f "$actual_release" "$actual_rauc"
        fail "cannot read RAUC system.conf from candidate rootfs"
    fi

    python3 - "$expected_release" "$actual_release" <<'PY'
import json
import sys

expected_path = sys.argv[1]
actual_path = sys.argv[2]

with open(expected_path, "r", encoding="utf-8") as f:
    expected = json.load(f)

with open(actual_path, "r", encoding="utf-8") as f:
    actual = json.load(f)

if actual != expected:
    print("Expected release identity:")
    print(json.dumps(expected, indent=2, sort_keys=True))

    print("Candidate rootfs release identity:")
    print(json.dumps(actual, indent=2, sort_keys=True))

    raise SystemExit(
        "candidate rootfs release identity does not match requested identity"
    )

print("Candidate rootfs release identity: PASS")
PY

    grep -qx \
        'compatible=EdgeGuard-ATK-DLRK3588-RK3588' \
        "$actual_rauc" ||
        fail "candidate rootfs RAUC compatible mismatch"

    grep -qx \
        'bootloader=custom' \
        "$actual_rauc" ||
        fail "candidate rootfs does not use RAUC custom bootloader"

    grep -qx \
        'statusfile=/userdata/edgeguard-remote-ota/rauc-status.raucs' \
        "$actual_rauc" ||
        fail "candidate rootfs RAUC statusfile mismatch"

    grep -qx \
        'bootloader-custom-backend=/usr/libexec/rauc/edgeguard-rk-ab-backend' \
        "$actual_rauc" ||
        fail "candidate rootfs custom backend missing"

    grep -qx \
        'pre-install=/usr/libexec/rauc/edgeguard-rk-ab-preinstall' \
        "$actual_rauc" ||
        fail "candidate rootfs pre-install gate missing"

    if grep -q '^readonly=true$' "$actual_rauc"; then
        fail "candidate rootfs contains deprecated readonly RAUC profile"
    fi

    rm -f "$actual_release" "$actual_rauc"

    echo "Candidate rootfs RAUC profile: PASS"
}


###############################################################################
# Record rootfs evidence
###############################################################################

record_rootfs_evidence()
{
    resolve_rootfs

    ROOTFS_SIZE="$(stat -Lc '%s' "$ROOTFS_REAL")"
    ROOTFS_SHA256="$(sha256sum "$ROOTFS_REAL" | awk '{print $1}')"

    printf '%s\n' "$ROOTFS_SIZE" \
        > "$ARTIFACT_DIR/rootfs.size"

    printf '%s\n' "$ROOTFS_SHA256" \
        > "$ARTIFACT_DIR/rootfs.sha256"

    printf '%s\n' "$ROOTFS_REAL" \
        > "$ARTIFACT_DIR/rootfs.path"

    echo
    echo "============================================================"
    echo "ROOTFS EVIDENCE"
    echo "============================================================"

    file "$ROOTFS_REAL"

    echo "ROOTFS=$ROOTFS_REAL"
    echo "ROOTFS_SIZE=$ROOTFS_SIZE"
    echo "ROOTFS_SHA256=$ROOTFS_SHA256"
}


###############################################################################
# Rootfs build
###############################################################################

build_rootfs()
{
    echo
    echo "============================================================"
    echo "GENERATE RELEASE IDENTITY"
    echo "============================================================"

    write_release "$RELEASE"

    cat "$RELEASE"

    # The permanent defconfig must already contain these integration settings.
    validate_buildroot_config

    echo
    echo "============================================================"
    echo "FORCE OTA PACKAGE REBUILD"
    echo "============================================================"

    # release.json is an external build input and is not automatically tracked
    # by the package stamp. Force this package to rebuild for every candidate.
    make \
        -C "$BR_DIR" \
        O="$BR_OUT" \
        BR2_EXTERNAL="$EXT" \
        edgeguard-remote-ota-dirclean

    echo
    echo "============================================================"
    echo "VENDOR BUILDROOT"
    echo "============================================================"

    (
        cd "$SDK"

        BR2_EXTERNAL="$EXT" \
            ./build.sh buildroot
    )

    # Prove vendor build.sh did not replace/drop the permanent settings.
    validate_buildroot_config

    echo
    echo "============================================================"
    echo "FINAL STAGED TARGET CHECK"
    echo "============================================================"

    HOST_DIR="$BR_OUT/host" \
        "$POST_CHECK" "$BR_OUT/target"

    "$BR_OUT/host/bin/python3" \
        "$PREPARE_RELEASE" \
        --input "$RELEASE" \
        --check-target "$BR_OUT/target"

    cp "$RELEASE" "$ARTIFACT_DIR/release.json"

    echo
    echo "============================================================"
    echo "FINAL ROOTFS IMAGE CHECK"
    echo "============================================================"

    verify_rootfs_identity "$ARTIFACT_DIR/release.json"

    record_rootfs_evidence
}


###############################################################################
# Host RAUC validation
###############################################################################

verify_rauc_host()
{
    [ -x "$RAUC_HOST" ] ||
        fail "RAUC executable missing: $RAUC_HOST"

    RAUC_VERSION="$("$RAUC_HOST" --version 2>&1)"

    echo "$RAUC_VERSION"

    [[ "$RAUC_VERSION" =~ (^|[[:space:]])1\.5\.1($|[[:space:]]) ]] ||
        fail "host RAUC is not exactly 1.5.1"
}


###############################################################################
# Signed bundle build
###############################################################################

build_bundle()
{
    resolve_rootfs

    [ -f "$BOOT_IMG" ] ||
        fail "boot.img missing: $BOOT_IMG"

    # bundle-only must still prove that VERSION / BUILD_ID correspond to
    # the rootfs being signed.
    if [ ! -f "$ARTIFACT_DIR/release.json" ]; then
        write_release "$ARTIFACT_DIR/release.json"
    fi

    verify_rootfs_identity "$ARTIFACT_DIR/release.json"

    verify_rauc_host

    ROOTFS_SIZE="$(stat -Lc '%s' "$ROOTFS_REAL")"
    ROOTFS_SHA256="$(sha256sum "$ROOTFS_REAL" | awk '{print $1}')"

    BOOT_SIZE="$(stat -Lc '%s' "$BOOT_IMG")"
    BOOT_SHA256="$(sha256sum "$BOOT_IMG" | awk '{print $1}')"

    echo
    echo "============================================================"
    echo "BUILD SIGNED RAUC BUNDLE"
    echo "============================================================"

    echo "VERSION=$VERSION"
    echo "BUILD_ID=$BUILD_ID"
    echo "ROOTFS=$ROOTFS_REAL"
    echo "BOOT_IMG=$BOOT_IMG"
    echo "RAUC=$RAUC_HOST"

    RAUC="$RAUC_HOST" \
        "$REPO/build-rauc-bundle.sh" \
        "$VERSION" \
        "$BUILD_ID" \
        "$ROOTFS_REAL" \
        "$BOOT_IMG" \
        "$BUNDLE"

    [ -f "$BUNDLE" ] ||
        fail "bundle was not created"

    [ -f "${BUNDLE}.info" ] ||
        fail "bundle info evidence missing"

    [ -f "${BUNDLE}.size" ] ||
        fail "bundle size evidence missing"

    [ -f "${BUNDLE}.sha256" ] ||
        fail "bundle SHA256 evidence missing"

    BUNDLE_SIZE="$(stat -Lc '%s' "$BUNDLE")"
    BUNDLE_SHA256="$(sha256sum "$BUNDLE" | awk '{print $1}')"

    echo
    echo "============================================================"
    echo "SIGNED BUNDLE EVIDENCE"
    echo "============================================================"

    echo "BUNDLE=$BUNDLE"
    echo "BUNDLE_SIZE=$BUNDLE_SIZE"
    echo "BUNDLE_SHA256=$BUNDLE_SHA256"

    echo
    echo "RAUC INFO:"
    cat "${BUNDLE}.info"

    {
        printf 'VERSION=%q\n' "$VERSION"
        printf 'BUILD_ID=%q\n' "$BUILD_ID"

        printf 'ROOTFS=%q\n' "$ROOTFS_REAL"
        printf 'ROOTFS_SIZE=%q\n' "$ROOTFS_SIZE"
        printf 'ROOTFS_SHA256=%q\n' "$ROOTFS_SHA256"

        printf 'BOOT_IMG=%q\n' "$BOOT_IMG"
        printf 'BOOT_SIZE=%q\n' "$BOOT_SIZE"
        printf 'BOOT_SHA256=%q\n' "$BOOT_SHA256"

        printf 'RAUC_HOST=%q\n' "$RAUC_HOST"

        printf 'BUNDLE=%q\n' "$BUNDLE"
        printf 'BUNDLE_SIZE=%q\n' "$BUNDLE_SIZE"
        printf 'BUNDLE_SHA256=%q\n' "$BUNDLE_SHA256"
    } > "$ARTIFACT_DIR/candidate.env"

    echo
    echo "Candidate evidence:"
    echo "  $ARTIFACT_DIR/candidate.env"
}


###############################################################################
# Main
###############################################################################

case "$MODE" in
    rootfs-only)
        build_rootfs

        echo
        echo "============================================================"
        echo "R3 ROOTFS BUILD PASS"
        echo "============================================================"
        ;;

    bundle-only)
        # Do not modify board/r3-g/release.json in bundle-only mode.
        write_release "$ARTIFACT_DIR/release.json"

        build_bundle

        echo
        echo "============================================================"
        echo "R3 BUNDLE BUILD PASS"
        echo "============================================================"
        ;;

    all)
        build_rootfs
        build_bundle

        echo
        echo "============================================================"
        echo "R3 CANDIDATE BUILD PASS"
        echo "============================================================"
        ;;
esac