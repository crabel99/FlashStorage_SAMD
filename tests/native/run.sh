#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/../.." && pwd)
build_dir="$repo_dir/build/native"
mkdir -p "$build_dir"
export TMPDIR="$build_dir"
compiler=${CXX:-g++-15}
failed=0
for branch in same54_regs samd51_bitfields; do
  define=-D__SAME54__
  if [ "$branch" = samd51_bitfields ]; then
    define="-D__SAMD51__ -DTEST_LEGACY_SAMD51"
  fi
  "$compiler" -std=c++17 -O0 -g -fpermissive $define \
    -I"$repo_dir/tests/native/fakes" -I"$repo_dir/src" \
    "$repo_dir/tests/native/nvm_cache_safety.cpp" -o "$build_dir/$branch"
  echo "Branch: $branch"
  "$build_dir/$branch" || failed=1
done
exit "$failed"
