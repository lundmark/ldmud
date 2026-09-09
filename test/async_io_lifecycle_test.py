#!/usr/bin/env python3
"""Private-mudlib process tests for the optional asynchronous file writer."""
import os
import fcntl
from pathlib import Path
import shutil
import resource
import signal
import socket
import subprocess
import sys
import tempfile
import time

TEST = Path(__file__).resolve().parent
DRIVER = Path(sys.argv[1]).resolve()
PEER = Path(sys.argv[2]).resolve()
HELPER = Path(os.environ.get("ASYNC_IO_HELPER", DRIVER.with_name("ldmud-async-io"))).resolve()
STARTUP_ONLY = "--startup" in sys.argv[3:]
OPTIONS = subprocess.check_output([str(DRIVER), "--options"], text=True)
if "Async file I/O supported" not in OPTIONS:
    print("SKIP async lifecycle: feature disabled")
    sys.exit(0)

DEFAULTS = "-u-1 -E 0 --no-compat -e -N --cleanup-time -1 --reset-time -1 --max-array 0 --max-callouts 0 --max-bytes 0 --max-file 0 -s-1 -sv-1 --hard-malloc-limit unlimited --min-malloc 0 -ru0 -rm0 -rs0 --no-strict-euids --no-wizlist-file --check-refcounts --check-state 2 --access-file none --access-log none".split()


def fixture(root, source):
    for file in (TEST / source).glob("*.c"):
        shutil.copyfile(file, root / file.name)
    (root / "inc").symlink_to(TEST / "inc", target_is_directory=True)
    (root / "sys").symlink_to(TEST / "sys", target_is_directory=True)


def wait_for(path, process, timeout=8):
    deadline = time.monotonic() + timeout
    while not path.exists():
        if process.poll() is not None:
            raise AssertionError(f"driver exited {process.returncode} before {path.name}")
        if time.monotonic() >= deadline:
            raise AssertionError(f"timed out waiting for {path.name}")
        time.sleep(0.02)


def run(name, source="t-async-io-lifecycle", case=None, scenario=None,
        defines=(), expected=0, release=False, responsive=False,
        startup_failure=False, helper=None, deadline=12, verify=None,
        broken_master=False, broken_simul=False, lower_fd_limit=False, kill_peer=False):
    if os.environ.get("ASYNC_TEST_FILTER", "") not in name:
        return
    with tempfile.TemporaryDirectory(prefix="ldmud-async-case-") as directory:
        root = Path(directory)
        fixture(root, source)
        if broken_master:
            (root / "master.c").write_text("int invalid = ;\n")
        if broken_simul:
            (root / "master.c").write_text('''#include "/inc/base.inc"
void done(int result) { write_file("unexpected", "callback"); }
void flag(string arg) { async_write("discarded", "x", 1, #'done); }
string get_simul_efun() { return "missing_simul"; }
''')
        os.mkfifo(root / "control", 0o600)
        gate = os.open(root / "control", os.O_RDWR | os.O_NONBLOCK)
        inherited = os.open(os.devnull, os.O_RDONLY)
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        if lower_fd_limit:
            high_fd = fcntl.fcntl(inherited, fcntl.F_DUPFD, 200)
            os.close(inherited)
            inherited = high_fd
        environment = os.environ.copy()
        environment["ASYNC_TEST_SCENARIO"] = scenario or "success"
        environment["ASYNC_TEST_INHERITED_FD"] = str(inherited)
        command = [str(DRIVER), *DEFAULTS, "--async-io-helper",
                   str(helper or (PEER if scenario else HELPER)),
                   "-m", str(root), "-Mmaster", "--debug-file", str(root / "debug.log"),
                   "-f", "test", "-DASYNC_TEST_HARNESS",
                   "--inherit", str(listener.fileno())]
        if "Python supported" in OPTIONS:
            startup = root / "python-startup.py"
            startup.write_text("")
            command += ["--python-script", str(startup)]
        if case is not None:
            command.append(f"-DASYNC_CASE={case}")
        command.extend(f"-D{define}" for define in defines)
        started = time.monotonic()
        process = None
        try:
            with (root / "stdout").open("w") as stdout, (root / "stderr").open("w") as stderr:
                process = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                           env=environment, pass_fds=(inherited, listener.fileno()),
                                           start_new_session=True,
                                           preexec_fn=(lambda: resource.setrlimit(resource.RLIMIT_NOFILE,
                                                       (64, resource.getrlimit(resource.RLIMIT_NOFILE)[1])))
                                                       if lower_fd_limit else None)
                if release:
                    wait_for(root / "peer-ready", process)
                    if responsive:
                        wait_for(root / "tick", process)
                        assert not (root / "completed").exists(), "callback preceded helper result"
                    if kill_peer:
                        os.kill(int((root / "peer-pid").read_text()), signal.SIGTERM)
                    else:
                        os.write(gate, b"x")
                process.wait(timeout=deadline)
            elapsed = time.monotonic() - started
            if (root / "peer-pid").exists():
                peer_pid = int((root / "peer-pid").read_text())
                try:
                    os.kill(peer_pid, 0)
                except ProcessLookupError:
                    pass
                else:
                    raise AssertionError("helper remained alive or unreaped after driver exit")
            logs = "\n".join(path.read_text(errors="replace") for path in
                             (root / "stdout", root / "stderr", root / "debug.log") if path.exists())
            if startup_failure:
                # Upstream aborts deliberately when the master cannot load.
                # Other signal exits must not masquerade as expected failures.
                master_abort = (broken_master and process.returncode == -signal.SIGABRT
                                and "Failed to load master object" in logs)
                assert process.returncode > 0 or master_abort, "unexpected startup exit or signal"
                assert not (root / "completed").exists() and not (root / "unexpected").exists(), "callback during failed initialization"
            else:
                assert process.returncode == expected, f"exit {process.returncode}, expected {expected}"
                assert (root / "completed").read_text() == "PASS", "callbacks not completed successfully"
                if source == "t-async-shutdown":
                    assert (root / "shutdown.txt").read_bytes() == b"before\nnotify\n", "shutdown log not persisted after exit"
                    snapshot = (root / "snapshot.o").read_bytes()
                    assert snapshot.startswith(b"#3:") and b"\nsaved_value 42\n" in snapshot, "shutdown snapshot not persisted after exit"
                    assert not (root / "recursive").exists() and not (root / "unexpected").exists(), "draining callback admitted another request"
            assert "ref count" not in logs.lower() and "lost block" not in logs.lower(), "reference/GC diagnostic"
            if verify:
                verify(root, elapsed, logs)
            print(f"PASS {name} ({elapsed:.2f}s)", flush=True)
        except BaseException:
            if process:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            for file in ("stdout", "stderr", "debug.log"):
                path = root / file
                if path.exists():
                    print(f"{name}: {file}\n{path.read_text(errors='replace')[-18000:]}", file=sys.stderr)
            raise
        finally:
            os.close(gate)
            os.close(inherited)
            listener.close()


def no_temporary(root, elapsed, logs):
    assert not list(root.glob(".ldmud-async-*")), "acknowledged temporary was not cleaned"


def committed(root, elapsed, logs):
    assert (root / "uncertain.o").read_text() == "committed", "uncertain mutation was replayed or rolled back"
    assert "uncertain" in logs, "lost acknowledgement diagnostic missing"


run("missing helper", startup_failure=True, helper=PEER.with_name("missing"))
run("malformed startup reply", scenario="badhello", startup_failure=True)
run("startup handshake deadline", scenario="nohello", startup_failure=True, deadline=8)
run("master initialization failure cleans helper", scenario="success", startup_failure=True, broken_master=True)
run("simul initialization failure discards callbacks", scenario="success", startup_failure=True, broken_simul=True)
for define in ("START_IN_FLAG", "START_IN_SIMUL"):
    run(f"orderly startup shutdown {define}", "t-async-shutdown",
        defines=(define, "EXPECT_EXIT=7"), expected=7)
run("ordinary shutdown drains notify writes", "t-async-shutdown")
run("startup drain survives callback exception", "t-async-shutdown",
    defines=("START_IN_FLAG", "THROW_FIRST"))
if not STARTUP_ONLY:
    run("fragmented result", case=1, scenario="fragment")
    run("inherited descriptor above lowered limit", case=1, scenario="success", lower_fd_limit=True)
    run("backpressure permits callouts and GC", case=1, scenario="stall", release=True, responsive=True)
    run("request-count rejection and recovery", case=3, scenario="stall", release=True, responsive=True)
    run("payload-capacity rejection and recovery", case=4, scenario="stall", release=True, responsive=True, deadline=20)
    for scenario in ("exit-ready", "header-eof", "lost", "wrongid", "badlength", "midreply"):
        run(f"helper loss {scenario}", case=2, scenario=scenario, release=scenario == "exit-ready")
    run("lost helper drains multiple callback batches", case=5, scenario="lost", expected=1)
    run("known temporary cleanup", case=6, scenario="temp-loss", release=True, verify=no_temporary)
    run("lost commit acknowledgement", case=6, scenario="committed-loss", verify=committed)
    run("save success requires temporary progress", case=6, scenario="success")
    run("idle helper death", case=7, scenario="exit-ready", release=True, responsive=True)
    run("helper SIGTERM", case=2, scenario="stall", release=True, responsive=True, kill_peer=True)
    if "ASYNC_TEST_SHUTDOWN_TIMEOUT" in os.environ:
        drain_timeout = float(os.environ["ASYNC_TEST_SHUTDOWN_TIMEOUT"])
        def timed_out(root, elapsed, logs):
            assert drain_timeout <= elapsed < drain_timeout + 6, "unbounded shutdown wait"
            assert "shutdown deadline" in logs, "deadline diagnostic missing"
        run("configured shutdown deadline", case=5, scenario="hang", expected=1,
            deadline=drain_timeout + 7, verify=timed_out)
        def timed_out_temporary(root, elapsed, logs):
            timed_out(root, elapsed, logs)
            no_temporary(root, elapsed, logs)
        run("configured deadline cleans known temporary", case=8, scenario="temp-hang", expected=1,
            deadline=drain_timeout + 7, verify=timed_out_temporary)
print("All async lifecycle scenarios passed.", flush=True)
