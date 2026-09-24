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
| `BufferHandle` | size, `vertex`/`index`/`uniform` usage flags, label | CPU-writable shared memory. `write_buffer` is immediate and not synchronized with frames in flight; per-frame allocation is #997. |
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

Retirement is unbounded by design in this issue. `wait_idle()` drains submitted frames and releases everything retired. Frames-in-flight limits, per-draw uniform allocation, and backpressure are #997. `stats()` reports live resources per kind, pending retirements, and submitted/completed frame serials. `MetalDevice::native_buffer_count()`/`native_texture_count()` count backend-owned objects, including those awaiting retirement.

## Presentation, resize, and shutdown

- `acquire_surface()` may be called once per frame outside a pass; later calls return the same target. The surface texture is transient and valid only until `end_frame`. It cannot be destroyed, sampled, or read back. It has the drawable's pixel size.
- `resize(width, height)` sets the drawable size in framebuffer pixels. Zero sizes (minimized windows) are ignored, and the new size applies at the next acquisition. Applications own their depth and offscreen targets and recreate them when the acquired size changes; see the [basic scene](../samples/basic_scene/basic_scene.cpp).
- A missing drawable, a zero-sized surface, or a headless session returns a diagnostic from `acquire_surface`. Callers skip presentation, and offscreen passes and submission continue.
- `shutdown()` is nonthrowing and idempotent. It ends an open pass, discards an uncommitted frame without presenting, waits for submitted frames, releases every live and retired resource, expires the resource lifetime, and invalidates all handles. `initialize()` starts a new session with fresh handles; a failed backend initialization rolls itself back.

`Mesh`, `Texture`, `Material`, and the legacy `Scene` use this API. `Mesh::draw`, `Texture::bind`, and `Scene::render` return the first diagnostic. `Scene::render` must run inside an open pass. It still rewrites uniform offset 0 for every draw, so every object in a frame reads the last object's constants until #997 adds per-draw allocations.

## Verification

- **CPU**: [rhi_validation_tests.cpp](../tests/rhi_validation_tests.cpp) links only MayaRHI and Catch2. On the null backend it covers every validation class above, stale handles and slot reuse, the encoder state machine, pipeline/pass compatibility, and deferred retirement against controlled completion. It also covers out-of-order completion, emulated surface acquisition/resize/missing drawables, and shutdown with open frames.
- **Metal**: [rhi_tests.cpp](../tests/rhi_tests.cpp) reads back real rendered pixels. It verifies clear/load/store for color and depth across passes, 256-byte uniform offsets, texture upload and nearest sampling, and error reporting. It also checks 60 frames whose uniform buffer is destroyed while encoding (correct output with unretained command buffers), mid-frame pipeline/texture destruction, and headless and open-frame shutdown.
- **Desktop**: [desktop_lifecycle_tests.cpp](../tests/desktop_lifecycle_tests.cpp) presents to a real window across three drawable sizes. It ignores zero sizes, skips presentation on some frames, and runs offscreen passes without the drawable. It also retires encoded mesh buffers after the final asset lease is released and shuts down with an acquired surface.

Validation on 24 September 2026:

- The default and a fresh Release build of all targets report no diagnostics from Maya sources.
- All 17 CTest entries pass (12 CPU/CLI, 5 GPU/smoke), repeated three times. The GPU entries run with Metal API validation enabled, which the Metal runtime confirms in its log.
- 40 repeated desktop runs and 20 repeated Metal suite runs passed after the drawable-lifetime fix.
- RHI (10 cases / 1,899 assertions), assets, scene, World, properties, the Metal/core suite (98 cases), and desktop tests pass under UBSan.
- Clang static analysis of graphics_device.cpp, null_device.cpp, and metal_device.mm reports no findings.
- A Release CPU run of validation overhead on the null backend measured about 60–75 ns per uniform bind + indexed draw pair (100,000 pairs: 5.8 ms). 100,000 buffer create/destroy pairs took 7.4 ms. This measures validation bookkeeping only, not Metal encoding or GPU time, and it is not #1004 evidence.
- ASan remains blocked by the local startup limitation recorded since #991.
- The basic scene's on-screen output is exercised by smoke runs only; its pixels are not compared.
