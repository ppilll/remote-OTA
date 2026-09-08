#!/usr/bin/env bash

# REAL SDK PREFLIGHT
#
# Recommended:
#   bash ./real_sdk_preflight.sh
#
# This script:
#   - DOES NOT build the SDK
#   - DOES NOT install anything
#   - DOES NOT modify Buildroot configuration
#   - DOES NOT deploy/publish files
#   - DOES NOT modify/restart systemd
#
# It only validates the existing SDK/project/configuration.

EG_PREFLIGHT_RC=0

(
    # Protect the caller if this script is sourced or pasted into a shell
    # that already has errexit/nounset/pipefail enabled.
    set +e
    set +u
    set +E
    set +o pipefail 2>/dev/null
    trap - ERR

    main()
    {
        local PROJECT_DIR="${PROJECT_DIR:-/home/liu2004/work/EG_OTA}"
        local SDK="${SDK:-/home/liu2004/work/Linux_SDK/atk_dlrk3588_linux5.10}"
        local BR="${BR:-${SDK}/buildroot}"
        local OUT="${OUT:-${BR}/output/alientek_rk3588}"

        local RAUC_EXPECTED_VERSION="${RAUC_EXPECTED_VERSION:-1.5.1}"
        local RAUC_HOST="${RAUC_HOST:-/home/liu2004/.local/rauc-${RAUC_EXPECTED_VERSION}/bin/rauc}"

        local EXTERNAL="${PROJECT_DIR}/buildroot-external"
        local REMOTE_OTA_DIR="${EXTERNAL}/package/edgeguard-remote-ota"
        local HEALTH_CHECK="${REMOTE_OTA_DIR}/edgeguard-health-check"
        local POST_BUILD_CHECK="${REMOTE_OTA_DIR}/post-build-check.sh"
        local PREPARE_RELEASE="${REMOTE_OTA_DIR}/prepare-release.py"
        local BUILDROOT_CONFIG="${OUT}/.config"

        local -a FAIL_NAMES=()
        local -a FAIL_RCS=()

        pass()
        {
            local name="$1"
            printf '%s: PASS\n' "$name"
        }

        fail()
        {
            local name="$1"
            local rc="$2"
            shift 2

            printf '%s: FAIL (rc=%d)\n' "$name" "$rc"

            if (( $# > 0 )); then
                printf '%s\n' "$@"
            fi

            FAIL_NAMES+=("$name")
            FAIL_RCS+=("$rc")

            # Never let an individual failed check stop the preflight.
            return 0
        }

        skip()
        {
            local name="$1"
            shift

            printf '%s: SKIP' "$name"

            if (( $# > 0 )); then
                printf ' (%s)' "$*"
            fi

            printf '\n'
        }

        check_directory()
        {
            local name="$1"
            local path="$2"

            if [[ -d "$path" ]]; then
                pass "$name"
            else
                fail "$name" 1 \
                    "expected directory:" \
                    "  $path" \
                    "actual:" \
                    "  directory does not exist"
            fi
        }

        check_file()
        {
            local name="$1"
            local path="$2"

            if [[ -f "$path" ]]; then
                pass "$name"
            else
                fail "$name" 1 \
                    "expected regular file:" \
                    "  $path" \
                    "actual:" \
                    "  file does not exist or is not a regular file"
            fi
        }

        check_executable()
        {
            local name="$1"
            local path="$2"

            if [[ -x "$path" ]]; then
                pass "$name"
                return 0
            fi

            if [[ ! -e "$path" ]]; then
                fail "$name" 1 \
                    "expected executable:" \
                    "  $path" \
                    "actual:" \
                    "  path does not exist"
            elif [[ ! -f "$path" ]]; then
                fail "$name" 1 \
                    "expected executable regular file:" \
                    "  $path" \
                    "actual:" \
                    "  path exists but is not a regular file"
            else
                fail "$name" 1 \
                    "expected executable:" \
                    "  $path" \
                    "actual:" \
                    "  file exists but executable bit/access is missing"
            fi
        }

        check_exact_line()
        {
            local name="$1"
            local file="$2"
            local expected="$3"

            local key
            local actual
            local rc

            if [[ ! -r "$file" ]]; then
                fail "$name" 2 \
                    "cannot read:" \
                    "  $file" \
                    "expected exact line:" \
                    "  $expected"
                return 0
            fi

            grep -Fqx -- "$expected" "$file"
            rc=$?

            if (( rc == 0 )); then
                pass "$name"
                return 0
            fi

            key="${expected%%=*}"

            actual="$(
                grep -Fn -- "$key" "$file" 2>&1
            )"

            if [[ -z "$actual" ]]; then
                actual="[no line containing ${key}]"
            fi

            fail "$name" "$rc" \
                "file:" \
                "  $file" \
                "expected exact line:" \
                "  $expected" \
                "actual relevant line(s):" \
                "$actual"

            return 0
        }

        check_rauc_version()
        {
            local output
            local rc
            local escaped_version

            if [[ ! -x "$RAUC_HOST" ]]; then
                skip \
                    "RAUC host version ${RAUC_EXPECTED_VERSION}" \
                    "RAUC host executable is unavailable"
                return 0
            fi

            output="$(
                "$RAUC_HOST" --version 2>&1
            )"
            rc=$?

            if (( rc != 0 )); then
                fail \
                    "RAUC host version ${RAUC_EXPECTED_VERSION}" \
                    "$rc" \
                    "command:" \
                    "  $RAUC_HOST --version" \
                    "actual output:" \
                    "${output:-[no output]}"

                return 0
            fi

            # Escape dots so version 1.5.1 means literal dots in the regex.
            escaped_version="${RAUC_EXPECTED_VERSION//./\\.}"

            if grep -Eq \
                "(^|[[:space:]])${escaped_version}([[:space:]]|$)" \
                <<<"$output"
            then
                pass "RAUC host version ${RAUC_EXPECTED_VERSION}"
            else
                fail \
                    "RAUC host version ${RAUC_EXPECTED_VERSION}" \
                    1 \
                    "command:" \
                    "  $RAUC_HOST --version" \
                    "expected version:" \
                    "  $RAUC_EXPECTED_VERSION" \
                    "actual output:" \
                    "${output:-[no output]}"
            fi

            return 0
        }

        printf '%s\n' \
            "============================================================" \
            "REAL SDK PREFLIGHT" \
            "============================================================"

        # ------------------------------------------------------------
        # Test harness prerequisites
        # ------------------------------------------------------------

        if ! command -v grep >/dev/null 2>&1; then
            printf '%s\n' \
                "preflight harness: FAIL (rc=127)" \
                "required command is missing: grep"

            return 127
        fi

        # ------------------------------------------------------------
        # Project / SDK layout
        # ------------------------------------------------------------

        check_directory \
            "project directory" \
            "$PROJECT_DIR"

        check_directory \
            "SDK directory" \
            "$SDK"

        check_executable \
            "SDK build.sh executable" \
            "$SDK/build.sh"

        check_directory \
            "Buildroot directory" \
            "$BR"

        check_file \
            "Buildroot Makefile" \
            "$BR/Makefile"

        check_directory \
            "Buildroot output directory" \
            "$OUT"

        check_file \
            "Buildroot output .config" \
            "$BUILDROOT_CONFIG"

        # ------------------------------------------------------------
        # Host RAUC
        # ------------------------------------------------------------

        check_executable \
            "RAUC host executable" \
            "$RAUC_HOST"

        check_rauc_version

        # ------------------------------------------------------------
        # Effective Buildroot configuration
        #
        # These deliberately remain exact checks. We are verifying the
        # generated .config state, not loosely searching source Kconfig.
        # ------------------------------------------------------------

        check_exact_line \
            "Buildroot RAUC enabled" \
            "$BUILDROOT_CONFIG" \
            "BR2_PACKAGE_RAUC=y"

        check_exact_line \
            "Buildroot EDGEGUARD_RK_AB enabled" \
            "$BUILDROOT_CONFIG" \
            "BR2_PACKAGE_EDGEGUARD_RK_AB=y"

        check_exact_line \
            "Buildroot EDGEGUARD_REMOTE_OTA enabled" \
            "$BUILDROOT_CONFIG" \
            "BR2_PACKAGE_EDGEGUARD_REMOTE_OTA=y"

        check_exact_line \
            "Buildroot libcurl enabled" \
            "$BUILDROOT_CONFIG" \
            "BR2_PACKAGE_LIBCURL=y"

        check_exact_line \
            "Buildroot GLib2 enabled" \
            "$BUILDROOT_CONFIG" \
            "BR2_PACKAGE_LIBGLIB2=y"

        check_exact_line \
            "Buildroot json-glib enabled" \
            "$BUILDROOT_CONFIG" \
            "BR2_PACKAGE_JSON_GLIB=y"

        # ------------------------------------------------------------
        # EdgeGuard remote OTA package files
        # ------------------------------------------------------------

        check_file \
            "edgeguard-health-check present" \
            "$HEALTH_CHECK"

        check_file \
            "post-build-check.sh present" \
            "$POST_BUILD_CHECK"

        check_file \
            "prepare-release.py present" \
            "$PREPARE_RELEASE"

        # ------------------------------------------------------------
        # Health-check policy
        #
        # Preserve original semantics: these values must appear as the
        # specified exact assignment lines.
        #
        # We deliberately do NOT "source" edgeguard-health-check merely
        # to inspect variables, because sourcing a production script could
        # execute arbitrary top-level shell code and create side effects.
        # ------------------------------------------------------------

        check_exact_line \
            "health REQUIRED_STREAK=3" \
            "$HEALTH_CHECK" \
            "REQUIRED_STREAK=3"

        check_exact_line \
            "health MAX_SAMPLES=5" \
            "$HEALTH_CHECK" \
            "MAX_SAMPLES=5"

        check_exact_line \
            "health SAMPLE_INTERVAL_SEC=5" \
            "$HEALTH_CHECK" \
            "SAMPLE_INTERVAL_SEC=5"

        # ------------------------------------------------------------
        # Summary
        # ------------------------------------------------------------

        printf '\n%s\n' \
            "============================================================"

        if (( ${#FAIL_NAMES[@]} == 0 )); then
            printf '%s\n' \
                "REAL SDK PREFLIGHT: PASS" \
                "NOTE: configuration/preflight verified; SDK build was NOT performed." \
                "============================================================"

            return 0
        fi

        printf 'REAL SDK PREFLIGHT: FAIL (%d failed check(s))\n' \
            "${#FAIL_NAMES[@]}"

        local i

        for ((i = 0; i < ${#FAIL_NAMES[@]}; i++)); do
            printf 'FAIL: %s (rc=%s)\n' \
                "${FAIL_NAMES[$i]}" \
                "${FAIL_RCS[$i]}"
        done

        printf '%s\n' \
            "NOTE: no SDK build was performed." \
            "============================================================"

        return 1
    }

    main

) || EG_PREFLIGHT_RC=$?

# When executed normally:
#   bash ./real_sdk_preflight.sh
#
# return the aggregate rc from the child script process.
#
# When sourced or pasted:
# never execute "exit" in the current interactive shell.
if [[ "${BASH_SOURCE[0]:-}" == "$0" ]]; then
    ( exit "$EG_PREFLIGHT_RC" )
else
    printf 'EG_PREFLIGHT_RC=%d; current shell remains active.\n' \
        "$EG_PREFLIGHT_RC"
fi
