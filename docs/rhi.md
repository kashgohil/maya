# Graphics device, render passes, and resource retirement

[Issue #996](https://work.rezee.app/kash/issues/996) replaces the single-window implicit-pass API. The new [GraphicsDevice](../include/maya/rhi/graphics_device.hpp) has:

- descriptor-created buffers, textures, samplers, and pipelines;
- explicit render passes with declared load/store actions;
- a presentable surface that is acquired separately from offscreen rendering;
- validated per-session handles;
- deferred GPU retirement.

`MayaRHI` (`Maya::RHI`) is the CPU-side library: handle tables, validation, retirement, and a [NullGraphicsDevice](../include/maya/rhi/null_device.hpp). The [Metal backend](../src/maya/rhi/metal/metal_device.mm) remains in `MayaRuntime`. `MayaAssets` links `MayaRHI` for mesh uploads.

## Frame structure

```cpp
auto& device = *engine_device;                 // Engine calls begin_frame/end_frame around on_render
const auto surface = device.acquire_surface(); // optional: only for presentation
if (surface) {
    auto pass = maya::RenderPassDesc{};
    pass.colors.push_back({surface.target.texture, maya::LoadAction::clear, maya::StoreAction::store,
                           {0.1, 0.1, 0.1, 1.0}});
    pass.depth = maya::DepthAttachment{depth_target, maya::LoadAction::clear, maya::StoreAction::dont_care, 1.0};
    if (auto error = device.begin_render_pass(pass)) { /* report error.message */ }
    device.set_pipeline(pipeline);            // must match the pass formats
    device.set_uniform_buffer(1, uniforms, 0); // offset multiple of limits().uniform_offset_alignment
    device.set_vertex_buffer(0, vertices);
    device.draw_indexed(indices, maya::IndexType::uint32, index_count);
    device.end_render_pass();
}
```

A frame runs `begin_frame` → any number of render passes → `end_frame`. `end_frame` presents an acquired surface and submits. Offscreen passes target any texture created with `render_target` usage and never touch the drawable. The same frame can render offscreen targets and present, only render offscreen, or only present. `Engine::tick` calls `begin_frame`/`end_frame` around `Application::on_render`, and treats a failure from either as a frame failure. GPU execution errors reported by completed frames are logged through `take_gpu_errors()`.

Handles and descriptors:

| Kind | Descriptor | Notes |
| --- | --- | --- |
| `BufferHandle` | size, `vertex`/`index`/`uniform` usage flags, label | CPU-writable shared memory. `write_buffer` is immediate and not synchronized with frames in flight; per-frame data belongs in [upload memory](#frame-pacing-and-upload-memory). |
| `TextureHandle` | width, height, format, `sampled`/`render_target`/`readback` usage, label | 2D, one mip level, GPU-private. Optional tightly packed initial data is uploaded through a staging copy. Depth textures cannot be uploaded or read back. |
| `SamplerHandle` | min/mag filter, U/V address mode, label | |
| `PipelineHandle` | Metal source, entry points, ordered color formats, depth format, depth test/write/compare, cull, winding, label | Shaders fetch vertices from bound buffers; there is no fixed-function vertex layout. |

Formats are `rgba8_unorm`, `rgba8_srgb`, `bgra8_unorm`, `bgra8_srgb`, `rgba16_float`, and `depth32_float`. The Metal surface is `bgra8_unorm`; the linear/HDR color pipeline is later rendering work.

A handle carries its device session token, slot, and slot generation. It is valid only for that device session and is never serialized. Destroying a resource bumps the generation at once. A new session invalidates every handle, even at a reused slot. Exhausted generations retire a slot permanently instead of wrapping.

## Validation and diagnostics

Every public call validates in `GraphicsDevice` before reaching a backend, so Metal and the null backend report identical errors. Failures return `RhiDiagnostic` (or `RhiResult`/`SurfaceResult`) with an `RhiError` code and a message naming the resource label. Nothing is encoded for a rejected call, and earlier encoder state is unchanged.

| Check | Error |
| --- | --- |
| Zero/oversized sizes, undefined formats, empty or unknown usage, unknown enum values, depth test without a depth format, color/depth format mix-ups, no attachments | `invalid_descriptor` |
| Metal shader compile failure (compiler text included) / missing entry point or pipeline rejection | `shader_compilation` / `invalid_descriptor` |
| Null, destroyed, reused-slot, or other-session handles | `stale_handle` |
| Calls in the wrong frame/pass state; draw without a pipeline in the current pass; `end_frame` with a pass still open (the pass is closed and the frame still submits) | `wrong_state` |
| Pipeline color formats (count and order) or depth format differ from the pass | `incompatible_pipeline`, listing both format sets |
| Missing usage (vertex/index/uniform/sampled/render_target/readback), sampling a texture that is an attachment of the current pass, duplicate attachments, zero counts | `invalid_usage` |
| Attachment sizes differ; clear depth outside [0,1]; nonfinite clear colors | `invalid_descriptor` |
| Offsets or ranges outside a buffer; binding index at or beyond the limit | `out_of_range` |
| Uniform offsets not multiples of 256 bytes, vertex offsets not multiples of 4, index offsets not multiples of 4 | `misaligned` |
| Headless surface / zero-sized surface or no drawable | `unsupported` / `surface_unavailable` |

Shader reflection is not used, so a pipeline that reads an unbound slot is not caught by this layer. GPU tests run with Metal API validation (`MTL_DEBUG_LAYER=1`, set by CTest), which aborts on such misuse. `limits()` reports the conservative binding limits: 31 buffer and texture indices, 16 samplers, 8 color attachments, 16384-texel textures, and the device's maximum buffer length. Vertex and uniform buffers share one index table per stage. Uniform buffers bind to both stages; textures and samplers bind to the fragment stage.

## Deferred retirement

`destroy()` invalidates the handle immediately and records the native object with a frame serial: the frame being encoded, or the last submitted frame when idle. Command-buffer completion handlers advance a completion watermark. Out-of-order callbacks wait until earlier frames complete. Retired objects whose serial is complete are released at `begin_frame`, `end_frame`, idle `destroy`, and `wait_idle`. Slots are reused only after native release. With no frame in flight, destruction releases at once.

Metal frame command buffers are created with **unretained references**. Correctness therefore rests on this retirement, not on Metal retaining encoded resources. The previous asset-lease behavior relied on retained references and is replaced. Texture uploads and readbacks use ordinary retained command buffers.

`CAMetalLayer` owns drawables and can discard them, for example on resize, while a frame is still executing. The frame's completion handler therefore holds the drawable and its texture. Metal API validation caught this during testing: the desktop test aborted in 2 of 3 runs before the fix, and 40 repeated runs passed after it.

`wait_idle()` drains submitted frames and releases everything retired. Because `begin_frame` bounds frames in flight (below), pending retirements are bounded by what callers destroy within that window. `stats()` reports live resources per kind, pending retirements, and submitted/completed frame serials. `MetalDevice::native_buffer_count()`/`native_texture_count()` count backend-owned objects, including those awaiting retirement.

## Presentation, resize, and shutdown

- `acquire_surface()` may be called once per frame outside a pass; later calls return the same target. The surface texture is transient and valid only until `end_frame`. It cannot be destroyed, sampled, or read back. It has the drawable's pixel size.
- `resize(width, height)` sets the drawable size in framebuffer pixels. Zero sizes (minimized windows) are ignored, and the new size applies at the next acquisition. Applications own their depth and offscreen targets and recreate them when the acquired size changes; see the [basic scene](../samples/basic_scene/basic_scene.cpp).
- A missing drawable, a zero-sized surface, or a headless session returns a diagnostic from `acquire_surface`. Callers skip presentation, and offscreen passes and submission continue.
- `shutdown()` is nonthrowing and idempotent. It ends an open pass, discards an uncommitted frame without presenting, waits for submitted frames, releases every live and retired resource, expires the resource lifetime, and invalidates all handles. `initialize()` starts a new session with fresh handles; a failed backend initialization rolls itself back.

`Mesh`, `Texture`, `Material`, and the legacy `Scene` use this API. `Mesh::draw`, `Texture::bind`, and `Scene::render` return the first diagnostic. `Scene::render` must run inside an open pass. It uploads each object's `SceneDrawUniforms` to its own slice of frame upload memory, so objects keep their own transforms and values within a frame.

## Frame pacing and upload memory

[#997](https://work.rezee.app/kash/issues/997) adds `DeviceOptions`, which is passed to `initialize` and fixed for the session:

| Option | Default | Meaning |
| --- | --- | --- |
| `frames_in_flight` | 3 (1-8) | Frames the CPU may encode ahead of GPU completion. |
| `transient_bytes_per_frame` | 4 MiB | Upload memory per frame slot; 0 disables it. |

Frame *n* uses upload slot *n mod frames_in_flight*. `begin_frame` waits until frame *n − frames_in_flight*, the previous user of that slot, has completed. So at most `frames_in_flight − 1` frames are executing when encoding starts, and a slot's memory is never rewritten while the GPU can still read it. Blocking waits are counted in `stats().frame_waits` and `frame_wait_microseconds`; they are reported, not hidden. A backend that cannot wait returns `timeout`; the null backend does this under manual completion. The Metal backend blocks on that frame's command buffer; completion is in commit order on its single queue. The upload buffers are device-owned: `frames_in_flight × transient_bytes_per_frame` bytes of shared memory, excluded from `stats().buffers`.

`upload_transient(data, size, alignment)` copies into the current frame's slot with a bump allocator. Alignment 0 means the 256-byte uniform alignment; smaller powers of two pack vertex data. It returns a `TransientSlice` (buffer, offset, size, frame serial) that binds through `set_uniform_buffer(index, slice)` or `set_vertex_buffer(index, slice)`. Rules:

- Uploads require an open frame. A slice from an earlier frame is rejected as `stale_handle`.
- The upload buffers cannot be destroyed, written with `write_buffer`, or bound by raw handle (including as an index buffer). Only slices address them.
- **Exhaustion** returns `out_of_memory` with the requested and used sizes, and increments `transient_failures`. The frame stays valid: callers skip that upload/draw. Memory never grows. `transient_bytes_used` and `transient_high_water` show headroom. `Scene::render` returns the diagnostic, and the basic sample treats it as a frame failure.
- **Submission failure**: if the backend throws while submitting, nothing reached the GPU. The frame is marked complete immediately, so throttling and retirement cannot stall, and the exception propagates; `Engine` ends the session. A frame that fails on the GPU still completes: its error is reported through `take_gpu_errors()` (logged by `Engine`), and its memory and retirements proceed normally.
- **Shutdown** drains every submitted frame before releasing upload memory and retired resources. `wait_idle()` does the same without ending the session.

Resources the caller owns remain the caller's responsibility: `write_buffer` into a buffer a submitted frame still reads is a data race. Use upload memory, or double-buffer and destroy through the device.

## Verification

The #997 additions:

- **CPU**: throttling waits for exactly the slot's previous frame, with timeouts under manual completion and invalid options rejected. Upload alignment, packing, exhaustion, stale slices, and raw-handle protection are covered, and 1,000 frames run with bounded memory. A failed submission completes its frame and retires its resources.
- **Metal** (pixel readback):
  - 16 draws in one pass keep 16 distinct uniform values.
  - Frames encoded ahead of the GPU never see later writes. Five rounds of 12 frames run without waiting, each with 200 filler uploads and 8 checked draws into its own target, and every target is read back afterwards.
  - Upload exhaustion skips only the overflowing draw.
  - Two `Scene` objects sharing one mesh keep separate transforms over 30 frames.
- **Mutation checks**: forcing every upload to offset 0 fails the per-draw, in-flight, overflow, and scene tests. Removing the throttle fails the in-flight test in 3 of 3 runs.

The #996 coverage:

- **CPU**: [rhi_validation_tests.cpp](../tests/rhi_validation_tests.cpp) links only MayaRHI and Catch2. On the null backend it covers every validation class above, stale handles and slot reuse, the encoder state machine, pipeline/pass compatibility, and deferred retirement against controlled completion. It also covers out-of-order completion, emulated surface acquisition/resize/missing drawables, and shutdown with open frames.
- **Metal**: [rhi_tests.cpp](../tests/rhi_tests.cpp) reads back real rendered pixels. It verifies clear/load/store for color and depth across passes, 256-byte uniform offsets, texture upload and nearest sampling, and error reporting. It also checks 60 frames whose uniform buffer is destroyed while encoding (correct output with unretained command buffers), mid-frame pipeline/texture destruction, and headless and open-frame shutdown.
- **Desktop**: [desktop_lifecycle_tests.cpp](../tests/desktop_lifecycle_tests.cpp) presents to a real window across three drawable sizes. It ignores zero sizes, skips presentation on some frames, and runs offscreen passes without the drawable. It also retires encoded mesh buffers after the final asset lease is released and shuts down with an acquired surface.

#997 validation on 24 September 2026:

- The default and a fresh Release build report no diagnostics from Maya sources. All 17 CTest entries pass, repeated three times, with GPU entries under Metal API validation. 20 more runs each of the Metal and desktop suites all passed, and a 300-frame sample smoke run completed.
- CPU RHI suite: 13 cases / 7,008 assertions. The Metal/core suite has 102 cases (17 tagged `[rhi]`); desktop has 4 cases. Everything passes under UBSan, and clang static analysis of graphics_device.cpp, scene.cpp, and metal_device.mm reports no findings.
- A local Release Metal run drew 300 frames × 2,000 per-draw uniform uploads into a 1024² target (GPU-bound): about 2.97 ms/frame with 1 frame in flight, 1.44 ms with 2, and 1.41 ms with 3. The device waited in nearly every frame (349.6 ms total at depth 3), and upload high water was 499 KiB. The default of 3 is kept for headroom; the choice still needs #1004 workloads on target hardware.

#996 validation on 24 September 2026:

- The default and a fresh Release build of all targets report no diagnostics from Maya sources.
- All 17 CTest entries pass (12 CPU/CLI, 5 GPU/smoke), repeated three times. The GPU entries run with Metal API validation enabled, which the Metal runtime confirms in its log.
- 40 repeated desktop runs and 20 repeated Metal suite runs passed after the drawable-lifetime fix.
- RHI (10 cases / 1,899 assertions), assets, scene, World, properties, the Metal/core suite (98 cases), and desktop tests pass under UBSan.
- Clang static analysis of graphics_device.cpp, null_device.cpp, and metal_device.mm reports no findings.
- A Release CPU run of validation overhead on the null backend measured about 60–75 ns per uniform bind + indexed draw pair (100,000 pairs: 5.8 ms). 100,000 buffer create/destroy pairs took 7.4 ms. This measures validation bookkeeping only, not Metal encoding or GPU time, and it is not #1004 evidence.
- ASan remains blocked by the local startup limitation recorded since #991.
- The basic scene's on-screen output is exercised by smoke runs only; its pixels are not compared.
