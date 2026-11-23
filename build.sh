#!/bin/bash
# Build the Peter runtime (Loader sources + Win32 shim) to WebAssembly and assemble
# the playable site in dist/.
#
# Needs: emsdk (em++ on PATH) and the MinGW-w64 headers (apt install mingw-w64-common,
# Ubuntu 24.04 / mingw-w64 v11). Override the header location with MINGW_INC.
# Env: OPT/LOPT (compile/link optimisation, default -O2), FORCE=1 (rebuild all), JOBS.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
B=$ROOT/build
DIST=$ROOT/dist
MINGW_INC=${MINGW_INC:-/usr/share/mingw-w64/include}
JOBS=${JOBS:-$(nproc)}
mkdir -p "$B/obj" "$B/logs"

# Patched copy of the MinGW headers (adds wasm32 to a few x86 checks).
if [ ! -f "$B/winhdr/.patched" ]; then
	[ -f "$MINGW_INC/windows.h" ] || { echo "MinGW headers not found in $MINGW_INC"; exit 1; }
	rm -rf "$B/winhdr"
	cp -r "$MINGW_INC" "$B/winhdr"
	patch -s "$B/winhdr/_mingw_mac.h" "$ROOT/winhdr-patches/_mingw_mac.h.diff"
	patch -s "$B/winhdr/winnt.h" "$ROOT/winhdr-patches/winnt.h.diff"
	touch "$B/winhdr/.patched"
fi

LOADER="bufd3d buficon bufint buflist bufmap bufmus bufpic bufreal bufsnd bufsprt buftext bufwin bufxfile jpeg bitmap compress d3gl0 d3gl1 d3gl2 d3no file main mainfrm memory comp compcom compico complog compmap compmus compnum comppic compsnd compspr comptxt load exec execcom execico execlog execmap execmus execnum execpic execsnd execspr exectxt"
SHIM="core file wnd gdi gl audio dlg"
FLAGS="$(cat "$ROOT/loader/build_flags.txt") ${OPT:--O2} -I$ROOT/loader -I$ROOT/shim -idirafter $B/winhdr"
newest_hdr=$(ls -t "$ROOT"/shim/*.h "$ROOT"/loader/*.h "$ROOT/loader/build_flags.txt" | head -1)

compile() { # src obj log
	if [ -z "${FORCE:-}" ] && [ -f "$2" ] && [ "$2" -nt "$1" ] && [ "$2" -nt "$newest_hdr" ]; then return; fi
	em++ -c "$1" -o "$2" $FLAGS > "$3" 2>&1 || echo "FAIL $1"
}
cd "$ROOT/loader"
for f in $LOADER; do
	compile "$ROOT/loader/$f.cpp" "$B/obj/$f.o" "$B/logs/$f.log" &
	while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do sleep 0.2; done
done
for f in $SHIM; do
	compile "$ROOT/shim/$f.cpp" "$B/obj/shim_$f.o" "$B/logs/shim_$f.log" &
	while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do sleep 0.2; done
done
wait

err=0
for l in "$B"/logs/*.log; do
	n=$(grep -c "error:" "$l" || true)
	if [ "$n" != "0" ]; then echo "$(basename "$l"): $n errors"; grep -m5 "error:" "$l"; err=1; fi
done
[ $err = 1 ] && exit 1

LINK="-sASYNCIFY -sASYNCIFY_STACK_SIZE=1048576 -sSTACK_SIZE=4194304 -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=134217728 \
 -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2 -sFORCE_FILESYSTEM -lidbfs.js \
 -sEXPORTED_FUNCTIONS=_main,_malloc,_free -sEXPORTED_RUNTIME_METHODS=ccall,callMain,addRunDependency,removeRunDependency,FS,IDBFS,HEAPU8,HEAP16,HEAP32,stringToNewUTF8,UTF8ToString \
 -sGLOBAL_BASE=1048576 -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sENVIRONMENT=web ${LOPT:--O2}"
mkdir -p "$B/out"
em++ "$B"/obj/*.o -o "$B/out/peter_rt.js" $LINK > "$B/logs/link.log" 2>&1 || {
	echo "LINK FAILED"; grep -E "error|undefined" "$B/logs/link.log" | head -40; exit 1; }

# Assemble the site.
rm -rf "$DIST"
mkdir -p "$DIST"
cp "$ROOT"/web/index.html "$ROOT"/web/peter.js "$B"/out/peter_rt.js "$B"/out/peter_rt.wasm "$DIST"/
for d in point236.json soundfont.json; do
	if [ -f "$ROOT/data/$d" ]; then cp "$ROOT/data/$d" "$DIST/"; else echo "warning: data/$d missing"; fi
done
touch "$DIST/.nojekyll"
ls -la "$DIST"
