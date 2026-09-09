#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ldmud-async-lifecycle.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
${CC:-cc} ${CPPFLAGS:-} ${CFLAGS:--O2 -g -Wall -Wextra -Werror} \
    -I"$test_dir/../src" "$test_dir/async_io_fault_helper.c" ${LDFLAGS:-} \
    -o "$build_dir/peer"
${PYTHON:-python3} "$test_dir/async_io_lifecycle_test.py" \
    "${DRIVER:-$test_dir/../src/ldmud}" "$build_dir/peer" "$@"
${PYTHON:-python3} "$test_dir/async_io_parent_exit_test.py" \
    "${DRIVER:-$test_dir/../src/ldmud}"
