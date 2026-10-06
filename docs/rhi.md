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

Within a pass, `set_scissor(rect)` (#999) restricts later draws to a nonempty rectangle inside the attachments, in pixels from the top left. Each pass starts unclipped. `draw_indexed` can also read indices from a [frame upload slice](#frame-pacing-and-upload-memory), with the offset relative to the slice. The editor's UI draws this way. Its last arguments draw several instances (`instance_count`) numbered from `first_instance` (#1025; Metal's `baseInstance`), so one bound per-instance array serves several draws.

A frame runs `begin_frame` → any number of render passes → `end_frame`. `end_frame` presents an acquired surface and submits. Offscreen passes target any texture created with `render_target` usage and never touch the drawable. The same frame can render offscreen targets and present, only render offscreen, or only present. `Engine::tick` calls `begin_frame`/`end_frame` around `Application::on_render`, and treats a failure from either as a frame failure. GPU execution errors reported by completed frames are logged through `take_gpu_errors()`.

Handles and descriptors:

| Kind | Descriptor | Notes |
| --- | --- | --- |
| `BufferHandle` | size, `vertex`/`index`/`uniform` usage flags, label | CPU-writable shared memory. `write_buffer` is immediate and not synchronized with frames in flight; per-frame data belongs in [upload memory](#frame-pacing-and-upload-memory). |
| `TextureHandle` | width, height, format, `sampled`/`render_target`/`readback` usage, label, mip levels | 2D, GPU-private, 1 to `full_mip_count` levels (#1031). Optional initial data, every level tightly packed and level 0 first, is uploaded through a staging copy. Depth textures cannot be uploaded or read back; mipmapped and compressed textures can only be sampled. |
| `SamplerHandle` | min/mag filter, U/V address mode, label, mip filter, anisotropy | Mip filtering is `none` (level 0 only), `nearest`, or `linear`; anisotropy is 1 (off) to `limits().max_anisotropy`. |
| `PipelineHandle` | Metal source, entry points, ordered color formats, depth format, depth test/write/compare, cull, winding, label, blend | Shaders fetch vertices from bound buffers; there is no fixed-function vertex layout. `BlendMode::alpha` (#999) is straight-alpha source-over on every color attachment; the default is opaque. |

Formats are `rgba8_unorm`, `rgba8_srgb`, `bgra8_unorm`, `bgra8_srgb`, `rgba16_float`, `depth32_float`, and (since #1031) the block-compressed `astc_4x4_unorm`, `astc_4x4_srgb`, `astc_6x6_unorm`, and `astc_6x6_srgb`, which need `limits().astc`. The Metal surface is `bgra8_unorm`; the linear/HDR color pipeline is later rendering work.

### Mip levels and compressed formats

[#1031](https://work.rezee.app/kash/issues/1031) adds what [texture assets](assets.md#textures) need. Level *n* of a texture is max(1, size >> *n*) on each side, and `full_mip_count(width, height)` is the chain to 1×1. Sizes come from `resource.hpp`, so callers and backends agree:

- `mip_level_bytes(format, width, height, level)`: rows of pixels, or of blocks for ASTC (16 bytes per 4×4 or 6×6 block, partial blocks rounded up);
- `texture_bytes(desc)`: every level, the size initial data must have and what `RhiStats` tracks;
- `is_compressed_format`, `is_srgb_format`, `block_extent`, `block_bytes`. `bytes_per_pixel` is 0 for compressed formats.

**Cube textures** (#1035, for [environments](renderer.md#environments)): `TextureDesc::type` is `texture_2d` (the default) or `cube`. A cube has six square faces per level, in Metal's order (+X, −X, +Y, −Y, +Z, −Z); its data holds each level's six faces in that order, level 0 first, and `texture_bytes` counts all of them (`texture_faces(desc)` is 6). Cubes must be square and can only be sampled, not rendered to or read back. Shaders sample them by direction (`texturecube`).

`create_texture(desc, std::span<const std::byte>)` refuses data whose size is not `texture_bytes(desc)`; the older pointer form trusts the caller. Sampling an sRGB format decodes to linear values. `limits().astc` is true on Apple GPUs (Metal's `MTLGPUFamilyApple2`); the null device reports it as `NullDeviceOptions::astc` says (true by default), so tests can check a device without it.

A handle carries its device session token, slot, and slot generation. It is valid only for that device session and is never serialized. Destroying a resource bumps the generation at once. A new session invalidates every handle, even at a reused slot. Exhausted generations retire a slot permanently instead of wrapping.

## Validation and diagnostics

Every public call validates in `GraphicsDevice` before reaching a backend, so Metal and the null backend report identical errors. Failures return `RhiDiagnostic` (or `RhiResult`/`SurfaceResult`) with an `RhiError` code and a message naming the resource label. Nothing is encoded for a rejected call, and earlier encoder state is unchanged.

| Check | Error |
| --- | --- |
| Zero/oversized sizes, undefined formats, empty or unknown usage, unknown enum values, depth test without a depth format, color/depth format mix-ups, compressed color attachment formats, no attachments, mip levels outside 1..`full_mip_count`, anisotropy outside 1..the limit, initial data (span form) of the wrong size | `invalid_descriptor` |
| Mipmapped or compressed textures with render-target or readback usage; ASTC on a device without it | `unsupported` |
| Metal shader compile failure (compiler text included) / missing entry point or pipeline rejection | `shader_compilation` / `invalid_descriptor` |
| Null, destroyed, reused-slot, or other-session handles | `stale_handle` |
| Calls in the wrong frame/pass state; draw without a pipeline in the current pass; `end_frame` with a pass still open (the pass is closed and the frame still submits) | `wrong_state` |
| Pipeline color formats (count and order) or depth format differ from the pass | `incompatible_pipeline`, listing both format sets |
| Missing usage (vertex/index/uniform/sampled/render_target/readback), sampling a texture that is an attachment of the current pass, duplicate attachments, zero counts | `invalid_usage` |
| Attachment sizes differ; clear depth outside [0,1]; nonfinite clear colors | `invalid_descriptor` |
| Offsets or ranges outside a buffer; binding index at or beyond the limit | `out_of_range` |
| Uniform offsets not multiples of 256 bytes, vertex offsets not multiples of 4, index offsets not multiples of 4 | `misaligned` |
| Headless surface / zero-sized surface or no drawable | `unsupported` / `surface_unavailable` |

Shader reflection is not used, so a pipeline that reads an unbound slot is not caught by this layer. GPU tests run with Metal API validation (`MTL_DEBUG_LAYER=1`, set by CTest), which aborts on such misuse. `limits()` reports the conservative binding limits: 31 buffer and texture indices, 16 samplers, 8 color attachments, 16384-texel textures, the device's maximum buffer length, anisotropy up to 16, and whether ASTC can be sampled. Vertex and uniform buffers share one index table per stage. Uniform buffers bind to both stages; textures and samplers bind to the fragment stage.

## Deferred retirement

`destroy()` invalidates the handle immediately and records the native object with a frame serial: the frame being encoded, or the last submitted frame when idle. Command-buffer completion handlers advance a completion watermark. Out-of-order callbacks wait until earlier frames complete. Retired objects whose serial is complete are released at `begin_frame`, `end_frame`, idle `destroy`, and `wait_idle`. Slots are reused only after native release. With no frame in flight, destruction releases at once.

Metal frame command buffers are created with **unretained references**. Correctness therefore rests on this retirement, not on Metal retaining encoded resources. The previous asset-lease behavior relied on retained references and is replaced. Texture uploads and readbacks use ordinary retained command buffers.

`CAMetalLayer` owns drawables and can discard them, for example on resize, while a frame is still executing. The frame's completion handler therefore holds the drawable and its texture. Metal API validation caught this during testing: the desktop test aborted in 2 of 3 runs before the fix, and 40 repeated runs passed after it.

`wait_idle()` drains submitted frames and releases everything retired. Because `begin_frame` bounds frames in flight (below), pending retirements are bounded by what callers destroy within that window. `stats()` reports live resources per kind, pending retirements, and submitted/completed frame serials. `MetalDevice::native_buffer_count()`/`native_texture_count()` count backend-owned objects, including those awaiting retirement.

## Presentation, resize, and shutdown

- `acquire_surface()` may be called once per frame outside a pass; later calls return the same target. The surface texture is transient and valid only until `end_frame`. It cannot be destroyed, sampled, or read back. It has the drawable's pixel size.
- The window layer's scale (`contentsScale`) matches the window's backing scale and is updated on resize, so Retina displays show drawables at full density; `MetalDevice::surface_scale()` reports it.
- `resize(width, height)` sets the drawable size in framebuffer pixels. Zero sizes (minimized windows) are ignored, and the new size applies at the next acquisition. Applications own their depth and offscreen targets and recreate them when the acquired size changes; [RenderTarget](renderer.md#views-and-targets) does this for renderer views.
- A missing drawable, a zero-sized surface, or a headless session returns a diagnostic from `acquire_surface`. Callers skip presentation, and offscreen passes and submission continue.
- `shutdown()` is nonthrowing and idempotent. It ends an open pass, discards an uncommitted frame without presenting, waits for submitted frames, releases every live and retired resource, expires the resource lifetime, and invalidates all handles. `initialize()` starts a new session with fresh handles; a failed backend initialization rolls itself back.

`Mesh`, `Texture`, and the [renderer](renderer.md) use this API. `Mesh::draw` and `Texture::bind` return the first diagnostic. `Renderer::render` opens and closes its own pass. It uploads all of a view's instances' transforms as one array in frame upload memory, and draws instances of one mesh and material with one instanced draw ([culling and batching](renderer.md#culling-and-batching), #1025).

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
- **Exhaustion** returns `out_of_memory` with the requested and used sizes, and increments `transient_failures`. The frame stays valid: callers skip that upload/draw. Memory never grows. `transient_bytes_used` and `transient_high_water` show headroom. `Renderer::render` returns the diagnostic after closing its pass, and the basic sample treats it as a frame failure.
- **Submission failure**: if the backend throws while submitting, nothing reached the GPU. The frame is marked complete immediately, so throttling and retirement cannot stall, and the exception propagates; `Engine` ends the session. A frame that fails on the GPU still completes: its error is reported through `take_gpu_errors()` (logged by `Engine`), and its memory and retirements proceed normally.
- **Shutdown** drains every submitted frame before releasing upload memory and retired resources. `wait_idle()` does the same without ending the session.

Resources the caller owns remain the caller's responsibility: `write_buffer` into a buffer a submitted frame still reads is a data race. Use upload memory, or double-buffer and destroy through the device.

## Measurement

Since [#1004](performance.md), the device measures as well as renders:

- **Per-frame counters.** `stats()` counts the current or most recent frame's `frame_passes`, `frame_draws`, `frame_instances`, and `frame_triangles`, as submitted; they reset in `begin_frame`.
- **Tracked bytes.** `stats()` also reports tracked bytes, computed from descriptors on request:
  - `buffer_bytes` and `texture_bytes` for live caller resources (excluding the upload buffers and the window's drawable), textures as `texture_bytes(desc)`: every level, compressed blocks included;
  - `pending_retirement_bytes`;
  - `upload_bytes`, the device's own per-frame memory times frames in flight.
- **GPU time.** When a frame completes, the Metal backend records its command buffer's GPU execution time, `GPUEndTime − GPUStartTime`, keyed by the frame's serial. `take_gpu_timings()` hands these over, and at most 1,024 wait (older ones are dropped and counted). `gpu_timing_supported()` is false where GPU time cannot be measured, including the null device; GPU time is never inferred from CPU submission.
- **Reported memory.** `reported_memory()` is the platform's figure for the device (Metal's `currentAllocatedSize`), or nullopt. It is kept apart from tracked bytes.
- **Pass time** (#1026). Each `GpuFrameTiming` carries its passes' GPU times, from timestamps at each pass's vertex and fragment stage boundaries ([measuring](performance.md#what-is-measured)). Backends implement `backend_pass_timing_reason` (empty when they can time passes) and `backend_attach_pass_timings`, called on the owner thread for completed frames. `frame_pass_timing()` and `frame_pass_index()` tell them whether, and which, pass is being timed. `set_gpu_pass_timing` switches timing per frame; at most `max_timed_passes` passes a frame are timed.
- **Present time** (#1026). `take_present_timings()` reports when presented frames were shown, or that they never were; `display_refresh_rate()` is the surface's display's maximum rate. The Metal backend registers a presented handler on each drawable it presents.

## Verification

The #1031 additions:

- **CPU**: level and block sizes for odd and non-square sizes; mip-level and data-size validation; mipmapped and compressed textures limited to sampling; a device without ASTC refusing ASTC only; sampler mip-filter and anisotropy limits; and tracked and pending bytes counting every level and block.
- **Metal** ([texture_gpu_tests.cpp](../tests/texture_gpu_tests.cpp), with API validation): each level of a four-level texture sampled at an explicit level, linear blending between levels, and level 0 only without mip filtering; sRGB decoding against linear storage; ASTC 6×6 and 4×4 cooked by astcenc, sampled at three levels, and a detailed image within a mean error of 3 per channel of its RGBA8 reference.

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
