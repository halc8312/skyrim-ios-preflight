"""Game-free tests of selection, exact adapter changes and reviewed execution gate."""
import importlib.util
import json
from pathlib import Path
import sys
import struct
import tempfile
import types
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1]/"scripts/check_private_primitives.py"
SPEC = importlib.util.spec_from_file_location("private_primitives",SCRIPT)
p = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(p)


class PrimitivePreparation(unittest.TestCase):
    def test_finalization_does_not_read_rejected_external_audit(self):
        with tempfile.TemporaryDirectory() as temp:
            outside=Path(temp)/"external";outside.mkdir()
            (outside/"audit.json").write_text('{"private":"must not read"}')
            args=types.SimpleNamespace(prepare=False,execute=True,prepared=outside)
            with mock.patch.object(Path,"read_text") as read:
                checks=p.final_integrity(args,Path(temp)/"work")
                read.assert_not_called()
            self.assertFalse(checks["prepared_path"]["matches"])

    def test_backward_bound_rejects_changed_compare_or_extra_incoming_edge(self):
        rows=[]
        for address in (0x100,0x110,0x120,0x130):
            rows.extend([{"address":hex(address),"size":3,"mnemonic":"cmp","operands":"ecx, 8"},
                         {"address":hex(address+3),"size":2,"mnemonic":"jb","operands":"0x200"}])
        rows.append({"address":"0x200","size":2,"mnemonic":"neg","operands":"ecx"})
        self.assertEqual(p.backward_bound_evidence(rows,0x202,{})["unsigned_count_before_negate"],[0,7])
        for changed in ([dict(x,operands="ecx, 9") if x["address"]=="0x100" else x for x in rows],
                        rows+[{"address":"0x140","size":2,"mnemonic":"jmp","operands":"0x200"}],
                        rows+[{"address":"0x1ff","size":1,"mnemonic":"nop","operands":""}]):
            with self.assertRaises(RuntimeError):p.backward_bound_evidence(changed,0x202,{})
        with self.assertRaises(RuntimeError):p.backward_bound_evidence(rows,0x202,{"0x444":["0x200"]})

    def test_final_integrity_records_original_and_copy_changes_after_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);prepared=root/"prepared";run=root/"run"
            original=root/"candidate";original.write_bytes(b"candidate")
            discovery=root/"discovery";discovery.write_bytes(b"discovery")
            input_sha=p.digest(original);discovery_sha=p.digest(discovery)
            for base in (prepared,run):
                (base/"inputs").mkdir(parents=True);(base/"source").mkdir()
                (base/"inputs/candidate.exe").write_bytes(b"candidate")
                (base/"inputs/original-functions.json").write_bytes(b"discovery")
                (base/"inputs"/p.SELECTION_FILE).write_bytes(b"selection")
                (base/"source/source.py").write_bytes(b"source")
            audit={"original_input":str(original),"original_discovery":str(discovery),"selected_functions_sha256":p.digest(prepared/"inputs"/p.SELECTION_FILE),"source_sha256":p.tree_hashes(prepared/"source")}
            p.write_json(prepared/"audit.json",audit)
            args=types.SimpleNamespace(prepare=False,execute=True,prepared=prepared,audit_sha256=p.digest(prepared/"audit.json"))
            original.write_bytes(b"changed-original")
            (run/"source/source.py").write_bytes(b"changed-copy")
            with mock.patch.object(p,"INPUT_SHA",input_sha),mock.patch.object(p,"DISCOVERY_SHA",discovery_sha),mock.patch.object(p,"private_output",side_effect=lambda x:x):
                checks=p.final_integrity(args,run)
            self.assertFalse(checks["original_candidate"]["matches"])
            self.assertFalse(checks["run/source/source.py"]["matches"])
            self.assertTrue(checks["prepared/source/source.py"]["matches"])
            self.assertTrue(checks["original_discovery"]["matches"])

    def test_compacted_slots_retain_non_code_prefix_and_reject_changed_targets(self):
        original=struct.pack("<4I",0x9000f538,0xf53888,0xf538b4,0xf538d8)
        targets=[0xf53888,0xf538b4,0xf538d8]
        actual,matching=p.table_slots(original,targets,0x9000f538)
        self.assertTrue(matching);self.assertEqual(actual[0],0x9000f538)
        self.assertFalse(p.table_slots(original,[0xf53888,0xf538b4,0x1234],0x9000f538)[1])
        self.assertFalse(p.table_slots(original,targets,0)[1])
        with self.assertRaises(RuntimeError):p.table_slots(original[:-1],targets,0x9000f538)

    def test_exact_selected_blocks_preserve_lower_shared_tails_and_empty_tables(self):
        functions = {"0x"+a:[[1,3],[int(a,16),int(a,16)+5]] for a in p.SELECTED}
        functions["0x123456"]=[[0x123456,0x123457]]
        data={"functions":functions,"jump_tables":{"0xf5386c":[]},"indirect_jumps":["0xf5386c"],"iat":{"0x20":"unreviewed"}}
        result=p.subset(data)
        self.assertEqual(len(result["functions"]),21)
        self.assertEqual(len(p.LANDING_PADS),14)
        self.assertIn("0xf53ae4",result["functions"])
        self.assertIn("0xf53124",result["functions"])
        self.assertEqual(result["functions"]["0xf537f0"],[[1,3],[0xf537f0,0xf537f5]])
        self.assertEqual(result["jump_tables"],data["jump_tables"])
        self.assertEqual(result["indirect_jumps"],data["indirect_jumps"])
        self.assertIn("0x123456",data["functions"])

    def test_invalid_block_bounds_rejected(self):
        for ranges in ([[2,1]],[[1,5000]],[[1,2]]*129):
            data={"functions":{"0x"+a:ranges for a in p.SELECTED}}
            with self.assertRaises(RuntimeError): p.subset(data)

    def test_adapter_keeps_real_helper_dispatch_and_comparison(self):
        original='uint8_t *MEM;int g_log_level=0;\nvoid rt_call(CPU *c,uint32_t t){switch(t){case 0xF6029Cu:f_F6029C(c);return;default:fprintf(stderr,"unexpected oracle call %x\\n",t);abort();}}\nunsigned args[4]; original[target](&a); native[target](&b);'
        adapted=p.adapt_oracle(original)
        self.assertIn("case 0xF6029Cu:f_F6029C(c);return;",adapted)
        self.assertIn("unsigned args[4]={0}; original[target](&a); native[target](&b);",adapted)
        self.assertIn("UNRESOLVED guest target=0x%08x",adapted)
        self.assertIn("UNSUPPORTED addr=0x%08x",adapted)
        for changed in (original+original,original.replace("unsigned args[4];","unsigned args[5];"),original.replace("unexpected oracle call","different")):
            with self.assertRaises(RuntimeError):p.adapt_oracle(changed)

    def test_review_hash_and_blockers_stop_before_commands(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); prepared=root/"prepared";prepared.mkdir()
            audit={"blockers":["unreviewed indirect branch"],"source_revision":p.PIN,"patch_sha256":p.PATCHES}
            p.write_json(prepared/"audit.json",audit)
            args=types.SimpleNamespace(prepared=prepared,audit_sha256="0"*64)
            with mock.patch.object(p,"private_output",side_effect=lambda x:x),mock.patch.object(p,"command") as command:
                with self.assertRaisesRegex(RuntimeError,"hash mismatch"):p.execute(args,root)
                args.audit_sha256=p.digest(prepared/"audit.json")
                with self.assertRaisesRegex(RuntimeError,"Audit blocked"):p.execute(args,root)
                command.assert_not_called()

    def test_cli_requires_explicit_mode_and_review_hash(self):
        for argv in ([str(SCRIPT),"--output","unused"],[str(SCRIPT),"--execute","--output","unused","--prepared","unused"]):
            with mock.patch.object(sys,"argv",argv),mock.patch.object(p,"execute") as execute,mock.patch.object(p,"prepare") as prepare:
                with self.assertRaises(SystemExit):p.main()
                execute.assert_not_called();prepare.assert_not_called()


if __name__=="__main__": unittest.main()
