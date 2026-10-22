"""Public synthetic export-list tests. No actual framework or SDK consumed."""
from pathlib import Path
import tempfile
import unittest

from restrict_link_exports import INTERNAL, restrict, rewrite


def export_list(extra=()):
    return "\n".join(["_wgpuGetVersion", "_wgpuCreateInstance", *sorted(INTERNAL), *extra]) + "\n"


class ExportRestrictionTests(unittest.TestCase):
    def test_only_four_known_bookkeeping_names_are_removed(self):
        text, report = restrict(export_list())
        self.assertEqual(text, "_wgpuGetVersion\n_wgpuCreateInstance\n")
        self.assertEqual(set(report["hidden_exact_symbols"]), INTERNAL)
        self.assertFalse(report["internal_definitions_or_references_removed"])

    def test_unexpected_rust_or_objective_c_symbols_are_not_silently_hidden(self):
        for name in ("_rust_eh_personality", "___CLASS_Other", "_OBJC_CLASS_$_Other"):
            with self.assertRaises(ValueError):
                restrict(export_list([name]))
        with self.assertRaises(ValueError):
            restrict("_wgpuGetVersion\n_wgpuCreateInstance\n")

    def test_combined_and_split_driver_arguments_preserve_other_flags(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = root / "before.txt"
            original.write_text(export_list())
            for flags in (["-Wl,-exported_symbols_list," + str(original)],
                          ["-Wl,-exported_symbols_list", "-Wl," + str(original)],
                          ["-exported_symbols_list", str(original)],
                          ["-Xlinker", "-exported_symbols_list", "-Xlinker", str(original)]):
                result = rewrite(["-dylib", "-install_name", "@rpath/WgpuNative.framework/WgpuNative", *flags, "-o", "output"], root / "policies")
                self.assertEqual(result[0], "-dylib")
                self.assertEqual(result[-2:], ["-o", "output"])
                self.assertEqual(original.read_text(), export_list())

    def test_missing_multiple_and_response_file_forms_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            for arguments in ([], ["@args.rsp"], ["-Wl,-exported_symbols_list"]):
                with self.assertRaises(ValueError):
                    rewrite(arguments, Path(temporary))


if __name__ == "__main__":
    unittest.main()
