#!/usr/bin/python3
"""Run bounded blueprint stress scenarios in fresh disposable mudlibs."""
import argparse
import hashlib
import json
import re
import shutil
import socket
import subprocess
import time
from pathlib import Path


def validate_measurements(debug, case):
    """Join native observations to independently checked LPC request results."""
    metrics = {"request": [], "batch": [], "lpc": []}
    errors, pending = [], []
    for line in debug.splitlines():
        fields = {key: int(value) if re.fullmatch(r"-?\d+", value) else value
                  for key, value in re.findall(r"(\w+)=(\S+)", line)}
        if line.startswith("BLUEPRINT_MEASURE_REQUEST:"):
            metrics["request"].append(fields)
            pending.append(fields)
        elif line.startswith("BLUEPRINT_MEASURE_BATCH:"):
            fields["ids"] = [row.get("id") for row in pending]
            fields["request_microseconds"] = sum(
                row.get("microseconds", 0) for row in pending
                if isinstance(row.get("microseconds"), int))
            metrics["batch"].append(fields)
            pending = []
        elif line.startswith(("BLUEPRINT_STRESS_RESULT:", "BLUEPRINT_STRESS_AUX_RESULT:")):
            fields["main"] = line.startswith("BLUEPRINT_STRESS_RESULT:")
            metrics["lpc"].append(fields)
    if pending:
        errors.append("request measurements lack a terminating batch record")
    rows, batches, observations = metrics["request"], metrics["batch"], metrics["lpc"]
    expected_attempts = case.get("cycles", 2) if case.get("expected", "completed") == "completed" else 1
    batch_size = case.get("batch", 1)
    if not rows or not batches:
        errors.append("missing actual backend measurement")
    if len(rows) != expected_attempts * batch_size or len(batches) != expected_attempts:
        errors.append("incomplete request or batch measurements")
    lpc = {}
    lpc_batches = {}
    main_count = 0
    for report in observations:
        if (any(not isinstance(report.get(key), int) for key in ("id", "batch", "matched", "updated"))
                or report.get("status") not in ("completed", "failed")):
            errors.append("incomplete LPC result record")
            continue
        ident = report["id"]
        if ident in lpc:
            errors.append("duplicate LPC request ID")
        lpc[ident] = report
        lpc_batches.setdefault(report["batch"], set()).add(ident)
        if report["main"]:
            main_count += 1
            if ident != report["batch"]:
                errors.append("main request has inconsistent batch membership")
    if main_count != expected_attempts or len(lpc) != expected_attempts * batch_size:
        errors.append("incomplete independent LPC request results")
    if len(lpc_batches) != expected_attempts or any(len(ids) != batch_size for ids in lpc_batches.values()):
        errors.append("incomplete independent LPC batch results")
    if len(rows) != len({row.get("id") for row in rows}):
        errors.append("duplicate request metric ID")
    if {row.get("id") for row in rows} != set(lpc):
        errors.append("native and LPC request IDs differ")
    memory_fields = [point + suffix for point in ("entry", "compiled", "prepared", "retired")
                     for suffix in ("_used", "_allocated")]
    required = ["id", "microseconds", "matched", "updated", "outcome", "points", "candidate_bytes"] + memory_fields
    for row in rows:
        if any(not isinstance(row.get(key), int) for key in required):
            errors.append("incomplete native request measurement")
            continue
        report = lpc.get(row["id"])
        if report:
            wanted = 1 if report["status"] == "completed" else 0
            if row["outcome"] != wanted or row["matched"] != report["matched"] or row["updated"] != report["updated"]:
                errors.append("native outcome/counts disagree with LPC result")
            if wanted == 1 and (row["points"] != 15 or row["candidate_bytes"] <= 0):
                errors.append("completed LPC request lacks candidate/preparation measurements")
        if row["microseconds"] < 0 or row["points"] not in (9, 11, 15):
            errors.append("invalid time or memory snapshot sequence")
        for point, bit in (("entry", 1), ("compiled", 2), ("prepared", 4), ("retired", 8)):
            if row["points"] & bit and not (0 <= row[point + "_used"] <= row[point + "_allocated"]):
                errors.append("invalid allocator snapshot")
    seen_batches = set()
    for batch in batches:
        if any(not isinstance(batch.get(key), int) for key in ("requests", "microseconds", "clock_ok", "overflow")):
            errors.append("incomplete native batch measurement")
            continue
        if batch["clock_ok"] != 1 or batch["overflow"] != 0 or batch["microseconds"] < batch["request_microseconds"]:
            errors.append("invalid or truncated batch timing")
        ids = set(batch["ids"])
        if batch["requests"] != batch_size or len(batch["ids"]) != batch_size or len(ids) != batch_size:
            errors.append("native batch shape differs from requested batch")
        matches = [ident for ident, expected_ids in lpc_batches.items() if ids == expected_ids]
        if len(matches) != 1 or matches[0] in seen_batches:
            errors.append("native batch membership differs from LPC results")
        else:
            seen_batches.add(matches[0])
    return metrics, errors


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--driver", type=Path, required=True)
    p.add_argument("--support", type=Path, default=Path(__file__).resolve().parent,
                   help="test directory containing inc and sys")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--cases", type=Path, required=True)
    p.add_argument("--timeout", type=int, default=180)
    p.add_argument("--allow-unmeasured", action="store_true",
                   help="run semantic controls without native timing; not a benchmark")
    a = p.parse_args()
    a.require_metrics = not a.allow_unmeasured
    if not 30 <= a.timeout <= 1800:
        p.error("timeout must be between 30 and 1800 seconds per case")
    a.output = a.output.absolute()
    a.output.mkdir()
    shutil.copy2(a.driver, a.output / "ldmud")
    shutil.copy2(__file__, a.output / "runner.py")
    driver = a.output / "ldmud"
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    cases = json.loads(a.cases.read_text())
    if not isinstance(cases, list) or not 1 <= len(cases) <= 64:
        p.error("cases must contain between 1 and 64 scenarios")
    (a.output / "cases.json").write_text(json.dumps(cases, indent=2) + "\n")
    source = Path(__file__).resolve().with_suffix("")
    results = []
    for index, case in enumerate(cases):
        if (not 1 <= case.get("count", 100) <= 100000
                or not 1 <= case.get("width", 8) <= 4096
                or not 1 <= case.get("cycles", 2) <= 32
                or not 1 <= case.get("batch", 1) <= 8
                or case.get("handles", 0) not in (0, 1, 2)):
            p.error("invalid bounded clone, width, cycle, batch or handle setting")
        print(f"case {index + 1}/{len(cases)}: {case}", flush=True)
        dest = a.output / f"case-{index:02d}"
        dest.mkdir()
        fixture = dest / "fixture"
        shutil.copytree(source, fixture)
        for shared in ("inc", "sys"):
            shutil.copytree(a.support / shared, fixture / shared)
        (fixture / "blueprint-measure").write_text("1\n")
        flags = ["-u-1", "-E", "0", "--no-compat", "-e", "-N",
                 "--cleanup-time", "-1", "--reset-time", "-1",
                 "--max-array", "0", "--max-callouts", "0", "--max-bytes", "0",
                 "--max-file", "0", "-s-1", "-sv-1", "--hard-malloc-limit", "unlimited",
                 "--min-malloc", "0", "-ru0", "-rm0", "-rs0", "--no-strict-euids",
                 "--no-wizlist-file", "--check-refcounts", "--check-state", "2",
                 "--access-file", "none", "--access-log", "none", "-f", "test",
                 "--alarm-time", "2", "-Mmaster",
                 "-m" + str(fixture), "--debug-file", str(dest / "debug")]
        if case.get("handles") == 2:
            flags += ["--python-script", "startup.py"]
        for key, val in case.items():
            if key in ("count", "width", "handles", "cycles", "late_target", "churn", "batch"):
                flags.append(f"-DSTRESS_{key.upper()}={int(val)}")
        if case.get("late_target") and case.get("handles"):
            raise ValueError("late target case requires handles=0")
        command = [str(driver)] + flags
        start, wall = time.monotonic(), time.time()
        timed_out = False
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            sock.listen()
            command += ["--inherit", str(sock.fileno())]
            with (dest / "stdout").open("w") as out, (dest / "stderr").open("w") as err:
                try:
                    run = subprocess.run(command, cwd=dest, stdout=out, stderr=err,
                                         pass_fds=(sock.fileno(),), timeout=a.timeout)
                    status = run.returncode
                except subprocess.TimeoutExpired:
                    status = None
                    timed_out = True
        duration, wall_duration = time.monotonic()-start, time.time()-wall
        combined = "\n".join((dest / name).read_text(errors="replace")
                             for name in ("stdout", "stderr", "debug") if (dest / name).exists())
        outcomes = re.findall(r"BLUEPRINT_STRESS_RESULT: .*?outcome=(\S+)", combined)
        expected = case.get("expected", "completed")
        errors = []
        if status != 0:
            errors.append(f"exit {status}")
        if not re.search(r"BLUEPRINT_STRESS: [1-9][0-9]* checks, [1-9][0-9]* terminal attempts passed\.", combined):
            errors.append("missing positive stress sentinel")
        if not outcomes or any(item != expected for item in outcomes):
            errors.append(f"outcomes {outcomes} != {expected}")
        if re.search(r"Bad ref|Freeing lost block|Unreferenced string", combined, re.I):
            errors.append("reference/GC diagnostic")
        if abs(duration-wall_duration) > 0.5:
            errors.append("wall/monotonic clock discontinuity")
        debug = (dest / "debug").read_text(errors="replace") if (dest / "debug").exists() else ""
        metrics, measurement_errors = validate_measurements(debug, case)
        if a.require_metrics:
            errors.extend(measurement_errors)
        result = {"case": case, "argv": command, "cwd": str(dest), "exit": status,
                  "timed_out": timed_out, "monotonic_seconds": duration,
                  "wall_seconds": wall_duration, "outcomes": outcomes, "metrics": metrics, "errors": errors,
                  "files": {str(path.relative_to(dest)): sha(path)
                            for path in sorted(dest.rglob("*")) if path.is_file()}}
        (dest / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        results.append({"path": dest.name, "result_sha256": sha(dest / "result.json"),
                        "errors": errors, "seconds": duration})
        print(json.dumps(results[-1]), flush=True)
    manifest = {"driver_sha256": sha(driver), "runner_sha256": sha(a.output / "runner.py"),
                "cases_sha256": sha(a.output / "cases.json"), "results": results,
                "measured": a.require_metrics}
    (a.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return int(any(r["errors"] for r in results))


if __name__ == "__main__":
    raise SystemExit(main())
