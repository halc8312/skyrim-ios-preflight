"""Authored console/SDK result fixtures only; no Apple SDK or guest execution."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("device_features_driver", Path(__file__).with_name("run_device_features_tests.py"))
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)
DESTINATION = "platform=iOS Simulator,id=12345678-1234-1234-1234-123456789abc"
GRAPHICS = dict(schema=1, backend=5, checks=255, rendered_cases=6, offscreen=True, displayed=False, game=False, iphone=False)
AUDIO = dict(schema=1, audio_version=1, transport_bytes=160, native_status_bytes=256,
             source_rate_hz=48000, source_channels=2, client_capacity=8, queue_capacity_frames=8192,
             profiles=3, authored_only=True, game_audio_verified=False, audible_output_verified=False,
             physical_iphone_verified=False)


def console(group, tests=None):
    cls, expected, _ = driver.GROUPS[group]
    tests = expected if tests is None else tests
    lines = []
    for name in tests:
        case = f"-[OwnedGameHostTests.{cls} {name}]"
        lines += [f"Test Case '{case}' started.", f"Test Case '{case}' passed (0.001 seconds)."]
    if group == "graphics": lines += [driver.GRAPHICS_PREFIX + json.dumps(GRAPHICS)]
    if group == "audio": lines += [driver.AUDIO_PREFIX + json.dumps(AUDIO)]
    lines += [f"Executed {len(expected)} tests, with 0 failures (0 unexpected)", "** TEST EXECUTE SUCCEEDED **"]
    return "\n".join(lines) + "\n"


def summary(group):
    count = len(driver.GROUPS[group][1])
    return dict(totalTestCount=count, passedTests=count, failedTests=0, skippedTests=0, expectedFailures=0)


class DeviceFeatureDriverTests(unittest.TestCase):
    def test_three_fixed_groups_exact_cases_and_either_serial_order(self):
        self.assertEqual({k: len(v[1]) for k,v in driver.GROUPS.items()}, dict(graphics=1,settings=6,audio=6))
        for group, (_, tests, _) in driver.GROUPS.items():
            for order in (tests, tuple(reversed(tests))):
                with self.subTest(group=group):
                    driver.validate_console(group, console(group,order))
                    driver.validate_summary(group, summary(group))
                    driver.validate_marker(group,console(group,order))

    def test_fixed_selectors_match_public_swift_sources_and_prior_settings_set_refuses(self):
        for group, (cls, names, _) in driver.GROUPS.items():
            source = (Path(__file__).parent / "tests" / (cls + ".swift")).read_text(encoding="utf-8")
            declared = driver.re.findall(r"\bfunc\s+(test\w+)\s*\(", source)
            self.assertEqual(len(declared), len(names), group)
            self.assertEqual(set(declared), set(names), group)
        added = "testAudioCommandWorkerCoalescesAndHonorsLatestIntent"
        self.assertIn(added, driver.SETTINGS_TESTS)
        prior = tuple(name for name in driver.SETTINGS_TESTS if name != added)
        old_console = console("settings", prior).replace("Executed 6 tests", "Executed 5 tests")
        with self.assertRaises(ValueError):
            driver.validate_console("settings", old_console)
        with self.assertRaises(ValueError):
            driver.validate_summary("settings", dict(summary("settings"), totalTestCount=5, passedTests=5))

    def test_missing_duplicate_unknown_failed_skipped_and_interleaved_cases_refuse(self):
        for group, (_, tests, _) in driver.GROUPS.items():
            valid = console(group)
            duplicated = tests[:-1]+tests[:1] if len(tests)>1 else tests+tests
            bad = [console(group,tests[:-1]),console(group,duplicated),
                   console(group,tests+("testUnknown",)),console(group,("testUnknown",)+tests[1:]),
                   valid.replace("passed (","failed (",1),valid.replace("passed (","skipped (",1),
                   valid.replace("started.","skipped.",1),valid+"Test Case 'unknown' passed.\n"]
            if len(tests)>1:
                lines=valid.splitlines(keepends=True);lines[1],lines[2]=lines[2],lines[1];bad.append("".join(lines))
            for value in bad:
                with self.subTest(group=group,value=value),self.assertRaises(ValueError):
                    driver.validate_console(group,value)

    def test_console_wrong_counts_terminals_and_ui_warning_refuse(self):
        for group in driver.GROUPS:
            valid=console(group)
            for value in [valid.replace("0 failures","1 failure"),valid.replace("0 unexpected","1 unexpected"),
                          valid.replace("** TEST EXECUTE SUCCEEDED **","** TEST SUCCEEDED **"),
                          valid+"** TEST EXECUTE SUCCEEDED **\n",valid+"** TEST EXECUTE FAILED **\n",
                          "\n".join(x for x in valid.splitlines() if not x.startswith("Executed")),
                          valid+"Modifying properties of a view’s layer off the main thread"]:
                with self.subTest(group=group),self.assertRaises(ValueError):driver.validate_console(group,value)

    def test_summary_counts_are_strict_integers_no_skips_or_expected_failure(self):
        for group in driver.GROUPS:
            for key,wanted in summary(group).items():
                for value in (True,False,None,"0",0.0,wanted+1,-1):
                    wrong=dict(summary(group),**{key:value})
                    with self.subTest(group=group,key=key,value=value),self.assertRaises(ValueError):driver.validate_summary(group,wrong)
                wrong=summary(group);del wrong[key]
                with self.assertRaises(ValueError):driver.validate_summary(group,wrong)
            for value in (None,[],"summary"):
                with self.assertRaises(ValueError):driver.validate_summary(group,value)

    def test_native_markers_exact_typed_fields_and_no_false_success(self):
        for group,prefix,value in [("graphics",driver.GRAPHICS_PREFIX,GRAPHICS),("audio",driver.AUDIO_PREFIX,AUDIO)]:
            raw=prefix+json.dumps(value)
            self.assertEqual(driver.validate_marker(group,raw),value)
            for key,wanted in value.items():
                bad_values=(None,0,1,"true") if type(wanted)is bool else (True,False,None,str(wanted),wanted+1,float(wanted))
                for bad in bad_values:
                    changed=dict(value,**{key:bad})
                    with self.subTest(group=group,key=key,bad=bad),self.assertRaises(ValueError):driver.validate_marker(group,prefix+json.dumps(changed))
                changed=dict(value);del changed[key]
                with self.assertRaises(ValueError):driver.validate_marker(group,prefix+json.dumps(changed))
            for bad in ["",raw+"\n"+raw,"fake "+raw,prefix+'[]',prefix+'"'+str(1)+'"',
                        prefix+json.dumps(dict(value,unknown=0)),raw[:-1]+',"schema":1}',prefix+'{"schema":NaN}']:
                with self.subTest(group=group,bad=bad),self.assertRaises(ValueError):driver.validate_marker(group,bad)
        with self.assertRaises(ValueError):driver.validate_marker("settings",driver.GRAPHICS_PREFIX+json.dumps(GRAPHICS))
        with self.assertRaises(ValueError):driver.validate_marker("audio",console("graphics"))

    def test_commands_are_separate_fixed_classes_without_build_retry_or_iterations(self):
        for group,(cls,tests,allowance) in driver.GROUPS.items():
            command=driver.make_test_command(group,Path("built/DerivedData"),DESTINATION,Path("fresh/result.xcresult"))
            self.assertEqual(command[-1],"test-without-building")
            self.assertEqual(command[command.index("-destination")+1],DESTINATION)
            self.assertEqual(command[command.index("-parallel-testing-enabled")+1],"NO")
            self.assertEqual(command[command.index("-maximum-test-execution-time-allowance")+1],str(allowance))
            self.assertEqual([x for x in command if x.startswith("-only-testing:")],
                             [f"-only-testing:OwnedGameHostTests/{cls}/{name}" for name in tests])
            for forbidden in ("build-for-testing","build","-retry-tests-on-failure","-test-iterations"):
                self.assertNotIn(forbidden,command)
            self.assertFalse(any("MobileControlsTests" in x or "OwnedHostFullChainTests" in x for x in command))

    def paths(self,root):
        stage=root/"stage";built=root/"built";output=root/"features"
        (stage/"OwnedGameHost.xcodeproj").mkdir(parents=True)
        (built/"DerivedData/Build/Products/Debug-iphonesimulator/OwnedGameHost.app").mkdir(parents=True)
        return stage,built,output

    def test_no_non_darwin_execution_or_output(self):
        with tempfile.TemporaryDirectory()as temp:
            stage,built,output=self.paths(Path(temp))
            with mock.patch.object(driver.sys,"platform","linux"),mock.patch.object(driver._sdk,"run_bounded")as run,self.assertRaises(RuntimeError):
                driver.main(["--stage",str(stage),"--built-output",str(built),"--output",str(output),"--destination",DESTINATION])
            run.assert_not_called();self.assertFalse(output.exists())

    def simulate(self,fail_group=None,bad_summary=False,expire_first_group=False):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        stage,built,output=self.paths(Path(temp.name));commands=[];clock=[100.0]
        def run(command,cwd,prefix,timeout):
            group=prefix.parent.name;commands.append((group,command,timeout))
            self.assertGreater(timeout,0);self.assertLessEqual(timeout,driver.GROUP_DEADLINE_SECONDS)
            if command[0]=="xcodebuild":
                Path(command[command.index("-resultBundlePath")+1]).mkdir()
                stdout=console(group);code=1 if group==fail_group else 0
            else:
                data=summary(group)
                if bad_summary and group=="audio":data["skippedTests"]=1
                stdout=json.dumps(data);code=0
            Path(str(prefix)+".stdout.log").write_text(stdout,encoding="utf-8")
            Path(str(prefix)+".stderr.log").write_text("",encoding="utf-8")
            if expire_first_group:clock[0]+=driver.GROUP_DEADLINE_SECONDS+1
            return code
        attachments=dict(attachment_export_available=1,attachment_exported=1,exported_png_count=2)
        with mock.patch.object(driver.sys,"platform","darwin"),mock.patch.object(driver._sdk,"run_bounded",side_effect=run),\
             mock.patch.object(driver.time,"monotonic",side_effect=lambda:clock[0]),\
             mock.patch.object(driver._mobile,"export_attachments",return_value=attachments)as export,contextlib.redirect_stdout(io.StringIO()):
            args=["--stage",str(stage),"--built-output",str(built),"--output",str(output),"--destination",DESTINATION]
            if fail_group or bad_summary or expire_first_group:
                with self.assertRaises((ValueError,RuntimeError)):driver.main(args)
                self.assertFalse((output/"public-device-features-result.json").exists())
            else:
                self.assertEqual(driver.main(args),0)
                report=json.loads((output/"public-device-features-result.json").read_text())
                self.assertEqual(report["expected_cases"],13)
                self.assertEqual(set(report["groups"]),set(driver.GROUPS))
                for group in driver.GROUPS:
                    self.assertTrue((output/group/"xcresult-summary.json").is_file())
                    self.assertTrue((output/group/"public-feature-result.json").is_file())
                self.assertEqual(export.call_count,1);self.assertEqual(export.call_args.args[1],output.resolve()/"settings")
                for key in ("game_executed","game_frame_rendered","physical_iphone_verified","ready_to_play"):
                    self.assertEqual(report[key],0)
        return commands

    def test_success_reuses_build_and_publishes_only_after_three_independent_groups(self):
        commands=self.simulate()
        self.assertEqual([g for g,c,_ in commands if c[0]=="xcodebuild"],["graphics","settings","audio"])
        self.assertEqual(len({c[c.index("-resultBundlePath")+1] for _,c,_ in commands if c[0]=="xcodebuild"}),3)

    def test_failed_class_or_bad_xcresult_preserves_failure_without_aggregate(self):
        for group in driver.GROUPS:self.simulate(fail_group=group)
        self.simulate(bad_summary=True)

    def test_exhausted_group_deadline_stops_before_summary_or_next_class(self):
        commands=self.simulate(expire_first_group=True)
        self.assertEqual([(group,command[0]) for group,command,_ in commands],[("graphics","xcodebuild")])


if __name__=="__main__":unittest.main()
