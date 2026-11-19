import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/audit_classic_skse.py"
SPEC = importlib.util.spec_from_file_location("audit_classic_skse", SCRIPT)
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)

HEADER = """#define SKSE_VERSION_INTEGER 1
#define SKSE_VERSION_INTEGER_MINOR 7
#define SKSE_VERSION_INTEGER_BETA 3
"""


class ClassicSkseAudit(unittest.TestCase):
    def test_literal_source_counts_skip_comments_strings_and_raw_strings(self):
        source = r'''
        /* new NativeFunction0<F>("InComment", "Fake", callback); */
        // new NativeFunction0<F>("InLine", "Fake", callback); \
        new NativeFunction0<F>("LineContinued", "Fake", callback);
        const char* example = R"tag(new NativeFunction0<F>("Raw", "Fake", callback);)tag";
        const char* quoted = "a \"quoted\" // not a comment";
        #ifdef OPTIONAL
        new LatentNativeFunction1 <Tag, Pair<List<F>, Obj>>
            ( "Start", /* between arguments */ "Quest", callback, registry );
        #endif
        '''
        self.assertEqual(list(audit.official_names(source)), [("Quest", "Start")])
        unusual = 'new NativeFunction0<T>("Read", "Example*", fn);'
        self.assertEqual(list(audit.official_names(unusual)), [("Example*", "Read")])

    def test_matched_names_are_qualified_and_case_insensitive(self):
        table = '''
        #define F(cls,n,...) {cls,n}
        const char *not_code = "SkseNative natives[] = {";
        static const SkseNative natives[] = {
            F("math","sum",3,2,3,3,0,0,OP),
            F("B","Sum",3,0,0,0,0,0,OTHER(CALL())),
        };
        void ignored() { F("Fake","Outside",0); }
        '''
        rows = list(audit.skydream_names(table))
        result = audit.compare([("Math", "Sum"), ("C", "Sum"), ("MATH", "SUM")],
                               rows, details=True)
        self.assertEqual(result["counts"], {"official": 2, "skydream": 2, "shared": 1,
                                           "official_only": 1, "skydream_only": 1})
        self.assertEqual(result["names"]["official_only"], ["C.Sum"])
        self.assertEqual(result["names"]["skydream_only"], ["B.Sum"])
        self.assertEqual(result["names"]["shared"][0].casefold(), "math.sum")

    def test_unparsed_or_empty_tables_cannot_be_reported_as_coverage(self):
        for text in ["", "SkseNative natives[] = { UNRECOGNIZED_ROW(123) };",
                     'SkseNative natives[] = { F("C","Name",0) ']:
            with self.subTest(text=text), self.assertRaises(ValueError):
                list(audit.skydream_names(text))
        with self.assertRaises(ValueError):
            list(audit.official_names('new NativeFunction1<T>(variable, "C", fn);'))
        with self.assertRaises(ValueError):
            audit.compare([("C", "Name")], [], details=False)

    def test_version_is_read_from_definitions(self):
        audit.check_version(HEADER)
        with self.assertRaises(ValueError):
            audit.check_version(HEADER.replace("BETA 3", "BETA 2 /* #define SKSE_VERSION_INTEGER_BETA 3 */"))

    def test_fingerprints_reject_partial_and_label_local_candidates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            skse = root / "src/skse/skse"
            skse.mkdir(parents=True)
            (skse / "skse_version.h").write_bytes(HEADER.encode())
            code = b'new NativeFunction0<T>("Name", "C", fn);\n'
            (skse / "PapyrusExample.cpp").write_bytes(code.replace(b"\n", b"\r\n"))
            table = b'const SkseNative natives[] = { F("C","Name",0) };\n'
            (root / "runtime").mkdir()
            path = root / "runtime/skse_compat_values.c"
            path.write_bytes(table.replace(b"\n", b"\r\n"))
            fingerprint = hashlib.sha256(b"PapyrusExample.cpp\0" + hashlib.sha256(code).digest()).hexdigest()
            with mock.patch.object(audit, "OFFICIAL_SOURCES", fingerprint), \
                    mock.patch.object(audit, "SKYDREAM_TABLE", hashlib.sha256(table).hexdigest()):
                summary = audit.audit(root, root)
                self.assertTrue(summary["skydream_table_matches_reference"])
                self.assertNotIn("names", summary)
                path.write_bytes(table.replace(b'"C"', b'"OtherClass"'))
                with self.assertRaisesRegex(ValueError, "project pin"):
                    audit.audit(root, root)
                local = audit.audit(root, root, candidate=True)
                self.assertFalse(local["skydream_table_matches_reference"])
                self.assertEqual(local["counts"]["official_only"], 1)
                (skse / "PapyrusExample.cpp").unlink()
                with self.assertRaisesRegex(ValueError, "incomplete or modified"):
                    audit.audit(root, root, candidate=True)


if __name__ == "__main__":
    unittest.main()
