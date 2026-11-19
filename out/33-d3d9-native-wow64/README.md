# Spec 33: Direct3D 9 for 32-bit programs as native code (the thunk side)

Session T's half of spec 33 (clarification 6). `alloc/` is session N's.

| Path | What |
|---|---|
| `table/d3d9w_table.txt` | F1: the 15 interfaces, every method in vtable order, each argument's kind. |
| `tools/gen_d3d9w.py`, `tools/d3d9w_parse.py` | F2: the generator. `python3 -I tools/gen_d3d9w.py <wine-11.4>/include/d3d9.h` checks the table against the header and writes `gen/`; `--check` only compares. |
| `gen/` | Generated, committed: ids and pop bytes, the i386 stubs and their prototypes, the vtables, the native dispatch functions, the IID table, the header check. |
| `include/d3d9w_proxy.h` | Shared by both sides: the proxy and arena layout in guest memory, the registration call's parameters, the thunk's own calls. |
| `include/d3d9w_layout.h`, `native/d3d9w_layout_check.c` | F4: the structs that cross, their i386 mirrors and the layout checks. |
| `native/` | D2, D3, F3, F5, G: the dispatch (`d3d9w_dispatch`), the proxy table and `d3d9w_object_gone`, the marshalling helpers, the special methods, the exports and the thunk's calls, the registration unix call. |
| `thunk/` | E and H: the i386 thunk's hand-written code (Part H's decision and the exports; the window, focus and cursor code; private data and GetDC), GetNPatchMode's stub, `d3d9.def`. |
| `meson.build`, `meson.options`, `build-pe.sh` | Clarification 6: the thunk as an i386 Wine builtin `d3d9.dll`. |
| `build-ios.sh` | The dispatch as `libd3d9w_dispatch.a` for aarch64-apple-ios. |
| `tests/` | J1–J3 (`run-host-tests.sh`). |

## How a call goes

The program calls a method of a proxy: `[proxy]` is the thunk's vtable for that interface, whose
entry is `mov eax, IOS_WOW_NATIVE_ID(1, n); jmp [d3d9w_gate]`. The app's gate calls
`d3d9w_dispatch(id, esp, ret)`. The method number `n` holds the argument words, the kind and the
slot, so the dispatch indexes `d3d9w_methods[kind][slot]`; the generated function reads the
arguments from `esp + 4` in one `ios_wow_native_read`, finds This's native object from the proxy's
address alone (its 64 KB arena chunk and slot: no lock, no guest read), copies in what the method
reads, calls the native method through `d3d9.h`'s C vtable, writes the out-arguments back, and
sets `ret->eax` and `ret->pop`.

## For session N and the app's build

- `build-ios.sh <wine-11.4 tree> <wine build dir> <out dir>` (the convention of `ci/ios-part.sh`)
  writes `<out>/libd3d9w_dispatch.a` and `<out>/exports.txt` (`_d3d9_unix_call_funcs`,
  `_d3d9_unix_call_wow64_funcs`). The library needs, at the app's link: the seam's
  `d3d9w_native_create9`, `d3d9w_native_create9ex`, `d3d9w_native_window_state`,
  `d3d9w_native_focus_activation`, `d3d9w_native_process_gone` (the native frontend);
  `ios_wow_window_alloc(size_t size, size_t
  align, uint32_t *guest)` (the allocator); `ios_wow_base` and `__wine_dbg_output` (Wine's unix
  side); Section A of `interfaces/ios_wow_native.h` (weak). It defines `d3d9w_object_gone`.
- The dispatch calls `ios_wow_window_alloc` for 64 KB proxy chunks (64 KB aligned), only on game
  threads inside gate calls, and never frees them while the process owns the frontend.
- `d3d9w_object_gone(iface)` takes the interface pointer the frontend handed out (the same for
  the Ex and non-Ex interfaces of one object). It may run on any thread.
- The end of a process (clarification 13): for the gate's reserved method 0xffffff, when the
  ending process owns the native frontend, the dispatch forgets its proxies (no guest memory is
  touched) and calls `d3d9w_native_process_gone()` once. Init, the creates and the process end
  run one at a time (`d3d9w_create_lock`), so a new process's create never lands between the two;
  `d3d9w_native_process_gone` is called with no dispatch lock but that one held, and must not wait
  for a gate call.
- The dispatch's sources see `windows.h` without Winsock (clarification 14):
  `native/d3d9w_dispatch.h` defines `WIN32_LEAN_AND_MEAN` before including it, because Wine's
  `winsock.h` can't be compiled after the iOS SDK's `htonl`/`htons`/`ntohl`/`ntohs` macros.
  Include a specific header for anything that turns out missing, never `winsock.h` or
  `winsock2.h`.
- `build-pe.sh <wine i386 build> <out dir>` writes `<out>/i386-windows/d3d9.dll` (meson + ninja,
  llvm-mingw from `LLVM_MINGW` or `PATH`). The app's DXMT PE build puts it in place of the full
  frontend's i386 `d3d9.dll` and renames that one `d3d9_dxmt32.dll`.
