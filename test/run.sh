#!/bin/sh
# Builds the library against the host fake in test/stub and runs the tests.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -I "$here/stub" -I "$here/../src" \
    "$here/../src/OpenTherm.cpp" "$here/../src/OpenThermGasEstimator.cpp" "$here/test.cpp" -o "$out/test"
"$out/test"
