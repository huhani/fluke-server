#!/usr/bin/env bash
# Builds the 32-bit (x86) plugin DLL with zig (MinGW target) instead of Visual Studio + vcpkg.
# Run from Git Bash:  bash build/build-zig.sh
# Output: build/out/aimp_remote_reitansora.dll (libc++ is linked statically, no extra runtime DLLs)
#
# Dependency folders default to the repository root and can be overridden:
#   ZIG, HTTPLIB_DIR, JSON_DIR, IXWEBSOCKET_DIR, JOBS
# Third-party objects are cached in build/out/obj/deps; delete build/out to rebuild them.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZIG="${ZIG:-$ROOT/zig-x86_64-windows-0.16.0/zig.exe}"
HTTPLIB_DIR="${HTTPLIB_DIR:-$ROOT/cpp-httplib-0.57.1}"
JSON_DIR="${JSON_DIR:-$ROOT/json-3.12.0}"
IXWEBSOCKET_DIR="${IXWEBSOCKET_DIR:-$ROOT/IXWebSocket-12.0.1}"
JOBS="${JOBS:-$(nproc)}"

OUT="$ROOT/build/out"
OBJ="$OUT/obj"
DLL="aimp_remote_reitansora.dll"

# zig.exe is a native Windows program: pass it C:/-style paths
win() { cygpath -m "$1"; }

for path in "$ZIG" "$HTTPLIB_DIR/httplib.h" "$JSON_DIR/single_include/nlohmann/json.hpp" "$IXWEBSOCKET_DIR/ixwebsocket"; do
    if [ ! -e "$path" ]; then
        echo "missing: $path" >&2
        exit 1
    fi
done

# Mirrors the Release|Win32 settings of aimp_remote_reitansora.vcxproj.
# The vcxproj-only _SSIZE_T_DEFINED/_TIMESPEC_DEFINED/HAVE_STRUCT_TIMESPEC workarounds are not needed with MinGW.
CXXFLAGS=(
    -target x86-windows-gnu -std=c++20 -O2
    -Wno-nullability-completeness -Wno-macro-redefined
    -DWIN32 -D_WINDOWS -D_USRDLL -DNDEBUG -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE
    -D_WIN32_WINNT=0x0A00 -DAIMPREMOTEREITANSORA_EXPORTS
    -I"$(win "$ROOT/src")" -I"$(win "$ROOT/sdk/aimp/5.40")"
    -I"$(win "$HTTPLIB_DIR")" -I"$(win "$JSON_DIR/single_include")" -I"$(win "$IXWEBSOCKET_DIR")"
)

# MSVC links gdi32/user32/... by default; MinGW needs them listed. src/aimp_remote_reitansora.rc holds no resources and is skipped.
LIBS=(-lws2_32 -lcrypt32 -lbcrypt -liphlpapi -lgdi32 -luser32 -lole32 -ladvapi32 -lshell32)

# compile <source> <object>: prints FAIL and keeps the log next to the object on error
compile() {
    if "$ZIG" c++ "${CXXFLAGS[@]}" -c "$(win "$1")" -o "$(win "$2")" > "$2.log" 2>&1; then
        rm -f "$2.log"
    else
        echo "FAIL $1 (log: $2.log)"
    fi
}

mkdir -p "$OBJ/src" "$OBJ/deps"
rm -f "$OBJ"/src/*.o "$OBJ"/src/*.log "$OBJ"/deps/*.log

# Plugin sources are always rebuilt; IXWebSocket sources (TLS and zlib disabled) only when their object is missing
UNITS=()
while IFS= read -r src; do
    UNITS+=("$src|$OBJ/src/$(echo "${src#"$ROOT/src/"}" | tr '/' '_' | sed 's/\.cpp$/.o/')")
done < <(find "$ROOT/src" -name '*.cpp' | sort)
for rel in $(sed -n '/set( *IXWEBSOCKET_SOURCES/,/)/p' "$IXWEBSOCKET_DIR/CMakeLists.txt" | grep -o 'ixwebsocket/[A-Za-z]*\.cpp'); do
    obj="$OBJ/deps/$(basename "$rel" .cpp).o"
    [ -f "$obj" ] || UNITS+=("$IXWEBSOCKET_DIR/$rel|$obj")
done

echo "compiling ${#UNITS[@]} files with $JOBS jobs"
for unit in "${UNITS[@]}"; do
    compile "${unit%%|*}" "${unit#*|}" &
    if [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; then
        wait -n
    fi
done
wait

if compgen -G "$OBJ/*/*.log" > /dev/null; then
    echo "build failed" >&2
    exit 1
fi

OBJECTS=()
for obj in "$OBJ"/src/*.o "$OBJ"/deps/*.o; do
    OBJECTS+=("$(win "$obj")")
done

# Link inside build/out so the import library and PDB land there
cd "$OUT" || exit 1
if ! "$ZIG" c++ -target x86-windows-gnu -shared -o "$DLL" "${OBJECTS[@]}" "$(win "$ROOT/src/Source.def")" "${LIBS[@]}"; then
    echo "link failed" >&2
    exit 1
fi
echo "built $(win "$OUT/$DLL")"
