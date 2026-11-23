# Point 236: WebAssembly port of the Peter runtime

**Play it in your browser: https://lanrat.github.io/point236/**

`loader/` is the original Peter 2.50 Loader (runtime) source by Miroslav Němeček
(https://breatharian.eu/Petr/en/download.htm). File names are lowercased and include
paths normalised. Changes for wasm:

- `memory.cpp`: Peter's region allocator replaced with malloc/realloc. Blocks are
  padded to power-of-two sizes, as the original was.
- `main.cpp`: shared "empty" objects are created in a priority-101 static constructor,
  because Emscripten runs C++ global constructors before `main()`. The custom
  `InitTerm(__xc_a, __xc_z)` call is removed.
- `memory.h`: `MemCopy` uses `memmove`; the runtime does overlapping copies.
- `buf*.cpp`, `compnum.cpp`: `m_Data[NewItem()] = x` is split into two statements.
  C++17 evaluates `m_Data` before `NewItem()` reallocates it; VC6 did the reverse.
- VC6 for-loop scoping is fixed by hoisting declarations, MSVC friend declarations
  are made visible, and the native-DLL call (`FDLLExecNum`) is stubbed.
- The Direct3D backends (`d3dx*.cpp`) are not built. The OpenGL backends are used.

`shim/` is a Win32 layer for the browser:
- `core.cpp`: memory, strings, time, PE image mapping and resources, registry, INI files.
- `file.cpp`: files on Emscripten's FS. `C:\` maps to `/c/`, backed by IDBFS.
- `wnd.cpp`: windows, message queue, timers, keyboard/mouse, and controls as DOM elements.
- `gdi.cpp`: device contexts, DIB blits and text (via canvas) on RGBA surfaces.
- `gl.cpp`: the OpenGL 1.1 fixed-function subset Peter uses, emulated on WebGL2.
- `audio.cpp`: waveOut on Web Audio, and the MCI MIDI sequencer via TinySoundFont.
- `dlg.cpp`: message boxes, resource dialogs, file dialogs and `main()`.

## Building

Needs emsdk (tested with 4.0.10) and the MinGW-w64 v11 headers
(`apt install mingw-w64-common` on Ubuntu 24.04). `build.sh` copies the headers into
`build/winhdr`, applies the two patches in `winhdr-patches/`, compiles, and assembles
the playable site in `dist/`. Set `MINGW_INC` if the headers live elsewhere.

```bash
./build.sh
python3 -m http.server 8236 --directory dist
```

Or build in Docker, with no local toolchain (put the MinGW headers in `build/mingw-include` first):

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e MINGW_INC=/src/build/mingw-include -v "$PWD:/src" emscripten/emsdk:4.0.10 bash /src/build.sh
```

`prelude.h` defines `_WIN32` only after libc++ has configured itself, so libc++ does not
select its MSVC locale layer. It also pre-marks MinGW's x86 inline-asm intrinsics as defined.
Key link flags: `-sASYNCIFY`, `-sGLOBAL_BASE=1048576` (keeps pointers above 64 KB so
`IS_INTRESOURCE` works), and `-sMAX_WEBGL_VERSION=2`.

Pushing to `main` builds and deploys to GitHub Pages via `.github/workflows/pages.yml`.

The runtime reads any Peter-made EXE: change `EXE_URL` in `web/index.html`.

## Web data files

The hosted build doesn't ship the EXE. `tools/make_peter_data.py` copies only the
`.rsrc` and `.petprg` sections (no x86 code) into `data/point236.json`.
`tools/make_sf_json.py` wraps the soundfont as `data/soundfont.json`. The runtime still
accepts a real EXE if `Module.peterImage` is left empty.
