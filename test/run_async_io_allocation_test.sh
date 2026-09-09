#!/bin/sh
# Requires a configured, built driver with --enable-use-async-io and
# --enable-malloc-trace. SRC_DIR selects a separate configured source tree.
# VALGRIND optionally names a Memcheck executable; VALGRIND_LIB is inherited.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src_dir=$(CDPATH= cd -- "${SRC_DIR:-$test_dir/../src}" && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ldmud-async-allocation.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

for setting in USE_ASYNC_IO MALLOC_TRACE; do
    if ! grep -Eq "^#define[[:space:]]+$setting([[:space:]]|$)" "$src_dir/config.h"; then
        echo "Allocation test requires $setting in $src_dir/config.h." >&2
        exit 1
    fi
done
if [ ! -x "$src_dir/ldmud" ] || [ ! -x "$src_dir/ldmud-async-io" ]; then
    echo "Build the configured driver and async helper in $src_dir first." >&2
    exit 1
fi

# Reuse the configured compiler, object list, and libraries. This additional
# target never replaces ldmud, its helper, or the configured Makefile.
cat > "$build_dir/wrap.mk" <<'MAKE'
.PHONY: async-allocation-test
async-allocation-test: $(OBJ)
	$(CC) $(CFLAGS) -I. -c "$(ASYNC_ALLOC_WRAP)" -o "$(ASYNC_ALLOC_OUTPUT).o"
	$(CC) $(LDFLAGS) $(OBJ) "$(ASYNC_ALLOC_OUTPUT).o" -Wl,--wrap=xalloc_traced -Wl,--wrap=rexalloc_traced -o "$(ASYNC_ALLOC_OUTPUT)" $(LIBS)
MAKE
if ! "${MAKE:-make}" -C "$src_dir" -f Makefile -f "$build_dir/wrap.mk" \
    async-allocation-test ASYNC_ALLOC_WRAP="$test_dir/async_io_alloc_wrap.c" \
    ASYNC_ALLOC_OUTPUT="$build_dir/ldmud" > "$build_dir/build.log" 2>&1; then
    cat "$build_dir/build.log" >&2
    exit 1
fi

"${PYTHON:-/usr/bin/python3}" - "$test_dir" "$src_dir" "$build_dir" <<'PY'
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys

test, source, build = map(Path, sys.argv[1:])
driver = build / "ldmud"
helper = source / "ldmud-async-io"
options = subprocess.check_output([str(driver), "--options"], text=True)
defaults = "-u-1 -E 0 --no-compat -e -N --cleanup-time -1 --reset-time -1 --max-array 0 --max-callouts 0 --max-bytes 0 --max-file 0 -s-1 -sv-1 --hard-malloc-limit unlimited --min-malloc 0 -ru0 -rm0 -rs0 --no-strict-euids --no-wizlist-file --check-refcounts --check-state 2 --access-file none --access-log none".split()
valgrind = os.environ.get("VALGRIND")

for case, name in ((1, "request"), (2, "initial payload"), (3, "payload growth")):
    root = build / f"case-{case}"
    root.mkdir()
    shutil.copyfile(test / "t-async-alloc/master.c", root / "master.c")
    (root / "inc").symlink_to(test / "inc", target_is_directory=True)
    (root / "sys").symlink_to(test / "sys", target_is_directory=True)
    environment = os.environ.copy()
    environment["ASYNC_ALLOC_FAIL"] = str(case)
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    command = [str(driver), *defaults, "--async-io-helper", str(helper),
               "-m", str(root), "-Mmaster", "--debug-file", str(root / "debug.log"),
               "-DASYNC_ALLOC_TEST", f"-DASYNC_ALLOC_CASE={case}",
               "--inherit", str(listener.fileno())]
    if "Python supported" in options:
        startup = root / "python-startup.py"
        startup.write_text("")
        command += ["--python-script", str(startup)]
    if valgrind:
        command = [valgrind, "--tool=memcheck", "--error-exitcode=99",
                   "--leak-check=full", "--errors-for-leak-kinds=definite,indirect",
                   "--suppressions=" + str(source / "valgrind/ldmud.supp"),
                   "--log-file=" + str(root / "valgrind.log"), "--", *command]
    process = None
    try:
        with (root / "stdout").open("w") as stdout, (root / "stderr").open("w") as stderr:
            process = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                       env=environment, start_new_session=True,
                                       pass_fds=(listener.fileno(),))
            process.wait(timeout=60 if valgrind else 40)
        logs = "\n".join((root / file).read_text(errors="replace")
                         for file in ("stdout", "stderr", "debug.log"))
        assert process.returncode == 0, f"driver exit {process.returncode}"
        assert (root / "completed").read_text() == "PASS", "LPC assertions did not finish"
        assert not (root / "rejected").exists(), "rejected destination exists"
        assert (root / "accepted").read_bytes() == b"R" * 12288, "recovery output differs"
        injections = re.findall(r"ASYNC_ALLOC_FAIL case=(\d+) kind=(\w+) size=(\d+)", logs)
        expected_kind = "rexalloc" if case == 3 else "xalloc"
        assert len(injections) == 1 and injections[0][:2] == (str(case), expected_kind), "wrong or missing one-shot injection"
        assert "Success: No lost block." in logs, "final GC was not verified"
        assert not re.search(r"Discarding \d+ async file requests during cleanup", logs), "request survived until shutdown cleanup"
        assert not re.search(r"ref count|FAIL|freeing.*block|tabled string.*was left unreferenced",
                             logs.replace("ASYNC_ALLOC_FAIL", "injection")), "ownership or fixture diagnostic"
        if valgrind:
            vg = (root / "valgrind.log").read_text(errors="replace")
            assert "ERROR SUMMARY: 0 errors" in vg, "Memcheck errors"
            possible = re.search(r"possibly lost: ([^\n]+)", vg)
            if possible:
                print(f"Memcheck {name}: possibly lost: {possible.group(1).strip()}", flush=True)
        print(f"PASS async allocation {name}: rejection, recovery, callbacks, GC" +
              (", Memcheck" if valgrind else ""), flush=True)
    except BaseException:
        if process:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        for file in ("stdout", "stderr", "debug.log", "valgrind.log"):
            path = root / file
            if path.exists():
                print(f"case {case}: {file}\n{path.read_text(errors='replace')}", file=sys.stderr)
        raise
    finally:
        listener.close()
print("All three async allocation failure cases passed.", flush=True)
PY
