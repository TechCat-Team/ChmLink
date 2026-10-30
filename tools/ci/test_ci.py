#!/usr/bin/env python3
"""Tests for the CI helper scripts. Run: python3 -m unittest discover -s tools/ci"""
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_driver_version as cdv   # noqa: E402
import compare_bench as cb           # noqa: E402

GIT_ENV = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@t", GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")


def run_main(module, argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = module.main(argv)
    return code, out.getvalue(), err.getvalue()


class Repo:
    def __init__(self, root):
        self.root = root
        self.git("init", "-q", "-b", "main")

    def git(self, *args):
        return subprocess.run(("git", "-C", self.root) + args, env=GIT_ENV, check=True,
                              capture_output=True, text=True).stdout.strip()

    def write(self, path, text):
        full = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as handle:
            handle.write(text)

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")


def inf(version):
    return "[Version]\nSignature = \"$WINDOWS NT$\"\nDriverVer = 10/30/2026,%s\nCatalogFile = chmlink.cat\n" % version


class DriverVersionTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.repo = Repo(self.dir.name)
        self.repo.write("driver/chmlink.inf", inf("0.3.0.0"))
        self.repo.write("driver/control.c", "int a;\n")
        self.repo.write("include/chmlink/api.h", "/* sdk */\n")
        self.repo.write("include/chmlink/query.h", "/* wire */\n")
        self.repo.write("README.md", "x\n")
        self.base = self.repo.commit("base")

    def tearDown(self):
        self.dir.cleanup()

    def check(self, base=None):
        return run_main(cdv, ["--repo", self.dir.name, "--base", base or self.base])

    def test_unrelated_change_needs_no_bump(self):
        self.repo.write("README.md", "y\n")
        self.repo.commit("docs")
        self.assertEqual(self.check()[0], 0)

    def test_sdk_only_header_needs_no_bump(self):
        self.repo.write("include/chmlink/api.h", "/* changed */\n")
        self.repo.commit("sdk")
        self.assertEqual(self.check()[0], 0)

    def test_driver_source_without_bump_fails(self):
        self.repo.write("driver/control.c", "int b;\n")
        self.repo.commit("driver")
        code, out, _ = self.check()
        self.assertEqual(code, 1)
        self.assertIn("still 0.3.0.0", out)

    def test_shared_header_without_bump_fails(self):
        self.repo.write("include/chmlink/query.h", "#define CHMLINK_INFO_SIZE 256u\n")
        self.repo.commit("wire")
        self.assertEqual(self.check()[0], 1)

    def test_bump_passes(self):
        self.repo.write("driver/control.c", "int b;\n")
        self.repo.write("driver/chmlink.inf", inf("0.3.1.0"))
        self.repo.commit("driver")
        self.assertEqual(self.check()[0], 0)

    def test_downgrade_fails(self):
        self.repo.write("driver/control.c", "int b;\n")
        self.repo.write("driver/chmlink.inf", inf("0.2.9.9"))
        self.repo.commit("driver")
        code, out, _ = self.check()
        self.assertEqual(code, 1)
        self.assertIn("backwards", out)

    def test_versions_compare_numerically(self):
        # As strings "10" < "9"; as numbers 0.3.9.0 -> 0.3.10.0 is an increase.
        self.repo.write("driver/chmlink.inf", inf("0.3.9.0"))
        nine = self.repo.commit("nine")
        self.repo.write("driver/chmlink.inf", inf("0.3.10.0"))
        self.repo.write("driver/control.c", "int b;\n")
        self.repo.commit("ten")
        self.assertEqual(self.check(base=nine)[0], 0)
        self.repo.write("driver/chmlink.inf", inf("0.3.9.0"))
        self.repo.commit("back")
        self.assertEqual(self.check(base=nine)[0], 1)   # equal to the base: not raised

    def test_inf_only_change_without_bump_fails(self):
        self.repo.write("driver/chmlink.inf", inf("0.3.0.0") + "; comment\n")
        self.repo.commit("inf")
        self.assertEqual(self.check()[0], 1)

    def test_null_base_falls_back_to_parent(self):
        self.repo.write("driver/control.c", "int b;\n")
        self.repo.commit("driver")
        self.assertEqual(self.check(base="0" * 40)[0], 1)

    def test_duplicate_driverver_is_an_error(self):
        self.repo.write("driver/chmlink.inf", inf("0.3.1.0") + "DriverVer = 10/30/2026,0.3.2.0\n")
        self.repo.write("driver/control.c", "int b;\n")
        self.repo.commit("driver")
        self.assertEqual(self.check()[0], 2)

    def test_main_advancing_after_the_branch_point_is_not_blamed_on_the_branch(self):
        # main gains a driver change with a bump after feature branched off. Comparing the tip of
        # main with the feature tip would see that change "reverted" by the feature.
        self.repo.git("checkout", "-q", "-b", "feature")
        self.repo.write("README.md", "feature work\n")
        self.repo.commit("feature")
        self.repo.git("checkout", "-q", "main")
        self.repo.write("driver/control.c", "int main_change;\n")
        self.repo.write("driver/chmlink.inf", inf("0.3.1.0"))
        main_tip = self.repo.commit("main driver change")
        self.repo.git("checkout", "-q", "feature")
        self.assertEqual(self.check(base=main_tip)[0], 0)

    def test_comment_only_change_needs_no_bump(self):
        # Same compiled code: a comment edit, a reflowed block comment, trailing whitespace.
        self.repo.write("driver/control.c", "int a; /* now with a note */\n")
        self.repo.commit("comment")
        self.assertEqual(self.check()[0], 0)
        self.repo.write("include/chmlink/query.h", "/* wire,\n   reworded */\n")
        self.repo.commit("header comment")
        self.assertEqual(self.check()[0], 0)

    def test_line_comment_changes_need_no_bump(self):
        self.repo.write("driver/control.c", "int a; // added a note\n")
        self.repo.commit("line comment")
        self.assertEqual(self.check()[0], 0)
        self.repo.write("driver/control.c", "int a; // a different note\n// and a new line\n")
        self.repo.commit("line comment again")
        self.assertEqual(self.check()[0], 0)

    def test_code_change_hidden_next_to_a_comment_still_needs_a_bump(self):
        self.repo.write("driver/control.c", "int b; /* a is now b */\n")
        self.repo.commit("code")
        self.assertEqual(self.check()[0], 1)

    def test_comment_markers_inside_strings_are_code(self):
        self.repo.write("driver/control.c", 'int a; char *s = "x";\n')
        base = self.repo.commit("one")
        self.repo.write("driver/control.c", 'int a; char *s = "/* not a comment */";\n')
        self.repo.commit("string")
        self.assertEqual(self.check(base=base)[0], 1)

    def test_line_comment_markers_in_strings_and_urls(self):
        self.repo.write("driver/control.c", 'char *u = "http://a";\n')
        base = self.repo.commit("url")
        self.repo.write("driver/control.c", 'char *u = "http://b";\n')
        self.repo.commit("url changed")
        self.assertEqual(self.check(base=base)[0], 1)

    def test_inf_comment_change_still_needs_a_bump(self):
        # Windows hashes the INF text, comments included.
        self.repo.write("driver/chmlink.inf", inf("0.3.0.0") + "; comment\n")
        self.repo.commit("inf")
        self.assertEqual(self.check()[0], 1)

    def test_bad_repo_is_an_error(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertEqual(run_main(cdv, ["--repo", empty, "--base", "HEAD"])[0], 2)


def result(scenario, **fields):
    record = {"event": "result", "scenario": scenario}
    record.update(fields)
    return record


def full_results(scale=1.0, drops=0, rx_loss=0.0, consumer_error=0, p50=50.0, loopback=280000):
    return [
        {"event": "info", "cpus": 8},
        result("latency", payload=64, samples=1000, lost=0, p50_us=p50, p99_us=240.0, p999_us=430.0, max_us=1000.0),
        result("tx_64B", tx_delivered_pps=500000 * scale, drv_tx_packets=1000000, drv_tx_queue_drops=drops,
               tx_consumer_error=consumer_error),
        result("rx_64B_batch32", rx_received_pps=550000 * scale, rx_loss_pct=rx_loss, rx_inject_errors=0,
               rx_last_error=0),
        result("duplex_64B", tx_delivered_pps=470000 * scale, drv_tx_packets=1000000, drv_tx_queue_drops=drops,
               tx_consumer_error=0, rx_received_pps=540000 * scale, rx_loss_pct=0.0, rx_inject_errors=0,
               rx_last_error=0),
        result("loopback", payload=64, received_pps=loopback),
        {"event": "done"},
    ]


class CompareBenchTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.env = self.path("env.json")
        with open(self.env, "w", encoding="utf-8-sig") as handle:
            json.dump({"Cpu": "TestCPU", "Logical": 8}, handle)
        self.baseline = self.path("baseline.json")
        self.run_bench(full_results(), "--update-baseline", "--commit", "abc", "--driver", "1.0.0.0")

    def tearDown(self):
        self.dir.cleanup()

    def path(self, name):
        return os.path.join(self.dir.name, name)

    def write_results(self, name, records):
        path = self.path(name)
        with open(path, "w", encoding="utf-8-sig") as handle:
            handle.write("\n".join(json.dumps(r) for r in records) + "\n")
        return path

    def run_bench(self, records, *extra, repeats=None):
        files = [self.write_results("r%d.jsonl" % i, rec) for i, rec in enumerate(repeats or [records])]
        return run_main(cb, files + ["--env", self.env, "--baseline", self.baseline] + list(extra))

    def test_identical_run_passes(self):
        self.assertEqual(self.run_bench(full_results())[0], 0)

    def test_noise_within_tolerance_passes(self):
        self.assertEqual(self.run_bench(full_results(scale=0.9))[0], 0)

    def test_throughput_regression_fails(self):
        code, out, _ = self.run_bench(full_results(scale=0.8))
        self.assertEqual(code, 1)
        self.assertIn("**FAIL**", out)

    def test_drop_regression_fails(self):
        code, out, _ = self.run_bench(full_results(drops=100000))
        self.assertEqual(code, 1)
        self.assertIn("tx_drop_pct", out)

    def test_small_drop_within_allowance_passes(self):
        self.assertEqual(self.run_bench(full_results(drops=2000))[0], 0)   # 0.2% < 0.5%

    def test_receive_loss_fails(self):
        self.assertEqual(self.run_bench(full_results(rx_loss=1.0))[0], 1)

    def test_consumer_error_always_fails(self):
        self.assertEqual(self.run_bench(full_results(consumer_error=5))[0], 1)

    def test_latency_regression_fails_and_p999_is_informational(self):
        self.assertEqual(self.run_bench(full_results(p50=90.0))[0], 1)
        records = full_results()
        records[1]["p999_us"] = 99999.0
        self.assertEqual(self.run_bench(records)[0], 0)

    def test_median_ignores_one_bad_repeat(self):
        good, bad = full_results(), full_results(scale=0.5)
        self.assertEqual(self.run_bench(None, repeats=[good, bad, full_results(scale=1.02)])[0], 0)

    def test_errors_use_worst_repeat(self):
        runs = [full_results(), full_results(consumer_error=3), full_results()]
        self.assertEqual(self.run_bench(None, repeats=runs)[0], 1)

    def test_missing_baseline_profile(self):
        with open(self.env, "w") as handle:
            json.dump({"Cpu": "Other", "Logical": 4}, handle)
        self.assertEqual(self.run_bench(full_results())[0], 0)
        self.assertEqual(self.run_bench(full_results(), "--require-baseline")[0], 3)
        # Error counters are still gated without a baseline.
        self.assertEqual(self.run_bench(full_results(consumer_error=1))[0], 1)

    def test_loopback_drift_warns(self):
        code, out, _ = self.run_bench(full_results(loopback=150000))
        self.assertEqual(code, 0)
        self.assertIn("Warning", out)

    def test_markdown_file_is_written(self):
        out = self.path("summary.md")
        self.assertEqual(self.run_bench(full_results(), "--markdown", out)[0], 0)
        with open(out, encoding="utf-8") as handle:
            self.assertIn("| scenario | metric |", handle.read())

    def test_update_baseline_records_metadata(self):
        with open(self.baseline) as handle:
            profile = json.load(handle)["profiles"]["TestCPU|8"]
        self.assertEqual((profile["commit"], profile["driver"]), ("abc", "1.0.0.0"))
        self.assertEqual(profile["scenarios"]["tx_64B"]["tx_pps"], 500000)

    def test_environment_file_in_the_system_code_page_is_readable(self):
        with open(self.env, "wb") as handle:
            handle.write('{"Os": "Windows 11 \u4e13\u4e1a\u4e2d\u6587\u7248", "Cpu": "TestCPU", "Logical": 8}'.encode("gb18030"))
        self.assertEqual(self.run_bench(full_results())[0], 0)

    def test_bad_input_is_a_usage_error(self):
        bad = self.path("bad.jsonl")
        with open(bad, "w") as handle:
            handle.write("{not json\n")
        self.assertEqual(run_main(cb, [bad, "--env", self.env, "--baseline", self.baseline])[0], 2)
        empty = self.write_results("empty.jsonl", [{"event": "info"}])
        self.assertEqual(run_main(cb, [empty, "--env", self.env, "--baseline", self.baseline])[0], 2)


class PowerShellLintTest(unittest.TestCase):
    """Windows PowerShell 5.1 evaluates $PSScriptRoot as empty inside a parameter default of a
    [CmdletBinding()] script started with -File (README documents that form). A default such as
    "$PSScriptRoot/../build" then silently becomes "/../build", the root of the current drive."""

    def test_no_psscriptroot_in_param_defaults(self):
        root = os.path.dirname(os.path.dirname(HERE))
        offenders = []
        for base, _, names in os.walk(os.path.join(root, "tools")):
            for name in names:
                if not name.endswith(".ps1"):
                    continue
                path = os.path.join(base, name)
                with open(path, encoding="utf-8-sig") as handle:
                    text = handle.read()
                start = text.find("param(")
                if start < 0:
                    continue
                depth, end = 0, start + len("param")
                for index in range(end, len(text)):
                    depth += (text[index] == "(") - (text[index] == ")")
                    if depth == 0:
                        end = index
                        break
                if "$PSScriptRoot" in text[start:end]:
                    offenders.append(os.path.relpath(path, root))
        self.assertEqual(offenders, [], "compute these defaults in the script body instead")


if __name__ == "__main__":
    unittest.main()
