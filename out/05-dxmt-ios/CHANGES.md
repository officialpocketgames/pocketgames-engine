# Spec 05 changes to upstream DXMT a3a6e8f

One line per change: file (relative to `dxmt/` unless noted), what changed, and the Part of
`specs/05-dxmt-ios.md` it implements.

## Part A: build

- `meson.build`: detect ARM64EC by test-compiling a file that needs `__arm64ec__`/`_M_ARM64EC` (A).
- `meson.build`: `aarch64` cpu family; install to `arm64ec-windows` / `aarch64-windows`, also for the native-install case (A).
- `meson.build`: define `DXMT_IOS=1` for C and C++ whenever the host system is Windows (A).
- `meson.build`: `metal_shader_args` = `-std=<metal_std> --target=air64-apple-macos14.0`; native `-arch x86_64` only for x86 targets (A).
- `meson.options`: new option `metal_std`, default `metal3.1` (A).
- `src/dxmt/meson.build`: `dxmt_command.metal` compiled with `metal_shader_args` (A).
- `src/airconv/meson.build`: helper shaders compiled with `metal_shader_args` (A).
- `build-win-aarch64.txt`, `build-win-arm64ec.txt` (new): meson cross files, toolchain directory as placeholder `@LLVM_MINGW_DIR@` (A).
- `src/d3d11/d3d11_swapchain.cpp`: `CreateSwapChain` no longer refuses a window owned by another process id under `DXMT_IOS` (A).
- `../build-ios.sh` (new): compiles the unix half for iOS arm64 into `obj/` and `libdxmt_unix.a`, generates the helper-shader headers (A).
- `src/winemetal/unix/winemetal_unix.c`: tables renamed to `dxmt_winemetal_unix_call_funcs` / `dxmt_winemetal_unix_call_wow64_funcs` under `DXMT_IOS`; the wow64 table is all `STATUS_NOT_IMPLEMENTED`, same length (A).
- `src/winemetal/unix/winemetal_unix.c`: iOS includes (Foundation/CoreGraphics/objc runtime instead of Cocoa/ColorSync/bootstrap); one display with id 1 described as sRGB SDR; bootstrap calls fail; `MTLCopyAllDevices` replaced by the system default device; private logic-op setters and `shouldMaximizeConcurrentCompilation` only sent when the object answers them; 32-bit thunks compiled out (A, needed to compile with the iOS SDK).
- `src/winemetal/unix/ios_support.h`, `ios_support.m` (new): iOS-only unix-side code, compiled by `build-ios.sh` (A and later Parts).

## Part B: Metal on iOS

- `src/winemetal/winemetal_thunks.c`: `MTLDevice_newBuffer`, `MTLDevice_newTexture`, `MTLBuffer_newTexture`, `MTLDevice_newSharedTexture` turn Managed storage into Shared; `MTLBuffer_didModifyRange` is a no-op under `DXMT_IOS` (B1).
- `src/winemetal/unix/winemetal_unix.c`: `didModifyRange:` and the Managed check in `updateContents` compiled out on iOS; texture descriptors get Shared for Managed; buffer-backed textures take the buffer's storage and CPU cache mode (B1).
- `src/winemetal/unix/winemetal_unix.c`, `ios_support.m`: buffers drop `MTLResourceCPUCacheModeWriteCombined` before `newBufferWithLength:`/`newBufferWithBytesNoCopy:` unless `DXMT_WRITE_COMBINED=1` (B2).
- `src/dxmt/dxmt_format.cpp`: a device with neither Apple7 nor Mac2 logs a warning and uses the Apple7 format table instead of throwing (B3).
- `src/dxmt/dxmt_device.cpp`: Metal 3.2 shaders from iOS 18 on (the version check compared an iOS version with the macOS threshold 15) (B3, needed for iOS).
- `src/d3d11/d3d11.cpp`: `D3D11CoreCreateDevice` logs the exception's reason through `__wine_dbg_output` (found with `GetProcAddress`) or stderr; `D3D11CreateDeviceAndSwapChain` logs a failing HRESULT the same way (B4).
- `src/d3d11/d3d11.cpp`: one log line at the first device creation with BC support and Apple7/8/9/Mac2 (B5).
- `src/d3d11/d3d11_resource_helper.cpp`: unsupported sample counts fall back to the largest supported power of two below them, logged once; fail only when none above 1 is supported (B6).
- `src/d3d11/d3d11_texture_device.cpp`: a shared texture without a mach port is created as an ordinary, usable texture (B7).
- `src/winemetal/unix/winemetal_unix.c`: `newSharedTexture` reports port 0 when iOS gives none (or `createMachPort` is missing) and falls back to a plain texture if needed (B7).
- `src/winemetal/winemetal.h`: `enum WMTBlitOption`; new `options` field at the end of `wmtcmd_blit_copy_from_buffer_to_texture` and `wmtcmd_blit_copy_from_texture_to_buffer` (B8).
- `src/winemetal/unix/winemetal_unix.c`: buffer↔texture blits pass `options:` to Metal (B8).
- `src/d3d11/d3d11_context_impl.cpp`: `DepthStencilPlaneBlitOption` picks depth or stencil for copies between buffers and combined depth-stencil textures; set on the four buffer↔texture copy sites (B8).
- `src/dxmt/dxmt_context.hpp`, `src/dxmt/dxmt_resource_initializer.hpp`: blit commands are zeroed when taken from the command heap (B8).
- `src/dxmt/dxmt_command_queue.cpp`: comment only; upstream's `CommitChunkInternal` already creates an autorelease pool that is drained when it returns, which is what B9 asks for (B9).
- `src/winemetal/unix/winemetal_unix.c`, `ios_support.m`: `replaceRegion:` and buffer-to-texture copies into uncompressed textures are skipped (and logged rarely) when the row pitch is below width × bytes per pixel of the texture's actual Metal format (B10).

## Part C: block-compressed textures on GPUs without BC

- `src/dxmt/dxmt_bc_decode.hpp`, `.cpp` (new): pure C++ decoder for BC1, BC2, BC3, BC4/BC5 (unsigned and signed) and BC7 (all modes, partition and anchor tables, rotation, index selection), `DecodeSubresource` for a whole subresource and `FillBC6HGrey`; allocates nothing (C).
- `src/dxmt/dxmt_bc_emulation.hpp`, `.cpp` (new): BC support worked out once per process, the BC→physical format table, `BCDecodeToPhysical` (C).
- `src/dxmt/meson.build`: builds the two new files (C).
- `src/util/util_log_rare.hpp` (new): `WARN_RARE`, logs the first and every 256th occurrence (C and later Parts).
- `src/winemetal/unix/winemetal_unix.c`, `ios_support.m`: `to_metal_pixel_format` maps BC formats to the physical format when the system default device has no BC support (worked out once); used for every texture and view the unix half creates (C).
- `src/dxmt/dxmt_resource_initializer.cpp`, `.hpp`: initial data of emulated BC textures is decoded; zero-fill sized for the physical format; BC6H filled with opaque grey; 3D BC initial data decoded slice by slice (clarification 3); failed decode-buffer allocation skips the upload and logs (C1, C2).
- `src/d3d11/d3d11_context_impl.cpp`: `UpdateSubresource`/`UpdateSubresource1` and dynamic-texture `Unmap` into emulated BC textures decode on the calling thread (`UploadEmulatedBC`) (C3).
- `src/d3d11/d3d11_context_impl.cpp`: `CopyTextureEmulatedBC`: staging→BC texture decoded from the staging resource's CPU memory at the call; BC→BC device copies with the same physical format copied as textures; other copies that would move block bytes into or out of an uncompressed texture are skipped and logged (C4).
- `src/d3d11/d3d11_texture.cpp`: dynamic textures' initial data (`InitializeTextureData`) decoded for emulated 2D BC textures, and slice by slice for 3D (C, clarification 3).

## Part D: memory

- `src/winemetal/unix/ios_support.m`, `.h`: settings-file reader for `ram-reserve-mb`, `vram-mb`, `vram-trim-mb`; memory cap (`hw.memsize − reserve`), limit `min(footprint + os_proc_available_memory(), cap)`, base budget and trimmed budget (re-evaluated at most every 250 ms, moves of 64 MB logged) (D1).
- `src/winemetal/unix/winemetal_unix.c`: `recommendedMaxWorkingSetSize` returns the budget on iOS (D1).
- `src/dxgi/dxgi_adapter.cpp`: `DedicatedVideoMemory` = the budget, not halved on unified memory (D1).
- `src/winemetal/*`: control call (slot 127) op 7 returns `os_proc_available_memory()`; PE helper `WMTQueryAvailableMemory` (D1, H6).
- `src/dxmt/dxmt_staging.cpp`, `.hpp`: process-wide count of staging bytes held beyond each resource's first buffer; `hasReusableAllocation`, `gpuOccupiedUntil` (D2).
- `src/d3d11/d3d11_context_imm.cpp`: staging `Map(WRITE)` that would rename without a reusable buffer and past `d3d11.stagingRenameBudgetMB` (default 256, 0 = never) waits for the GPU instead; with `DO_NOT_WAIT` it flushes and returns `DXGI_ERROR_WAS_STILL_DRAWING` (D2).
- `src/dxmt/dxmt_resource_initializer.cpp`, `.hpp`: pending uploads are submitted once 64 MB of upload memory were used since the last submission, before the next allocation (D3).
- `src/dxmt/dxmt_dynamic.cpp`, `.hpp`: the dynamic buffer's retired queue is a deque; after each allocation the leading run of finished entries is trimmed to 64 (D4).
- `src/dxmt/dxmt_mip_clamp.hpp` (new): pure policy functions (bias choice, requery rule, pressure rule, view-range translation, min LOD over lost levels, 2×2 box filter) (D5).
- `src/d3d11/d3d11_texture_device.cpp`: `MipClampBias` (`d3d11.mipClampBC`, `d3d11.mipClampAuto`, `d3d11.mipClampAutoMB`); clamped textures get a physical description; initial data/zero-fill skip levels above the bias; SRV mip ranges translated to physical levels (D5).
- `src/dxmt/dxmt_texture.hpp`, `.cpp`: mip bias and lost-level mask on `Texture`; `lostLevelMinLod` (D5).
- `src/dxmt/dxmt_context.cpp`: SRV metadata carries the min LOD that skips lost levels (D5).
- `src/d3d11/d3d11_context_impl.cpp`: `TranslateClampedCopy` (subresource translation, dropped writes, lost marks), lost marks cleared by full writes, `RefillLostLevels` (2×2 box filter from a full CPU write, chained through consecutive lost levels; a write to logical level bias−1 refills physical level 0); `UpdateSubresource` level translation (D5).
- `src/dxmt/dxmt_ring_bump_allocator.hpp`: staging blocks release their Metal buffer before freeing the memory under it (D6).
- `src/dxmt/dxmt_ring_bump_allocator.hpp`: `DXMT_RING_OVERSIZE_REUSE` keeps up to two oversized blocks (≤ 16 MB) per ring for reuse; a finished one smaller than the request is dropped (D7).
- `src/dxmt/dxmt_resource_initializer.cpp`: `DXMT_ZERO_BUFFER_POW2` grows the zero buffer to the next power of two (≥ 1 MB) (D8).

## Part E: presenting

- `src/util/wsi_monitor_ios.cpp`, `src/util/wsi_window_ios.cpp` (new), `src/util/meson.build`: headless WSI under `DXMT_IOS`: one monitor (`(HMONITOR)1`, or user32's primary monitor with `DXMT_WSI_MONITOR_IDENTITY`), `\.\DISPLAY1`, screen size from `GetSystemMetrics` (1024×768 fallback), mode list 640×480/800×600/screen at 60 Hz 32 bpp (or user32's list with `DXMT_WSI_MODE_TABLE`), client size from `GetClientRect`, fullscreen/mode changes succeed without doing anything, never minimized, always foreground (E1).
- `src/util/util_ios_switch.hpp` (new): `env::readSwitch` (unset/empty = default, `0` = off, else on) (E1 and other switches).
- `src/dxmt/dxmt_context.cpp`: plain `presentDrawable:` under `DXMT_IOS`; no present when there is no drawable (E2.1, E2.2).
- `src/dxmt/dxmt_presenter.cpp`: no blit when `nextDrawable` gives nil (E2.2).
- `src/d3d11/d3d11_swapchain.cpp`, `src/dxmt/dxmt_command_queue.hpp`, `src/dxmt/dxmt_command_list.hpp`: `Present` commits a non-empty chunk before recording the present (`d3d11.commitBeforePresent`, default 1) (E2.3).
- `src/dxmt/dxmt_presenter.hpp`, `src/winemetal/*`: `Presenter::setDisplaySyncEnabled`, sent to the layer (unix-call slot 128) only when the value changes (E2.4).
- `src/winemetal/unix/winemetal_unix.c`: `displaySyncEnabled` read/written by selector on iOS; `getProps` reports contents scale 1.0 with `DXMT_PRESENT_PIXELS` (E2.4, E2.5).
- `src/winemetal/unix/ios_support.m`, `winemetal_unix.c`: pacing modes 0–4, `DXMT_PRESENT_LIMITER`, RAW mode's nil drawables, present count, `nextDrawable` timing logs, GPU meter on every commit, and the exported `madeira_*` functions (E3).

## Part F: shader converter (airconv)

- `src/airconv/dxbc_signature.cpp`: `SV_CullDistance` outputs (vertex, domain, geometry) and pixel inputs are left out instead of asserting (F1).
- `src/airconv/nt/dxbc_converter_base.cpp`, `.hpp`: `TexelAccessInRange` for 2D, 2D-array and 3D (and depth) textures: mip count, then size at the clamped level, then array length; out-of-range coordinate/level/slice clamped to 0; `ld` and typed UAV loads select 0, typed UAV stores are skipped by a branch; multisampled loads unchanged (F2).
- `src/dxmt/dxmt_shader_cache.hpp`: shader cache version 15 → 16 (F2–F4).
- `src/airconv/airconv_public.h`: new argument kind `SM50_SHADER_PSO_VERTEX_SHADER_BYTECODE` (8, appended) with `SM50_SHADER_PSO_VERTEX_SHADER_BYTECODE_DATA` (F3).
- `src/airconv/dxbc_converter.cpp`, `.hpp`, `dxbc_signature.cpp`: the pixel shader compile parses the vertex shader's output signature; pixel inputs it doesn't write (matched by `fullSemanticString`) aren't declared and read as zero; the count is logged rarely (F3).
- `src/d3d11/d3d11_shader.hpp`, `d3d11_shader.cpp`, `d3d11_pipeline_cache.cpp`, `d3d11_pipeline.cpp`: shaders keep their bytecode; the VS→PS pipeline passes the vertex shader with the pixel shader variant (part of the variant key and digest) (F3).
- `src/airconv/dxbc_signature.cpp`: render-target output components the pixel shader doesn't declare are initialised to 0, alpha to 1 (integer outputs too) (F4).
- `src/airconv/dxbc_converter.cpp`, `.hpp`: `SM50Initialize` keeps only the bytecode and the argument reflection (`SM50ShaderHandle`); every compile entry point parses again for the compile and fails with "Invalid DXBC bytecode" if that fails; `ParseShaderHandle` for tools (F5).
- `src/airconv/airconv_cli.cpp`: the command-line tool parses handles through `ParseShaderHandle` (F5, keeps the tool working).

## Part G: DXGI and NVAPI

- `src/dxgi/dxgi_factory.cpp`: the factory is an `IDXGIFactory7` and answers `QueryInterface` for it; `RegisterAdaptersChangedEvent` checks its arguments and hands out a non-zero cookie; `UnregisterAdaptersChangedEvent` returns `S_OK`; no event is signalled (G1).
- `src/dxgi/dxgi_factory.cpp`: `EnumAdapterByLuid` walks the Metal devices, compares `GetAdapterLuid`, returns the adapter through `QueryInterface`; `DXGI_ERROR_NOT_FOUND` / `DXGI_ERROR_INVALID_CALL` (G2).
- `src/dxgi/dxgi_adapter.cpp`: `CheckInterfaceSupport` reports the UMD version from `D3DKMTQueryAdapterInfo(KMTQAITYPE_UMD_DRIVER_VERSION = 18)` (declared locally, looked up in gdi32/win32u), all ones on failure or 0 (G3).
- `src/dxgi/dxgi_adapter.cpp`: content-protection teardown registration returns `DXGI_ERROR_UNSUPPORTED` (`E_INVALIDARG` for a null cookie), budget-change registration `DXGI_ERROR_UNSUPPORTED`, unregistering is a no-op; no asserts (G4).
- `src/nvapi/nvapi.cpp`: NUL-terminated, cut strings; driver 58157 / `r580_00`; `NvAPI_GPU_GetAllClockFrequencies` (v1–3), `NvAPI_GPU_GetGpuCoreCount`, `NvAPI_GetLogicalGPUFromPhysicalGPU`, `NvAPI_GetPhysicalGPUsFromLogicalGPU`, `NvAPI_GetAssociatedNvidiaDisplayHandle`, `NvAPI_GetAssociatedDisplayOutputId`, `NvAPI_DISP_GetDisplayIdByDisplayName` (also the primary display device's name), `NvAPI_GPU_GetPCIIdentifiers`, `NvAPI_GPU_GetThermalSettings` (v1–2) with pointer, handle and struct-version checks (G5).

## Part H: the rest of the Windows half

- `src/dxmt/dxmt_occlusion_query.hpp`, `src/dxmt/dxmt_context.cpp`: with `DXMT_QUERY_SETTLE_EMPTY` (default off), a submission without visibility samples that ends pending queries gets an empty readback (no buffer) that marks them issued when the finish thread destroys it in submission order (H1).
- `src/dxmt/dxmt_command_queue.hpp`: `WaitCPUFenceBounded(seq, timeout_ns)`: immediate check, 64 reloads, then yield until the deadline (H2).
- `src/dxmt/dxmt_shader_cache.cpp`, `.hpp`: `ShaderCacheDirectory()` (absolute `DXMT_SHADER_CACHE_PATH` with trailing `/`, else `dxmt/<exe>/`) (H3).
- `src/winemetal/unix/cache.c`: on iOS relative cache paths resolve under `NSCachesDirectory`; directories are created; on failure Metal's cache path is left alone; `MTLSetShaderCachePath` looked up with `dlsym` (H3).
- `src/dxmt/dxmt_shader_cache.cpp`: `ShaderCache::getInstance` uses one fixed slot per Metal version in static storage (H4).
- `src/dxmt/dxmt_command_queue.cpp`, `.hpp`: `DXMT_GPU_PERF=1` adds up GPU busy time from each retired command buffer's GPU start/end (control call op 3) and logs one line per 64 frames (H5).
- `src/winemetal/winemetal_thunks.c`, `winemetal_thunks.h`, `winemetal.h`, `unix/winemetal_unix.c`: control call `{ uint32 op; uint64 ptr; uint64 ret; }` at unix-call slot 127 (op 3 GPU times, op 7 available memory, others `STATUS_NOT_IMPLEMENTED`); slot 128 for E2.4; a `_Static_assert` keeps the two halves' counts equal (H6).
- `src/dxmt/dxmt_texture.hpp`, `.cpp`: view descriptors carry a swizzle; view lookup compares it; `checkViewUseSwizzle` and `checkViewUseMipRange` (H7).

## Part I: tests

These live next to the tree, not in it (paths relative to `out/05-dxmt-ios/`).

- `tests/test_bc_decode.cpp` (new): hand-computed blocks for BC1 (four-colour, three-colour, transparent black), BC2, BC3, BC4/BC5 unsigned and signed (−128 → −127), BC7 modes 0–7 and the reserved mode, whole subresources (sizes not a multiple of 4, pitches, invalid arguments), BC6H grey fill (I).
- `tests/test_mip_clamp.cpp` (new): bias choice, requery rule, pressure rule, `RequestedBias` with a scripted headroom query, view-range translation, min LOD over lost levels, 2×2 box filter (I).
- `tests/test_common.hpp` (new): `CHECK`/`CHECK_EQ`/`CHECK_RGBA` and the summary line (I).
- `tests/run-host-tests.sh` (new): builds and runs both programs with any C++20 compiler (I).
- `tests/TESTS.md` (new): how to run them, the CI check of `build-ios.sh` with Xcode 26, the device checks (I).

## Clarifications (round 1)

The answers in the spec's "Clarifications (round 1)" confirm the other choices; these two
changed the code:

- `src/dxmt/dxmt_bc_decode.cpp`: signed BC4/BC5 apply −128 → −127 to both endpoints first, then compare `e0 > e1` on the results (`e0 = −127, e1 = −128` is the six-value mode) (clarification 2).
- `src/dxmt/dxmt_bc_decode.hpp`, `.cpp`: `DecodeVolume` decodes a 3D subresource slice by slice (each depth slice an independent 2D BC image at the depth pitch) (clarification 3).
- `src/dxmt/dxmt_bc_emulation.hpp`, `.cpp`: `BCDecodeVolumeToPhysical` (BC6H: grey for every slice) (clarification 3).
- `src/dxmt/dxmt_resource_initializer.cpp`, `.hpp`: initial data of 3D BC textures decoded and uploaded with its depth; zero-fill already covered 3D (C1, C2, clarification 3).
- `src/d3d11/d3d11_context_impl.cpp`: `UploadEmulatedBC` takes depth and depth pitch: `UpdateSubresource`, dynamic-texture `Unmap` and staging→BC copies into 3D BC textures are decoded slice by slice and uploaded with their depth (`UploadPhysicalTexels` gained a depth); the lost-level refill stays 2D-only (C3, C4, clarification 3).
- `src/d3d11/d3d11_texture.cpp`: dynamic 3D BC textures' initial data decoded slice by slice (clarification 3).
- `tests/test_bc_decode.cpp`: signed blocks with `e0 = −127, e1 = −128` (BC4 and BC5) expect the six-value mode; `TestVolume` covers volumes (tight and padded pitches, sizes not a multiple of 4, a too-small depth pitch, one slice) (clarifications 2, 3).

## Clarifications (round 2) and the first CI build

- `src/util/util_bit.hpp`: ARM64EC (`__arm64ec__` / `_M_ARM64EC`) is tested before `__x86_64__` / `_M_X64` and selects `DXMT_ARCH_ARM64`, so no x86 intrinsic header is included and the bit helpers use the compiler builtins; the BMI / LZCNT / MSVC-intrinsic branches are taken only on x86. `src/d3d11/d3d11_multithread.cpp` follows (`yield` instead of `_mm_pause`) (clarification 9; CI: `__builtin_ia32_*` undeclared in `util_string.cpp` for ARM64EC).
- `src/winemetal/wineunixlib.h`: ARM64EC uses the ARM64 `__wine_jmp_buf`, and `WINE_UNIX_CALL` goes through `__wine_unix_call_arm64ec` (provided by winecrt0 for ARM64EC builds) instead of the x64 dispatcher, as Wine 11.4's `include/wine/unixlib.h` does (clarification 9).
- `tests/dx11/stb_image.h` (test programs): ARM64EC selects neither x86 target, so no SSE2 (clarification 9).
- `src/d3d11/d3d11_swap_effect.hpp` (new), `src/d3d11/d3d11_swapchain.cpp`: `DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL` with 2–16 buffers (Unity's usual D3D11 swap chain) is presented as asked and no longer logs "unsupported swap effect": like every swap chain here it has one back buffer that keeps its contents after Present and is copied into the layer's next drawable, which gives a flip-sequential game the newest frame in the buffer it draws into next. Blit-model `SEQUENTIAL` or an unknown effect with several buffers still logs. `tests/test_swap_effect.cpp` (new) (device log, 2026-10-20).
- `build-ios.sh`: the helper shaders are found under `<dxmt-tree>/src/airconv/shaders` (the loop globbed relative to the current directory, so `metal` got the unexpanded pattern and no `air_*.h` was generated); a run that finds none fails with a message (CI round 1).

## Dynamic-buffer discard (performance requirement, 2026-10-22)

A game that runs OpenGL ES on D3D11 maps one 64 KB dynamic constant buffer with
`WRITE_DISCARD` about 33,000 times a frame. Before this change a 64 KB buffer got one slice per
allocation, so nearly every discard created a new Metal buffer (the spec 05 D4 pool keeps 64
finished allocations, far fewer than a frame needs), and three frames in flight held about
6 GiB.

- `src/dxmt/dxmt_dynamic_policy.hpp` (new): the rules: slice size (256-byte aligned), the slice
  count of new allocations (doubles when a new allocation is needed in the same command chunk
  as the previous one, up to 1 MiB per allocation; a buffer discarded now and then keeps one),
  and `DynamicDiscardTracker` (discarded bytes per chunk, implicit commit, in-flight cap).
- `src/dxmt/dxmt_buffer.{hpp,cpp}`: `Buffer::allocate(flags, min_slices)` and the
  `BufferAllocation` constructor make an allocation of that many slices; `suballocationCount()`;
  `std::max<uint64_t>` so the file also builds on LP64 hosts.
- `src/dxmt/dxmt_dynamic.{hpp,cpp}`: `DynamicBuffer::allocate(coherent, minted, current_seq)`
  grows the slices of new allocations from the caller's chunk (the Direct3D 9 frontend passes no
  chunk and keeps the old sizes). Reusing finished allocations and the D4 trim are unchanged.
- `src/d3d11/d3d11_context_imm.cpp`: before each `WRITE_DISCARD` of a dynamic buffer or texture,
  the context commits its chunk once 32 MiB of discarded slices are in it
  (`d3d11.dynamicCommitMB`, 0 off), and waits for the GPU only while more than 96 MiB of them are
  in committed chunks it hasn't finished (`d3d11.dynamicInflightMB`, 0 off). `Commit()` hands
  the chunk's discarded bytes to the tracker. A discard is otherwise a move to the next slice,
  or a rename to a finished allocation: no Metal buffer, no wait, no zero fill, no copy. Draws
  already recorded keep their slice: the chunk keeps the allocation alive, and an allocation
  is reused only after the chunk that last used it has finished.
- `Map(WRITE_NO_OVERWRITE)` on dynamic constant buffers (current slice, no sync),
  `*SetConstantBuffers1` with first-constant offsets (an offset-only rebind is one small
  command) and `MapNoOverwriteOnDynamicConstantBuffer = TRUE` were already in the tree; unchanged.
- Log: a context's first implicit commit writes `[dxmt] implicit commit #1 after 32 MiB of
  discarded slices`, and every 600th after it `[dxmt] implicit commits so far: N (each after 32 MiB
  of discarded slices)`, so the device log shows the pass breaks a frame costs (answer to the
  QUESTIONS entry; the 32 / 96 MiB defaults and the D4 limit stand).
- `tests/bench_dynamic_discard.cpp` (new), `tests/run-host-tests.sh`: DXMT's own buffer code
  against fake Metal and a GPU model, 33,000 cycles a frame; `--check` in the host tests (no
  slice reused early, memory bounded, no new Metal buffers after warm-up).

# Spec 06 changes (32-bit programs through WoW64)

Tagged "spec 06"; the letter is the Part of `specs/06-dxmt-wow64.md`.

## Part A: i386 build

- `build-win-i686.txt` (new): meson cross file for `i686-w64-mingw32` (llvm-mingw, `@LLVM_MINGW_DIR@` placeholder); `DXMT_IOS` comes from the Windows host as before (spec 06 A).
- `src/util/util_bitness.hpp` (new): `dxmt::kI386Build`, true in an i386 build (spec 06 A).
- `src/dxmt/dxmt_ring_bump_allocator.hpp`: `DXMT_RING_OVERSIZE_REUSE` defaults to `kI386Build` (spec 06 A).
- `src/dxmt/dxmt_resource_initializer.cpp`: `DXMT_ZERO_BUFFER_POW2` defaults to `kI386Build` (spec 06 A).
- `src/dxmt/dxmt_context.cpp`: `DXMT_QUERY_SETTLE_EMPTY` defaults to `kI386Build` (spec 06 A).
- `src/util/wsi_monitor_ios.cpp`: `DXMT_WSI_MODE_TABLE` and `DXMT_WSI_MONITOR_IDENTITY` default to `kI386Build` (spec 06 A).
- `src/d3d11/d3d11_texture_device.cpp`: `d3d11.mipClampAuto` defaults to `kI386Build` (spec 06 A).
- `src/winemetal/unix/ios_support.m`, `.h`: `dxmt_ios_switch_for_caller` (unset/empty: on for a 32-bit caller, i.e. `ios_wow_base() != 0`; `0`: off; else on), used for `DXMT_PRESENT_PIXELS`, decided per call (spec 06 A).

## Part B: memory the 32-bit program can address

- `src/dxmt/dxmt_buffer.cpp`: `Buffer::allocate` sets `CpuPlaced` for every allocation that isn't `CpuInvisible` in the i386 build (spec 06 B1).
- `src/dxmt/dxmt_ring_bump_allocator.hpp`: `StagingBufferBlockAllocator` always uses DLL-allocated memory in the i386 build (the command queue's and command list's staging rings and the resource initializer's heap, created with `placed_buffer = false`) (spec 06 B2).
- `src/dxmt/dxmt_ring_bump_allocator.hpp`, `src/dxmt/dxmt_resource_initializer.hpp`: 8 MB staging ring blocks (and resource-initializer upload heap blocks) in the i386 build instead of 32 MB (spec 06 B3).
- `src/util/util_placed_memory.hpp` (new), `dxmt_buffer.cpp`, `dxmt_texture.cpp`, `dxmt_occlusion_query.hpp`, `dxmt_context.cpp`, `dxmt_ring_bump_allocator.hpp`: under `DXMT_IOS` placed memory is 16 KB-aligned and its length rounded up to 16 KB (iOS host pages; `newBufferWithBytesNoCopy:` wants page-aligned memory); staging blocks use `wsi::aligned_malloc`/`aligned_free` there (spec 06 B, confirmed by spec 06 clarification 1).
- `src/winemetal/unix/ios_wow64_thunks.inc`: a 32-bit `newBuffer` of CPU-visible storage (Shared or Managed) without its own memory returns a null buffer and logs once; a host pointer is never truncated (spec 06 B4).

## Part C: the 32-bit unix-call table

- `src/winemetal/unix/ios_wow64.h` (new): `ios_wow_base()` declaration; C1/C2 pointer helpers (a value with its high half set is refused); the C4 chain walker (converts head, links and payloads to host in one walk and back in one walk; fails on a link that is already a host address, i.e. a cycle, on more than 2^20 records, or on a bad payload pointer, and then restores what it converted); payload converters for render and compute records (spec 06 C1, C2, C4).
- `src/winemetal/unix/ios_wow64_thunks.inc` (new): the `wow_*` handlers; `winemetal_unix.c` includes it before the tables (spec 06 C).
- `src/winemetal/unix/winemetal_unix.c`: `UInt32ToPtr` adds `ios_wow_base()` on iOS (NULL stays NULL), so upstream's `thunk32_*` handlers follow C1 unchanged; they are compiled on iOS again; the 32-bit converter of shader-compilation arguments handles spec 05 F3's `SM50_SHADER_PSO_VERTEX_SHADER_BYTECODE` (spec 06 C).
- `src/winemetal/unix/winemetal_unix.c`: `dxmt_winemetal_unix_call_wow64_funcs` filled per entry as below (spec 06 C).

Decision per entry of the 32-bit table (slot: entry: rule). "64-bit" = the 64-bit handler, no embedded pointers (C6).

| Slot | Entry | Decision |
|---|---|---|
| 0, 1 | `NSObject_retain`, `NSObject_release` | handle by value (C3); counted for Part E (`wow_NSObject_retain/release`) |
| 2–7 | `NSArray_object`, `NSArray_count`, `MTLCopyAllDevices`, `recommendedMaxWorkingSetSize`, `currentAllocatedSize`, `MTLDevice_name` | 64-bit |
| 8 | `NSString_getCString` | C1: `buffer_ptr` (raw uint64) |
| 9–17 | `newCommandQueue`, `NSAutoreleasePool_alloc_init`, `commandBuffer`, `commit`, `waitUntilCompleted`, `status`, `newSharedEvent`, `signaledValue`, `encodeSignalEvent` | 64-bit |
| 18 | `MTLDevice_newBuffer` | C1: `info`; `info->memory` in (C1) and out (C2); B4 refusal; Part E record |
| 19, 20 | `newSamplerState`, `newDepthStencilState` | C1: `info` (outputs inline) |
| 21, 22 | `MTLDevice_newTexture`, `MTLBuffer_newTexture` | C1: `info`; Part E record |
| 23 | `MTLTexture_newTextureView` | no pointers; Part E record (a view keeps its texture alive) |
| 24, 25 | `minimumLinearTextureAlignmentForPixelFormat`, `newLibrary` (data is a handle) | 64-bit |
| 26 | `MTLLibrary_newFunction` | C5: name (`arg`, raw uint64) |
| 27, 28 | `lengthOfBytesUsingEncoding`, `NSObject_description` | 64-bit |
| 29 | `newComputePipelineState` | C1: `info` and nested `binary_archives_for_lookup` (array of handles), converted in a local copy |
| 30, 31 | `blitCommandEncoder`, `computeCommandEncoder` | 64-bit |
| 32 | `renderCommandEncoder` | C1: `arg` (raw uint64 → `WMTRenderPassInfo`, no nested pointers) |
| 33 | `endEncoding` | 64-bit |
| 34, 35 | `newRenderPipelineState`, `newMeshRenderPipelineState` | C1: `info` and nested `binary_archives_for_lookup` |
| 36 | blit `encodeCommands` | C4: links only (no blit record carries a pointer) |
| 37 | compute `encodeCommands` | C4: links; `SetBytes.bytes` |
| 38 | render `encodeCommands` | C4: links; `SetFragmentBytes.bytes`, `SetViewports.viewports`, `SetScissorRects.scissor_rects` (the only pointer payloads in `winemetal.h`) |
| 39–44 | texture queries | 64-bit |
| 45 | `MTLTexture_replaceRegion` | C1: `data` |
| 46–53 | `didModifyRange`, `presentDrawable`, `presentDrawableAfterMinimumDuration`, `supportsFamily`, `supportsBCTextureCompression`, `supportsTextureSampleCount`, `hasUnifiedMemory`, `sharedCaptureManager` | 64-bit |
| 54 | `startCapture` | C1: `info`, nested `output_url` (C5) |
| 55 | `stopCapture` | 64-bit |
| 56, 57 | `newTemporalScaler`, `newSpatialScaler` | C1: `info` |
| 58 | `encodeTemporalScale` | C1: `props` |
| 59 | `encodeSpatialScale` | 64-bit |
| 60, 61 | `NSString_string`, `NSString_alloc_init` | C5: `buffer_ptr` |
| 62–69 | HUD entries, `MetalDrawable_texture`, `MetalLayer_nextDrawable`, `supportsFX*` | 64-bit |
| 70, 71 | `MetalLayer_setProps`, `MetalLayer_getProps` | C1: `arg` (getProps also applies A's per-call `DXMT_PRESENT_PIXELS`) |
| 72, 73 | `CreateMetalViewFromHWND` (hwnd is a handle), `ReleaseMetalView` | 64-bit |
| 74, 76, 77, 79, 81, 82, 84, 85, 88 | SM50 initialize, compile, get bitcode, error message, geometry/tessellation compiles, arguments info | upstream `thunk32_*`, embedded pointers through `UInt32ToPtr` (C1). The compiled bitcode's `Data` is written as an opaque 64-bit value (C3): the Windows half only passes it back to `DispatchData_alloc_init` |
| 75, 78, 80 | `SM50Destroy`, `SM50DestroyBitcode`, `SM50FreeError` | 64-bit (opaque 64-bit handles) |
| 83 | (NULL in the 64-bit table) | `STATUS_NOT_IMPLEMENTED` |
| 86, 87 | `setLabel`, `setShouldMaximizeConcurrentCompilation` | 64-bit |
| 89, 90 | `MTLCommandBuffer_error`, `logs` | 64-bit |
| 91 | `MTLLogContainer_enumerate` | C1: `buffer` (filled with 8-byte handles) |
| 92–95 | color space entries, display ids | 64-bit |
| 96, 97 | `WMTGetDisplayDescription`, `MetalLayer_getEDRValue` | C1: `arg` |
| 98 | `newFunctionWithConstants` | C1/C5: `name`, `constants`, and each constant's `data`, converted in a local copy of the array (at most 4096 constants) |
| 99–101 | `WMTQueryDisplaySetting`, `WMTUpdateDisplaySetting`, `WMTQueryDisplaySettingForLayer` | C1: `hdr_metadata` |
| 102–106 | `encodeWaitForEvent`, `signalValue`, `setWin32EventAtValue` (NT event handle, not a pointer), `newFence`, `newEvent` | 64-bit |
| 107 | `MTLBuffer_updateContents` | C1: `data` |
| 108–111 | shared event listener entries, `WMTGetOSVersion` (outputs inline) | 64-bit |
| 112, 113 | `newBinaryArchive`, `MTLBinaryArchive_serialize` | C5: `url` |
| 114 | `DispatchData_alloc_init` | below 4 GB: guest address (C1); above: a host value the unix half handed out (SM50 bitcode `Data`), passed through (C3); confirmed by spec 06 clarification 2 |
| 115–118 | `CacheReader_alloc_init`, `CacheReader_get`, `CacheWriter_alloc_init`, `CacheWriter_set` | C5/C1: `path`, `key` |
| 119 | `WMTSetMetalShaderCachePath` | C5: `path` |
| 120 | `MTLDevice_newSharedTexture` | C1: `info`; Part E record |
| 121, 122 | `WMTBootstrapRegister`, `WMTBootstrapLookUp` | 64-bit (name inline; fail on iOS anyway) |
| 123–126 | `createMachPort`, `newSharedEventWithMachPort`, `registryID`, `waitUntilSignaledValue` | 64-bit |
| 127 | control call (spec 05 H6) | C7: op 7 only; other ops `STATUS_NOT_IMPLEMENTED` |
| 128 | layer display sync (spec 05 E2.4) | 64-bit (handle and flag) |

Upstream a3a6e8f has no residency-set or placement-heap entries, so C7 has nothing else to refuse. A pointer argument with its high half set makes a `wow_*` handler refuse the call (`STATUS_INVALID_PARAMETER`, logged rarely).

## Part D: waiting on another thread

- `src/util/util_atomic_wait.hpp`, `.cpp` (new): `dxmt::atomic_wait` / `atomic_notify_one` / `atomic_notify_all`; ntdll `RtlWaitOnAddress` / `RtlWakeAddressSingle` / `RtlWakeAddressAll` (declared locally) on a Windows host, `std::atomic` members elsewhere; the backend is logged once per module (spec 06 D).
- `src/util/meson.build`: builds `util_atomic_wait.cpp`; `util_dep` links `ntdll` for Windows hosts (spec 06 D).
- `src/dxmt/dxmt_command_queue.cpp` (`ready_for_encode`, `ready_for_commit`, `chunk_ongoing`), `src/util/util_cpu_fence.hpp` (`CpuFence`, used by the command queue, the swapchain and the presenter), `src/d3d11/d3d11_pipeline.cpp`, `d3d11_pipeline_ts.cpp`, `d3d11_pipeline_gs.cpp` (pipeline readiness): every `std::atomic` wait/notify call goes through them (spec 06 D). `dxmt_tasks.hpp` uses a condition variable, not an atomic, and is unchanged.

## Part E: freeing a finished 32-bit program's Metal objects

- `src/winemetal/unix/ios_wow64_objects.h` (new): fixed-size open-addressing record table `{object, base, count}` (linear probing, backward-shift removal), create / retain / release / take-window / give-back (spec 06 E).
- `src/winemetal/unix/ios_support.m`, `.h`: one lock around a 65536-record table allocated on the first 32-bit creation; `dxmt_ios_wow_created/retain/release`; exported `winemetal_release_window(unsigned long base)` (releases the program's remaining references while more than the table's own is left, then the table's own; logs the count per teardown); a full table is logged once and stops recording (spec 06 E).
- When a 32-bit program releases its last counted reference to a recorded object, the record is dropped and the table's own reference released too, so objects the program frees normally don't wait for the teardown (spec 06 E; confirmed by spec 06 clarification 3).

## Part F: tests

- `tests/test_wow64.cpp` (new): pointer helpers (NULL, normal, last byte, outside the window, high half set), chain walker (empty, one, many, compute payloads, cycle, self-cycle, too long, bad links; refused chains left byte-for-byte unchanged), record table on fake objects (create/retain/release, teardown with references left, teardown after the program released everything, uncounted release, address reused, full table and removals) (spec 06 F).
- `tests/test_atomic_wait.cpp` (new): generic backend: a waiter wakes on notify, notify_all wakes every waiter, a changed value returns at once, backend logged once (spec 06 F).
- `tests/run-host-tests.sh`, `tests/TESTS.md`: build and run them; on-device checks for 32-bit programs (spec 06 F).

# Spec 07 changes (Direct3D 9)

Tagged "spec 07"; the letter and number are the Part and item of `specs/07-dxmt-d3d9.md`. Everything
taken from dxmt-d3d9 tag v0.4-d3d9 (commit e8dd4c6, `refs/dxmt-d3d9-v0.4-e8dd4c6/`) is
LGPL-2.1-or-later and marked in the file: whole files carry the tag's notice at the top, blocks
inside MIT files sit between "LGPL-2.1-or-later block" and "end of LGPL block" comments.
`LICENSE-NOTES.md` at the top of the tree says which parts are which.

## Part A: import

- `src/d3d9/` (new, all 71 files, LGPL): the tag's frontend with its `meson.build`, `d3d9.def` and `version.rc` (spec 07 A1).
- `src/airconv/dxso_header.hpp`, `dxso_decoder.hpp`, `dxso_compile.{hpp,cpp}`, `ffp_compile.{hpp,cpp}` (new, LGPL): the DXSO and fixed-function translator (spec 07 A1).
- `COPYING.LIB`, `LICENSE.LGPL` (the tag's `LICENSE`), `LICENSE-NOTES.md` (new) (spec 07 A1).
- `src/airconv/airconv_public.h`: LGPL blocks with `dxso_shader_t`/`dxso_bitcode_t` and the DXSO API (argument structs, enums, the five `DXSO*` entry points). The tag's SM50 changes to the header are not taken (spec 07 A2).
- `src/airconv/air_signature.{hpp,cpp}`: point-coordinate input (`air.point_coord`) and point-size output (`air.point_size`). `src/airconv/nt/air_builder.{hpp,cpp}`: `pow`. `air_operations.{hpp,cpp}`: the UDEC3 vertex fetch and the DEC3N snorm fetch the DXSO path needs. `src/airconv/meson.build`: compiles `dxso_compile.cpp` and `ffp_compile.cpp` (spec 07 A2).
- `src/dxmt/dxmt_command.metal`, `dxmt_command.{hpp,cpp}`: shader resolve (colour average, depth) and stretch-blit shaders, `ResolveTextureMode`, `ResolveTextureContext`, `StretchBlitContext` (spec 07 A2).
- `src/dxmt/dxmt_context.{hpp,cpp}`: `resolveDepthTexture`, `stretchBlit`, `copyTexture`, `optimizeTextureForGPUAccess`, `signalEventByHandle`, the `BufferAllocation` overload of `access`, the stretch-blit encoder data and the extended resolve encoder data with their encode bodies; `resolveTexture` takes the pipeline and region as defaulted arguments so D3D11's calls don't change (spec 07 A2).
- `src/dxmt/dxmt_command_queue.{hpp,cpp}`: GPU completion targets (called by the finish thread with the command buffer's status), `HasDeviceError`/`MarkDeviceError`, `FrameLatencySignaled`/`WaitFrameLatency` (spec 07 A2).
- `src/dxmt/dxmt_ring_bump_allocator.hpp`: `preallocate`, `seal_latest`, the single-writer option (checked only with `DXMT_DEBUG`) (spec 07 A2).
- `src/dxmt/dxmt_format.hpp`: `Recall_sRGB_ForRenderTarget`. `dxmt_texture.hpp`: `TextureAllocation::buffer`, `Texture::miplevelCount`, `Texture::depth(view)`, `Texture::fullView` (a constant 0: the first view is the whole texture in this tree). `dxmt_dynamic.{hpp,cpp}`: `DynamicBuffer::allocate` reports whether it minted a fresh allocation. `src/util/wsi_window*`: `foregroundWindow` (on iOS always null, so the frontend never sees a focus change) (spec 07 A2).
- `src/winemetal/winemetal.h`, `Metal.hpp`, `unix/winemetal_unix.c`: render commands for blend factor, fragment sampler, vertex texture and vertex sampler, and the blit command "optimize contents for GPU access" (spec 07 A2).
- `src/winemetal/airconv_thunks.{h,c}`, `winemetal_thunks.h`, `unix/winemetal_unix.c`, `unix/dxso_args32.h` (new, LGPL): the five DXSO entry points at slots 129–133 of both unix-call tables. The 32-bit entries convert the i386 argument chain into 64-bit copies (the links and embedded pointers get the window base added), call, and free the copies; the 32-bit chain is never written. A copy instead of converting in place because the 32-bit and 64-bit argument structs have different sizes (spec 07 A3).
- `src/dxmt/dxmt_buffer.hpp`: `BufferAllocation::length` and `currentSuballocation` (LGPL block; `length` wasn't in this tree although spec 07 A2 lists it as existing) (spec 07 A4).
- `src/d3d9/d3d9_clear_quad.cpp`, `d3d9_device.cpp`: the tag's `access<PipelineStage::…>(…, ResourceAccess::…)` calls become this tree's `access(…, DXMT_ENCODER_RESOURCE_ACESS_…)`, with `access<true>` for the vertex stage (spec 07 A4).
- `src/d3d9/d3d9_device.cpp`: the two whole-texture slice walks (zero-initialise, generate mipmaps) use `arrayLength(fullView)`: in the tag `arrayLength()` counts a cube's six faces, in this tree only the full view's length does (spec 07 A4).
- `src/d3d9/d3d9_interface.cpp`: the Metal cache path is built on `ShaderCacheDirectory()` (spec 05 H3) instead of the tag's `GetDXMTShaderCacheDirectory()` (spec 07 A4).
- `src/meson.build`: builds `src/d3d9` for the Windows half (spec 07 A4).

## Part B: what ships

- `d3d9.dll`: built by `src/d3d9/meson.build` for every Windows-half target (i386 and ARM64EC cross files), post-processed with `winebuild --builtin` when `-Dwine_builtin_dll=true`, like the other DLLs. Its x87 control-word setup is compiled only for i386 / x86-64 (not ARM64EC), as in the tag (spec 07 B).
- `build-ios.sh`: unchanged; it takes the airconv library sources from `src/airconv/meson.build`, which now lists `dxso_compile.cpp` and `ffp_compile.cpp` (the comment line in the list doesn't end in `.cpp` and is skipped). Neither uses exceptions or RTTI, so the existing `-fno-exceptions -fno-rtti` flags hold (spec 07 B).
- `src/d3d9/d3d9_device.cpp` (pipeline compile task), `d3d9_shader.cpp` (shader compile task): readiness waits and wake-ups go through spec 06 D's `dxmt::atomic_wait` / `atomic_notify_all` (spec 07 B).

## Part C: iOS behaviour of the frontend

New MIT files: `src/d3d9/d3d9_ios_policy.hpp` (the decision rules, free of Windows and Metal types so the host tests build them), `src/d3d9/d3d9_ios.{hpp,cpp}` (each switch read once; the module-wide small-mirror budget; the per-device log line). `src/d3d9/meson.build` builds `d3d9_ios.cpp`. A switch that is unset or empty takes its default, `0` is off, anything else on.

- C1 `d3d9_device.{hpp,cpp}`: the constructor asks `BCEmulationActive` once and keeps the answer. `stageTextureUpload` (every upload path: Unlock of managed / default / dynamic surfaces and volumes, UpdateTexture, UpdateSurface, the BC ColorFill) decodes BC blocks into the staging memory with spec 05's decoder (`BCDecodeToPhysical`, `BCDecodeVolumeToPhysical` for 3D) at the physical format's tight pitch; origin and size stay in texels. `readbackSurfaceMirror` and the batched readback skip emulated BC textures. `d3d9_texture.cpp`, `d3d9_cube_texture.cpp`: `dropMirror` never evicts an emulated BC texture's mirror (spec 07 C1).
- C2 `stageTextureUpload`: when the area's row is shorter than the source pitch, the last row of the last slice is copied with the row's length (blocks × block bytes, or width × texel size) (spec 07 C2).
- C3 `stageTextureUpload` counts staged bytes; every commit (`commitCurrentChunkTimed`) resets the count. `commitForUploadPressure` runs at the start of `UpdateTexture` / `UpdateSurface` (after argument checks), at the end of surface `UnlockRect` and volume `UnlockBox`, and after the pre-draw managed sweep in `QueueBatchedDraw`. When the count reaches `DXMT_D9_UPLOAD_COMMIT_MB` (default 64, 0 never) it waits for the previous pressure commit's tail signal, flushes, signals and commits, remembers the new signal, and trims the rings without the usual throttle (`refreshSignaledAndTrimRings(force)`) (spec 07 C3).
- C4 `FlushDrawBatch`: adds the handed-over pending vectors' used size (elements × element size) to a count reset by every commit; at 16 MiB it signals and commits. Before the hand-off, a vector holding more than 64 KiB of capacity and less than half full, or more than 4 KiB and less than a quarter full, is copied into a tight one (`ShrinkPendingVector`). On reset each vector reserves `min(high-water count, 64 KiB / element size)` (`PendingReserveCount`). `MADEIRA_D3D9_BATCH_BUDGET=0` turns off the threshold and the reserve cap (spec 07 C4, clarification 1).
- C5 `EndScene`: keeps the recorded work when fewer than 256 pending operations are recorded (`DXMT_D9_SCENE_BATCH`); an empty batch (a scene of clears only) takes the normal drain (spec 07 C5).
- C6 `readbackSurfaceMirrors` (new): the GPU-to-buffer copies of several surfaces in one blit pass and one submission, one wait per batch of at most 16 MiB; if the staging for a batch can't be had, that batch goes one surface at a time. Used where several mirrors are read back at once: `restoreMirrorForSource` of 2D and cube textures (`DXMT_D9_BATCH_READBACK=0` or `MADEIRA_D3D9_MIRROR_BATCH=0` disables). `GetRenderTargetData` reads one surface per call and must return with its data, so it has nothing to batch (spec 07 C6; see QUESTIONS.md).
- C7 `d3d9_query.{hpp,cpp}`: `GetDataSize` takes no lock (the type is fixed at creation); `TestCooperativeLevel` takes none (it reads create-time fields and atomics). The forward-progress submit happens at most once per `Issue(END)`. A poll of an `EVENT` / `OCCLUSION` query that finds the result not ready reads the clock and counts toward a run (not-ready polls whose starts are at most 250 µs apart, the earlier poll's park included; a longer gap starts a new run at 1); ready polls never read the clock. The first two not-ready polls of a run return at once; from the third (with `DXMT_D9_QUERY_ADAPTIVE=0`: once more than 2 not-ready polls since `Issue`) the device lock is released and the thread waits with `WaitCPUFenceBounded(seq, 100 µs)` on the query's sequence number; if the wait succeeds, the poll takes the lock again and returns the result, otherwise it returns `S_FALSE` (spec 07 C7, clarification 2).
- C8 `enterFullscreenWindow` / `leaveFullscreenWindow`: with `MADEIRA_SCREEN_W` set (and `DXMT_D9_VIRTUAL_MODE` not 0), going fullscreen reads the current mode (`EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS)`), remembers it the first time, and sets the back buffer size with `ChangeDisplaySettingsW(…, CDS_FULLSCREEN)` before the window is placed; leaving fullscreen (including device teardown) restores the remembered mode the same way (spec 07 C8).
- C9 `d3d9_interface.cpp`: `GetAdapterModeCount`, `EnumAdapterModes` and their Ex forms list every mode for `X8R8G8B8`, `R5G6B5` and `X1R5G5B5`, reporting the requested format (spec 07 C9).
- C10 `d3d9_swapchain.{hpp,cpp}`: after `synchronizeLayerProperties` (when the layer holds every property asked for) Present reads the layer's drawable size; if it differs from the size last handed to the presenter, the presenter takes the layer's size, so the back buffer is scaled into the drawable that exists (spec 07 C10).
- C11 pipeline build: with blending off, an attachment's operations become add and its factors one / zero before the key is hashed (`DXMT_D9_CANONICAL_BLEND`) (spec 07 C11).
- C12 `ResolveClusterState`, `EmitCommonRenderSetup_d9`: for a pixel shader, stages outside its `sampler_usage_mask` get no sampler lookup, view, reference or bind (`DXMT_D9_BINDING_PRUNE`; a texture an earlier draw left on such a stage is cleared once, the mask travels with the cluster cache). The variant key gets `UNKNOWN` for those stages (`DXMT_D9_SHADER_PRUNE`) (spec 07 C12).
- C13 `getOrCreateSampler`: maximum anisotropy capped at `DXMT_D9_ANISO_LIMIT` (1, 2, 4 or 8) before the cache key (spec 07 C13).
- C14 `d3d9_surface.{hpp,cpp}`, `d3d9_texture.hpp`: a deferred `NO_DIRTY_UPDATE` write-unlock of a managed level marks the level for the pre-draw upload (`markLevelNeedsUpload`) and moves the managed-upload epoch (spec 07 C14).
- C15 `d3d9_texture.{hpp,cpp}`, `d3d9_cube_texture.{hpp,cpp}`: instead of evicting a managed mirror of 256 KB or less, keep it while the module-wide total stays within 32 MB (`DXMT_D9_SMALL_MIRROR_CACHE`); the bytes go back to the budget when the texture is destroyed (spec 07 C15).
- C16 `LogD9IosDevice`: one line per device with the BC answer and every switch. The other new log lines are failures (BC decode, mode change) (spec 07 C16).

Choices the spec leaves open are listed in `QUESTIONS.md`, "Spec 07".

## Part D: tests

- `tests/test_dxso_args32.cpp` (new): the i386 DXSO argument chain conversion in a fake window (struct sizes, pointers, every argument type, skipped types, cycles, the program's chain left unwritten) (spec 07 D).
- `tests/test_d3d9_policy.cpp` (new): the Part C rules (switches, C3 threshold, C4 budget and reserve, C5, C6 batches, C7 poll runs and the once-per-END flush, C9 mode lists, C13, C15) (spec 07 D).
- `tests/run-host-tests.sh`, `tests/TESTS.md`: build and run them; on-device checks 8–11 for Direct3D 9 (spec 07 D).
