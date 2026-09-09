"""Run isolated blueprint failure/swap fixtures with bounded child lifetimes.

Use the existing DRIVER/DRIVER_DEFAULTS protocol (including Valgrind). Bound
loopback sockets are inherited by each child so concurrent variants do not
race on guessed ports. Raw logs, commands, exits and the point union survive;
temporary mudlibs and all child processes are always cleaned up.
"""

import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import time


ROOT = Path.cwd()
KIND = sys.argv[1]
OUTPUT = Path(os.environ["TEST_LOGFILE"] + ".d").resolve()
OUTPUT.mkdir(parents=True, exist_ok=True)
DRIVER = shlex.split(os.environ["DRIVER"])
OPTIONS = shlex.split(os.environ["DRIVER_DEFAULTS"])
CHILDREN = set()
CANCELED = False


def interrupted(number, frame):
    """Suite cancellation also terminates every still-owned driver."""
    global CANCELED
    CANCELED = True
    for child in tuple(CHILDREN):
        if child.poll() is None:
            child.kill()
    raise SystemExit(128 + number)


signal.signal(signal.SIGINT, interrupted)
signal.signal(signal.SIGTERM, interrupted)


def report(message):
    print(message, flush=True)
    with Path(os.environ["TEST_LOGFILE"]).open("a") as output:
        output.write(message + "\n")


Path(os.environ["TEST_LOGFILE"]).write_text("")
if KIND == "swap":
    # The automatic DEBUG checker unswaps variables/programs at every backend
    # loop (interpret.c:check_a_lot_ref_counts). The native fixture invokes
    # that checker at its explicit, scratch-free preparation checkpoint.
    OPTIONS = [option for option in OPTIONS if option != "--check-refcounts"]


def run(name, fixture, defines=(), timeout=840):
    """Run exactly one driver, preserving evidence on success and failure."""
    with tempfile.TemporaryDirectory(prefix=name + "-", dir=OUTPUT) as temporary:
        mudlib = Path(temporary)
        source = ROOT / "t-blueprint-migration" / fixture
        (mudlib / "master.c").write_bytes(source.read_bytes())
        (mudlib / "inc").symlink_to(ROOT / "inc", target_is_directory=True)
        (mudlib / "sys").symlink_to(ROOT.parent / "mudlib/sys", target_is_directory=True)
        # A Python-enabled driver may request its standard optional startup.
        (mudlib / "startup.py").write_text("")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            command = DRIVER + OPTIONS + list(defines) + [
                "-Mmaster", "-m" + str(mudlib), "--inherit", str(listener.fileno()),
                "--debug-file", str(OUTPUT / (name + ".debug")),
            ]
            evidence = {"command": command, "fixture_sha256": hashlib.sha256(source.read_bytes()).hexdigest()}
            start = time.monotonic()
            with (OUTPUT / (name + ".out")).open("w") as stdout, (OUTPUT / (name + ".err")).open("w") as stderr:
                child = None
                try:
                    child = subprocess.Popen(command, stdout=stdout, stderr=stderr, pass_fds=(listener.fileno(),))
                    CHILDREN.add(child)
                    if CANCELED:
                        child.kill()
                    evidence["exit"] = child.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    evidence["timeout"] = timeout
                    child.kill()
                    evidence["exit"] = child.wait()
                finally:
                    if child is not None and child.poll() is None:
                        child.kill()
                        child.wait()
                    CHILDREN.discard(child)
                    evidence["seconds"] = time.monotonic() - start
                    (OUTPUT / (name + ".json")).write_text(json.dumps(evidence, indent=2) + "\n")
                    for artifact in mudlib.glob("*-evidence.log"):
                        (OUTPUT / (name + "-" + artifact.name)).write_bytes(artifact.read_bytes())
        if evidence["exit"]:
            raise RuntimeError(f"{name} failed: {evidence}; see {OUTPUT}")
        log = (OUTPUT / (name + ".debug")).read_text()
        # DEBUG refcount checking and GC report failures without changing the
        # exit status. GC normally writes to stderr, not the debug log. The
        # documented "high ref count" warning can merely mean an inheriting
        # program is swapped; ordinary injected OOMs are expected.
        diagnostics = log + (OUTPUT / (name + ".out")).read_text() + (OUTPUT / (name + ".err")).read_text()
        failures = [line for line in diagnostics.splitlines() if re.search(
            r"Bad ref count|Out of memory while checking all refcounts"
            r"|check-refcounts: .*can't be swapped in"
            r"|Found destructed object .*where it shouldn't be"
            r"|freeing.*block|tabled string.*was left unreferenced", line)]
        if failures:
            raise RuntimeError(f"{name} reference check failed: {failures}; see {OUTPUT}")
        return log


def main():
    if KIND == "swap":
        log = run("swap", "swapping.c", timeout=240)
        report("\n".join(line for line in log.splitlines() if "BLUEPRINT_SWAP" in line))
        return
    detection = run("detect", "pipeline.c", ["-DPIPELINE_DETECT_ONLY"], timeout=30)
    match = re.search(r"BLUEPRINT_PIPELINE_CONFIG: (testing|ordinary|disabled)", detection)
    if not match:
        raise RuntimeError("configuration probe did not report the fixture mode")
    mode = match.group(1)
    if mode == "disabled":
        report("BLUEPRINT_PIPELINE: feature disabled.")
        return
    shards = 8 if mode == "testing" else 1
    # Every child has an 840-second bound; all start together. The collector
    # also has an absolute deadline below the parent suite's 900-second gate.
    deadline = time.monotonic() + 870
    with concurrent.futures.ThreadPoolExecutor(max_workers=shards) as pool:
        futures = [pool.submit(run, f"shard-{index}", "pipeline.c", [
            f"-DPIPELINE_FIRST={index}", f"-DPIPELINE_STRIDE={shards}"])
            for index in range(shards)]
        logs = [future.result(timeout=max(0, deadline - time.monotonic())) for future in futures]
    records = [tuple(match) for log in logs for match in re.findall(
        r"BLUEPRINT_PIPELINE_POINT: (\d+) (\d+) (admission|execution|completed)", log)]
    summary = {}
    for phase in range(3):
        rows = [(int(point), outcome) for row_phase, point, outcome in records if int(row_phase) == phase]
        points = [point for point, _ in rows]
        if len(points) != len(set(points)):
            raise RuntimeError(f"mode{phase} repeated a countdown point")
        successes = sorted(point for point, outcome in rows if outcome == "completed")
        if len(successes) != shards:
            raise RuntimeError(f"mode{phase} has no real successful recovery in every shard")
        first = successes[0]
        failures = sorted(point for point, outcome in rows if outcome != "completed")
        if failures != list(range(first)) or successes != list(range(first, first + shards)):
            raise RuntimeError(f"mode{phase} has a gap or inconsistent first-success boundary: {rows}")
        summary[phase] = {"first_success": first, "failures": failures, "successes": successes}
        if mode == "testing":
            report(f"BLUEPRINT_PIPELINE: mode {phase}, every point 0..{first - 1} failed; successful recovery at {first}.")
        else:
            report(f"BLUEPRINT_PIPELINE: mode {phase}, ordinary migration completed; allocation controls unavailable.")
    (OUTPUT / "coverage.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
