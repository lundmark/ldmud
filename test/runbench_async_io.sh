#!/bin/sh
# Private-mudlib comparisons; no installed helper or production paths needed.
set -eu
BENCH_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export BENCH_SCRIPT_DIR
exec /usr/bin/python3 - "$@" <<'PY'
"""Run matched save/write benchmarks and summarize validated observations."""
import collections
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time

if len(sys.argv) != 1:
    print("Usage: DRIVER=/absolute/path/to/ldmud test/runbench_async_io.sh\n"
          "BENCH_MODES=auto|baseline|all|comma-separated mode names\n"
          "Modes: string,file,write_file,async_write,async_save\n"
          "auto uses all modes when supported, otherwise string,file.\n"
          "ASYNC_IO_HELPER overrides the absolute sibling ldmud-async-io.\n"
          "BENCH_RUNS=3 BENCH_REPETITIONS=20 BENCH_WARMUP=2\n"
          "BENCH_SCALARS=6000 BENCH_STRING_BYTES=131072 BENCH_STRINGS=8\n"
          "BENCH_NESTED_WIDTH=128 BENCH_NESTED_DEPTH=4 BENCH_SHARED=512\n"
          "BENCH_LOOP_BATCH=4 BENCH_TIMEOUT=120 BENCH_KEEP=0\n"
          "BENCH_TMPDIR selects the filesystem; BENCH_REPORT saves JSON.\n"
          "BENCH_BUILD_DIR selects observed config.h/Makefile metadata;\n"
          "BENCH_SOURCE_DIR selects observed Git revision/dirty metadata.\n"
          "BENCH_QUEUE_REQUESTS/BYTES/REQUEST_BYTES override build limits;\n"
          "when config.h is absent the released defaults are assumed.\n"
          "Each run starts a fresh driver. Writes use OS caches, without fsync.")
    sys.exit(0 if sys.argv[1] in ("-h", "--help") else 2)


def integer(name, default, minimum=1, maximum=10000000):
    try:
        value = int(os.environ.get(name, default))
        if not minimum <= value <= maximum:
            raise ValueError()
        return value
    except ValueError:
        sys.exit(f"{name} must be an integer in [{minimum}, {maximum}]")


def executable(value, label):
    path = Path(value)
    if not path.is_absolute() or not path.is_file() or not os.access(path, os.X_OK):
        sys.exit(f"{label} must name an executable by absolute path")
    return path.resolve()


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            result.update(chunk)
    return result.hexdigest()


script_dir = Path(os.environ["BENCH_SCRIPT_DIR"])
driver = executable(os.environ.get("DRIVER", str(script_dir.parent / "src/ldmud")), "DRIVER")
try:
    driver_options = subprocess.check_output([str(driver), "--options"], text=True, timeout=10)
except (OSError, subprocess.SubprocessError) as error:
    sys.exit(f"Cannot inspect DRIVER: {error}")
has_async = "Async file I/O supported" in driver_options
all_modes = ["string", "file", "write_file", "async_write", "async_save"]
selection = os.environ.get("BENCH_MODES", "auto")
if selection == "auto":
    modes = all_modes if has_async else all_modes[:2]
elif selection == "baseline":
    modes = all_modes[:2]
elif selection == "all":
    modes = all_modes
else:
    modes = selection.split(",")
if not modes or len(set(modes)) != len(modes) or any(mode not in all_modes for mode in modes):
    sys.exit("BENCH_MODES has unknown or duplicate mode names; see --help")
if any(mode.startswith("async_") for mode in modes) and not has_async:
    sys.exit("Selected asynchronous modes require an async-enabled driver")
helper = executable(os.environ.get("ASYNC_IO_HELPER", str(driver.with_name("ldmud-async-io"))),
                    "ASYNC_IO_HELPER") if has_async else None
build_dir = Path(os.environ.get("BENCH_BUILD_DIR", str(driver.parent))).resolve()
config_path = build_dir / "config.h"
config_text = config_path.read_text() if config_path.is_file() else ""
queue_defaults = {"REQUESTS": 1024, "BYTES": 67108864, "REQUEST_BYTES": 16777216}
queue_limits, queue_sources = {}, {}
for name, default in queue_defaults.items():
    match = re.search(r"^#define ASYNC_IO_MAX_" + name + r"\s+(\d+)\s*$", config_text, re.M)
    value = int(match[1]) if match else default
    key = "BENCH_QUEUE_" + name
    queue_limits[key] = integer(key, value, maximum=2147483647)
    queue_sources[key] = "environment" if key in os.environ else "config.h" if match else "released default (assumed)"
if queue_limits["BENCH_QUEUE_REQUEST_BYTES"] > queue_limits["BENCH_QUEUE_BYTES"]:
    sys.exit("Per-request queue bytes must not exceed total queue bytes")
runs = integer("BENCH_RUNS", 3, maximum=1000)
timeout = integer("BENCH_TIMEOUT", 120, maximum=1800)
config = {
    "BENCH_REPETITIONS": integer("BENCH_REPETITIONS", 20),
    "BENCH_WARMUP": integer("BENCH_WARMUP", 2, minimum=0),
    "BENCH_SCALARS": integer("BENCH_SCALARS", 6000),
    "BENCH_STRING_BYTES": integer("BENCH_STRING_BYTES", 131072),
    "BENCH_STRINGS": integer("BENCH_STRINGS", 8),
    "BENCH_NESTED_WIDTH": integer("BENCH_NESTED_WIDTH", 128),
    "BENCH_NESTED_DEPTH": integer("BENCH_NESTED_DEPTH", 4, maximum=20),
    "BENCH_SHARED": integer("BENCH_SHARED", 512),
    "BENCH_LOOP_BATCH": integer("BENCH_LOOP_BATCH", 4, maximum=1024),
    "BENCH_HAS_ASYNC": int(has_async),
    **queue_limits,
}
root = Path(tempfile.mkdtemp(prefix="ldmud-bench-async-io-", dir=os.environ.get("BENCH_TMPDIR")))
keep = os.environ.get("BENCH_KEEP", "0") == "1"
samples, burst_samples, loops, fixtures, resources = [], [], [], [], []
completed = False
fixture_names = ("scalars", "strings", "nested", "shared_cycles")
driver_hash = digest(driver)
helper_hash = digest(helper) if helper else None


def distribution(values):
    ordered = sorted(values)
    return {"n": len(ordered), "median": statistics.median(ordered),
            "p95": ordered[math.ceil(len(ordered) * .95) - 1],
            "p99": ordered[math.ceil(len(ordered) * .99) - 1], "max": ordered[-1]}


def source_metadata():
    source_dir = Path(os.environ.get("BENCH_SOURCE_DIR", str(script_dir.parent))).resolve()
    result = {"path": str(source_dir), "provenance": "observed checkout, not embedded binary provenance"}
    def git(*args):
        return subprocess.check_output(["git", "-C", str(source_dir), *args], stderr=subprocess.DEVNULL)
    try:
        result["head"] = git("rev-parse", "HEAD").decode().strip()
        diff = git("diff", "--binary", "HEAD", "--", "src")
        result["tracked_src_diff_sha256"] = hashlib.sha256(diff).hexdigest()
        names = git("ls-files", "--others", "--exclude-standard", "-z", "--", "src").decode().split("\0")
        result["untracked_src_sha256"] = {name: digest(source_dir / name) for name in names if name}
        result["src_dirty"] = bool(diff or any(names))
    except (OSError, subprocess.SubprocessError):
        result["head"] = None
    return result


def build_metadata():
    result = {"path": str(build_dir), "provenance": "observed build files, not proof of binary correspondence"}
    for name in ("config.h", "Makefile"):
        path = build_dir / name
        if path.is_file():
            result[name + "_sha256"] = digest(path)
    result["config_defines"] = re.findall(r"^#(?:define|undef) (?:DEBUG|USE_[A-Z_]+|ASYNC_IO_[A-Z_]+).*$", config_text, re.M)
    makefile = build_dir / "Makefile"
    if makefile.is_file():
        result["make_settings"] = re.findall(
            r"^(?:CC|CFLAGS|LDFLAGS|LIBS|OPTIMIZE|MED_OPTIMIZE|DEBUG|WARN|MPATH|PROFIL)\s*=.*$",
            makefile.read_text(), re.M)
    return result


def process_sample(pid, expected_executable):
    """Linux samples exclude a child's pre-exec inherited memory image."""
    proc = Path("/proc") / str(pid)
    try:
        if (proc / "exe").resolve() != expected_executable:
            return None
        fields = (proc / "stat").read_text().rpartition(")")[2].split()
        status = (proc / "status").read_text()
        sizes = dict(re.findall(r"^(VmRSS|VmHWM):\s+(\d+) kB$", status, re.M))
        ticks = os.sysconf("SC_CLK_TCK")
        return {"pid": pid, "user_seconds": int(fields[11]) / ticks,
                "system_seconds": int(fields[12]) / ticks,
                "rss_bytes": int(sizes.get("VmRSS", 0)) * 1024,
                "hwm_bytes": int(sizes.get("VmHWM", 0)) * 1024}
    except (OSError, ValueError, IndexError):
        return None


def collect_run(run):
    mudlib = root / str(run)
    mudlib.mkdir()
    (mudlib / "inc").symlink_to(script_dir / "inc", target_is_directory=True)
    (mudlib / "sys").symlink_to(script_dir.parent / "mudlib/sys", target_is_directory=True)
    shutil.copyfile(script_dir / "bench_async_io.c", mudlib / "bench_async_io.c")
    header = "".join(f"#define {key} {value}\n" for key, value in dict(config, BENCH_RUN_NUMBER=run).items())
    header += "#define BENCH_MODES ({ " + ", ".join(json.dumps(mode) for mode in modes) + " })\n"
    (mudlib / "bench_config.h").write_text(header)
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    command = [str(driver), "-u-1", "-E", "0", "--no-compat", "-N",
               "--cleanup-time", "-1", "--reset-time", "-1",
               "--max-array", "0", "--max-mapping", "0", "--max-mapping-keys", "0",
               "--max-bytes", "0", "--max-file", "0", "-s-1", "-sv-1",
               "--hard-malloc-limit", "unlimited", "--no-strict-euids",
               "--no-wizlist-file", "--access-file", "none", "--access-log", "none",
               "--alarm-time", "1", "--heart-interval", "1", "-e",
               "-Mbench_async_io", "-m" + str(mudlib),
               "--debug-file", str(mudlib / "driver.log"),
               "--inherit", str(listener.fileno())]
    if helper:
        command += ["--async-io-helper", str(helper)]
    (mudlib / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    started = time.monotonic()
    observed = {}
    combined_rss = 0
    poll_count = 0
    with (mudlib / "stdout.log").open("w") as out, (mudlib / "stderr.log").open("w") as err:
        process = subprocess.Popen(command, cwd=mudlib, stdout=out, stderr=err,
                                   start_new_session=True, pass_fds=(listener.fileno(),))
        listener.close()
        while True:
            candidates = [(process.pid, "driver", driver)]
            if helper:
                try:
                    children = (Path("/proc") / str(process.pid) / "task" / str(process.pid) / "children").read_text()
                    candidates += [(int(pid), "helper", helper) for pid in children.split()]
                except OSError:
                    pass
            current_rss = 0
            for pid, role, executable_path in candidates:
                sample = process_sample(pid, executable_path)
                if sample:
                    sample["role"] = role
                    current_rss += sample["rss_bytes"]
                    previous = observed.get(pid, {})
                    sample["max_sampled_hwm_bytes"] = max(previous.get("max_sampled_hwm_bytes", 0), sample["hwm_bytes"])
                    observed[pid] = sample
            combined_rss = max(combined_rss, current_rss)
            poll_count += 1
            pid, status, usage = os.wait4(process.pid, os.WNOHANG)
            if pid:
                process.returncode = os.waitstatus_to_exitcode(status)
                break
            if time.monotonic() - started > timeout:
                os.killpg(process.pid, signal.SIGKILL)
                _, status, usage = os.wait4(process.pid, 0)
                process.returncode = os.waitstatus_to_exitcode(status)
                raise RuntimeError(f"Run {run} exceeded BENCH_TIMEOUT={timeout}")
            time.sleep(.005)
    run_resource = {"run": run, "command": command, "wall_seconds": time.monotonic() - started,
                    "wait4_user_seconds": usage.ru_utime, "wait4_system_seconds": usage.ru_stime,
                    "wait4_maxrss_bytes": usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024),
                    "linux_process_samples": list(observed.values()), "resource_poll_count": poll_count,
                    "max_simultaneous_sampled_rss_bytes": combined_rss}
    resources.append(run_resource)
    output = (mudlib / "stdout.log").read_text()
    errors = (mudlib / "stderr.log").read_text()
    if process.returncode or "BENCH_DONE" not in output or "BENCH_ERROR" in output or "BENCH_WARNING" in output:
        raise RuntimeError(f"Run {run} failed ({process.returncode})\n{output}\n{errors}")
    seen, burst_seen, loop_modes, fixture_seen = set(), set(), set(), set()
    for line in output.splitlines():
        fields = line.split()
        if fields[:1] == ["SAMPLE"]:
            _, kind, mode, *raw = fields
            rep, wall, completion, size, user, system, before, after = map(int, raw)
            key = (kind, mode, rep)
            if key in seen or wall < 0 or size <= 0 or (mode.startswith("async_") and completion < wall):
                raise RuntimeError(f"Invalid/duplicate sample: {line}")
            seen.add(key)
            samples.append(dict(run=run, fixture=kind, mode=mode, repetition=rep, wall_us=wall,
                                completion_us=completion, output_bytes=size, user_ms=user,
                                system_ms=system, memory_before_bytes=before, memory_after_bytes=after))
        elif fields[:1] == ["BURST_SAMPLE"]:
            _, kind, mode, index, wall, completion, size = fields
            key = (mode, int(index))
            if key in burst_seen or int(wall) < 0 or int(size) <= 0:
                raise RuntimeError(f"Invalid/duplicate burst sample: {line}")
            if mode.startswith("async_") and int(completion) < int(wall):
                raise RuntimeError(f"Completion preceded submission: {line}")
            burst_seen.add(key)
            burst_samples.append(dict(run=run, fixture=kind, mode=mode, index=int(index),
                                      wall_us=int(wall), completion_us=int(completion), output_bytes=int(size)))
        elif fields[:1] == ["LOOP"]:
            _, kind, mode, batch, blocking, delay, completion, at_probe, memory = fields
            if mode in loop_modes or not 0 <= int(at_probe) <= int(batch) or int(delay) < int(blocking):
                raise RuntimeError(f"Invalid/duplicate loop probe: {line}")
            loop_modes.add(mode)
            loops.append(dict(run=run, fixture=kind, mode=mode, batch=int(batch), blocking_us=int(blocking),
                              callout_delay_us=int(delay), completion_us=int(completion),
                              completed_at_probe=int(at_probe), allocator_after_submit_bytes=int(memory)))
        elif fields[:1] == ["FIXTURE"]:
            _, kind, shared, total = fields
            if kind in fixture_seen:
                raise RuntimeError(f"Duplicate fixture: {line}")
            fixture_seen.add(kind)
            fixtures.append(dict(run=run, fixture=kind, shared_data_bytes=int(shared), total_data_bytes=int(total)))
        elif fields[:1] == ["PROCESS_CPU"]:
            run_resource["driver_user_ms"], run_resource["driver_system_ms"] = map(int, fields[1:])
    expected = {(kind, mode, rep) for kind in fixture_names for mode in modes
                for rep in range(config["BENCH_REPETITIONS"])}
    if seen != expected or loop_modes != set(modes) or fixture_seen != set(fixture_names) or "driver_user_ms" not in run_resource:
        raise RuntimeError(f"Run {run} has missing/unexpected measurements")
    expected_bursts = {(item["mode"], index) for item in loops if item["run"] == run for index in range(item["batch"])}
    if burst_seen != expected_bursts:
        raise RuntimeError(f"Run {run} has missing/unexpected burst measurements")
    for kind in fixture_names:
        sizes = {s["output_bytes"] for s in samples if s["run"] == run and s["fixture"] == kind}
        if len(sizes) != 1:
            raise RuntimeError(f"Output size changed between matched {kind} operations")
    if digest(driver) != driver_hash or (helper and digest(helper) != helper_hash):
        raise RuntimeError("A measured executable changed during the run; preserve copies before benchmarking")


def summarize(observations, label):
    print(f"\n{label} (microseconds; nearest-rank p95/p99):")
    print(f"{'fixture':15} {'mode':12} {'interval':10} {'n':>6} {'median':>10} {'p95':>10} {'p99':>10} {'max':>10}")
    groups = collections.defaultdict(list)
    for item in observations:
        groups[(item["fixture"], item["mode"])].append(item)
    summaries = []
    for (kind, mode), group in sorted(groups.items()):
        summary = dict(fixture=kind, mode=mode, output_bytes=group[0]["output_bytes"])
        for metric in ("wall_us", "completion_us"):
            values = [item[metric] for item in group if item[metric] >= 0]
            if not values:
                continue
            stats = distribution(values)
            summary[metric] = stats
            interval = "completion" if metric == "completion_us" else "submission" if mode.startswith("async_") else "call"
            print(f"{kind:15} {mode:12} {interval:10} {stats['n']:6} {stats['median']:10.1f} "
                  f"{stats['p95']:10} {stats['p99']:10} {stats['max']:10}")
        if "user_ms" in group[0]:
            summary.update(measured_user_ms=sum(s["user_ms"] for s in group),
                           measured_system_ms=sum(s["system_ms"] for s in group),
                           max_observed_allocator_bytes=max(s["memory_after_bytes"] for s in group))
        summaries.append(summary)
    return summaries


try:
    metadata = {"schema_version": 2, "driver": str(driver), "driver_sha256": driver_hash,
                "driver_options": driver_options, "helper": str(helper) if helper else None,
                "helper_sha256": helper_hash, "source": source_metadata(), "build": build_metadata(),
                "platform": platform.platform(), "machine": platform.machine(), "logical_cpus": os.cpu_count(),
                "temporary_filesystem_path": str(root), "runs": runs, "modes": modes,
                "config": config, "queue_limit_sources": queue_sources,
                "resource_sampling_seconds": .005, "post_measurement_settle_seconds": 1}
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        metadata["cpu_model"] = next((line.split(":", 1)[1].strip() for line in
                                      cpuinfo.read_text().splitlines() if line.startswith("model name")), "unknown")
    for run in range(runs):
        print(f"Run {run + 1}/{runs}: matched modes {','.join(modes)}", flush=True)
        collect_run(run)
    print(f"Driver: {driver}\nSHA256: {driver_hash}\nHelper: {helper}\nHelper SHA256: {helper_hash}")
    print(f"Observed source HEAD: {metadata['source']['head']}; dirty src: {metadata['source'].get('src_dirty', 'unknown')}")
    print(f"Host: {metadata.get('cpu_model', platform.machine())}; {platform.platform()}")
    print(f"Temporary filesystem path: {root}\nSettings: {json.dumps(config, sort_keys=True)}")
    print(f"Queue limit provenance: {json.dumps(queue_sources, sort_keys=True)}")
    summaries = summarize(samples, "Individual operations")
    burst_summaries = summarize(burst_samples, "Bounded string-fixture bursts")
    print("\nMeasured calls: output bytes, summed driver CPU milliseconds, maximum sampled allocator bytes")
    for item in summaries:
        print(f"{item['fixture']:15} {item['mode']:12} bytes={item['output_bytes']} user={item['measured_user_ms']} "
              f"system={item['measured_system_ms']} allocator={item['max_observed_allocator_bytes']}")
    print("\nObject data bytes before prepared text (sharing-adjusted / unadjusted, first run):")
    for item in fixtures:
        if item["run"] == 0:
            print(f"{item['fixture']:15} {item['shared_data_bytes']} / {item['total_data_bytes']}")
    print("\nBurst blocking / zero-delay callout / batch completion (microseconds):")
    for mode in modes:
        group = [item for item in loops if item["mode"] == mode]
        for metric in ("blocking_us", "callout_delay_us", "completion_us"):
            print(f"{mode:12} {metric:18} {json.dumps(distribution([item[metric] for item in group]), sort_keys=True)}")
        print(f"  batches={[item['batch'] for item in group]}; completed at callout={[item['completed_at_probe'] for item in group]}")
    print("\nWhole runs, including startup, fixtures, validation, and probes:")
    print(f"Driver rusage before settling: user={sum(r['driver_user_ms'] for r in resources)}ms "
          f"system={sum(r['driver_system_ms'] for r in resources)}ms")
    print(f"wait4 accounting (can include reaped helper usage): user={sum(r['wait4_user_seconds'] for r in resources):.6f}s "
          f"system={sum(r['wait4_system_seconds'] for r in resources):.6f}s")
    for role in ("driver", "helper"):
        observed = [item for run in resources for item in run["linux_process_samples"] if item["role"] == role]
        if observed:
            print(f"Linux sampled {role}: user={sum(item['user_seconds'] for item in observed):.3f}s "
                  f"system={sum(item['system_seconds'] for item in observed):.3f}s "
                  f"max observed RSS high-water={max(item['max_sampled_hwm_bytes'] for item in observed)} bytes")
    print(f"Maximum simultaneously sampled driver+helper RSS: {max(r['max_simultaneous_sampled_rss_bytes'] for r in resources)} bytes")
    print("\nCompletion intervals start before submission and end at callback entry.\n"
          "Prepared-text construction and validation are outside measured intervals.\n"
          "Individual async requests run serially; the next can be submitted from the preceding callback.\n"
          "Burst callbacks only record timestamps/status until all requests complete.\n"
          "Callout delays include configured 1-second scheduling granularity; they are not disk-wait measurements.\n"
          "CPU has millisecond/tick resolution. Linux helper CPU is the last sampled value, so can undercount.\n"
          "Allocator samples are not transient peaks; async samples can include a finishing request.\n"
          "Prepared text stays allocated; synchronous string burst results are retained for validation.\n"
          "File operations use cached writes/close (and rename for saves), without fsync.\n"
          "Source/build metadata describe observed files; executable hashes identify the measured binaries.\n"
          "These results do not establish lower total CPU or off-thread serialization.")
    report = dict(metadata=metadata, summaries=summaries, burst_summaries=burst_summaries,
                  samples=samples, burst_samples=burst_samples, loops=loops, fixtures=fixtures, resources=resources)
    report_path = os.environ.get("BENCH_REPORT")
    if report_path:
        Path(report_path).write_text(json.dumps(report, indent=2) + "\n")
        print(f"JSON report: {Path(report_path).resolve()}")
    completed = True
except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
    print(str(error), file=sys.stderr)
    sys.exit(1)
finally:
    if keep or not completed:
        print(f"Benchmark artifacts retained: {root}", file=sys.stderr)
    else:
        shutil.rmtree(root)
PY
