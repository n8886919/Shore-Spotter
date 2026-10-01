#!/usr/bin/env python3
"""Inspect and validate fixed Shore Spotter device/RF-group assignments.

This never opens a serial device, builds, flashes, or reads secrets.
"""
from __future__ import annotations

import argparse
import json
import sys
import shlex
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DEVICES = ROOT / "config" / "devices.local.json"
DEFAULT_GROUPS = ROOT / "config" / "rf-groups.json"
BOARD_ROLES = {
    "heltec-v4": {"station"},
    "heltec-t096": {"client"},
    "lilygo-tbeam": {"station", "client"},
}


class ConfigError(ValueError):
    pass


def read_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise ConfigError(f"找不到設定檔：{path}") from error
    except json.JSONDecodeError as error:
        raise ConfigError(f"JSON 無法解析：{path}: {error.msg}") from error


def load_groups(path: Path) -> dict[str, dict]:
    from load_rf_profile import load_groups as canonical_load
    try:
        return canonical_load(path)
    except (ValueError, OSError) as error:
        raise ConfigError(str(error)) from error


def serial_key(value: str) -> str:
    return "".join(char for char in value.upper() if char.isalnum())


def load_devices(path: Path, groups: dict[str, dict]) -> dict[str, dict]:
    data = read_json(path)
    if data.get("schema_version") != 1 or not isinstance(data.get("devices"), list):
        raise ConfigError("devices 設定必須是 schema_version 1 且有 devices 陣列")
    devices: dict[str, dict] = {}
    serials: dict[str, str] = {}
    required = {"name", "board", "role", "environment", "usb_serial", "rf_group"}
    for device in data["devices"]:
        if not isinstance(device, dict) or not required.issubset(device) or set(device) - (required | {"client", "radio_id"}):
            raise ConfigError("每個 device 必須只使用 name/board/role/environment/usb_serial/rf_group/client/radio_id 欄位")
        name = device["name"]
        if not isinstance(name, str) or not name or name in devices:
            raise ConfigError(f"device name 重複或無效：{name!r}")
        board, role = device["board"], device["role"]
        if board not in BOARD_ROLES or role not in BOARD_ROLES[board]:
            raise ConfigError(f"{name}: board {board!r} 不支援 role {role!r}")
        if not isinstance(device["environment"], str) or not device["environment"]:
            raise ConfigError(f"{name}: environment 不可空白")
        if device["rf_group"] not in groups:
            raise ConfigError(f"{name}: rf_group 必須存在於 rf-groups.json")
        if not isinstance(device["usb_serial"], str) or not serial_key(device["usb_serial"]):
            raise ConfigError(f"{name}: usb_serial 無效")
        key = serial_key(device["usb_serial"])
        if key in serials:
            raise ConfigError(f"USB serial 重複：{name} 與 {serials[key]}")
        serials[key] = name
        if role == "station" and not isinstance(device.get("client"), str):
            raise ConfigError(f"{name}: Station 必須以 client 指向配對 Client device name")
        if role == "client" and "client" in device:
            raise ConfigError(f"{name}: Client 不可再有 client 綁定欄位")
        if "radio_id" in device and (type(device["radio_id"]) is not int or not 1 <= device["radio_id"] < 65535):
            raise ConfigError(f"{name}: radio_id 必須是 1..65534 的整數（來自裝置開機資訊）")
        devices[name] = device
    paired_clients: dict[str, str] = {}
    for station in (item for item in devices.values() if item["role"] == "station"):
        client_name = station["client"]
        client = devices.get(client_name)
        if client is None or client["role"] != "client":
            raise ConfigError(f"{station['name']}: 找不到配對 Client {client_name!r}")
        if client["rf_group"] != station["rf_group"]:
            raise ConfigError(f"{station['name']} 與 {client_name} 使用不同 RF group")
        if client_name in paired_clients:
            raise ConfigError(f"Client {client_name} 被兩個 Station 配對：{paired_clients[client_name]}、{station['name']}")
        paired_clients[client_name] = station["name"]
    for client in (item for item in devices.values() if item["role"] == "client"):
        if client["name"] not in paired_clients:
            raise ConfigError(f"Client {client['name']} 沒有配對 Station")
    return devices


def summary(device: dict, groups: dict[str, dict]) -> dict:
    group = groups[device["rf_group"]]
    return {
        "device_id": device["name"], "board": device["board"], "role": device["role"],
        "environment": device["environment"], "usb_serial": device["usb_serial"],
        "rf_group": group["name"], "rf_group_id": group["id"], "frequency_mhz": group["frequency_mhz"],
        "paired_client": device.get("client"),
    }


def print_device(item: dict) -> None:
    pairing = f"；配對 Client {item['paired_client']}" if item["paired_client"] else ""
    print(f"{item['device_id']}: {item['role']} / {item['board']} / env {item['environment']} / USB {item['usb_serial']}")
    print(f"  RF {item['rf_group']} (id {item['rf_group_id']}, {item['frequency_mhz']:.1f} MHz){pairing}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="固定 A/B RF group 的 device manifest 工具（不建置、不燒錄）")
    parser.add_argument("--devices", type=Path, default=DEFAULT_DEVICES, help="devices.local.json 路徑")
    parser.add_argument("--groups", type=Path, default=DEFAULT_GROUPS, help="rf-groups.json 路徑")
    parser.add_argument("--json", action="store_true", dest="as_json", help="plan 輸出 JSON")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list")
    sub.add_parser("check")
    plan = sub.add_parser("plan")
    plan.add_argument("name")
    args = parser.parse_args(argv)
    try:
        groups = load_groups(args.groups)
        devices = load_devices(args.devices, groups)
        if args.command == "check":
            print(f"OK：{len(devices)} 台裝置、{len(groups)} 個固定 RF groups；未執行 build、flash 或 USB 操作。")
        elif args.command == "list":
            for name in sorted(devices):
                print_device(summary(devices[name], groups))
        else:
            if args.name not in devices:
                raise ConfigError(f"找不到 device：{args.name}")
            item = summary(devices[args.name], groups)
            if args.as_json:
                print(json.dumps({"deployment_config": item, "custom_rf_group": item["rf_group"]}, ensure_ascii=False, indent=2))
            else:
                print("部署規劃（未建置／未燒錄）：")
                print_device(item)
                print(f"  SHORE_DEVICE={shlex.quote(args.name)} pio run -e {shlex.quote(item['environment'])}")
                print("  build 會依本機 manifest 選 RF group；尚未燒錄，USB target 須另行核對。")
        return 0
    except ConfigError as error:
        print(f"設定錯誤：{error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
