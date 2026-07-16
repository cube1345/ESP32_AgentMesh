#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECRETS_FILE="${ROOT_DIR}/main/espagent_secrets.h"
BACKUP_FILE="${TMPDIR:-/tmp}/espagent_secrets.h.flash_roles_usb0_3.$$"

IDF_PATH="${IDF_PATH:-/home/cube/WorkSpace/ESP/esp-idf}"
IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-/home/cube/.espressif/python_env/idf6.1_py3.13_env}"
IDF_PYTHON="${IDF_PYTHON:-${IDF_PYTHON_ENV_PATH}/bin/python}"
ESPAGENT_FLASH_BAUD="${ESPAGENT_FLASH_BAUD:-}"
ESPAGENT_FLASH_MODE="${ESPAGENT_FLASH_MODE:-app}"
ESPAGENT_ERASE_BEFORE_FLASH="${ESPAGENT_ERASE_BEFORE_FLASH:-0}"
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
  "control,gpio,ws2812,status_light,servo,relay,actuator"
  "guardian,security,policy,privacy,audit,watchdog,stateboard"
)

NODE_RESPONSIBILITIES=(
  "receive user messages, call LLM, plan dispatch, publish timeline, and notify users"
  "read environment sensors and publish telemetry for coordinator and display terminals"
  "execute whitelisted hardware actions after schema validation and tool guard checks"
  "enforce policy decisions, audit OutputMessages, watch node health, and protect private data"
)

THINK_LED_GPIO_1=(
  "-1"
  "-1"
  "-1"
  "-1"
)

THINK_LED_GPIO_2=(
  "-1"
  "-1"
  "-1"
  "-1"
)

THINK_LED_GPIO_3=(
  "-1"
  "-1"
  "-1"
  "-1"
)

THINK_LED_ACTIVE_LEVEL=(
  "1"
  "1"
  "1"
  "1"
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

flash_target_for_mode() {
  case "$ESPAGENT_FLASH_MODE" in
    app)
      printf '%s' "app-flash"
      ;;
    full)
      printf '%s' "flash"
      ;;
    spiffs)
      printf '%s' "__spiffs_manual__"
      ;;
    *)
      echo "ERROR: unsupported ESPAGENT_FLASH_MODE=${ESPAGENT_FLASH_MODE}; use app, full, or spiffs" >&2
      exit 1
      ;;
  esac
}

spiffs_offset_hex() {
  awk -F',' '
    $1 !~ /^[[:space:]]*#/ {
      name=$1
      gsub(/[[:space:]]/, "", name)
      if (name == "spiffs") {
        offset=$4
        gsub(/[[:space:]]/, "", offset)
        print offset
        exit
      }
    }
  ' "$ROOT_DIR/partitions.csv"
}

set_profile() {
  local node_id="$1"
  local node_role="$2"
  local capabilities="$3"
  local responsibilities="$4"
  local think_led_1="$5"
  local think_led_2="$6"
  local think_led_3="$7"
  local think_led_active_level="$8"

  "${PYTHON:-python3}" - "$SECRETS_FILE" "$node_id" "$node_role" "$capabilities" "$responsibilities" "$think_led_1" "$think_led_2" "$think_led_3" "$think_led_active_level" <<'PY'
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
(
    node_id,
    node_role,
    capabilities,
    responsibilities,
    think_led_1,
    think_led_2,
    think_led_3,
    think_led_active_level,
) = sys.argv[2:10]
text = path.read_text()

replacements = {
    "ESPAGENT_SECRET_NODE_ID": node_id,
    "ESPAGENT_SECRET_NODE_ROLE": node_role,
    "ESPAGENT_SECRET_NODE_CAPABILITIES": capabilities,
    "ESPAGENT_SECRET_NODE_RESPONSIBILITIES": responsibilities,
    "ESPAGENT_SECRET_THINK_LED_GPIO_1": think_led_1,
    "ESPAGENT_SECRET_THINK_LED_GPIO_2": think_led_2,
    "ESPAGENT_SECRET_THINK_LED_GPIO_3": think_led_3,
    "ESPAGENT_SECRET_THINK_LED_ACTIVE_LEVEL": think_led_active_level,
}

for key, value in replacements.items():
    if re.fullmatch(r"-?\d+", value):
        pattern = rf'(#define\s+{re.escape(key)}\s+)\(?-?\d+\)?'
        text, count = re.subn(pattern, rf'\g<1>{value}', text, count=1)
    else:
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
  local think_led_1="${THINK_LED_GPIO_1[$index]}"
  local think_led_2="${THINK_LED_GPIO_2[$index]}"
  local think_led_3="${THINK_LED_GPIO_3[$index]}"
  local think_led_active_level="${THINK_LED_ACTIVE_LEVEL[$index]}"
  local flash_target
  local flash_label
  flash_target="$(flash_target_for_mode)"
  flash_label="$flash_target"
  if [[ "$flash_target" == "__spiffs_manual__" ]]; then
    flash_label="spiffs"
  fi

  require_file "$port"
  echo
  echo "==> Flashing USB${index}: ${port} -> ${node_id} / ${node_role} (mode=${ESPAGENT_FLASH_MODE}, target=${flash_label})"
  set_profile "$node_id" "$node_role" "$capabilities" "$responsibilities" \
    "$think_led_1" "$think_led_2" "$think_led_3" "$think_led_active_level"

  if [[ "${ESPAGENT_FLASH_FULLCLEAN:-0}" == "1" ]]; then
    (cd "$ROOT_DIR" && "$IDF_PYTHON" "$IDF_PATH/tools/idf.py" fullclean)
  fi

  if [[ "$ESPAGENT_ERASE_BEFORE_FLASH" == "1" ]]; then
    if [[ "$ESPAGENT_FLASH_MODE" != "full" ]]; then
      echo "ERROR: ESPAGENT_ERASE_BEFORE_FLASH=1 requires ESPAGENT_FLASH_MODE=full" >&2
      exit 1
    fi
    echo "==> Erasing flash on ${port} before flashing"
    if [[ -n "$ESPAGENT_FLASH_BAUD" ]]; then
      (cd "$ROOT_DIR" && "$IDF_PYTHON" "$IDF_PATH/tools/idf.py" -p "$port" -b "$ESPAGENT_FLASH_BAUD" erase-flash)
    else
      (cd "$ROOT_DIR" && "$IDF_PYTHON" "$IDF_PATH/tools/idf.py" -p "$port" erase-flash)
    fi
  fi

  if [[ "$flash_target" == "__spiffs_manual__" ]]; then
    local spiffs_offset
    spiffs_offset="$(spiffs_offset_hex)"
    if [[ -z "$spiffs_offset" ]]; then
      echo "ERROR: failed to resolve spiffs offset from partitions.csv" >&2
      exit 1
    fi

    (cd "$ROOT_DIR" && "$IDF_PYTHON" "$IDF_PATH/tools/idf.py" build)

    if [[ -n "$ESPAGENT_FLASH_BAUD" ]]; then
      (cd "$ROOT_DIR" && "$IDF_PYTHON" -m esptool --chip esp32s3 -p "$port" -b "$ESPAGENT_FLASH_BAUD" --before default-reset --after hard-reset write-flash "$spiffs_offset" build/spiffs.bin)
    else
      (cd "$ROOT_DIR" && "$IDF_PYTHON" -m esptool --chip esp32s3 -p "$port" --before default-reset --after hard-reset write-flash "$spiffs_offset" build/spiffs.bin)
    fi
  elif [[ -n "$ESPAGENT_FLASH_BAUD" ]]; then
    (cd "$ROOT_DIR" && "$IDF_PYTHON" "$IDF_PATH/tools/idf.py" -p "$port" -b "$ESPAGENT_FLASH_BAUD" "$flash_target")
  else
    (cd "$ROOT_DIR" && "$IDF_PYTHON" "$IDF_PATH/tools/idf.py" -p "$port" "$flash_target")
  fi
}

restore_coordinator_profile() {
  set_profile \
    "esp32s3-coordinator-01" \
    "coordinator_agent" \
    "coordinator,communication,llm,dispatch,timeline,alerts" \
    "receive user messages, call LLM, plan dispatch, publish timeline, and notify users" \
    "-1" \
    "-1" \
    "-1" \
    "1"
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
echo "OK: flashed role index(es): ${SELECTED_INDICES[*]} with mode=${ESPAGENT_FLASH_MODE}. main/espagent_secrets.h restored to coordinator_agent profile."
