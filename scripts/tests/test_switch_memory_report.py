#!/usr/bin/env python3
"""Exercise the production TraceLab analyzer with synthetic validation sessions.

Checks report interpretation and exclusion rules, not device correctness or
measured hardware performance. No Switch SDK or npm packages are required.
"""

import argparse
import copy
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def fixture():
    phases = []
    for index, variant in enumerate(("baseline", "dedup", "dedup", "baseline",
                                     "dedup", "baseline", "baseline", "dedup")):
        dedup = variant == "dedup"
        phases.append({
            "index": index, "variant": variant, "complete": True,
            "measured_intervals": 550 if dedup else 500,
            "duration_ms": 20000,
            "loops_per_second": 27.5 if dedup else 25,
            "p50_ms": 36 if dedup else 40,
            "p95_ms": 44 if dedup else 50,
            "p99_ms": 60 if dedup else 75,
            "publish_calls": 40, "publish_bytes": 65536,
            "dedup_calls": 20 if dedup else 0,
            "dedup_bytes": 32768 if dedup else 0,
            "excluded_intervals": 75, "excluded_duration_ms": 3000,
        })
    return {
        "schema_version": 1, "title": "PCSD00035", "enabled": True,
        "strict_hash": True, "hash_skipping": False, "timebase_hz": 19200000,
        "reference_scope": "ordinary_texture_cache_existing_hash_footprint",
        "tests_dropped": 0,
        "tests": [
            {"suite": "jit_selftest", "name": "first write", "status": "pass",
             "detail": "isolated self-test", "elapsed_ticks": 200, "iterations": 5},
            {"suite": "capability", "name": "stack permission", "status": "skip",
             "detail": "unsupported <capability>&", "elapsed_ticks": 400, "iterations": 1},
            {"suite": "store_microbench", "name": "direct writes", "status": "pass",
             "detail": "iterations are stores", "elapsed_ticks": 19200, "iterations": 100000},
        ],
        "benchmark": {
            "order": "ABBA/BAAB", "auto_start": True,
            "warmup_seconds": 120, "settle_seconds": 3, "measure_seconds": 20,
            "requested": True, "completed": True, "aborted": False,
            "run_id": "synthetic<&", "frames_overflow": False, "phases": phases,
        },
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--node", default=os.environ.get("CODEX_PRIMARY_RUNTIME_NODE")
                        or shutil.which("node"))
    args = parser.parse_args()
    if not args.node:
        raise SystemExit("Node is required: set CODEX_PRIMARY_RUNTIME_NODE or pass --node.")

    with tempfile.TemporaryDirectory(prefix="vita3k-memory-report-") as directory:
        session = Path(directory)
        manifest = {
            "schema_version": 1, "title": "PCSD00035", "frames_recorded": 3,
            "frames_overflow": False, "events_dropped": 0, "trace_bytes": 24,
            "gpu_timestamps": "not measured", "metadata": {},
        }
        (session / "manifest.json").write_text(json.dumps(manifest))
        (session / "hotspots.csv").write_text(
            "elapsed_ms,metric,calls,bytes,sampled_calls,sampled_wall_ms,max_sampled_us\n")
        (session / "frames.csv").write_text(
            "elapsed_ms,render_frame_id\n40,1\n80,2\n120,3\n")
        (session / "trace.bin").write_bytes(b"TLABv1\0\0" + struct.pack("<IIQ", 1, 24, 19200000))
        memory_path = session / "memory-validation.json"
        html_path = session / "report.html"

        def analyze(data, extra_args=()):
            if data is None:
                memory_path.unlink(missing_ok=True)
            elif isinstance(data, str):
                memory_path.write_text(data)
            else:
                memory_path.write_text(json.dumps(data))
            result = subprocess.run(
                [args.node, str(ROOT / "scripts/analyze-switch-tracelab.mjs"),
                 str(session), "--html", str(html_path), *extra_args],
                check=True, capture_output=True, text=True, timeout=15)
            return json.loads(result.stdout)["memory_validation"], html_path.read_text()

        old, html = analyze(None)
        assert old["available"] is False
        assert "no memory-validation.json" in html

        good = fixture()
        report, html = analyze(good)
        assert report["schema_supported"]
        assert report["test_results_complete"] is True and report["tests_dropped"] == 0
        assert report["test_counts"] == {"pass": 2, "fail": 0, "skip": 1, "unknown": 0}
        assert report["test_groups"]["self_tests"]["pass"] == 1
        assert report["test_groups"]["capabilities"]["skip"] == 1
        assert report["tests"][0]["ns_per_store"] is None
        assert report["tests"][1]["ns_per_store"] is None
        assert math.isclose(report["tests"][2]["ns_per_store"], 10)
        benchmark = report["benchmark"]
        assert benchmark["all_pairs_eligible"]
        assert benchmark["comparison_exclusions"] == []
        assert len(benchmark["phases"]) == 8
        assert benchmark["phases"][0]["excluded_intervals"] == 75
        assert benchmark["phases"][0]["excluded_duration_ms"] == 3000
        assert [(pair["baseline_phase"], pair["dedup_phase"]) for pair in benchmark["pairs"]] == [
            (0, 1), (3, 2), (5, 4), (6, 7)]
        assert all(math.isclose(pair["loops_per_second_delta_pct"], 10)
                   for pair in benchmark["pairs"])
        assert all(math.isclose(pair["p99_interval_delta_pct"], -20)
                   for pair in benchmark["pairs"])
        assert math.isclose(benchmark["paired_summary"]["median_loops_per_second_delta_pct"], 10)
        assert "Correctness self-tests" in html and "Hardware capabilities" in html
        assert "unsupported &lt;capability&gt;&amp;" in html
        assert "synthetic&lt;&amp;" in html
        assert "does not prove complete Guest/HLE/GPU write coverage" in html
        assert "Surface-cache hits, DoubleBuffer block memoization" in html
        assert "does not compare GPU output images" in html

        # Dedicated phase measurements retain their full-session scope even
        # when the user requests a shorter window for ordinary TraceLab tables.
        selected, _ = analyze(good, ("--start-ms", "70", "--end-ms", "110"))
        assert selected["benchmark"]["paired_summary"] == benchmark["paired_summary"]
        assert "independent of --start-ms/--end-ms" in selected["scope"]

        def excluded(data, reason):
            rejected, rejected_html = analyze(data)
            bench = rejected["benchmark"]
            assert reason in bench["comparison_exclusions"], bench
            assert not bench["all_pairs_eligible"]
            assert bench["paired_summary"] is None
            assert all(pair["loops_per_second_delta_pct"] is None for pair in bench["pairs"])
            assert "Median paired loops/s delta: n/a" in rejected_html
            return rejected

        for key, value, reason in (
            ("aborted", True, "benchmark_aborted_or_unknown"),
            ("completed", False, "benchmark_incomplete"),
            ("frames_overflow", True, "frame_overflow_or_unknown"),
            ("requested", False, "benchmark_not_requested"),
        ):
            data = copy.deepcopy(good)
            data["benchmark"][key] = value
            excluded(data, reason)

        for key, value in (("strict_hash", False), ("hash_skipping", True)):
            data = copy.deepcopy(good)
            data[key] = value
            excluded(data, "strict_hash_reference_not_confirmed")

        data = copy.deepcopy(good)
        data["title"] = "different title"
        excluded(data, "title_mismatch")
        data = copy.deepcopy(good)
        data.pop("reference_scope")
        excluded(data, "reference_scope_not_confirmed")
        data = copy.deepcopy(good)
        data["tests_dropped"] = 3
        dropped = excluded(data, "test_results_incomplete")
        assert dropped["tests_dropped"] == 3 and dropped["test_results_complete"] is False
        assert "3 results dropped" in html_path.read_text()
        data = copy.deepcopy(good)
        data.pop("tests_dropped")
        excluded(data, "test_capture_completeness_not_reported")

        data = copy.deepcopy(good)
        data["benchmark"]["phases"].pop()
        excluded(data, "incomplete_or_unexpected_phase_order")
        for key, value in (("complete", False), ("measured_intervals", 20),
                           ("duration_ms", 4000), ("dedup_calls", -1)):
            data = copy.deepcopy(good)
            data["benchmark"]["phases"][0][key] = value
            excluded(data, "phase_quality_check_failed")

        data = copy.deepcopy(good)
        data["tests"][0]["status"] = "fail"
        failed = excluded(data, "self_test_failed_or_unknown")
        assert failed["failed_tests"][0]["category"] == "self_tests"
        assert failed["failed_tests"][0]["name"] == "first write"
        assert "Correctness self-tests failed; paired performance deltas are withheld" in html_path.read_text()

        # A reported capability failure is prominent but does not become a
        # fabricated JIT self-test failure. Ordinary unsupported probes use skip.
        data = copy.deepcopy(good)
        data["tests"][1]["status"] = "fail"
        failed_capability, capability_html = analyze(data)
        assert failed_capability["test_groups"]["self_tests"]["fail"] == 0
        assert failed_capability["test_groups"]["capabilities"]["fail"] == 1
        assert failed_capability["failed_tests"][0]["category"] == "capabilities"
        assert "Recorded failures:" in capability_html and "stack permission" in capability_html
        assert "a cadence result does not certify them" in capability_html

        data = copy.deepcopy(good)
        for phase in data["benchmark"]["phases"]:
            phase["dedup_calls"] = phase["dedup_bytes"] = 0
        no_work, no_work_html = analyze(data)
        assert no_work["benchmark"]["paired_summary"] is None
        assert all("no_actual_staging_dedup" in pair["comparison_exclusions"]
                   and pair["loops_per_second_delta_pct"] is None
                   for pair in no_work["benchmark"]["pairs"])
        assert "no_actual_staging_dedup" in no_work_html

        # Labels alone must not turn unrelated elapsed_ticks into a store cost.
        data = copy.deepcopy(good)
        data["tests"][2]["suite"] = "a_test_with_store_in_its_name"
        other, _ = analyze(data)
        assert other["tests"][2]["ns_per_store"] is None
        data = copy.deepcopy(good)
        data["tests"][2]["iterations"] = 0
        zero, _ = analyze(data)
        assert zero["tests"][2]["ns_per_store"] is None

        for malformed in ('{"schema_version":', {"schema_version": 2, "tests": []},
                          {"schema_version": 1, "tests": [None]}):
            invalid, _ = analyze(malformed)
            assert invalid["available"] and not invalid["schema_supported"]

    print("Switch memory validation analyzer JSON/HTML and exclusion tests OK")


if __name__ == "__main__":
    main()
