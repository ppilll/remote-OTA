#!/usr/bin/env bash

set -uo pipefail


###############################################################################
# EdgeGuard Server host_app deployment / app -> host_app migration
#
# Responsibilities:
#   - validate local host_app source
#   - validate production Python environment
#   - synchronize Python dependencies
#   - preserve the currently active OTA release metadata
#   - preflight host_app with production interpreter/service user
#   - deploy host_app to /opt/edgeguard-ota/host_app
#   - switch systemd from app.main:app to host_app.main:app
#   - restart and verify the server
#   - prove that deployment did NOT change the active OTA manifest
#   - rollback automatically if deployment/runtime verification fails
#
# Intentionally NOT touched:
#   - /opt/edgeguard-ota/artifacts
#   - published RAUC bundles
#   - current OTA release/version/build/artifact selection
#
# Release switching belongs ONLY to:
#   publish-r3-candidate.sh
#
# Usage:
#   bash deploy-host-app.sh
#
# Optional laboratory IP override:
#   EDGEGUARD_SERVER_IP=192.168.77.1 bash deploy-host-app.sh
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
RELEASE_SNAPSHOT=""
PRE_MANIFEST=""
RUNTIME_MANIFEST=""

HAD_HOST_APP=0
HAD_DROPIN=0
APP_SWITCHED=0
DROPIN_CHANGED=0

CURRENT_APP_KIND=""
CURRENT_CONFIG=""
PRE_MANIFEST_STATUS=""
RUNTIME_MANIFEST_STATUS=""


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

    if [ -n "${RELEASE_SNAPSHOT:-}" ]; then
        rm -f "$RELEASE_SNAPSHOT" >/dev/null 2>&1 || true
    fi

    if [ -n "${PRE_MANIFEST:-}" ]; then
        rm -f "$PRE_MANIFEST" >/dev/null 2>&1 || true
    fi

    if [ -n "${RUNTIME_MANIFEST:-}" ]; then
        rm -f "$RUNTIME_MANIFEST" >/dev/null 2>&1 || true
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
# Snapshot current production release metadata
#
# Preserve ONLY deployment/runtime release selection:
#
#   version
#   build_id
#   artifact_path
#
# Protocol/application source itself still comes from the repository.
###############################################################################

snapshot_release_config()
{
    local source_config="$1"

    sudo python3 - "$source_config" >"$RELEASE_SNAPSHOT" <<'PY'
from pathlib import Path
import json
import re
import sys

path = Path(sys.argv[1])

try:
    text = path.read_text(encoding="utf-8")
except OSError as exc:
    raise SystemExit(
        "cannot read current production config {}: {}".format(path, exc)
    )

specs = {
    "version": (
        r'(?m)^(    version: str = "([^"]+)")$',
        2,
    ),
    "build_id": (
        r'(?m)^(    build_id: str = "([^"]+)")$',
        2,
    ),
    "artifact_path": (
        r'(?m)^(    artifact_path: Path = .+)$',
        None,
    ),
}

result = {}

for name, (pattern, value_group) in specs.items():
    matches = list(re.finditer(pattern, text))

    if len(matches) != 1:
        raise SystemExit(
            "{} assignment count={} in {}".format(
                name,
                len(matches),
                path,
            )
        )

    match = matches[0]

    result[name + "_line"] = match.group(1)

    if value_group is not None:
        result[name] = match.group(value_group)

if not result["version"]:
    raise SystemExit("empty production version")

if not result["build_id"]:
    raise SystemExit("empty production build_id")

print(json.dumps(result, sort_keys=True))
PY
}


###############################################################################
# Inject preserved release state into a new config.py.
###############################################################################

apply_release_snapshot_user()
{
    local target_config="$1"

    python3 - \
        "$RELEASE_SNAPSHOT" \
        "$target_config" <<'PY'
from pathlib import Path
import json
import os
import re
import stat
import sys

snapshot_path = Path(sys.argv[1])
target_path = Path(sys.argv[2])

snapshot = json.loads(
    snapshot_path.read_text(encoding="utf-8")
)

text = target_path.read_text(encoding="utf-8")

updates = [
    (
        r'(?m)^    version: str = "[^"]+"$',
        snapshot["version_line"],
        "version",
    ),
    (
        r'(?m)^    build_id: str = "[^"]+"$',
        snapshot["build_id_line"],
        "build_id",
    ),
    (
        r'(?m)^    artifact_path: Path = .+$',
        snapshot["artifact_path_line"],
        "artifact_path",
    ),
]

for pattern, replacement, name in updates:
    text, count = re.subn(
        pattern,
        lambda match, replacement=replacement: replacement,
        text,
    )

    if count != 1:
        raise SystemExit(
            "{} target assignment count={}".format(
                name,
                count,
            )
        )

st = target_path.stat()

tmp = target_path.with_name(
    target_path.name + ".release-preserve.tmp"
)

with open(tmp, "w", encoding="utf-8") as f:
    f.write(text)
    f.flush()
    os.fsync(f.fileno())

os.chmod(
    tmp,
    stat.S_IMODE(st.st_mode),
)

os.replace(
    tmp,
    target_path,
)

print("RELEASE_CONFIG_INJECT=PASS")
PY
}


apply_release_snapshot_root()
{
    local target_config="$1"

    sudo python3 - \
        "$RELEASE_SNAPSHOT" \
        "$target_config" <<'PY'
from pathlib import Path
import json
import os
import re
import stat
import sys

snapshot_path = Path(sys.argv[1])
target_path = Path(sys.argv[2])

snapshot = json.loads(
    snapshot_path.read_text(encoding="utf-8")
)

text = target_path.read_text(encoding="utf-8")

updates = [
    (
        r'(?m)^    version: str = "[^"]+"$',
        snapshot["version_line"],
        "version",
    ),
    (
        r'(?m)^    build_id: str = "[^"]+"$',
        snapshot["build_id_line"],
        "build_id",
    ),
    (
        r'(?m)^    artifact_path: Path = .+$',
        snapshot["artifact_path_line"],
        "artifact_path",
    ),
]

for pattern, replacement, name in updates:
    text, count = re.subn(
        pattern,
        lambda match, replacement=replacement: replacement,
        text,
    )

    if count != 1:
        raise SystemExit(
            "{} target assignment count={}".format(
                name,
                count,
            )
        )

st = target_path.stat()

tmp = target_path.with_name(
    target_path.name + ".release-preserve.tmp"
)

with open(tmp, "w", encoding="utf-8") as f:
    f.write(text)
    f.flush()
    os.fsync(f.fileno())

os.chmod(
    tmp,
    stat.S_IMODE(st.st_mode),
)

os.replace(
    tmp,
    target_path,
)

print("RELEASE_CONFIG_INJECT=PASS")
PY
}


###############################################################################
# Verify the activated production config still contains the exact release
# assignments captured before deployment.
###############################################################################

verify_release_snapshot_root()
{
    local target_config="$1"

    sudo python3 - \
        "$RELEASE_SNAPSHOT" \
        "$target_config" <<'PY'
from pathlib import Path
import json
import re
import sys

snapshot_path = Path(sys.argv[1])
target_path = Path(sys.argv[2])

snapshot = json.loads(
    snapshot_path.read_text(encoding="utf-8")
)

text = target_path.read_text(encoding="utf-8")

checks = [
    (
        "version",
        r'(?m)^    version: str = "[^"]+"$',
        snapshot["version_line"],
    ),
    (
        "build_id",
        r'(?m)^    build_id: str = "[^"]+"$',
        snapshot["build_id_line"],
    ),
    (
        "artifact_path",
        r'(?m)^    artifact_path: Path = .+$',
        snapshot["artifact_path_line"],
    ),
]

for name, pattern, expected in checks:
    matches = re.findall(pattern, text)

    if len(matches) != 1:
        raise SystemExit(
            "{} assignment count={} after activation".format(
                name,
                len(matches),
            )
        )

    actual = matches[0]

    if actual != expected:
        raise SystemExit(
            "{} changed during deployment:\n"
            "actual  = {!r}\n"
            "expected= {!r}".format(
                name,
                actual,
                expected,
            )
        )

print("ACTIVE_RELEASE_CONFIG_PRESERVED=PASS")
PY
}


###############################################################################
# Compare pre-deployment and post-deployment HTTP manifests semantically.
###############################################################################

verify_manifest_preserved()
{
    python3 - \
        "$PRE_MANIFEST_STATUS" \
        "$PRE_MANIFEST" \
        "$RUNTIME_MANIFEST_STATUS" \
        "$RUNTIME_MANIFEST" <<'PY'
import json
import sys

(
    before_status,
    before_path,
    after_status,
    after_path,
) = sys.argv[1:]

if before_status != after_status:
    raise SystemExit(
        "manifest HTTP status changed: {} -> {}".format(
            before_status,
            after_status,
        )
    )

if before_status == "204":
    print("ACTIVE_RELEASE_MANIFEST_PRESERVED=PASS")
    raise SystemExit(0)

if before_status != "200":
    raise SystemExit(
        "unsupported preserved manifest status: {}".format(
            before_status
        )
    )

with open(before_path, encoding="utf-8") as f:
    before = json.load(f)

with open(after_path, encoding="utf-8") as f:
    after = json.load(f)

if before != after:
    print("manifest before =", before)
    print("manifest after  =", after)
    raise SystemExit(
        "active OTA manifest changed during host_app deployment"
    )

print("ACTIVE_RELEASE_MANIFEST_PRESERVED=PASS")
PY
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


    if [ "$APP_SWITCHED" -eq 1 ]; then

        echo "Restoring previous application..."

        sudo rm -rf "$PROD_APP"

        if [ "$HAD_HOST_APP" -eq 1 ]; then

            if sudo test -d "$PREVIOUS_APP"; then

                sudo mv -T \
                    "$PREVIOUS_APP" \
                    "$PROD_APP" \
                    || true

            else

                echo "CRITICAL: previous host_app backup missing"

            fi

        else

            echo "Legacy $PROD/app retained for rollback."

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


LOG="$(
    mktemp /tmp/edgeguard-host-app-deploy.XXXXXX.log
)" || fail "cannot create temporary log"

STAGE="$(
    mktemp -d /tmp/edgeguard-host-app-preflight.XXXXXX
)" || fail "cannot create temporary preflight directory"

DROPIN_BACKUP="$(
    mktemp /tmp/edgeguard-host-app-systemd.XXXXXX
)" || fail "cannot create temporary systemd backup"

RELEASE_SNAPSHOT="$(
    mktemp /tmp/edgeguard-host-app-release.XXXXXX.json
)" || fail "cannot create release snapshot"

PRE_MANIFEST="$(
    mktemp /tmp/edgeguard-host-app-manifest-before.XXXXXX.json
)" || fail "cannot create pre-deployment manifest file"

RUNTIME_MANIFEST="$(
    mktemp /tmp/edgeguard-host-app-manifest-after.XXXXXX.json
)" || fail "cannot create runtime manifest file"


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

for cmd in \
    systemctl \
    curl \
    python3 \
    ip
do
    command -v "$cmd" >/dev/null 2>&1 ||
        fail "required command missing: $cmd"
done

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

    CURRENT_APP_KIND="host_app"
    CURRENT_CONFIG="$PROD/host_app/config.py"

    echo "current systemd application: host_app.main:app"

elif grep -Eq \
    '(^|[[:space:]=])app\.main:app([[:space:]]|$)' \
    <<<"$UNIT_TEXT"
then

    CURRENT_APP_KIND="app"
    CURRENT_CONFIG="$PROD/app/config.py"

    echo "current systemd application: app.main:app"
    echo "migration required: YES"

else

    echo
    echo "Current ExecStart:"
    grep '^ExecStart=' <<<"$UNIT_TEXT" || true

    fail "unknown systemd application entry"

fi

sudo test -f "$CURRENT_CONFIG" ||
    fail "active production config missing: $CURRENT_CONFIG"

echo "active production config = $CURRENT_CONFIG"
echo "systemd source contract: PASS"


###############################################################################
# 4. Preserve active OTA release state
###############################################################################

echo
echo "=== 4. Preserve active OTA release state ==="

snapshot_release_config "$CURRENT_CONFIG" ||
    fail "cannot snapshot current active release configuration"

echo "--- preserved release configuration ---"

python3 - "$RELEASE_SNAPSHOT" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)

print("version       =", data["version"])
print("build_id      =", data["build_id"])
print("artifact_path =", data["artifact_path_line"])
PY


if ! PRE_MANIFEST_STATUS="$(
    curl \
        --noproxy '*' \
        --silent \
        --show-error \
        --connect-timeout 2 \
        --max-time 5 \
        --output "$PRE_MANIFEST" \
        --write-out '%{http_code}' \
        http://127.0.0.1:8000/manifest.json
)"; then

    fail "cannot snapshot current runtime manifest"

fi

case "$PRE_MANIFEST_STATUS" in

    200)
        test -s "$PRE_MANIFEST" ||
            fail "current manifest returned HTTP 200 with empty body"

        echo "current runtime manifest HTTP 200: PASS"
        cat "$PRE_MANIFEST"
        echo
        ;;

    204)
        echo "current runtime manifest HTTP 204: PASS"
        ;;

    *)
        fail \
            "current runtime manifest returned HTTP $PRE_MANIFEST_STATUS"
        ;;

esac

echo "ACTIVE_RELEASE_SNAPSHOT=PASS"


###############################################################################
# 5. Production dependencies
###############################################################################

echo
echo "=== 5. Synchronize production dependencies ==="

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
# 6. Preflight using production interpreter
###############################################################################

echo
echo "=== 6. Production-interpreter preflight ==="

mkdir -p "$STAGE/host_app" ||
    fail "cannot create staged host_app"

cp -a \
    "$LOCAL_APP/." \
    "$STAGE/host_app/" \
    || fail "cannot copy host_app into preflight staging"


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


apply_release_snapshot_user \
    "$STAGE/host_app/config.py" \
    || fail "cannot inject active release into preflight config"


chmod -R a+rX "$STAGE" ||
    fail "cannot make staged source readable"


: > "$LOG"


if (
    cd "$STAGE" || exit 1

    exec sudo -u "$SERVICE_USER" \
        env \
            PYTHONDONTWRITEBYTECODE=1 \
        "$PROD/.venv/bin/python" -
) >"$LOG" 2>&1 <<'PY'

from pathlib import Path
import sys

import host_app
import host_app.config
import host_app.main
import host_app.models


expected_root = Path.cwd().resolve()
expected_app = (expected_root / "host_app").resolve()


print("cwd          =", expected_root)
print("sys.path[0]  =", sys.path[0])
print("expected_app =", expected_app)


modules = (
    host_app,
    host_app.config,
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


rm -rf "$STAGE"
STAGE=""


###############################################################################
# 7. Stage production host_app
###############################################################################

echo
echo "=== 7. Stage production host_app ==="

sudo rm -rf "$NEW_APP"


run_quiet \
    "copy new host_app" \
    sudo cp -a \
        "$LOCAL_APP" \
        "$NEW_APP" \
    || fail "cannot stage new host_app"


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


apply_release_snapshot_root \
    "$NEW_APP/config.py" \
    || fail "cannot preserve active release in staged production host_app"


run_quiet \
    "normalize host_app ownership" \
    sudo chown -R root:root "$NEW_APP" \
    || fail "cannot normalize host_app ownership"


echo "production host_app staging: PASS"
echo "active release injection: PASS"


###############################################################################
# 8. Activate host_app source
###############################################################################

echo
echo "=== 8. Activate host_app source ==="


if [ "$HAD_HOST_APP" -eq 1 ]; then

    sudo rm -rf "$PREVIOUS_APP" ||
        fail "cannot remove stale host_app.previous"

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


verify_release_snapshot_root \
    "$PROD_APP/config.py" \
    || {
        rollback
        fail "active release metadata changed during host_app activation"
    }


echo "new production host_app: PASS"
echo "active release config preserved: PASS"


###############################################################################
# 9. Switch systemd to host_app.main:app
###############################################################################

echo
echo "=== 9. Switch systemd to host_app.main:app ==="


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
# 10. Verify effective systemd configuration
###############################################################################

echo
echo "=== 10. Effective systemd configuration ==="


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
# 11. Restart server
###############################################################################

echo
echo "=== 11. Restart server ==="


: > "$LOG"

if ! sudo systemctl restart "$SERVICE" >"$LOG" 2>&1; then

    echo "new server restart: FAIL"
    cat "$LOG"

    rollback

    fail "new host_app deployment rejected"

fi


echo "service restart: PASS"


###############################################################################
# 12. Runtime HTTP + active-release preservation verification
###############################################################################

echo
echo "=== 12. Runtime verification ==="


READY=0

for attempt in $(seq 1 20)
do

    if systemctl is-active --quiet "$SERVICE"; then

        if RUNTIME_MANIFEST_STATUS="$(
            curl \
                --noproxy '*' \
                --silent \
                --show-error \
                --connect-timeout 2 \
                --max-time 5 \
                --output "$RUNTIME_MANIFEST" \
                --write-out '%{http_code}' \
                http://127.0.0.1:8000/manifest.json \
                2>"$LOG"
        )"
        then

            case "$RUNTIME_MANIFEST_STATUS" in
                200|204)
                    READY=1
                    break
                    ;;
            esac

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


if ! verify_manifest_preserved; then

    echo
    echo "Active OTA manifest changed during deployment."

    rollback

    fail "host_app deployment mutated active OTA release"

fi


echo "active OTA manifest preserved: PASS"


###############################################################################
# 13. Laboratory endpoint verification
###############################################################################

echo
echo "=== 13. Laboratory endpoint verification ==="


LAB_IP="${EDGEGUARD_SERVER_IP:-192.168.77.1}"


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
    # Network identity is an environment issue.
    # Do not roll back a successfully validated host_app deployment.
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
# 14. Verify systemd runtime command
###############################################################################

echo
echo "=== 14. Runtime command provenance ==="


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

echo
echo "Active OTA release:"
python3 - "$RELEASE_SNAPSHOT" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)

print("  version  =", data["version"])
print("  build_id =", data["build_id"])
PY

echo
echo "Active release preservation:"
echo "  config.py = PASS"
echo "  manifest  = PASS"

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