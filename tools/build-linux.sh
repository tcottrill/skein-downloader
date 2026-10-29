#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cmake -S "$root" -B "$root/build/linux" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build/linux" --parallel 8
if [ -d "$root/tests" ]; then ctest --test-dir "$root/build/linux" --output-on-failure; fi
