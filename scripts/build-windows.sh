#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
toolchain_root="${1:-${LLVM_MINGW_ROOT:-}}"
if [[ -z "$toolchain_root" ]]; then
    printf 'Usage: bash scripts/build-windows.sh /absolute/path/to/llvm-mingw\n' >&2
    exit 1
fi
toolchain_root="$(cd -- "$toolchain_root" && pwd)"

cmake -S "$project_root" -B "$project_root/build-windows" \
    "-DCMAKE_TOOLCHAIN_FILE=$project_root/cmake/llvm-mingw-x64.cmake" \
    "-DLLVM_MINGW_ROOT=$toolchain_root" -DCMAKE_BUILD_TYPE=Release
cmake --build "$project_root/build-windows" --parallel
python3 "$project_root/scripts/package-windows.py" --toolchain "$toolchain_root"
