#!/usr/bin/env bash
# macOS/Linux build, regression tests and local launcher preparation.
set -euo pipefail
source_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "${1:-}" == --steamdeck ]]; then
  shift
  exec python3 "$source_root/tools/build_steamdeck.py" "$@"
fi
build_dir="${1:-${source_root}/build}"
configure_args=()
if [[ "$(uname -s)" == Darwin ]]; then
  if [[ -n "${CXX:-}" ]]; then
    compiler="$CXX"
  elif [[ -x "$(brew --prefix llvm@21 2>/dev/null)/bin/clang++" ]]; then
    compiler="$(brew --prefix llvm@21)/bin/clang++"
  else
    compiler="$(brew --prefix llvm)/bin/clang++"
  fi
  sdk="$(xcrun --sdk macosx --show-sdk-path)"
  configure_args+=("-DCMAKE_CXX_COMPILER=$compiler" "-DCMAKE_OSX_SYSROOT=$sdk"
    "-DCMAKE_CXX_FLAGS=${CXXFLAGS:-} -nostdinc++ -isystem \"$sdk/usr/include/c++/v1\"")
fi
cmake -S "$source_root" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPSPRECOMP_GENERATED_OPT_LEVEL=1 -DPSPRECOMP_HOT_GENERATED_OPT_LEVEL=1 \
  -DPSPRECOMP_BUILD_TESTS=ON -DPSPRECOMP_BUILD_PROFILE_TESTS=ON \
  "-DPSPRECOMP_ENABLE_SANITIZERS=${PSPRECOMP_ENABLE_SANITIZERS:-OFF}" \
  "${configure_args[@]}"
# Build all enabled test targets, including graphics and mission regressions.
cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PSPRECOMP_WINDOW=0 PSPRECOMP_AUDIO=0 \
  ctest --test-dir "$build_dir" --output-on-failure
if [[ "${PSPRECOMP_ENABLE_SANITIZERS:-OFF}" != ON ]]; then
  python3 "$source_root/tools/prepare_run.py" "$build_dir"
fi
