"""PlatformIO/SCons bridge for config/rf-groups.json.

Every firmware environment selects a fixed custom_rf_group (currently A/B).  The JSON is the
single source for the selected group's numeric compile definitions; it does
not perform RF certification or channel scanning.
"""
from __future__ import annotations

import json
import math
import re
import os
import sys
from pathlib import Path


try:
    Import("env")  # type: ignore[name-defined]  # SCons executes without __file__
except NameError:
    env = None
ROOT = Path(env.subst("$PROJECT_DIR")) if env is not None else Path(__file__).resolve().parents[1]
PROFILE_PATH = ROOT / "config" / "rf-groups.json"


def load_groups(path: Path = PROFILE_PATH) -> dict[str, dict]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema_version") != 1 or not isinstance(data.get("groups"), list):
        raise ValueError(f"{path}: expected schema_version 1 and groups list")
    groups, ids, frequencies = {}, set(), set()
    for group in data["groups"]:
        if not isinstance(group, dict):
            raise ValueError("RF group must be an object")
        name = group.get("name")
        if not isinstance(name, str) or not re.fullmatch(r"[A-Z][A-Z0-9_-]*", name) or name in groups:
            raise ValueError("RF group names must be unique uppercase labels")
        group_id, frequency = group.get("id"), group.get("frequency_mhz")
        if type(group_id) is not int or not 0 <= group_id <= 255 or group_id in ids:
            raise ValueError("RF group ids must be unique integers 0..255")
        if type(frequency) not in (int, float) or not math.isfinite(frequency) or not 863 <= frequency <= 928 or frequency in frequencies:
            raise ValueError("RF groups need distinct fixed frequencies within 863..928 MHz hardware range")
        if (group.get("spreading_factor"), group.get("bandwidth_khz"), group.get("coding_rate"), str(group.get("sync_word")).lower()) != (10, 125, 5, "0x12"):
            raise ValueError("This firmware profile uses SF10/BW125/CR5/sync 0x12")
        groups[name] = group
        ids.add(group_id)
        frequencies.add(frequency)
    if not groups:
        raise ValueError("At least one RF group is required")
    return groups


def _has_define(defines, name: str) -> bool:
    for item in defines:
        if isinstance(item, tuple):
            key = item[0]
        else:
            key = str(item).split("=", 1)[0].lstrip("-D ")
        if key == name:
            return True
    return False


def apply_profile(env) -> None:
    group_name = env.GetProjectOption("custom_rf_group")
    selected_name = os.environ.get("SHORE_DEVICE")
    if selected_name:
        sys.path.insert(0, str(ROOT / "tools"))
        from device_config import load_devices
        devices = load_devices(ROOT / "config/devices.local.json", load_groups())
        if selected_name not in devices:
            raise ValueError(f"SHORE_DEVICE unknown: {selected_name}")
        device = devices[selected_name]
        if device["environment"] != env.subst("$PIOENV"):
            raise ValueError("SHORE_DEVICE environment does not match selected PlatformIO environment")
        group_name = device["rf_group"]
        if device["role"] == "station":
            client = devices[device["client"]]
            if "radio_id" in client:
                env.Append(CPPDEFINES=[("SHORE_DEFAULT_CLIENT_ID", str(client["radio_id"]))])
    groups = load_groups()
    if group_name not in groups:
        raise ValueError("custom_rf_group must name a group from config/rf-groups.json")
    existing = env.get("CPPDEFINES", [])
    for name in ("SHORE_RF_GROUP", "SHORE_RF_FREQUENCY_MHZ"):
        if _has_define(existing, name):
            raise ValueError(f"{name} is already defined; remove the hard-coded build flag")
    group = groups[group_name]
    env.Append(CPPDEFINES=[
        ("SHORE_RF_GROUP", str(group["id"])),
        ("SHORE_RF_FREQUENCY_MHZ", f"{float(group['frequency_mhz'])}f"),
    ])


try:
    Import("env")  # type: ignore[name-defined]  # provided by PlatformIO/SCons
except NameError:
    env = None
if env is not None:
    apply_profile(env)
