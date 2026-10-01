#!/usr/bin/env python3
"""Regression checks for fixed-group device manifest validation."""
from __future__ import annotations

import copy
import json
import os
from unittest.mock import patch
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import device_config  # noqa: E402
import load_rf_profile  # noqa: E402


GROUPS = json.loads((ROOT / "config" / "rf-groups.json").read_text())
DEVICES = json.loads((ROOT / "config" / "devices.example.json").read_text())


class FakeEnv(dict):
    def __init__(self, group: str, defines=()):
        super().__init__(CPPDEFINES=list(defines))
        self.group = group

    def subst(self, key):
        return "heltec-v4-station"

    def GetProjectOption(self, name):
        self.assertEqual(name, "custom_rf_group")
        return self.group

    def Append(self, **kwargs):
        self["CPPDEFINES"].extend(kwargs["CPPDEFINES"])

    def assertEqual(self, actual, expected):
        if actual != expected:
            raise AssertionError((actual, expected))


class DeviceConfigTest(unittest.TestCase):
    def write(self, directory: Path, devices=DEVICES, groups=GROUPS):
        group_path = directory / "rf-groups.json"
        device_path = directory / "devices.json"
        group_path.write_text(json.dumps(groups), encoding="utf-8")
        device_path.write_text(json.dumps(devices), encoding="utf-8")
        return group_path, device_path

    def test_example_list_check_and_plan(self):
        with tempfile.TemporaryDirectory() as tmp:
            groups, devices = self.write(Path(tmp))
            command = [sys.executable, str(ROOT / "tools" / "device_config.py"), "--groups", str(groups), "--devices", str(devices)]
            checked = subprocess.run(command + ["check"], text=True, capture_output=True, check=True)
            self.assertIn("未執行 build、flash 或 USB 操作", checked.stdout)
            listed = subprocess.run(command + ["list"], text=True, capture_output=True, check=True)
            self.assertIn("station-v4", listed.stdout)
            self.assertIn("RF B (id 1, 923.8 MHz)", listed.stdout)
            self.assertIn("shore-<完整 Wi-Fi MAC>.local/", listed.stdout)
            planned = subprocess.run(command + ["plan", "station-v4"], text=True, capture_output=True, check=True)
            self.assertIn("配對 Client t096-client", planned.stdout)
            self.assertIn("SHORE_DEVICE=station-v4 pio run -e heltec-v4-station", planned.stdout)

    def rejected(self, mutate, text):
        devices = copy.deepcopy(DEVICES)
        mutate(devices["devices"])
        with self.assertRaisesRegex(device_config.ConfigError, text):
            device_config.load_devices_from_test(devices["devices"], GROUPS)

    def test_invalid_pairing_and_identity_are_rejected(self):
        def load(data, groups=GROUPS):
            return device_config.load_devices_from_test(data, groups)
        # Test helper is installed below to exercise the same parser after a temporary JSON write.
        device_config.load_devices_from_test = lambda data, groups: self._load_data(data, groups)
        self.rejected(lambda d: d[1].update(name="station-v4"), "name")
        self.rejected(lambda d: d[1].update(usb_serial=DEVICES["devices"][0]["usb_serial"]), "USB serial")
        self.rejected(lambda d: d[0].update(client="missing"), "找不到配對")
        self.rejected(lambda d: d[1].update(rf_group="A"), "不同 RF group")
        self.rejected(lambda d: d[1].update(role="station"), "不支援 role")
        self.rejected(lambda d: d[1].update(board="unknown"), "不支援 role")

    def _load_data(self, devices, groups):
        with tempfile.TemporaryDirectory() as tmp:
            group_path, device_path = self.write(Path(tmp), devices={"schema_version": 1, "devices": devices}, groups=groups)
            return device_config.load_devices(device_path, device_config.load_groups(group_path))

    def test_station_hostnames_are_required_valid_and_unique(self):
        for value in [None, "", "Shore-b", "shore_b", "shore-b.local", "-shore", "shore-", "x" * 32, 'x";bad']:
            with self.subTest(hostname=value):
                devices = copy.deepcopy(DEVICES["devices"])
                devices[0]["hostname"] = value
                with self.assertRaisesRegex(device_config.ConfigError, "hostname"):
                    self._load_data(devices, GROUPS)
        devices = copy.deepcopy(DEVICES["devices"])
        devices[0]["hostname"] = "shore-b"
        devices.append(dict(devices[0], name="station-other", usb_serial="OTHER"))
        with self.assertRaisesRegex(device_config.ConfigError, "hostname 重複"):
            self._load_data(devices, GROUPS)
        devices[2].update(hostname="a" * 31, client="client-other", rf_group="A")
        devices.append(dict(devices[1], name="client-other", usb_serial="CLIENTOTHER", rf_group="A"))
        self.assertEqual(len(self._load_data(devices, GROUPS)), 4)
        devices[0]["hostname"] = devices[2]["hostname"] = "auto"
        parsed = self._load_data(devices, GROUPS)
        self.assertEqual(len(parsed), 4)
        self.assertIsNone(device_config.summary(parsed["station-v4"], device_config.load_groups(ROOT / "config/rf-groups.json"))["local_url"])
        devices[1]["hostname"] = "client-host"
        with self.assertRaisesRegex(device_config.ConfigError, "hostname 只適用 Station"):
            self._load_data(devices, GROUPS)

    def test_additional_fixed_group_uses_config_only(self):
        groups = copy.deepcopy(GROUPS)
        c = dict(groups["groups"][0], name="C", id=2, frequency_mhz=924.4)
        groups["groups"].append(c)
        with tempfile.TemporaryDirectory() as tmp:
            path, _ = self.write(Path(tmp), groups=groups)
            self.assertEqual(load_rf_profile.load_groups(path)["C"]["frequency_mhz"], 924.4)
            c["frequency_mhz"] = groups["groups"][0]["frequency_mhz"]
            path.write_text(json.dumps(groups))
            with self.assertRaisesRegex(ValueError, "distinct"):
                load_rf_profile.load_groups(path)

    def test_selected_device_overrides_default_group_and_checks_environment(self):
        devices = copy.deepcopy(DEVICES)
        devices["devices"][1]["radio_id"] = 0x1234
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "config").mkdir()
            (root / "config/devices.local.json").write_text(json.dumps(devices))
            with patch.object(load_rf_profile, "ROOT", root), patch.dict(os.environ, {"SHORE_DEVICE":"station-v4"}):
                env = FakeEnv("A")
                load_rf_profile.apply_profile(env)
                self.assertIn(("SHORE_RF_GROUP", "1"), env["CPPDEFINES"])
                self.assertIn(("SHORE_DEFAULT_CLIENT_ID", "4660"), env["CPPDEFINES"])
                self.assertFalse(any(d[0] == "SHORE_STATION_HOSTNAME" for d in env["CPPDEFINES"]))
                devices["devices"][0]["hostname"] = "shore-b"
                (root / "config/devices.local.json").write_text(json.dumps(devices))
                named = FakeEnv("B")
                load_rf_profile.apply_profile(named)
                self.assertIn(("SHORE_STATION_HOSTNAME", '\\"shore-b\\"'), named["CPPDEFINES"])
                with self.assertRaisesRegex(ValueError, "SHORE_STATION_HOSTNAME is already defined"):
                    load_rf_profile.apply_profile(FakeEnv("B", [("SHORE_STATION_HOSTNAME", "duplicate")]))
                env.subst = lambda key: "tbeam-station"
                with self.assertRaisesRegex(ValueError, "environment"):
                    load_rf_profile.apply_profile(env)

    def test_profile_loader_uses_canonical_group_without_duplicates(self):
        env = FakeEnv("B")
        old_path = load_rf_profile.PROFILE_PATH
        try:
            load_rf_profile.PROFILE_PATH = ROOT / "config" / "rf-groups.json"
            load_rf_profile.apply_profile(env)
        finally:
            load_rf_profile.PROFILE_PATH = old_path
        self.assertIn(("SHORE_RF_GROUP", "1"), env["CPPDEFINES"])
        self.assertIn(("SHORE_RF_FREQUENCY_MHZ", "923.8f"), env["CPPDEFINES"])
        with self.assertRaisesRegex(ValueError, "already defined"):
            load_rf_profile.apply_profile(FakeEnv("A", [("SHORE_RF_GROUP", "0")]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
