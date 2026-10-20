# Validation record

Run date: 20 October 2026. Environment: Windows 11 x64 (10.0.26300), AMD Ryzen 7 5800H, 8 cores / 16 logical processors, approximately 64 GiB RAM. Release compilation used MSVC 19.51, C++20, `/O2`, link-time optimization and the static CRT.

## Artbook rendering optimization

The optimized reader was measured against the original renderer on `The_Witcher_3_Wild_Hunt_Artbook_EN.pdf`: 198 pages, 527,185,943 bytes. The PDF was not modified. Measurements below use the same release configuration and machine described above.

The controlled comparison used three fresh processes per page and three renders per process, with other CPU-heavy work stopped. The table reports the median first render, at scale 1 (approximately 689 x 874 output pixels). Renderer resources start empty; the operating-system file cache is warm. Opening the document takes about 8 ms separately. Process startup and PNG encoding are excluded.

| Page | Original first render | Optimized first render | Speedup | Optimized resource-cached render |
| --- | ---: | ---: | ---: | ---: |
| 1 | 673.925 ms | 234.036 ms | 2.9x | 3.159 ms |
| 2 | 1,178.096 ms | 102.854 ms | 11.5x | 5.194 ms |
| 10 | 5,422.329 ms | 190.277 ms | 28.5x | 21.996 ms |
| 20 | 4,533.721 ms | 178.270 ms | 25.4x | 21.966 ms |
| 50 | 5,991.444 ms | 278.466 ms | 21.5x | 51.538 ms |
| 100 | 4,254.834 ms | 207.683 ms | 20.5x | 23.365 ms |
| 150 | 3,488.567 ms | 165.598 ms | 21.1x | 29.507 ms |
| 198 | 589.763 ms | 161.324 ms | 3.7x | 2.781 ms |

Resource-cached renders reuse decoded images and fonts but still rasterize the page. Their latency is essentially unchanged from the original renderer. The GUI also has a separate completed-page bitmap cache and now prefetches two pages in the navigation direction and one behind. A ready bitmap avoids rendering on the page turn; the table does not measure GUI input-to-display latency or assume prefetch always finishes in time.

A final sequential render of all 198 pages in one process took **27.753 seconds**: median **129.222 ms**, 95th percentile **224.702 ms**, maximum **287.024 ms**. This includes reuse of resources between pages and a warmed decoder pool. Private memory at the end was 178.93 MiB; this is not peak memory. An earlier original whole-book run overlapped other work, so it is not used to claim a controlled whole-book speedup.

At scale 2 (approximately 1,378 x 1,747 pixels), median first renders were 440.199 ms on page 10, 572.474 ms on page 50 and 947.821 ms on page 144. Cached-resource renders were 45.031, 87.383 and 49.811 ms respectively. Uncached high-zoom rendering therefore still has a noticeable delay.

### Bottlenecks and changes

JPEG 2000 coefficient decoding dominated the original first-render profile. Each bit-plane pass repeatedly scanned neighboring coefficients. The decoder now maintains neighbor flags, uses context tables, reconstructs only the wavelet levels needed at the requested display size, and parallelizes independent tiles on a bounded Windows thread pool. Strip-based vertical transforms improve memory locality. Serial decoding remains available with `--image-workers 1`.

Common color conversion, grayscale expansion, mask application and box downsampling now use direct or packed pixel paths. JPEG images request scaled decoding through WIC. Decoded images use a bounded LRU cache that supports resolution upgrades and preserves live shared images on eviction. Per-render samples and optional stage/image profiles make these decisions measurable. See [ARCHITECTURE.md](ARCHITECTURE.md#measuring-rendering) and [benchmark-render.py](../scripts/benchmark-render.py).

Reduced codec decoding changes the resampling filter compared with full decoding followed by box filtering. Default display pixels are consequently not claimed to be identical. `--full-resolution` retains full-source decoding for reference comparisons, and higher zoom requests automatically decode more detail.

### Verification after optimization

- Release and AddressSanitizer builds passed all **four CTest groups** in 16.10 and 37.70 seconds respectively: engine, render resources, generated PDFs and codecs. These include 37 engine assertions, 61 generated PDF cases, 13 full-resolution and 13 reduced-resolution JPEG 2000 fixtures, and two malformed-stream checks.
- The new render-resource checks cover image resolution upgrades, LRU accounting/eviction, retained images, exact box-filter rounding at odd/even edges, worker exception propagation and cancellation.
- The independent Pillow/OpenJPEG comparison passed **116 cases, including 62 reduced-resolution decodes**, with no failures. Lossless samples match exactly; irreversible samples differ by at most one 8-bit channel value. Pillow rejects some reduced decodes with nonzero image origins, so those origin cases are tested only at full resolution.
- With full-source decoding enabled, pages 1, 2, 10, 20, 50, 100, 150 and 198 were **pixel-identical to the original renderer**. A reduced artbook image was also byte-identical to OpenJPEG at 692 x 663 RGB pixels.
- AddressSanitizer rendered artbook pages 10, 50 and 144 twice at scale 2 without reported errors. A further 160 deterministic PDF mutations produced no crashes or timeouts. These remain bounded checks, not exhaustive security testing.
- The actual native application opened and displayed the artbook through its screenshot harness. The captured application view was inspected. The updated release reader is 3,911,168 bytes.

All pages completed without a fatal document error, but the original compatibility warnings remain: RGB overprint approximation, isolated-backdrop approximation for some non-isolated groups, and an image on **page 72 (object 659)** rejected for a missing or out-of-order JPEG 2000 tile part. The original executable rejects the same image. Successful execution is not a claim that this image or all PDF features render correctly.

Raw local evidence is under ignored `artifacts/render-perf/`: `final-baseline.json`, `final-optimized.json`, `final-scale2.json`, `final-all.json`, build logs, oracle results, mutation results and comparison images. To repeat the optimized measurements:

```powershell
$artbook = 'C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3\ARTBOOK\The_Witcher_3_Wild_Hunt_Artbook_EN.pdf'
python scripts\benchmark-render.py $artbook --pages 1,2,10,20,50,100,150,198 --runs 3 --repeats 3 --output artifacts\render-perf\repeat.json
python scripts\benchmark-render.py $artbook --pages 10,50,144 --scale 2 --runs 3 --repeats 3 --output artifacts\render-perf\repeat-scale2.json
.\build\pdfprobe.exe $artbook --all --timings
```

## Baseline automated checks

The remaining sections record the validation performed before the artbook optimization; corpus timings and binary size below describe that earlier build.

Both the optimized release and MSVC AddressSanitizer builds passed all three CTest groups:

| Group | Exercised checks | Result |
| --- | --- | --- |
| Engine | 37 assertions covering parsing, filters, password preparation, crypto, functions, CMaps and the standard MQ arithmetic known-answer sequence | Passed |
| Generated PDFs | 61 fixtures with text, pixel, page geometry, xref, encryption/password and rejection assertions | Passed |
| JPEG 2000 | 13 independent pixel fixtures, plus 2 malformed-codestream rejection checks | Passed |

The AddressSanitizer CTest run completed in 37.72 seconds with no reported memory errors. The generated PDF fixtures include encrypted revisions 2–6 and Unicode passwords; classic, object-stream, hybrid and incremental xrefs; Type 1/Type 3 fonts; all seven shading types; images/masks; generated form appearances; and shared-symbol, streamed-row-count and random-access JBIG2 cases.

A deterministic AddressSanitizer mutation run tried 320 modified generated PDFs, including truncation, bit flips, insertion/replacement and stream-byte mutation. Another 96 mutations targeted the three backdrop-group fixtures after the transparency changes. Neither run produced crashes, sanitizer reports or four-second timeouts. Expected parser/decoder rejections were allowed. These are bounded smoke tests, not sustained coverage-guided fuzzing.

An additional 50-case JPEG 2000 differential run used Pillow/OpenJPEG solely as an encoder and reference decoder. Lossless output was byte-for-byte identical; irreversible output differed by at most one 8-bit channel value. Cases included noise and gradients, grayscale/RGB, multiple sizes, all five progression orders, tiling, multiple layers and precincts. The regular checked-in fixtures additionally cover alpha, signed grayscale and unsigned 16-bit grayscale.

The three [ITU-T T.88](https://www.itu.int/rec/T-REC-T.88/en) Annex H example pages were wrapped in PDFs and compared against an independent PDFium rendering at native image resolution. All three had zero differing pixels. This exercises arithmetic/Huffman symbols, refinement, generic and halftone paths. Reference decoders and downloaded standards stay under ignored `artifacts/`; they are not application dependencies.

Three non-isolated transparency fixtures cover an opaque backdrop, a translucent parent and a luminosity mask. Their asserted interior pixels match a MuPDF 2.0 reference to within one channel value. The opaque-backdrop case also demonstrates a difference from PDFium's output; the expected result follows the PDF backdrop-compositing equations and the second renderer. Keeping the parent target paused across group rendering removed a duplicate flush: rendering the attention paper twice took 9.86 seconds with the corrected backdrop path versus 9.80 seconds with the earlier approximate path in a local comparison. These timings are within ordinary run-to-run variation.

## External document corpus

The final all-pages run opened and rendered **31 documents / 1,275 pages**, with no fatal document errors. Three documents generated compatibility notes. This establishes successful execution on this corpus, not pixel-perfect rendering or general conformance. First pages were exported and selected outputs inspected visually; the entire corpus was not manually reviewed page by page.

Raw results, warnings and source-file SHA-256 hashes are in [validation-corpus.json](validation-corpus.json). The run used scale 1: one output pixel per PDF point, equivalent to 72 dpi. Timings below sum the renderer's measured time across every page, excluding PNG export and process startup. They are single-run observations on this machine, not a comparison against other readers.

| Document | Pages | Open | All-page rendering |
| --- | ---: | ---: | ---: |
| ISO 32000-1 PDF reference | 756 | 97.55 ms | 9.73 s |
| ITU-T T.800 JPEG 2000 specification | 231 | 13.40 ms | 5.65 s |
| ITU-T T.88 JBIG2 specification | 164 | 3.80 ms | 1.53 s |
| Adobe CFF specification | 62 | 6.10 ms | 0.48 s |
| Attention Is All You Need | 15 | 3.30 ms | 5.22 s |
| Entire 31-document corpus | 1,275 | 136.29 ms total | 28.10 s total |

Sources include the [Adobe PDF reference](https://opensource.adobe.com/dc-acrobat-sdk-docs/pdfstandards/PDF32000_2008.pdf), [Adobe font specifications](https://github.com/adobe-type-tools/font-tech-notes), [ITU-T T.800](https://www.itu.int/rec/T-REC-T.800/en), [ITU-T T.88](https://www.itu.int/rec/T-REC-T.88/en), [the attention paper](https://arxiv.org/abs/1706.03762), [Mozilla PDF.js regression fixtures](https://github.com/mozilla/pdf.js/tree/master/test/pdfs), and a shared-symbol scan from [pdfminer.six's issue attachments](https://github.com/pdfminer/pdfminer.six/files/8399364/64.pdf).

The notes were:

- `pdf-reference.pdf`: overprint is approximated in RGB output.
- `annotation-text-widget.pdf`: dynamic XFA is unavailable; stored pages and ordinary field appearances can still be drawn.
- `jpx_numcomps.pdf`: unsupported or inconsistent color-component configurations prevent some image objects from being drawn.

Other differences may not produce notes. In particular, the `complex_ttf_font.pdf` fixture rendered but produced no useful extracted text. Memory values in the raw report are private bytes at measurement time, **not peak process memory**. Large images may require substantial temporary allocations even when they are not retained in a cache.

After the final codec changes, six external JPX/JBIG2 regression files and all three Annex H wrappers were rendered again with no warnings or fatal errors. The independent pixel comparisons were also repeated successfully.

## Native application and packaging

The actual Win32 application was launched through its hidden-window screenshot harness, opened the attention paper, rendered it through the worker, drew the toolbar/sidebar/page/status area and exited successfully. The resulting client-area image was inspected. This checks native initialization and first-page presentation; it is not an automated test of every dialog or physical printing.

The x64 release reader is 3,886,592 bytes. `dumpbin /dependents` lists only Windows system DLLs, with no separately installed C++ runtime or PDF library. WIC codecs are activated through COM. The portable package contains the reader, command-line probe, documentation and data-license notices.

## Reproduction and limits

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\build.ps1 -Configuration Release -Sanitize
python tests\jpx_oracle.py build\codec_probe.exe artifacts\jpx-oracle
python scripts\validate-corpus.py build\pdfprobe.exe artifacts\validation C:\path\to\pdfs
```

The first two commands need no third-party Python modules. The optional JPEG 2000 differential script needs Pillow with JPEG 2000 support. Corpus PDFs and independent reference engines are not redistributed with the app.

From an x64 Visual Studio developer shell, after the sanitizer build:

```powershell
python tests\fuzz_smoke.py build-asan\pdfprobe.exe build\fixtures artifacts\fuzz-smoke 320
```

The developer shell supplies the MSVC AddressSanitizer runtime on `PATH`; the portable release does not need this runtime.

The native engine currently shares the application process. A finite regression corpus, sanitizer pass and bounded mutation tests do not establish security or universal PDF conformance. Physical-printer behavior, advanced print flags, dynamic XFA, exact ICC/overprint/transparency and uncommon codec extensions remain outside the validated scope. See [COMPATIBILITY.md](COMPATIBILITY.md) for the implementation's current limits.
