#!/usr/bin/env bash
# Build Discord's libdave (DAVE E2EE) and Cisco's mlspp at pinned revisions into third_party/dave/prefix.
# Pins: libdave per docs/DAVE.md; mlspp = the revision libdave's openssl_3 vcpkg overlay pins.
# Flags follow libdave's README: MLS_CXX_NAMESPACE=mlspp, DISABLE_GREASE=ON, OpenSSL 3.
# Needs network once (git clone); afterwards everything (build, tests) is offline. Idempotent.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party/dave"; PREFIX="$TP/prefix"
LIBDAVE_REF="${LIBDAVE_REF:-8de72b1f8a2ac3c5a5270755bb8091a62e3c6169}"
MLSPP_REF="${MLSPP_REF:-1cc50a124a3bc4e143a787ec934280dc70c1034d}"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "$TP"
fetch() {  # name url ref
    local d="$TP/src/$1"
    if [[ ! -d "$d/.git" ]]; then git clone -q "$2" "$d"; fi
    git -C "$d" fetch -q origin "$3" 2>/dev/null || true
    git -C "$d" checkout -q "$3"
}
[[ -f "$PREFIX/lib/libdave.a" && "$(cat "$PREFIX/.refs" 2>/dev/null)" == "$LIBDAVE_REF $MLSPP_REF" ]] && { echo "[build-dave] up to date: $PREFIX"; exit 0; }
fetch mlspp https://github.com/cisco/mlspp.git "$MLSPP_REF"
fetch libdave https://github.com/discord/libdave.git "$LIBDAVE_REF"
# nlohmann_json: reuse the Conan-provided package config from the main build if present.
GEN="$ROOT/build/build/Release/generators"
JSON_ARGS=()
# Prefer the Conan packages (same OpenSSL as the main binary) when the main build has been configured.
[[ -f "$GEN/nlohmann_json-config.cmake" ]] && JSON_ARGS=(-Dnlohmann_json_DIR="$GEN" -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON)
cmake -S "$TP/src/mlspp" -B "$TP/build/mlspp" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DDISABLE_GREASE=ON -DMLS_CXX_NAMESPACE=mlspp -DTESTING=OFF \
  -DCMAKE_PREFIX_PATH="$GEN" -DCMAKE_CXX_FLAGS="-Wno-error" "${JSON_ARGS[@]}" >/dev/null
cmake --build "$TP/build/mlspp" -j"$JOBS" >/dev/null
cmake --install "$TP/build/mlspp" >/dev/null
cmake -S "$TP/src/libdave/cpp" -B "$TP/build/libdave" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DTESTING=OFF -DCMAKE_PREFIX_PATH="$PREFIX;$GEN" -DCMAKE_CXX_FLAGS="-Wno-error" "${JSON_ARGS[@]}" >/dev/null
cmake --build "$TP/build/libdave" -j"$JOBS" >/dev/null
cmake --install "$TP/build/libdave" >/dev/null
echo "$LIBDAVE_REF $MLSPP_REF" >"$PREFIX/.refs"
echo "[build-dave] installed libdave $LIBDAVE_REF + mlspp $MLSPP_REF into $PREFIX"
