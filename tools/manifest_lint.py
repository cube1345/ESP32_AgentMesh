#!/usr/bin/env python3
"""Validate ESPAgent runtime hardware manifests without external dependencies."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


READ_PROTOCOLS = {"i2c", "uart", "modbus_rtu", "spi", "adc", "gpio_input"}
CONTROL_PROTOCOLS = {"gpio_output", "relay_control", "pwm_output", "ledc_pwm"}
PLANNED_PROTOCOLS = {"can_twai", "ble_gatt", "one_wire", "i2s_pdm", "rmt_ir", "usb_cdc", "sdmmc"}
CONTROL_RISKS = {"low_control", "medium_control", "high_control"}
I2C_DECODE_TYPES = {"raw_u8", "raw_u16_be", "raw_u16_le", "aht20_temp_humidity"}
GPIO_ALLOWLIST = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 46}
NAME_RE = re.compile(r"^[A-Za-z0-9_-]{1,47}$")
MANIFEST_VERSION = 1


class ManifestError(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ManifestError(message)


def int_value(data: dict[str, Any], key: str, default: int | None = None) -> int:
    value = data.get(key, default)
    require(isinstance(value, int), f"{key} must be an integer")
    return value


def pin_value(data: dict[str, Any], key: str) -> int:
    pin = int_value(data, key)
    require(pin in GPIO_ALLOWLIST, f"{key}={pin} is outside ESPAgent GPIO allowlist")
    require(pin not in {19, 20}, f"{key}={pin} is reserved for ESP32-S3 USB Serial/JTAG")
    return pin


def validate_common(data: dict[str, Any], path: Path) -> None:
    name = data.get("name")
    protocol = data.get("protocol")
    role = data.get("role")
    risk = data.get("risk")
    version = data.get("manifest_version")
    permissions = data.get("permissions")
    require(isinstance(name, str) and NAME_RE.match(name) is not None, "name must match ^[A-Za-z0-9_-]{1,47}$")
    require(path.stem == name, f"file name {path.stem}.json must match manifest name {name}")
    require(version == MANIFEST_VERSION, f"manifest_version must be {MANIFEST_VERSION}")
    require(isinstance(protocol, str), "protocol must be a string")
    require(isinstance(role, str), "role must be a string")
    require(isinstance(risk, str), "risk must be a string")
    require(isinstance(permissions, list) and permissions, "permissions must be a non-empty array")
    require(protocol not in PLANNED_PROTOCOLS, f"protocol={protocol} is planned but not executable in firmware")
    require(protocol in READ_PROTOCOLS | CONTROL_PROTOCOLS, f"unsupported protocol={protocol}")
    if protocol in READ_PROTOCOLS:
        require(role == "sensor_agent", f"{protocol} must use role=sensor_agent")
        require(risk == "read_only", f"{protocol} must use risk=read_only")
        require("read" in permissions, f"{protocol} permissions must include read")
    else:
        require(role == "control_agent", f"{protocol} must use role=control_agent")
        require(risk in CONTROL_RISKS, f"{protocol} risk must be one of {sorted(CONTROL_RISKS)}")
        require("control" in permissions, f"{protocol} permissions must include control")


def validate_pins_object(data: dict[str, Any], required: list[str]) -> dict[str, int]:
    pins = data.get("pins")
    require(isinstance(pins, dict), "pins must be an object")
    out: dict[str, int] = {}
    for key in required:
        require(key in pins, f"pins.{key} is required")
        require(isinstance(pins[key], int), f"pins.{key} must be an integer")
        out[key] = pins[key]
    return out


def validate_read_manifest(data: dict[str, Any]) -> None:
    protocol = data["protocol"]
    if protocol == "i2c":
        pins = validate_pins_object(data, ["sda", "scl"])
        require(pins["sda"] != pins["scl"], "I2C SDA and SCL must differ")
        require("address" in data, "i2c address is required")
        operations = data.get("operations")
        require(isinstance(operations, list) and operations, "i2c operations array is required")
        decode = data.get("decode", {})
        if isinstance(decode, dict) and "type" in decode:
            dtype = decode["type"]
            require(dtype in I2C_DECODE_TYPES, f"unsupported i2c decode.type={dtype}")
            if dtype == "aht20_temp_humidity":
                read_lengths = [
                    op.get("length")
                    for op in operations
                    if isinstance(op, dict) and op.get("type") == "read"
                ]
                require(any(isinstance(length, int) and length >= 6 for length in read_lengths),
                        "aht20_temp_humidity requires an I2C read operation with length >= 6")
    elif protocol == "uart":
        pins = validate_pins_object(data, ["tx", "rx"])
        require(pins["tx"] != pins["rx"], "UART tx and rx must differ")
        require(int_value(data, "uart_port", 1) > 0, "UART0 is reserved for console")
        require(1 <= int_value(data, "read_length", 64) <= 128, "uart read_length must be 1..128")
    elif protocol == "modbus_rtu":
        validate_pins_object(data, ["tx", "rx"])
        require(int_value(data, "function_code") in {3, 4}, "modbus function_code must be read-only 3 or 4")
        require(1 <= int_value(data, "register_count") <= 16, "modbus register_count must be 1..16")
    elif protocol == "spi":
        validate_pins_object(data, ["mosi", "miso", "sclk", "cs"])
        require(int_value(data, "host", 2) in {2, 3}, "spi host must be 2 or 3")
        require(1 <= int_value(data, "read_length") <= 64, "spi read_length must be 1..64")
    elif protocol == "adc":
        require(int_value(data, "adc_unit", 1) in {1, 2}, "adc_unit must be 1 or 2")
        require(0 <= int_value(data, "channel") <= 9, "adc channel must be 0..9")
        require(1 <= int_value(data, "samples", 4) <= 32, "adc samples must be 1..32")
    elif protocol == "gpio_input":
        pin_value(data, "pin")


def validate_control_manifest(data: dict[str, Any]) -> None:
    protocol = data["protocol"]
    pin_value(data, "pin")
    require(0 <= int_value(data, "max_duration_ms", 5000) <= 30000, "max_duration_ms must be 0..30000")
    require(0 <= int_value(data, "cooldown_ms", 1000) <= 60000, "cooldown_ms must be 0..60000")
    if protocol == "relay_control":
        require(data["risk"] == "high_control", "relay_control must use risk=high_control")
    if protocol in {"pwm_output", "ledc_pwm"}:
        require(1 <= int_value(data, "frequency_hz", 1000) <= 40000, "frequency_hz must be 1..40000")
        require(0 <= int_value(data, "default_duty_pct", 0) <= 100, "default_duty_pct must be 0..100")


def validate_manifest(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ManifestError(f"invalid JSON: {exc}") from exc
    require(isinstance(data, dict), "manifest root must be an object")
    validate_common(data, path)
    if data["protocol"] in READ_PROTOCOLS:
        validate_read_manifest(data)
    else:
        validate_control_manifest(data)
    return data


def signature_path(path: Path) -> Path:
    return path.with_name(path.name + ".sha256")


def manifest_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_signature(path: Path, required: bool) -> str:
    sig_path = signature_path(path)
    if not sig_path.exists():
        require(not required, f"missing required signature sidecar {sig_path}")
        return "missing"
    expected = "".join(ch.lower() for ch in sig_path.read_text(encoding="utf-8") if ch in "0123456789abcdefABCDEF")
    require(len(expected) == 64, f"signature sidecar {sig_path} must contain 64 hex chars")
    actual = manifest_sha256(path)
    require(expected == actual, f"signature mismatch for {path}: expected={expected} actual={actual}")
    return "verified"


def dry_run(data: dict[str, Any]) -> str:
    protocol = data["protocol"]
    tool = "virtual_device_read" if protocol in READ_PROTOCOLS else "virtual_device_control"
    target = data["role"]
    risk = data["risk"]
    return f"{data['name']}: tool={tool} target_role={target} protocol={protocol} risk={risk}"


def template_for(protocol: str, name: str) -> dict[str, Any]:
    require(NAME_RE.match(name) is not None, "template name must match ^[A-Za-z0-9_-]{1,47}$")
    require(protocol not in PLANNED_PROTOCOLS, f"protocol={protocol} is planned but not executable")
    require(protocol in READ_PROTOCOLS | CONTROL_PROTOCOLS, f"unsupported protocol={protocol}")
    base: dict[str, Any] = {
        "manifest_version": MANIFEST_VERSION,
        "name": name,
        "protocol": protocol,
        "publisher": "local_developer",
        "trust": {
            "source": "local_spiffs",
            "integrity": "sha256_sidecar",
            "review": "developer"
        },
    }
    if protocol in READ_PROTOCOLS:
        base.update({"role": "sensor_agent", "risk": "read_only", "permissions": ["read"]})
    else:
        base.update({"role": "control_agent", "risk": "medium_control", "permissions": ["control"]})
    if protocol == "i2c":
        base.update({
            "pins": {"sda": 11, "scl": 12},
            "i2c_port": 0,
            "scl_hz": 100000,
            "address": "0x23",
            "operations": [{"type": "write", "bytes": [0]}, {"type": "read", "length": 2}],
            "decode": {"type": "raw_u16_be", "field": "value", "scale": 1, "offset": 0, "unit": "raw"},
        })
    elif protocol == "uart":
        base.update({
            "pins": {"tx": 17, "rx": 16},
            "uart_port": 2,
            "baud": 115200,
            "command_ascii": "ATI",
            "terminator": "\\r\\n",
            "read_length": 64,
            "timeout_ms": 500,
        })
    elif protocol == "modbus_rtu":
        base.update({
            "pins": {"tx": 17, "rx": 16, "de_re": 15},
            "uart_port": 2,
            "baud": 9600,
            "slave_id": 1,
            "function_code": 4,
            "register": "0x0000",
            "register_count": 1,
            "decode": {"field": "register", "scale": 1, "offset": 0, "unit": "raw"},
        })
    elif protocol == "spi":
        base.update({
            "pins": {"mosi": 7, "miso": 8, "sclk": 9, "cs": 10},
            "host": 2,
            "mode": 0,
            "frequency_hz": 1000000,
            "command_bytes": [159],
            "read_length": 3,
        })
    elif protocol == "adc":
        base.update({
            "adc_unit": 1,
            "channel": 0,
            "samples": 8,
            "decode": {"field": "analog_value", "scale": 1, "offset": 0, "unit": "raw"},
        })
    elif protocol == "gpio_input":
        base.update({"pin": 13, "pullup": False, "pulldown": False, "invert": False, "field": "digital_level"})
    elif protocol == "relay_control":
        base.update({"risk": "high_control", "pin": 5, "active_level": 1, "safe_level": 0, "max_duration_ms": 10000, "cooldown_ms": 5000})
    elif protocol in {"gpio_output", "pwm_output", "ledc_pwm"}:
        base.update({"pin": 4 if protocol == "gpio_output" else 6, "max_duration_ms": 5000, "cooldown_ms": 1000})
        if protocol in {"pwm_output", "ledc_pwm"}:
            base.update({"frequency_hz": 1000, "default_duty_pct": 25, "ledc_channel": 1, "ledc_timer": 1})
    base["notes"] = "Generated template. Review pins, limits, permissions, and datasheet values before flashing."
    return base


def print_support_matrix() -> None:
    print("Executable read protocols: " + ", ".join(sorted(READ_PROTOCOLS)))
    print("Executable control protocols: " + ", ".join(sorted(CONTROL_PROTOCOLS)))
    print("Planned but non-executable protocols: " + ", ".join(sorted(PLANNED_PROTOCOLS)))


def iter_paths(paths: list[Path]) -> list[Path]:
    out: list[Path] = []
    for path in paths:
        if path.is_dir():
            out.extend(sorted(path.glob("*.json")))
        else:
            out.append(path)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path, default=[Path("spiffs_data/devices")])
    parser.add_argument("--dry-run", action="store_true", help="print routing/execution plan for each manifest")
    parser.add_argument("--write-signatures", action="store_true", help="write/update <manifest>.sha256 sidecar files")
    parser.add_argument("--support-matrix", action="store_true", help="print executable and planned manifest protocols")
    parser.add_argument("--init-template", metavar="PROTOCOL", help="print a reviewed starter manifest template")
    parser.add_argument("--name", default="new_device", help="manifest name for --init-template")
    args = parser.parse_args()

    if args.support_matrix:
        print_support_matrix()
        return 0

    if args.init_template:
        try:
            print(json.dumps(template_for(args.init_template, args.name), indent=2, ensure_ascii=False))
            return 0
        except ManifestError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1

    ok = True
    for path in iter_paths(args.paths):
        try:
            data = validate_manifest(path)
            if args.write_signatures:
                signature_path(path).write_text(manifest_sha256(path) + "\n", encoding="utf-8")
            sig_status = validate_signature(path, required=data["protocol"] in CONTROL_PROTOCOLS)
            print(f"OK: {path} signature={sig_status}")
            if args.dry_run:
                print(f"DRY-RUN: {dry_run(data)}")
        except ManifestError as exc:
            ok = False
            print(f"ERROR: {path}: {exc}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
