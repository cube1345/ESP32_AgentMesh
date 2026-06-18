#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECRETS_FILE="${ROOT_DIR}/main/espagent_secrets.h"
BACKUP_FILE="${TMPDIR:-/tmp}/espagent_secrets.h.flash_roles_usb0_3.$$"

IDF_PATH="${IDF_PATH:-/home/cube/WorkSpace/ESP/esp-idf}"
IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-/home/cube/.espressif/python_env/idf6.1_py3.13_env}"
IDF_PYTHON="${IDF_PYTHON:-${IDF_PYTHON_ENV_PATH}/bin/python}"
export ESP_IDF_VERSION="${ESP_IDF_VERSION:-6.1.0}"
export IDF_PATH
export IDF_PYTHON_ENV_PATH
export PATH="${IDF_PYTHON_ENV_PATH}/bin:${IDF_PATH}/tools:${PATH}"

PORTS=(
  "/dev/ttyUSB0"
  "/dev/ttyUSB1"
  "/dev/ttyUSB2"
  "/dev/ttyUSB3"
)

NODE_IDS=(
  "esp32s3-coordinator-01"
  "esp32s3-sensor-01"
  "esp32s3-control-01"
  "esp32s3-guardian-01"
)

NODE_ROLES=(
  "coordinator_agent"
  "sensor_agent"
  "control_agent"
  "guardian_agent"
)

NODE_CAPABILITIES=(
  "coordinator,communication,llm,dispatch,timeline,alerts"
  "sensor,telemetry,environment,air_quality,light,presence"
  "control,gpio,rgb,servo,relay,actuator"
  "guardian,security,policy,privacy,audit,watchdog,stateboard"
)

NODE_RESPONSIBILITIES=(
  "receive user messages, call LLM, plan dispatch, publish timeline, and notify users"
  "read environment sensors and publish telemetry for coordinator and display terminals"
  "execute whitelisted hardware actions after schema validation and tool guard checks"
  "enforce policy decisions, audit OutputMessages, watch node health, and protect private data"
)

SELECTED_INDICES=("$@")
if [[ "${#SELECTED_INDICES[@]}" -eq 0 ]]; then
  SELECTED_INDICES=(0 1 2 3)
fi

require_file() {
  local path="$1"
  if [[ ! -e "${path}" ]]; then
    echo "ERROR: required path does not exist: ${path}" >&2
    echo "ERROR: ESP32-S3 port not detected. This script only flashes /dev/ttyUSB0-3 and will not use /dev/ttyACM*." >&2
    exit 1
  fi
}

require_fixed_usb_port() {
  local index="$1"
  local port="$2"
  local expected="/dev/ttyUSB${index}"

  if [[ "$port" != "$expected" ]]; then
    echo "ERROR: refusing to flash ${port}; expected ${expected}" >&2
    echo "ERROR: /dev/ttyACM* is intentionally ignored for ESP32-S3 role flashing." >&2
    exit 1
  fi
  require_file "$port"
}

set_profile() {
  local node_id="$1"
  local node_role="$2"
  local capabilities="$3"
  local responsibilities="$4"

  "${PYTHON:-python3}" - "$SECRETS_FILE" "$node_id" "$node_role" "$capabilities" "$responsibilities" <<'PY'
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
node_id, node_role, capabilities, responsibilities = sys.argv[2:6]
text = path.read_text()

replacements = {
    "ESPAGENT_SECRET_NODE_ID": node_id,
    "ESPAGENT_SECRET_NODE_ROLE": node_role,
    "ESPAGENT_SECRET_NODE_CAPABILITIES": capabilities,
    "ESPAGENT_SECRET_NODE_RESPONSIBILITIES": responsibilities,
}

for key, value in replacements.items():
    pattern = rf'(#define\s+{re.escape(key)}\s+)".*?"'
    text, count = re.subn(pattern, rf'\1"{value}"', text, count=1)
    if count != 1:
        raise SystemExit(f"ERROR: failed to replace {key}")

path.write_text(text)
PY
}

flash_one() {
  local index="$1"
  local port="${PORTS[$index]}"
  local node_id="${NODE_IDS[$index]}"
  local node_role="${NODE_ROLES[$index]}"
  local capabilities="${NODE_CAPABILITIES[$index]}"
  local responsibilities="${NODE_RESPONSIBILITIES[$index]}"

  require_file "$port"
  echo
  echo "==> Flashing USB${index}: ${port} -> ${node_id} / ${node_role}"
  set_profile "$node_id" "$node_role" "$capabilities" "$responsibilities"

  if [[ "${ESPAGENT_FLASH_FULLCLEAN:-0}" == "1" ]]; then
    (cd "$ROOT_DIR" && "$IDF_PYTHON" "$IDF_PATH/tools/idf.py" fullclean)
  fi

  (cd "$ROOT_DIR" && "$IDF_PYTHON" "$IDF_PATH/tools/idf.py" -p "$port" flash)
}

restore_coordinator_profile() {
  set_profile \
    "esp32s3-coordinator-01" \
    "coordinator_agent" \
    "coordinator,communication,llm,dispatch,timeline,alerts" \
    "receive user messages, call LLM, plan dispatch, publish timeline, and notify users"
}

cleanup_on_error() {
  if [[ -f "$BACKUP_FILE" ]]; then
    cp "$BACKUP_FILE" "$SECRETS_FILE"
  fi
}

require_file "$SECRETS_FILE"
for i in "${SELECTED_INDICES[@]}"; do
  if [[ ! "$i" =~ ^[0-3]$ ]]; then
    echo "ERROR: role index must be 0, 1, 2, or 3; got ${i}" >&2
    exit 1
  fi
  require_fixed_usb_port "$i" "${PORTS[$i]}"
done

cp "$SECRETS_FILE" "$BACKUP_FILE"
trap cleanup_on_error ERR INT TERM

for i in "${SELECTED_INDICES[@]}"; do
  flash_one "$i"
done

restore_coordinator_profile
rm -f "$BACKUP_FILE"
trap - ERR INT TERM

echo
echo "OK: flashed role index(es): ${SELECTED_INDICES[*]}. main/espagent_secrets.h restored to coordinator_agent profile."
