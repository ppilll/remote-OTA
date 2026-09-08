#!/usr/bin/env bash

set -uo pipefail


###############################################################################
# EdgeGuard Server host_app deployment / app -> host_app migration
#
# Responsibilities:
#   - validate local host_app source
#   - validate production Python environment
#   - synchronize Python dependencies
#   - preflight host_app with production interpreter/service user
#   - deploy host_app to /opt/edgeguard-ota/host_app
#   - switch systemd from app.main:app to host_app.main:app
#   - restart and verify the server
#   - rollback automatically if restart/HTTP verification fails
#
# Intentionally NOT touched:
#   - /opt/edgeguard-ota/artifacts
#   - published RAUC bundles
#   - current OTA release/version
#
# First migration:
#   - /opt/edgeguard-ota/app is intentionally retained for rollback
#
# Subsequent deployments:
#   - only one /opt/edgeguard-ota/host_app.previous is retained
#
# Usage:
#   bash deploy-host-app.sh
#
# Do NOT run with:
#   source deploy-host-app.sh
###############################################################################


REPO="${EDGEGUARD_REPO:-/home/liu2004/work/EG_OTA}"
PROD="${EDGEGUARD_SERVER_ROOT:-/opt/edgeguard-ota}"

SERVICE="${EDGEGUARD_SERVER_SERVICE:-edgeguard-ota.service}"
SERVICE_USER="${EDGEGUARD_SERVER_USER:-edgeguard-ota}"

LOCAL_APP="$REPO/host_app"
PROD_APP="$PROD/host_app"

NEW_APP="$PROD/.host_app.new.$$"
PREVIOUS_APP="$PROD/host_app.previous"

DROPIN_DIR="/etc/systemd/system/${SERVICE}.d"
DROPIN="$DROPIN_DIR/10-host-app.conf"

LOG=""
STAGE=""
DROPIN_BACKUP=""

HAD_HOST_APP=0
HAD_DROPIN=0
APP_SWITCHED=0
DROPIN_CHANGED=0


###############################################################################
# Helpers
###############################################################################

cleanup()
{
    if [ -n "${STAGE:-}" ]; then
        rm -rf "$STAGE" >/dev/null 2>&1 || true
    fi

    if [ -n "${LOG:-}" ]; then
        rm -f "$LOG" >/dev/null 2>&1 || true
    fi

    if [ -n "${DROPIN_BACKUP:-}" ]; then
        rm -f "$DROPIN_BACKUP" >/dev/null 2>&1 || true
    fi

    sudo rm -rf "$NEW_APP" >/dev/null 2>&1 || true
}


trap cleanup EXIT INT TERM


fail()
{
    echo
    echo "============================================================"
    echo "SERVER DEPLOYMENT: FAIL"
    echo "============================================================"
    echo "$*" >&2
    exit 1
}


run_quiet()
{
    local name="$1"
    shift

    : > "$LOG"

    if "$@" >"$LOG" 2>&1; then
        echo "$name: PASS"
        return 0
    fi

    local rc=$?

    echo
    echo "$name: FAIL (rc=$rc)"
    echo "-------------------- detail --------------------"
    cat "$LOG"
    echo "------------------------------------------------"

    return "$rc"
}


###############################################################################
# Rollback
###############################################################################

rollback()
{
    echo
    echo "============================================================"
    echo "ROLLBACK"
    echo "============================================================"

    #
    # Restore systemd configuration first.
    #
    if [ "$DROPIN_CHANGED" -eq 1 ]; then

        if [ "$HAD_DROPIN" -eq 1 ]; then

            echo "Restoring previous systemd drop-in..."

            if ! sudo install \
                -m 0644 \
                "$DROPIN_BACKUP" \
                "$DROPIN"
            then
                echo "CRITICAL: cannot restore previous systemd drop-in"
            fi

        else

            echo "Removing host_app systemd drop-in..."

            sudo rm -f "$DROPIN" || true

        fi

        sudo systemctl daemon-reload || true
    fi


    #
    # Restore Python application.
    #
    if [ "$APP_SWITCHED" -eq 1 ]; then

        echo "Restoring previous application..."

        sudo rm -rf "$PROD_APP"

        if [ "$HAD_HOST_APP" -eq 1 ]; then

            if sudo test -d "$PREVIOUS_APP"; then
                sudo mv -T \
                    "$PREVIOUS_APP" \
                    "$PROD_APP"
            else
                echo "CRITICAL: previous host_app backup missing"
            fi

        else

            #
            # First app -> host_app migration.
            #
            # Nothing needs to be moved back because the legacy:
            #
            #   /opt/edgeguard-ota/app
            #
            # was never modified.
            #
            echo "Legacy /opt/edgeguard-ota/app retained for rollback."

        fi
    fi


    echo "Restarting previous server..."

    if sudo systemctl restart "$SERVICE"; then
        echo "rollback service restart: PASS"
    else
        echo "CRITICAL: rollback service restart failed"

        systemctl status \
            --no-pager \
            --lines=40 \
            "$SERVICE" \
            || true
    fi
}


###############################################################################
# Start
###############################################################################

cd "$REPO" || fail "cannot cd to $REPO"


LOG="$(mktemp /tmp/edgeguard-host-app-deploy.XXXXXX.log)" ||
    fail "cannot create temporary log"

STAGE="$(mktemp -d /tmp/edgeguard-host-app-preflight.XXXXXX)" ||
    fail "cannot create temporary preflight directory"

DROPIN_BACKUP="$(mktemp /tmp/edgeguard-host-app-systemd.XXXXXX)" ||
    fail "cannot create temporary systemd backup"


echo "============================================================"
echo "EDGEGUARD SERVER APPLICATION DEPLOYMENT"
echo "============================================================"

echo "repository = $REPO"
echo "production = $PROD"
echo "service    = $SERVICE"


###############################################################################
# 1. Local source
###############################################################################

echo
echo "=== 1. Local host_app source ==="


test -d "$LOCAL_APP" ||
    fail "local host_app directory missing: $LOCAL_APP"

for file in \
    __init__.py \
    config.py \
    main.py \
    models.py
do
    test -f "$LOCAL_APP/$file" ||
        fail "local host_app file missing: $LOCAL_APP/$file"
done

echo "local host_app source: PASS"


###############################################################################
# 2. Production environment
###############################################################################

echo
echo "=== 2. Production environment ==="


sudo test -d "$PROD" ||
    fail "production root missing: $PROD"

sudo test -x "$PROD/.venv/bin/python" ||
    fail "production Python missing"

sudo test -x "$PROD/.venv/bin/uvicorn" ||
    fail "production uvicorn missing"

command -v systemctl >/dev/null 2>&1 ||
    fail "systemctl missing"

command -v curl >/dev/null 2>&1 ||
    fail "curl missing"


echo -n "production Python: "

sudo "$PROD/.venv/bin/python" --version ||
    fail "production Python cannot execute"


if sudo test -d "$PROD_APP"; then
    HAD_HOST_APP=1

    echo "production host_app: EXISTS"
    echo "deployment mode: host_app update"
else
    HAD_HOST_APP=0

    echo "production host_app: MISSING"

    if sudo test -d "$PROD/app"; then
        echo "legacy production app: EXISTS"
        echo "deployment mode: first app -> host_app migration"
    else
        fail \
            "neither $PROD/host_app nor $PROD/app exists"
    fi
fi


###############################################################################
# 3. Current systemd contract
###############################################################################

echo
echo "=== 3. Current systemd contract ==="


UNIT_TEXT="$(
    systemctl cat --no-pager "$SERVICE" 2>"$LOG"
)"
RC=$?

if [ "$RC" -ne 0 ]; then
    cat "$LOG"
    fail "cannot read $SERVICE"
fi


if grep -Eq \
    '(^|[[:space:]=])host_app\.main:app([[:space:]]|$)' \
    <<<"$UNIT_TEXT"
then

    echo "current systemd application: host_app.main:app"

elif grep -Eq \
    '(^|[[:space:]=])app\.main:app([[:space:]]|$)' \
    <<<"$UNIT_TEXT"
then

    echo "current systemd application: app.main:app"
    echo "migration required: YES"

else

    echo
    echo "Current ExecStart:"
    grep '^ExecStart=' <<<"$UNIT_TEXT" || true

    fail "unknown systemd application entry"

fi


echo "systemd source contract: PASS"


###############################################################################
# 4. Production dependencies
###############################################################################

echo
echo "=== 4. Synchronize production dependencies ==="


run_quiet \
    "pip requirements sync" \
    sudo "$PROD/.venv/bin/python" \
        -m pip install \
        --quiet \
        --disable-pip-version-check \
        --no-input \
        --no-cache-dir \
        -r "$REPO/requirements.txt" \
    || fail "production dependency synchronization failed"


run_quiet \
    "pip dependency check" \
    sudo "$PROD/.venv/bin/python" \
        -m pip check \
    || fail "production dependency check failed"


###############################################################################
# 5. Preflight using production interpreter
###############################################################################

echo
echo "=== 5. Production-interpreter preflight ==="


mkdir -p "$STAGE/host_app" ||
    fail "cannot create staged host_app"

cp -a \
    "$LOCAL_APP/." \
    "$STAGE/host_app/" \
    || fail "cannot copy host_app into preflight staging"


#
# Do not include local bytecode/cache.
#
find "$STAGE/host_app" \
    -type d \
    -name '__pycache__' \
    -prune \
    -exec rm -rf {} + \
    2>/dev/null \
    || true

find "$STAGE/host_app" \
    -type f \
    \( -name '*.pyc' -o -name '*.pyo' \) \
    -delete \
    2>/dev/null \
    || true


chmod -R a+rX "$STAGE" ||
    fail "cannot make staged source readable"


: > "$LOG"


#
# IMPORTANT:
#
# Run Python with STAGE as the current working directory.
#
# Production systemd uses:
#
#   WorkingDirectory=/opt/edgeguard-ota
#
# and imports:
#
#   host_app.main
#
# Therefore this preflight intentionally reproduces the same layout:
#
#   $STAGE/
#       host_app/
#
# We do NOT use PYTHONPATH here. This prevents the repository directory
# /home/liu2004/work/EG_OTA from winning module resolution.
#
if (
    cd "$STAGE" || exit 1

    exec sudo -u "$SERVICE_USER" \
        env \
            PYTHONDONTWRITEBYTECODE=1 \
        "$PROD/.venv/bin/python" -
) >"$LOG" 2>&1 <<'PY'

from pathlib import Path
import os
import sys

import host_app
import host_app.main
import host_app.models


expected_root = Path.cwd().resolve()
expected_app = (expected_root / "host_app").resolve()


print("cwd          =", expected_root)
print("sys.path[0]  =", sys.path[0])
print("expected_app =", expected_app)


modules = (
    host_app,
    host_app.main,
    host_app.models,
)


for module in modules:

    module_path = Path(module.__file__).resolve()

    print(
        "{:<20} = {}".format(
            module.__name__,
            module_path,
        )
    )

    if (
        module_path != expected_app
        and expected_app not in module_path.parents
    ):
        raise SystemExit(
            "{} loaded from unexpected source: {}".format(
                module.__name__,
                module_path,
            )
        )


print("PRODUCTION_INTERPRETER_HOST_APP=PASS")

PY
then

    echo "production interpreter host_app import: PASS"

else

    RC=$?

    echo
    echo "production interpreter host_app import: FAIL"
    echo "-------------------- detail --------------------"
    cat "$LOG"
    echo "------------------------------------------------"

    fail "preflight failed rc=$RC"

fi


#
# Preflight staging is no longer needed.
#
rm -rf "$STAGE"
STAGE=""


###############################################################################
# 6. Stage production host_app
###############################################################################

echo
echo "=== 6. Stage production host_app ==="


sudo rm -rf "$NEW_APP"


run_quiet \
    "copy new host_app" \
    sudo cp -a \
        "$LOCAL_APP" \
        "$NEW_APP" \
    || fail "cannot stage new host_app"


#
# Do not deploy developer bytecode/cache.
#
sudo find "$NEW_APP" \
    -type d \
    -name '__pycache__' \
    -prune \
    -exec rm -rf {} + \
    2>/dev/null \
    || true


sudo find "$NEW_APP" \
    -type f \
    \( -name '*.pyc' -o -name '*.pyo' \) \
    -delete \
    2>/dev/null \
    || true


run_quiet \
    "normalize host_app ownership" \
    sudo chown -R root:root "$NEW_APP" \
    || fail "cannot normalize host_app ownership"


echo "production host_app staging: PASS"


###############################################################################
# 7. Activate host_app source
###############################################################################

echo
echo "=== 7. Activate host_app source ==="


if [ "$HAD_HOST_APP" -eq 1 ]; then

    #
    # Only one backup is retained.
    #
    sudo rm -rf "$PREVIOUS_APP" ||
        fail "cannot remove stale host_app.previous"


    #
    # -T makes destination semantics unambiguous.
    #
    sudo mv -T \
        "$PROD_APP" \
        "$PREVIOUS_APP" \
        || fail "cannot backup current host_app"

fi


if ! sudo mv -T \
    "$NEW_APP" \
    "$PROD_APP"
then

    if [ "$HAD_HOST_APP" -eq 1 ] &&
       sudo test -d "$PREVIOUS_APP"
    then
        sudo mv -T \
            "$PREVIOUS_APP" \
            "$PROD_APP" \
            || true
    fi

    fail "cannot activate new host_app"
fi


APP_SWITCHED=1

echo "new production host_app: PASS"


###############################################################################
# 8. systemd host_app switch
###############################################################################

echo
echo "=== 8. Switch systemd to host_app.main:app ==="


sudo install \
    -d \
    -m 0755 \
    "$DROPIN_DIR" \
    || {
        rollback
        fail "cannot create systemd drop-in directory"
    }


if sudo test -f "$DROPIN"; then

    HAD_DROPIN=1

    sudo cat "$DROPIN" > "$DROPIN_BACKUP" ||
        {
            rollback
            fail "cannot backup existing systemd drop-in"
        }

else

    HAD_DROPIN=0

fi


#
# The original service currently contains:
#
# ExecStart=/opt/edgeguard-ota/.venv/bin/uvicorn \
#     app.main:app \
#     --host 0.0.0.0 \
#     --port 8000 \
#     --workers 1
#
# Reset ExecStart and replace it with the new package.
#
if ! sudo tee "$DROPIN" >/dev/null <<EOF
[Service]
ExecStart=
ExecStart=$PROD/.venv/bin/uvicorn host_app.main:app --host 0.0.0.0 --port 8000 --workers 1
EOF
then

    rollback
    fail "cannot install systemd host_app override"

fi


DROPIN_CHANGED=1


if ! sudo systemctl daemon-reload; then
    rollback
    fail "systemctl daemon-reload failed"
fi


echo "systemd host_app override: PASS"


###############################################################################
# 9. Verify effective systemd config before restart
###############################################################################

echo
echo "=== 9. Effective systemd configuration ==="


EFFECTIVE_UNIT="$(
    systemctl cat --no-pager "$SERVICE" 2>"$LOG"
)"
RC=$?

if [ "$RC" -ne 0 ]; then
    rollback
    cat "$LOG"
    fail "cannot read effective systemd unit"
fi


if ! grep -Eq \
    '(^|[[:space:]=])host_app\.main:app([[:space:]]|$)' \
    <<<"$EFFECTIVE_UNIT"
then

    rollback
    fail "effective unit does not contain host_app.main:app"

fi


echo "effective host_app.main:app: PASS"


###############################################################################
# 10. Restart server
###############################################################################

echo
echo "=== 10. Restart server ==="


: > "$LOG"

if ! sudo systemctl restart "$SERVICE" >"$LOG" 2>&1; then

    echo "new server restart: FAIL"
    cat "$LOG"

    rollback

    fail "new host_app deployment rejected"

fi


echo "service restart: PASS"


###############################################################################
# 11. Runtime HTTP verification
###############################################################################

echo
echo "=== 11. Runtime verification ==="


READY=0

for attempt in $(seq 1 20)
do

    if systemctl is-active --quiet "$SERVICE"; then

        if curl \
            --noproxy '*' \
            --silent \
            --show-error \
            --fail \
            --connect-timeout 2 \
            --max-time 5 \
            http://127.0.0.1:8000/manifest.json \
            -o /dev/null \
            2>"$LOG"
        then
            READY=1
            break
        fi

    fi

    sleep 1
done


if [ "$READY" -ne 1 ]; then

    echo
    echo "new host_app runtime verification: FAIL"

    echo
    echo "--- service status ---"

    systemctl status \
        --no-pager \
        --lines=40 \
        "$SERVICE" \
        || true

    echo
    echo "--- recent journal ---"

    journalctl \
        -u "$SERVICE" \
        --since '-2 minutes' \
        --no-pager \
        -n 60 \
        || true


    rollback

    fail "new host_app failed runtime verification"

fi


echo "$SERVICE active: PASS"
echo "HTTP /manifest.json: PASS"

###############################################################################
# 12. Laboratory endpoint verification
###############################################################################

echo
echo "=== 12. Laboratory endpoint verification ==="

LAB_IP="${EDGEGUARD_SERVER_IP:-10.52.46.50}"


if ip -4 -o addr show \
    | awk '{print $4}' \
    | cut -d/ -f1 \
    | grep -Fxq "$LAB_IP"
then

    echo "$LAB_IP assigned to this VM: PASS"

else

    echo "FAIL: $LAB_IP is not currently assigned to this VM"

    echo
    echo "Current IPv4 addresses:"

    ip -4 -br addr || true

    #
    # 注意：
    # 此处不 rollback host_app。
    #
    # IP 不存在是网络环境问题，
    # 并不意味着新 Server 程序本身部署失败。
    #
    fail "laboratory server IP unavailable"

fi


echo
echo "--- laboratory endpoint HTTP verification ---"


if curl \
    --noproxy '*' \
    --silent \
    --show-error \
    --fail \
    --connect-timeout 3 \
    --max-time 10 \
    "http://${LAB_IP}:8000/manifest.json" \
    -o /dev/null
then

    echo "HTTP http://${LAB_IP}:8000/manifest.json: PASS"

else

    fail "laboratory endpoint HTTP verification failed"

fi

###############################################################################
# 13. Verify systemd runtime command
###############################################################################

echo
echo "=== 12. Runtime command provenance ==="


RUNTIME_EXEC="$(
    systemctl show \
        "$SERVICE" \
        --property=ExecStart \
        --value
)"


if grep -Fq 'host_app.main:app' <<<"$RUNTIME_EXEC"; then

    echo "running command uses host_app.main:app: PASS"

else

    echo "ExecStart:"
    echo "$RUNTIME_EXEC"

    rollback

    fail "running service does not use host_app.main:app"

fi


###############################################################################
# Result
###############################################################################

echo
echo "============================================================"
echo "SERVER APPLICATION DEPLOYMENT: PASS"
echo "============================================================"

echo "production source = $PROD_APP"
echo "systemd entry     = host_app.main:app"

if [ "$HAD_HOST_APP" -eq 0 ]; then

    echo
    echo "Migration note:"
    echo "  legacy $PROD/app was intentionally retained"
    echo "  as the first-migration rollback source."

else

    echo
    echo "Previous release:"
    echo "  $PREVIOUS_APP"

fi

echo
echo "OTA artifact directory was not modified:"
echo "  $PROD/artifacts"

echo
echo "SERVER_DEPLOY_RC=0"