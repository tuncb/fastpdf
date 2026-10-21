# Engine architecture

## Ownership and threads

`Document` owns a read-only memory mapping, xref entries, lazily parsed indirect objects, page descriptions, decoded function caches, and security state. Indirect objects live in stable `unique_ptr<Value>` allocations. Stream views point into the mapping or, for decoded data, into explicitly owned byte arrays. Close the renderer's document resources before closing or replacing its document.

`Renderer` owns thread-confined Direct2D, DirectWrite and WIC interfaces, embedded font handles, character maps and decoded images. Initialize COM on its thread. Use separate `Document` and `Renderer` instances for concurrent page workers. JPEG 2000 decoding uses bounded CPU tasks internally: independent tiles (or codeblocks in a single-tile image) share read-only parameters and write disjoint buffers. They do not touch COM or document resource caches. All tasks finish before decoding returns; failures and cancellation propagate to the calling thread. `RenderedPage` owns BGRA pixels, text spans, links and render metadata and can be passed between threads after rendering.

The reader has a GUI thread and one rendering worker. A mutex protects its request/result queues. Atomic cancellation is checked during parsing, decompression, content interpretation, mesh generation and image decoding. Document generations and render tickets prevent old work from replacing a newer page. Printing retains its request ownership when new view work is queued.

Initialization and cleanup are free functions: `open` / `close`, `init_renderer` / `close_renderer`, `map_file` / `unmap_file`. COM references use `release`. The Win32 print job uses an explicit free-function deleter. Standard-library smart pointers and containers manage allocations; all application structs expose their state.

## Main modules

- `pdf.cpp`: lexical and object parser; classic/stream/hybrid xrefs; incremental revisions and repair; object streams; page inheritance; outlines and destinations.
- `filters.cpp`: owned DEFLATE, LZW, ASCII85, ASCIIHex and RunLength decoders; TIFF and PNG predictors.
- `security.cpp`: standard security revisions 2–6, RC4, object-key derivation, AES-CBC/AES-256 handling, and Unicode password preparation. CNG supplies hashes and AES.
- `functions.cpp`: sampled, exponential, stitching and bounded PostScript calculator functions, with decoded-sample and compiled-program caches.
- `fonts.cpp`, `font_data.cpp`: CMaps, glyph mappings, embedded TrueType/OpenType/CFF, Type 1 parsing/outlines and fallback fonts. Type 3 charprocs run through the content interpreter.
- `arithmetic.cpp`, `jpeg2000.cpp`, `jbig2.cpp`: owned MQ arithmetic decoder, JPEG 2000 packet/codeblock/wavelet processing, and JBIG2 segment, symbol, text, refinement and halftone processing.
- `images.cpp`: PDF image samples, color spaces, masks, JPEG/CCITT integration and PNG output.
- `render.cpp`: PDF graphics state and operators, paths, text, forms, tiling patterns, shading types 1–7, blending/soft masks, stored annotation appearances and basic generated widget appearances.
- `app.cpp`: native reader, toolbar/sidebar, worker queue, input, selection/search, export and print.
- `cli.cpp`: inspect/render/extract/benchmark tool used by integration checks.

## Performance decisions

Files are memory-mapped, indirect objects are parsed on demand, and images and embedded fonts are loaded only when used. Font mappings are embedded compressed data and expanded on demand. Fixed DEFLATE tables are reused. Sampled functions and calculator programs are cached rather than decoded or parsed per pixel.

DEFLATE expands repeated bytes with block copies. PNG prediction uses its input buffer for output. The Up filter adds 16 bytes at a time with SSE2, which is available on the required x64 architecture.

Ordinary vectors and text go directly to Direct2D. Offscreen transparency work is restricted to visible object bounds. Fully opaque non-isolated normal-blend groups can draw directly against the existing backdrop. Translucent normal-blend groups copy the bounded backdrop, render against it, then interpolate the result with the original premultiplied pixels using the group opacity/mask. Unchanged pixels are skipped. Images build a small box-filtered pyramid for large reductions, improving scan legibility and avoiding full-size uploads at low zoom.

JPEG 2000 images reconstruct only the wavelet levels needed for their displayed size; JPEG scaling is requested through WIC's native transform pipeline. These paths retain at least the requested image-axis resolution, decoding more detail when zooming in. Original source dimensions, packet structure and resource limits are still validated. Masks follow the decoded image resolution. This changes the resampling filter compared with full decoding followed by box filtering, so display pixels need not be identical. `Renderer::full_resolution_images` or `pdfprobe --full-resolution` retains full-source decoding for comparisons.

Raw images use the existing box filter before entering the image cache. This lets large figures fit the cache at display size. Zooming in replaces the cached image with more detail. For 8-bit RGB, per-channel tables apply the Decode array. With no image mask, conversion and the first box-filter step share one pass. With a mask, reduction follows mask application. Both paths retain the existing filter's pixel values and rounding. The full-resolution override keeps the original-size image.

The coefficient decoder maintains neighbor significance/sign flags in a padded grid and uses context lookup tables, avoiding repeated neighbor scans across bit planes. Reversible lifting retains its rounding rules; vertical wavelet reconstruction gathers columns in strips for locality. Independent tiles run in parallel without nested pool work. The shared Windows pool uses at most eight tasks per decode by default, including the caller. Common RGB/grayscale conversion, palette expansion and matching-size masks use direct pixel paths. The packed integer box filter matches the original scalar rounding, including odd edges. No approximate floating-point compiler mode is enabled.

GUI rendered-page caching has a 192 MiB budget. Decoded images use a 128 MiB LRU budget including reserved pyramid overhead; individual cached images are limited to 64 MiB. A finer cached image can satisfy a lower-resolution request, and upgrades replace their accounting. Eviction preserves in-flight shared images. API clients can configure `Renderer::image_cache_limit`. Function samples have a 64 MiB cache. Large images can still render without being cached. The GUI prefetches two pages in the navigation direction and one behind, only while no interactive work is waiting.

This implementation uses a software Direct2D surface for predictable offscreen rendering, export, print and testing. It does not yet use display-list replay, GPU presentation, parallel PDF-page rasterization or a persistent document-wide text index. Parallel JPEG 2000 tiles are codec work, not parallel PDF rendering.

## Measuring rendering

`pdfprobe --timings` emits each render separately. The first render populates image/font resources; later renders reuse them. It does not use the GUI's rendered-page cache. `--benchmark 1` explicitly renders once. `--profile` adds stage timings, source image metadata and decoded dimensions. The `surface_ms` field measures page surface creation. The `interpret_ms` field measures content interpretation and annotations, including the font work in `font_load_ms`. `--image-workers 1` selects serial decoding; `--full-resolution` disables codec downscaling.

Profile fields are not additive: `image_load_ms` includes nested masks, codec time includes image preparation, and JPEG 2000 block/wavelet/output fields sum task time across parallel tiles. Compare page `ms` for wall latency and use stage fields to locate work.

```powershell
python scripts\benchmark-render.py example.pdf --pages 1,10,20 --runs 3 --repeats 3 --profile --output artifacts\render-benchmark.json
.\build\pdfprobe.exe example.pdf --all --timings
```

The harness records raw results and median first/warm times per page. Render timings exclude PNG encoding and process startup; opening is reported separately. A fresh process does not imply a cold operating-system file cache. Keep other CPU-heavy work idle for final comparisons.

## Resource boundaries

The parser bounds nesting, objects and allocated value nodes. A decoded stream is limited to 256 MiB; a rendered page/image to roughly 64 million pixels. Page dimensions are bounded, as are graphics stacks, recursive forms, CMaps, codeblocks, packets, symbol counts, calculator instructions and mesh tessellation. Zoom is reduced by the UI when its output would exceed the page-pixel budget.

These are per-operation/cache boundaries rather than a hard process memory cap. A complex document can simultaneously retain several buffers. The engine currently runs inside the reader process; a restricted render-helper process and sustained fuzzing remain production-hardening work.

## Reference material

Implementation references include [ISO 32000-1 published by Adobe](https://opensource.adobe.com/dc-acrobat-sdk-docs/pdfstandards/PDF32000_2008.pdf), Adobe's Type 1/CFF/font mapping documentation, [ITU-T T.800](https://www.itu.int/rec/T-REC-T.800/en), [ITU-T T.88](https://www.itu.int/rec/T-REC-T.88/en), and [RFC 4013](https://www.rfc-editor.org/rfc/rfc4013). Windows integration uses the Windows SDK interfaces. No external PDF or image-decoder implementation source is compiled into the engine.
