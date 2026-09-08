#!/usr/bin/env bash

# EdgeGuard live-server validation gate
#
# Recommended:
#   bash ./live_server_gate.sh
#
# Optional LAN-path validation:
#   LAN_IP=10.52.46.50 bash ./live_server_gate.sh
#
# Other optional overrides:
#   PORT=8000
#   LOCAL_BASE=http://127.0.0.1:8000
#   LAN_BASE=http://10.52.46.50:8000
#   SERVICE_UNIT=edgeguard-ota.service
#   CONNECT_TIMEOUT=3
#   MAX_TIME=10
#
# Important:
# - This script DOES send POST requests to /device/report.
# - Those requests may create production-side report/log/state records.
# - It does NOT deploy files, install packages, modify systemd, or restart services.
#
# Shell-safety design:
# - No top-level "set -e" / "set -u" / "pipefail".
# - Validation body is isolated in a subshell.
# - All individual test failures are recorded and execution continues.
# - Outer shell captures TEST_RC.
# - If sourced/pasted, failure does NOT exit the current shell.
# - When invoked normally with "bash script.sh", the child script still returns
#   the aggregate validation rc to its caller.

TEST_RC=0

(
    # Explicitly neutralize strict options that may have been inherited when this
    # text is sourced or pasted from a shell using errexit/nounset/pipefail.
    # This affects this subshell only.
    set +e
    set +u
    set +E
    set +o pipefail 2>/dev/null
    trap - ERR

    main()
    {
        local PORT="${PORT:-8000}"
        local LOCAL_BASE="${LOCAL_BASE:-http://127.0.0.1:${PORT}}"
        local LAN_IP="${LAN_IP:-}"
        local LAN_BASE="${LAN_BASE:-}"
        local SERVICE_UNIT="${SERVICE_UNIT:-edgeguard-ota.service}"

        local CONNECT_TIMEOUT="${CONNECT_TIMEOUT:-3}"
        local MAX_TIME="${MAX_TIME:-10}"

        local TMP_DIR=""
        local TMP_OUT=""
        local TMP_RC=0

        local TS
        local SINCE
        local ATTEMPT
        local BUILD_ID="r4-vm-report-validation"

        local TEST_SEQ=0
        local CURRENT_ID=""

        local -a FAIL_NAMES=()
        local -a FAIL_RCS=()

        # curl output is always captured. Do not use --fail here:
        # 400 / 416 are expected application-level results in some tests.
        #
        # --noproxy '*' is deliberate: localhost/LAN tests should exercise the
        # actual local/network path instead of accidentally going through an
        # HTTP_PROXY/HTTPS_PROXY configuration.
        local -a CURL_COMMON=(
            --silent
            --show-error
            --noproxy '*'
            --connect-timeout "$CONNECT_TIMEOUT"
            --max-time "$MAX_TIME"
        )

        cleanup()
        {
            local sig="${1:-EXIT}"

            if [[ -n "${TMP_DIR:-}" && -d "$TMP_DIR" ]]; then
                rm -rf -- "$TMP_DIR"
                TMP_DIR=""
            fi

            # For INT/TERM, clean first, then re-raise the signal inside this
            # validation subshell. The outer interactive shell is not killed.
            if [[ "$sig" != "EXIT" ]]; then
                trap - "$sig"
                kill -s "$sig" "$BASHPID"
            fi
        }

        trap 'cleanup EXIT' EXIT
        trap 'cleanup INT'  INT
        trap 'cleanup TERM' TERM

        printf '%s\n' \
            "============================================================" \
            "LIVE SERVER VALIDATION GATE" \
            "============================================================"

        # ------------------------------------------------------------
        # Harness prerequisites
        # ------------------------------------------------------------

        if ! command -v mktemp >/dev/null 2>&1; then
            printf 'test harness prerequisites: FAIL (rc=127)\n'
            printf 'missing command: mktemp\n'
            return 127
        fi

        TMP_OUT="$(
            mktemp -d "${TMPDIR:-/tmp}/edgeguard-live-gate.XXXXXX" 2>&1
        )"
        TMP_RC=$?

        if (( TMP_RC != 0 )); then
            printf 'temporary directory setup: FAIL (rc=%d)\n' "$TMP_RC"
            printf '%s\n' "$TMP_OUT"
            return "$TMP_RC"
        fi

        TMP_DIR="$TMP_OUT"

        require_commands()
        {
            local cmd
            local missing=0

            for cmd in \
                curl journalctl grep wc head tail tr awk od date sleep rm
            do
                if ! command -v "$cmd" >/dev/null 2>&1; then
                    printf 'missing command: %s\n' "$cmd"
                    missing=1
                fi
            done

            if (( missing != 0 )); then
                return 127
            fi

            return 0
        }

        if ! require_commands; then
            printf 'test harness prerequisites: FAIL (rc=127)\n'
            return 127
        fi

        # ------------------------------------------------------------
        # Runtime identifiers
        # ------------------------------------------------------------

        TS="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        SINCE="$(date '+%Y-%m-%d %H:%M:%S')"

        # Avoid reusing the same attempt_id on every production validation run.
        # /proc supplies a real UUID; no fake symlinks/import shims are created.
        if [[ -r /proc/sys/kernel/random/uuid ]]; then
            ATTEMPT="$(</proc/sys/kernel/random/uuid)"
        elif command -v uuidgen >/dev/null 2>&1; then
            ATTEMPT="$(uuidgen)"
        else
            # Preserve the original syntactically valid UUID only as a fallback.
            ATTEMPT="11111111-2222-4333-8444-555555555555"
        fi

        if [[ -z "$LAN_BASE" && -n "$LAN_IP" ]]; then
            LAN_BASE="http://${LAN_IP}:${PORT}"
        fi

        # ------------------------------------------------------------
        # Diagnostic helpers
        # ------------------------------------------------------------

        file_size()
        {
            local file="$1"
            local n

            n="$(wc -c <"$file" 2>/dev/null)"
            n="${n// /}"

            if [[ "$n" =~ ^[0-9]+$ ]]; then
                printf '%s\n' "$n"
            else
                printf '0\n'
            fi
        }

        show_failure_log()
        {
            local file="$1"
            local limit=16384
            local bytes

            if [[ ! -f "$file" ]]; then
                printf '[no diagnostic log was produced]\n'
                return 0
            fi

            bytes="$(file_size "$file")"

            printf '%s\n' '--- diagnostic output ---'
            head -c "$limit" "$file"
            printf '\n'

            if (( bytes > limit )); then
                printf '[diagnostic output truncated: %d of %d bytes shown]\n' \
                    "$limit" "$bytes"
            fi
        }

        show_headers()
        {
            local file="$1"

            printf '%s\n' '--- response headers ---'

            if [[ -s "$file" ]]; then
                head -c 8192 "$file"
                printf '\n'
            else
                printf '[no response headers]\n'
            fi
        }

        show_body()
        {
            local file="$1"
            local bytes

            bytes="$(file_size "$file")"

            printf '%s\n' '--- response body ---'
            printf 'body_bytes=%s\n' "$bytes"

            if (( bytes == 0 )); then
                printf '[empty body]\n'
                return 0
            fi

            # Prevent a failed Range request returning the complete RAUC bundle
            # from flooding/corrupting the terminal. Preserve printable text;
            # render non-printable bytes as ".".
            head -c 4096 "$file" |
                LC_ALL=C tr '\000-\010\013\014\016-\037\177-\377' '.'
            printf '\n'

            if (( bytes > 4096 )); then
                printf '[body truncated: first 4096 of %d bytes shown]\n' "$bytes"
            fi
        }

        show_binary_body()
        {
            local file="$1"
            local bytes

            bytes="$(file_size "$file")"

            printf '%s\n' '--- response body ---'
            printf 'body_bytes=%s\n' "$bytes"

            if (( bytes == 0 )); then
                printf '[empty body]\n'
                return 0
            fi

            printf '%s\n' 'first up to 64 bytes, hex:'
            head -c 64 "$file" | od -An -tx1 -v

            if (( bytes > 64 )); then
                printf '[hex preview truncated]\n'
            fi
        }

        print_curl_command()
        {
            local headers="$1"
            local body="$2"
            shift 2

            local arg

            printf 'command: curl'

            for arg in \
                "${CURL_COMMON[@]}" \
                -D "$headers" \
                -o "$body" \
                -w '%{http_code}' \
                "$@"
            do
                printf ' %q' "$arg"
            done

            printf '\n'
        }

        normalize_headers()
        {
            local src="$1"
            local dst="$2"

            tr -d '\r' <"$src" >"$dst"
        }

        check_content_length_if_present()
        {
            local headers="$1"
            local body="$2"

            local declared
            local actual

            declared="$(
                awk '
                    tolower($1) == "content-length:" {
                        value=$2
                    }
                    END {
                        if (value != "")
                            print value
                    }
                ' "$headers"
            )"

            actual="$(file_size "$body")"

            printf 'actual_body_bytes=%s\n' "$actual"

            if [[ -z "$declared" ]]; then
                return 0
            fi

            printf 'declared_content_length=%s\n' "$declared"

            if [[ ! "$declared" =~ ^[0-9]+$ ]]; then
                printf 'invalid Content-Length header: %s\n' "$declared"
                return 1
            fi

            if (( actual != declared )); then
                printf 'Content-Length mismatch: declared=%s actual=%s\n' \
                    "$declared" "$actual"
                return 1
            fi

            return 0
        }

        run_test()
        {
            local name="$1"
            shift

            TEST_SEQ=$((TEST_SEQ + 1))
            CURRENT_ID="test-${TEST_SEQ}"

            local log="${TMP_DIR}/${CURRENT_ID}.log"
            local rc

            "$@" >"$log" 2>&1
            rc=$?

            if (( rc == 0 )); then
                printf '%s: PASS\n' "$name"
            else
                printf '%s: FAIL (rc=%d)\n' "$name" "$rc"
                show_failure_log "$log"

                FAIL_NAMES+=("$name")
                FAIL_RCS+=("$rc")
            fi

            # A failed individual test must never terminate the test runner.
            return 0
        }

        # ------------------------------------------------------------
        # Generic HTTP status test
        # ------------------------------------------------------------

        http_code_test()
        {
            local expected="$1"
            shift

            local body="${TMP_DIR}/${CURRENT_ID}.body"
            local headers="${TMP_DIR}/${CURRENT_ID}.headers"
            local normalized="${TMP_DIR}/${CURRENT_ID}.headers.normalized"
            local err="${TMP_DIR}/${CURRENT_ID}.curl.stderr"

            local actual=""
            local curl_rc
            local failed=0

            print_curl_command "$headers" "$body" "$@"

            actual="$(
                curl \
                    "${CURL_COMMON[@]}" \
                    -D "$headers" \
                    -o "$body" \
                    -w '%{http_code}' \
                    "$@" \
                    2>"$err"
            )"
            curl_rc=$?

            printf 'expected_http=%s actual_http=%s curl_rc=%d\n' \
                "$expected" "${actual:-<none>}" "$curl_rc"

            if [[ -f "$headers" ]]; then
                normalize_headers "$headers" "$normalized"
            else
                : >"$normalized"
            fi

            if (( curl_rc != 0 )); then
                printf '%s\n' 'curl transport failure'

                if [[ -s "$err" ]]; then
                    printf '%s\n' '--- curl stderr ---'
                    cat "$err"
                fi

                show_headers "$headers"
                show_body "$body"

                # Preserve real curl rc, e.g. 7 connection failure / 28 timeout.
                return "$curl_rc"
            fi

            if [[ "$actual" != "$expected" ]]; then
                printf 'HTTP status mismatch: expected=%s actual=%s\n' \
                    "$expected" "$actual"
                failed=1
            fi

            if ! check_content_length_if_present "$normalized" "$body"; then
                failed=1
            fi

            if (( failed != 0 )); then
                if [[ -s "$err" ]]; then
                    printf '%s\n' '--- curl stderr ---'
                    cat "$err"
                fi

                show_headers "$headers"
                show_body "$body"
                return 1
            fi

            return 0
        }

        # ------------------------------------------------------------
        # HTTP Range: expected 206, Content-Range and exact 32 bytes
        # ------------------------------------------------------------

        range_206_test()
        {
            local url="$1"

            local body="${TMP_DIR}/${CURRENT_ID}.body"
            local headers="${TMP_DIR}/${CURRENT_ID}.headers"
            local normalized="${TMP_DIR}/${CURRENT_ID}.headers.normalized"
            local err="${TMP_DIR}/${CURRENT_ID}.curl.stderr"

            local actual=""
            local curl_rc
            local bytes
            local failed=0

            print_curl_command \
                "$headers" \
                "$body" \
                -H 'Range: bytes=0-31' \
                "$url"

            actual="$(
                curl \
                    "${CURL_COMMON[@]}" \
                    -D "$headers" \
                    -o "$body" \
                    -w '%{http_code}' \
                    -H 'Range: bytes=0-31' \
                    "$url" \
                    2>"$err"
            )"
            curl_rc=$?

            printf 'expected_http=206 actual_http=%s curl_rc=%d\n' \
                "${actual:-<none>}" "$curl_rc"

            if [[ -f "$headers" ]]; then
                normalize_headers "$headers" "$normalized"
            else
                : >"$normalized"
            fi

            bytes="$(file_size "$body")"
            printf 'expected_body_bytes=32 actual_body_bytes=%s\n' "$bytes"

            if (( curl_rc != 0 )); then
                printf '%s\n' 'curl transport failure'

                if [[ -s "$err" ]]; then
                    printf '%s\n' '--- curl stderr ---'
                    cat "$err"
                fi

                show_headers "$headers"
                show_binary_body "$body"
                return "$curl_rc"
            fi

            if [[ "$actual" != "206" ]]; then
                printf 'HTTP status mismatch: expected=206 actual=%s\n' "$actual"
                failed=1
            fi

            if ! grep -Eiq \
                '^Content-Range:[[:space:]]*bytes[[:space:]]+0-31/[0-9]+[[:space:]]*$' \
                "$normalized"
            then
                printf '%s\n' \
                    'Content-Range mismatch: expected "bytes 0-31/<total>"'
                failed=1
            fi

            if (( bytes != 32 )); then
                printf 'Range body-size mismatch: expected=32 actual=%s\n' "$bytes"
                failed=1
            fi

            if ! check_content_length_if_present "$normalized" "$body"; then
                failed=1
            fi

            if (( failed != 0 )); then
                if [[ -s "$err" ]]; then
                    printf '%s\n' '--- curl stderr ---'
                    cat "$err"
                fi

                show_headers "$headers"
                show_binary_body "$body"
                return 1
            fi

            return 0
        }

        # ------------------------------------------------------------
        # HTTP Range: expected 416 + Content-Range: bytes */<total>
        # ------------------------------------------------------------

        range_416_test()
        {
            local url="$1"

            local body="${TMP_DIR}/${CURRENT_ID}.body"
            local headers="${TMP_DIR}/${CURRENT_ID}.headers"
            local normalized="${TMP_DIR}/${CURRENT_ID}.headers.normalized"
            local err="${TMP_DIR}/${CURRENT_ID}.curl.stderr"

            local actual=""
            local curl_rc
            local bytes
            local failed=0

            print_curl_command \
                "$headers" \
                "$body" \
                -H 'Range: bytes=999999999999999999-' \
                "$url"

            actual="$(
                curl \
                    "${CURL_COMMON[@]}" \
                    -D "$headers" \
                    -o "$body" \
                    -w '%{http_code}' \
                    -H 'Range: bytes=999999999999999999-' \
                    "$url" \
                    2>"$err"
            )"
            curl_rc=$?

            printf 'expected_http=416 actual_http=%s curl_rc=%d\n' \
                "${actual:-<none>}" "$curl_rc"

            if [[ -f "$headers" ]]; then
                normalize_headers "$headers" "$normalized"
            else
                : >"$normalized"
            fi

            bytes="$(file_size "$body")"
            printf 'actual_body_bytes=%s\n' "$bytes"

            if (( curl_rc != 0 )); then
                printf '%s\n' 'curl transport failure'

                if [[ -s "$err" ]]; then
                    printf '%s\n' '--- curl stderr ---'
                    cat "$err"
                fi

                show_headers "$headers"
                show_body "$body"
                return "$curl_rc"
            fi

            if [[ "$actual" != "416" ]]; then
                printf 'HTTP status mismatch: expected=416 actual=%s\n' "$actual"
                failed=1
            fi

            if ! grep -Eiq \
                '^Content-Range:[[:space:]]*bytes[[:space:]]+\*/[0-9]+[[:space:]]*$' \
                "$normalized"
            then
                printf '%s\n' \
                    'Content-Range mismatch: expected "bytes */<total>"'
                failed=1
            fi

            # A 416 response body has no application-independent fixed size.
            # We still measure it, and if Content-Length exists verify that the
            # number of bytes curl actually received agrees with it.
            if ! check_content_length_if_present "$normalized" "$body"; then
                failed=1
            fi

            if (( failed != 0 )); then
                if [[ -s "$err" ]]; then
                    printf '%s\n' '--- curl stderr ---'
                    cat "$err"
                fi

                show_headers "$headers"
                show_body "$body"
                return 1
            fi

            return 0
        }

        # ------------------------------------------------------------
        # Structured correlation journal check
        # ------------------------------------------------------------

        structured_log_test()
        {
            local journal="${TMP_DIR}/${CURRENT_ID}.journal"
            local journal_err="${TMP_DIR}/${CURRENT_ID}.journal.stderr"
            local correlated="${TMP_DIR}/${CURRENT_ID}.correlated"

            local rc=0
            local privileged_retry_available=0
            local attempt
            local found=0
            local failed=0

            printf 'command: journalctl -u %q --since %q --no-pager -o cat\n' \
                "$SERVICE_UNIT" "$SINCE"

            # Give asynchronous logging a short, bounded opportunity to reach
            # journald. Total extra wait is at most ~1.5 seconds.
            for attempt in 1 2 3 4; do
                : >"$journal"
                : >"$journal_err"

                journalctl \
                    -u "$SERVICE_UNIT" \
                    --since "$SINCE" \
                    --no-pager \
                    -o cat \
                    >"$journal" 2>"$journal_err"
                rc=$?

                if (( rc == 0 )) &&
                   grep -F "$ATTEMPT" "$journal" >/dev/null 2>&1
                then
                    found=1
                    break
                fi

                # If the current user cannot see the relevant service journal,
                # retry non-interactively with cached sudo credentials.
                #
                # Do not hide a sudo password prompt inside redirected output:
                # if sudo authorization is unavailable, diagnostics below tell
                # the operator exactly what happened.
                if command -v sudo >/dev/null 2>&1 &&
                   sudo -n true >/dev/null 2>&1
                then
                    privileged_retry_available=1

                    : >"$journal"
                    : >"$journal_err"

                    sudo -n journalctl \
                        -u "$SERVICE_UNIT" \
                        --since "$SINCE" \
                        --no-pager \
                        -o cat \
                        >"$journal" 2>"$journal_err"
                    rc=$?

                    if (( rc == 0 )) &&
                       grep -F "$ATTEMPT" "$journal" >/dev/null 2>&1
                    then
                        found=1
                        break
                    fi
                fi

                if (( attempt < 4 )); then
                    sleep 0.5
                fi
            done

            if (( found == 0 )); then
                printf 'correlation log not found for attempt_id=%s\n' "$ATTEMPT"
                printf 'journalctl_rc=%d\n' "$rc"

                if [[ -s "$journal_err" ]]; then
                    printf '%s\n' '--- journalctl stderr ---'
                    cat "$journal_err"
                fi

                if (( privileged_retry_available == 0 )) &&
                   command -v sudo >/dev/null 2>&1
                then
                    printf '%s\n' \
                        'note: privileged journal retry was unavailable.' \
                        'If this user cannot read the service journal, authenticate with "sudo -v" and rerun.'
                fi

                printf '%s\n' '--- recent journal excerpt ---'
                if [[ -s "$journal" ]]; then
                    tail -n 80 "$journal"
                else
                    printf '[no visible journal entries since test start]\n'
                fi

                if (( rc != 0 )); then
                    return "$rc"
                fi

                return 1
            fi

            # Restrict field validation to the log record(s) containing this
            # run's unique attempt_id. This avoids accidentally passing because
            # unrelated log lines contain the other expected values.
            grep -F "$ATTEMPT" "$journal" >"$correlated"

            if ! grep -Eq \
                '"event"[[:space:]]*:[[:space:]]*"device_report_accepted"' \
                "$correlated"
            then
                printf '%s\n' 'missing field/value: event=device_report_accepted'
                failed=1
            fi

            if ! grep -Eq \
                "\"attempt_id\"[[:space:]]*:[[:space:]]*\"${ATTEMPT}\"" \
                "$correlated"
            then
                printf 'missing field/value: attempt_id=%s\n' "$ATTEMPT"
                failed=1
            fi

            if ! grep -Eq \
                '"target_version"[[:space:]]*:[[:space:]]*"1\.2\.5"' \
                "$correlated"
            then
                printf '%s\n' 'missing field/value: target_version=1.2.5'
                failed=1
            fi

            if ! grep -Eq \
                '"build_id"[[:space:]]*:[[:space:]]*"r4-vm-report-validation"' \
                "$correlated"
            then
                printf '%s\n' \
                    'missing field/value: build_id=r4-vm-report-validation'
                failed=1
            fi

            if ! grep -Eq \
                '"agent_state"[[:space:]]*:[[:space:]]*"REPORT_SUCCESS"' \
                "$correlated"
            then
                printf '%s\n' 'missing field/value: agent_state=REPORT_SUCCESS'
                failed=1
            fi

            if ! grep -Eq \
                '"error_code"[[:space:]]*:[[:space:]]*"NONE"' \
                "$correlated"
            then
                printf '%s\n' 'missing field/value: error_code=NONE'
                failed=1
            fi

            if (( failed != 0 )); then
                printf '%s\n' '--- correlated journal record(s) ---'
                head -c 8192 "$correlated"
                printf '\n'
                return 1
            fi

            return 0
        }

        # ------------------------------------------------------------
        # Tests
        # ------------------------------------------------------------

        run_test \
            "localhost manifest 200" \
            http_code_test \
            200 \
            "${LOCAL_BASE}/manifest.json"

        # LAN testing is intentionally GET-only.
        #
        # Replaying the entire suite over LAN would POST a second set of
        # production device reports without adding much network-path evidence.
        # A successful manifest request already proves application-level HTTP
        # reachability over the LAN address, separately from localhost.
        if [[ -n "$LAN_BASE" ]]; then
            run_test \
                "LAN manifest 200 (${LAN_BASE})" \
                http_code_test \
                200 \
                "${LAN_BASE}/manifest.json"
        else
            printf '%s\n' \
                'LAN manifest: SKIP (set LAN_IP or LAN_BASE to test the real LAN path)'
        fi

        run_test \
            "legacy report 204" \
            http_code_test \
            204 \
            -H 'Content-Type: application/json' \
            --data-binary "{
              \"device_id\":\"r4-vm-legacy\",
              \"current_version\":\"1.2.4\",
              \"status\":\"idle\",
              \"timestamp\":\"$TS\"
            }" \
            "${LOCAL_BASE}/device/report"

        run_test \
            "extended report 204" \
            http_code_test \
            204 \
            -H 'Content-Type: application/json' \
            --data-binary "{
              \"device_id\":\"r4-vm-extended\",
              \"current_version\":\"1.2.4\",
              \"status\":\"success\",
              \"timestamp\":\"$TS\",
              \"attempt_id\":\"$ATTEMPT\",
              \"target_version\":\"1.2.5\",
              \"build_id\":\"$BUILD_ID\",
              \"agent_state\":\"REPORT_SUCCESS\",
              \"error_code\":\"NONE\",
              \"timestamp_valid\":true,
              \"uptime_ms\":123456
            }" \
            "${LOCAL_BASE}/device/report"

        run_test \
            "invalid schema 422" \
            http_code_test \
            422 \
            -H 'Content-Type: application/json' \
            --data-binary "{
              \"device_id\":\"r4-invalid\",
              \"status\":\"idle\",
              \"timestamp\":\"$TS\"
            }" \
            "${LOCAL_BASE}/device/report"

        run_test \
            "malformed JSON 400" \
            http_code_test \
            400 \
            -H 'Content-Type: application/json' \
            --data-binary '{"device_id":' \
            "${LOCAL_BASE}/device/report"

        run_test \
            "Range 206 / Content-Range / exact 32 bytes" \
            range_206_test \
            "${LOCAL_BASE}/update.raucb"

        run_test \
            "malformed Range 400" \
            http_code_test \
            400 \
            -H 'Range: bytes=abc' \
            "${LOCAL_BASE}/update.raucb"

        run_test \
            "unsatisfiable Range 416 / Content-Range" \
            range_416_test \
            "${LOCAL_BASE}/update.raucb"

        run_test \
            "structured correlation logging" \
            structured_log_test

        # ------------------------------------------------------------
        # Aggregate result
        # ------------------------------------------------------------

        printf '\n%s\n' \
            "============================================================"

        if (( ${#FAIL_NAMES[@]} == 0 )); then
            printf '%s\n' \
                "LIVE SERVER VALIDATION GATE: PASS" \
                "============================================================"
            return 0
        fi

        printf 'LIVE SERVER VALIDATION GATE: FAIL (%d failed test(s))\n' \
            "${#FAIL_NAMES[@]}"

        local i
        for ((i = 0; i < ${#FAIL_NAMES[@]}; i++)); do
            printf 'FAIL: %s (rc=%s)\n' \
                "${FAIL_NAMES[$i]}" \
                "${FAIL_RCS[$i]}"
        done

        printf '%s\n' \
            "============================================================"

        return 1
    }

    main

) || TEST_RC=$?

# This line is deliberately outside the validation subshell.
#
# If pasted directly or sourced, report the rc but leave the current shell
# alive and return a successful status from this outer wrapper.
#
# If executed normally via "bash ./script.sh", make the child script's process
# status equal TEST_RC. The exit occurs only inside another child subshell; it
# cannot terminate the invoking interactive terminal.
if [[ "${BASH_SOURCE[0]:-}" == "$0" ]]; then
    ( exit "$TEST_RC" )
else
    printf 'TEST_RC=%d; current shell remains active.\n' "$TEST_RC"
fi