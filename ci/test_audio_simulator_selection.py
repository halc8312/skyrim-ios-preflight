"""Authored selection fixtures only; never invokes Apple tools or real audio."""
import copy
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

from audio_simulator_selection import select_simulator, runtime_identifier


def runtime(version="26.4"):
    suffix = "26-4" if version == "26.4.1" else version.replace(".", "-")
    return dict(version=version, isAvailable=True,
                identifier="com.apple.CoreSimulator.SimRuntime.iOS-" + suffix)


def device_type():
    return dict(name="iPhone 17 Pro Max", identifier="com.apple.CoreSimulator.SimDeviceType.iPhone-17-Pro-Max")


class SelectionTests(unittest.TestCase):
    def test_exact_requested_version_and_default_with_other_installed_versions(self):
        rows = [runtime(v) for v in ("26.3", "26.4", "26.5")]
        for version, minor in (("26.3", 3), ("26.4", 4), ("26.5", 5)):
            with self.subTest(version=version):
                selected = select_simulator(rows, [device_type()], version)
                self.assertEqual(selected, dict(schema=1, runtime_version=version, ios_major=26, ios_minor=minor,
                    runtime_identifier=runtime(version)["identifier"], device_type_name="iPhone 17 Pro Max",
                    device_type_identifier="com.apple.CoreSimulator.SimDeviceType.iPhone-17-Pro-Max"))
        self.assertEqual(select_simulator(rows, [device_type()])["runtime_version"], "26.5")

    def test_observed_full_version_requires_exact_version_and_identifier_without_fallback(self):
        row = runtime("26.4.1")
        self.assertEqual(runtime_identifier("26.4.1"), "com.apple.CoreSimulator.SimRuntime.iOS-26-4")
        selected = select_simulator([runtime("26.3"), row, runtime("26.5")], [device_type()], "26.4.1")
        self.assertEqual(selected["runtime_version"], "26.4.1")
        self.assertEqual((selected["ios_major"], selected["ios_minor"]), (26, 4))
        self.assertEqual(selected["runtime_identifier"], "com.apple.CoreSimulator.SimRuntime.iOS-26-4")
        wrong_identifier = copy.deepcopy(row)
        wrong_identifier["identifier"] = "com.apple.CoreSimulator.SimRuntime.iOS-26-4-1"
        wrong_version = copy.deepcopy(row); wrong_version["version"] = "26.4.2"
        for requested, candidate in (("26.4.1", runtime()), ("26.4", row),
                                     ("26.4.1", wrong_identifier), ("26.4.1", wrong_version)):
            with self.subTest(requested=requested, candidate=candidate), self.assertRaises(ValueError):
                select_simulator([candidate], [device_type()], requested)
        with self.assertRaises(ValueError): select_simulator([row], [device_type()])

    def test_unavailable_requested_version_never_falls_back(self):
        for unavailable in (False, None, 1, "true"):
            rows = [runtime(v) for v in ("26.3", "26.4", "26.5")]
            rows[1]["isAvailable"] = unavailable
            with self.subTest(available=unavailable), self.assertRaises(ValueError):
                select_simulator(rows, [device_type()], "26.4")

    def test_other_platform_crossed_runtime_and_other_device_type_refuse(self):
        for identifier in ("com.apple.CoreSimulator.SimRuntime.tvOS-26-4",
                           "com.apple.CoreSimulator.SimRuntime.iOS-26-5",
                           "com.apple.CoreSimulator.SimRuntime.iOS-26-4-suffix"):
            row = runtime(); row["identifier"] = identifier
            with self.subTest(identifier=identifier), self.assertRaises(ValueError):
                select_simulator([row], [device_type()], "26.4")
        for key, wrong in (("name", "iPhone 17 Pro"), ("identifier", "com.apple.CoreSimulator.SimDeviceType.iPhone-17-Pro")):
            device = device_type(); device[key] = wrong
            with self.subTest(key=key), self.assertRaises(ValueError):
                select_simulator([runtime()], [device], "26.4")

    def test_absent_fields_and_malformed_or_empty_listings_refuse(self):
        for key in ("version", "isAvailable", "identifier"):
            row = runtime(); del row[key]
            with self.subTest(runtime_field=key), self.assertRaises(ValueError):
                select_simulator([row], [device_type()], "26.4")
        for key in ("name", "identifier"):
            row = device_type(); del row[key]
            with self.subTest(device_field=key), self.assertRaises(ValueError):
                select_simulator([runtime()], [row], "26.4")
        for rows, devices in (([], [device_type()]), ([runtime()], []), ({}, [device_type()]),
                              ([runtime()], None), ([None], [device_type()]), ([runtime()], [1])):
            with self.subTest(rows=rows, devices=devices), self.assertRaises(ValueError):
                select_simulator(rows, devices, "26.4")

    def test_duplicate_available_runtimes_or_device_types_are_ambiguous(self):
        row, device = runtime(), device_type()
        for rows, devices in (([row, copy.deepcopy(row)], [device]), ([row], [device, copy.deepcopy(device)])):
            with self.subTest(rows=rows, devices=devices), self.assertRaises(ValueError):
                select_simulator(rows, devices, "26.4")

    def test_unknown_versions_and_coercions_refuse(self):
        for version in ("26.2", "26.3.1", "26.6", "latest", "26.4.0", "26.4.2", " 26.4", "", 26.4, 26, True, None):
            with self.subTest(version=version):
                with self.assertRaises(ValueError): runtime_identifier(version)
                with self.assertRaises(ValueError): select_simulator([runtime()], [device_type()], version)

    def test_actual_driver_wrapper_persists_only_public_selection_and_keeps_listing_deadlines(self):
        here = Path(__file__).resolve().parent
        script = (here / "run-ios-audio-environment.sh").read_text()
        source = script.split("<<'PY'\n", 1)[1].split("\nPY\n", 1)[0]
        for version in ("26.4", "26.4.1", "26.5", "26.4.2", "26.6"):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as root:
                path = Path(root) / "selection.json"
                inventory_path = Path(root) / "simulator-runtime-inventory.json"
                installed_versions = ("26.3", "26.4.1" if version == "26.4.1" else "26.4", "26.5")
                listings = dict(runtimes=[runtime(v) for v in installed_versions], devicetypes=[device_type()])
                for row in listings["runtimes"]:
                    row.update(buildversion="23Authored", name="omit this extra field", udid="authored-not-for-inventory")
                other_platform = runtime(); other_platform["identifier"] = "com.apple.CoreSimulator.SimRuntime.tvOS-26-4"
                listings["runtimes"].append(other_platform)
                def listing(command, timeout):
                    return json.dumps({command[3]: listings[command[3]]}).encode()
                with mock.patch.object(sys, "argv", ["-", str(here), version, str(path)]), \
                     mock.patch.object(sys, "path", sys.path.copy()), \
                     mock.patch("subprocess.check_output", side_effect=listing) as check, \
                     contextlib.redirect_stdout(io.StringIO()) as output:
                    if version in ("26.4.2", "26.6"):
                        with self.assertRaises(ValueError): exec(compile(source, "<authored-driver-selection>", "exec"), {})
                        check.assert_not_called(); self.assertFalse(path.exists())
                        self.assertFalse(inventory_path.exists()); self.assertEqual(list(Path(root).iterdir()), [])
                        self.assertEqual(output.getvalue(), "")
                        continue
                    exec(compile(source, "<authored-driver-selection>", "exec"), {})
                self.assertEqual(check.call_args_list, [
                    mock.call(["xcrun", "simctl", "list", "runtimes", "-j"], timeout=60),
                    mock.call(["xcrun", "simctl", "list", "devicetypes", "-j"], timeout=10)])
                selected = json.loads(path.read_bytes())
                inventory = json.loads(inventory_path.read_bytes())
                self.assertEqual(set(Path(root).iterdir()), {path, inventory_path})
                self.assertEqual(inventory, dict(schema=1, requested_version=version, runtimes=[
                    dict(identifier=runtime(v)["identifier"], version=v, isAvailable=True, buildversion="23Authored")
                    for v in installed_versions]))
                self.assertEqual(selected, select_simulator(listings["runtimes"], listings["devicetypes"], version))
                self.assertEqual(output.getvalue().splitlines(), [selected["runtime_identifier"], selected["device_type_identifier"]])

    def test_actual_driver_strict_selection_failure_keeps_inventory_without_selection_or_stdout(self):
        here = Path(__file__).resolve().parent
        script = (here / "run-ios-audio-environment.sh").read_text()
        source = script.split("<<'PY'\n", 1)[1].split("\nPY\n", 1)[0]
        unavailable = runtime(); unavailable["isAvailable"] = False
        for label, rows, devices in (
                ("absent", [runtime("26.3"), runtime("26.5")], [device_type()]),
                ("unavailable", [unavailable, runtime("26.5")], [device_type()]),
                ("ambiguous runtime", [runtime(), runtime()], [device_type()]),
                ("absent device", [runtime()], []),
                ("ambiguous device", [runtime()], [device_type(), device_type()])):
            with self.subTest(case=label), tempfile.TemporaryDirectory() as root:
                path = Path(root) / "selection.json"
                inventory_path = Path(root) / "simulator-runtime-inventory.json"
                listings = dict(runtimes=rows, devicetypes=devices)
                def listing(command, timeout):
                    return json.dumps({command[3]: listings[command[3]]}).encode()
                with mock.patch.object(sys, "argv", ["-", str(here), "26.4", str(path)]), \
                     mock.patch.object(sys, "path", sys.path.copy()), \
                     mock.patch("subprocess.check_output", side_effect=listing) as check, \
                     contextlib.redirect_stdout(io.StringIO()) as output:
                    with self.assertRaises(ValueError): exec(compile(source, "<authored-driver-selection>", "exec"), {})
                self.assertEqual(check.call_args_list, [
                    mock.call(["xcrun", "simctl", "list", "runtimes", "-j"], timeout=60),
                    mock.call(["xcrun", "simctl", "list", "devicetypes", "-j"], timeout=10)])
                self.assertFalse(path.exists()); self.assertEqual(output.getvalue(), "")
                self.assertEqual(set(Path(root).iterdir()), {inventory_path})
                self.assertEqual(json.loads(inventory_path.read_bytes()), dict(schema=1, requested_version="26.4", runtimes=[
                    dict(identifier=row["identifier"], version=row["version"], isAvailable=row["isAvailable"], buildversion=None)
                    for row in rows]))


if __name__ == "__main__":
    unittest.main()
