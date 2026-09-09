#!/bin/sh
# Build outside the checkout, including the fault-injected worker only in tests.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src_dir="$test_dir/../src"
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ldmud-async-worker.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

if [ -f "$src_dir/async_io_worker.c" ]; then
    ${CC:-cc} ${CPPFLAGS:-} ${CFLAGS:--O2 -g -Wall -Wextra -Werror} \
        -I"$src_dir" "$src_dir/async_io_worker.c" ${LDFLAGS:-} \
        -o "$build_dir/worker"
    test_define=-DASYNC_IO_WORKER_IN_TEST
else
    test_define=
fi
${CC:-cc} ${CPPFLAGS:-} ${CFLAGS:--O2 -g -Wall -Wextra -Werror} \
    $test_define -I"$src_dir" "$test_dir/async_io_worker_test.c" \
    ${LDFLAGS:-} -o "$build_dir/test"
"$build_dir/test" "$build_dir/worker" "$build_dir"
