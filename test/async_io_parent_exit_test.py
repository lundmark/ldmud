#!/usr/bin/env python3
"""Check helper lifetime after abrupt driver exit; no persistence is asserted."""
import ctypes
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

PR_SET_CHILD_SUBREAPER = 36
PR_GET_CHILD_SUBREAPER = 37


def children(pid):
    return [int(child) for child in
            Path(f"/proc/{pid}/task/{pid}/children").read_text().split()]


def reap_group(group):
    """Reap only adopted children belonging to this test's private session."""
    deadline = time.monotonic() + 3
    while True:
        try:
            pid, _ = os.waitpid(-group, os.WNOHANG)
        except ChildProcessError:
            return
        if pid:
            continue
        if time.monotonic() >= deadline:
            raise AssertionError("test process group did not exit during cleanup")
        time.sleep(0.02)


def run(driver, helper):
    test = Path(__file__).resolve().parent
    options = subprocess.check_output([str(driver), "--options"], text=True, timeout=5)
    if "Async file I/O supported" not in options:
        print("SKIP abrupt driver exit: async file I/O disabled", flush=True)
        return

    # Adopt the helper when the driver dies so this test, rather than PID 1,
    # can collect its exit status and cannot leave an orphan zombie behind.
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl.argtypes = [ctypes.c_int] + [ctypes.c_ulong] * 4
    libc.prctl.restype = ctypes.c_int
    previous = ctypes.c_int()
    if libc.prctl(PR_GET_CHILD_SUBREAPER, ctypes.addressof(previous), 0, 0, 0) < 0:
        raise OSError(ctypes.get_errno(), "PR_GET_CHILD_SUBREAPER")
    if libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) < 0:
        raise OSError(ctypes.get_errno(), "PR_SET_CHILD_SUBREAPER")

    try:
        with tempfile.TemporaryDirectory(prefix="ldmud-async-parent-exit-") as directory:
            root = Path(directory)
            (root / "inc").symlink_to(test / "inc", target_is_directory=True)
            (root / "sys").symlink_to(test / "sys", target_is_directory=True)
            (root / "master.c").write_text('''#include "/inc/base.inc"
string *epilog(int flag)
{
    write_file("ready", "ready", 1);
    return 0;
}
''')
            defaults = "-u-1 -E 0 --no-compat -e -N --cleanup-time -1 --reset-time -1 --max-array 0 --max-callouts 0 --max-bytes 0 --max-file 0 -s-1 -sv-1 --hard-malloc-limit unlimited --min-malloc 0 -ru0 -rm0 -rs0 --no-strict-euids --no-wizlist-file --check-refcounts --check-state 2 --access-file none --access-log none".split()
            if "Python supported" in options:
                startup = root / "python-startup.py"
                startup.write_text("")
                defaults += ["--python-script", str(startup)]

            process = None
            try:
                with socket.socket() as listener:
                    listener.bind(("127.0.0.1", 0))
                    listener.listen()
                    command = [str(driver), *defaults, "--async-io-helper", str(helper),
                               "-m", str(root), "-Mmaster", "--debug-file", str(root / "debug.log"),
                               "--inherit", str(listener.fileno())]
                    with (root / "stdout").open("w") as stdout, (root / "stderr").open("w") as stderr:
                        process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                                   stdout=stdout, stderr=stderr,
                                                   pass_fds=(listener.fileno(),), start_new_session=True)
                        deadline = time.monotonic() + 8
                        while not (root / "ready").exists():
                            assert process.poll() is None, f"driver exited early: {process.returncode}"
                            if time.monotonic() >= deadline:
                                raise AssertionError("driver did not reach its idle state")
                            time.sleep(0.02)

                        peers = [pid for pid in children(process.pid)
                                 if Path(f"/proc/{pid}/exe").samefile(helper)]
                        assert len(peers) == 1, f"expected one real helper, found {peers}"
                        helper_pid = peers[0]
                        assert os.getpgid(helper_pid) == process.pid, "helper escaped its private process group"
                        started = time.monotonic()
                        process.kill()
                        assert process.wait(timeout=3) == -signal.SIGKILL, "driver did not die from SIGKILL"

                        deadline = time.monotonic() + 5
                        while True:
                            pid, result = os.waitpid(helper_pid, os.WNOHANG)
                            if pid:
                                assert os.WIFEXITED(result) and os.WEXITSTATUS(result) == 0, \
                                    f"helper did not exit normally on IPC EOF: wait status {result}"
                                break
                            if time.monotonic() >= deadline:
                                raise AssertionError("helper stayed alive after abrupt driver exit")
                            time.sleep(0.02)
                        assert not any(os.getpgid(pid) == process.pid for pid in children(os.getpid())), \
                            "an adopted test process remained unreaped"
                        print("PASS abrupt driver exit: idle helper exits on IPC EOF and is reaped "
                              f"({time.monotonic() - started:.2f}s)", flush=True)
            except BaseException:
                for file in ("stdout", "stderr", "debug.log"):
                    path = root / file
                    if path.exists():
                        print(f"abrupt driver exit: {file}\n{path.read_text(errors='replace')}", file=sys.stderr)
                raise
            finally:
                if process:
                    # The group belongs to our live driver or adopted child.
                    # Do not signal a group after all its children are reaped.
                    owned = process.poll() is None or any(
                        os.getpgid(pid) == process.pid for pid in children(os.getpid()))
                    if owned:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    process.wait(timeout=3)
                    reap_group(process.pid)
    finally:
        if libc.prctl(PR_SET_CHILD_SUBREAPER, previous.value, 0, 0, 0) < 0:
            raise OSError(ctypes.get_errno(), "restore PR_SET_CHILD_SUBREAPER")


if __name__ == "__main__":
    if sys.platform != "linux":
        print("SKIP abrupt driver exit: Linux subreaper and /proc required", flush=True)
    else:
        source = Path(__file__).resolve().parent.parent / "src"
        driver = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DRIVER", source / "ldmud")).resolve()
        helper = Path(os.environ.get("ASYNC_IO_HELPER", driver.with_name("ldmud-async-io"))).resolve()
        run(driver, helper)
