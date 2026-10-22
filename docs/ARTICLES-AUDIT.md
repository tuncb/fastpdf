# Article collection audit

Completed on 22 October 2026. The final executable rendered all **562 PDFs and
53,535 pages** without render errors. All pages have a reference comparison.
The baseline had 44 failed pages. The fixes and optimizations are committed.

The final serial run has 1 document load above 500 ms. The page target is
not met everywhere: **47 pages in 20 files** exceed 500 ms.
The longest page takes 12.149 seconds.
The final reference checks retain 201 review flags in 62 files.
These are described under Remaining image differences. The source PDFs are unchanged.

## Scope and method

The source is the recursive PDF collection at
`C:\Users\tuncb\OneDrive\academic\articles\`. The inventory contains
2,229,937,583 bytes and 512 unique file hashes. All 50 duplicate entries are
included as separate files. The loader reports 13 encrypted files and eight
files with repaired cross-reference data.

Every page is rendered at 96 dpi with the FastPDF Release engine. MuPDF 1.28.5,
through PyMuPDF 2.0, is the primary image reference.
Selected difficult pages also have a PDFium 156.0.8076.0 comparison through
pypdfium2 5.14.0. Reference tools are used only for development.

The serial run opens one file and renders its pages in order. Page times include
rendering and pixel readback. They exclude pixel hashing, image-file encoding,
reference rendering, and comparison. Document-load times measure the loader.
They exclude process launch and window display. File-system or OneDrive waits
inside the loader remain in these times. The file cache is not flushed.

Load plus first-page render is a separate engine measure. It also excludes
renderer initialization and window display. Fresh-process checks use three
samples and report their median. They cannot reuse fonts or images from earlier
pages. Diagnostic profiles are collected separately from timing samples.

The machine has an AMD Ryzen 7 5800H CPU, eight cores, 16 logical processors,
and about 64 GiB of memory. It runs Windows 11 Home, build 26300. The final timing
run has no concurrent build or reference-comparison job.

The baseline includes profiling overhead and some concurrent development load.
Its aggregate timings are diagnostic observations. Use the paired checks below
to assess speed changes under the same conditions.

## Final timing results

The final serial render ran from `2026-10-22T07:39:46+0200` to `2026-10-22T08:15:54+0200`. PDF page numbers start
at one and include cover pages.

| Engine measure | Median | 95th percentile | 99th percentile | Maximum | Above 500 ms |
| --- | --- | --- | --- | --- | --- |
| Document load, 562 files | 2.234 ms | 37.423 ms | 186.063 ms | 1,779.173 ms | 1 |
| Page render, 53,535 pages | 22.285 ms | 87.531 ms | 200.079 ms | 12,149.179 ms | 47 |
| Load plus first-page render, 562 files | 113.687 ms | 253.786 ms | 399.160 ms | 1,889.944 ms | 3 |

There are 53,488 pages within the 500 ms limit in this run. The slow
load-plus-first-page cases are file 0288 (1,128.190 ms); file 0489 (703.952 ms); file 0553 (1,889.944 ms).

The fresh-process review covers 129 cases, including every final
threshold breach and cases that were slow in earlier runs. All samples completed
without errors. There are 56 page medians above 500 ms in
21 files. 54 cases exceed the limit in all three
samples; 56 cases have at least one sample above it.
No load median exceeds 500 ms. One of 387 load samples exceeds the limit. The largest load sample is 1,575.911 ms.
Keep fresh-process and serial results separate because their cache reuse differs.

The loader samples above the limit are retained below. Every repeat median
is below 500 ms. File 0553 loaded in about 8 ms in the two earlier full runs
and in all three final repeat samples. File 0079 has one high repeat sample
and two samples below 2 ms. The cause of these isolated delays was not
determined. These checks do not establish a persistent loader cost above the
limit.

| File | Serial load | Three fresh-process loads | Repeat median |
| --- | --- | --- | --- |
| 0079 — Buntinas - Unknown - MPI on Multicore Architectures using MPICH2.pdf | 1.523 ms | 1,575.911 ms, 1.451 ms, 1.491 ms | 1.491 ms |
| 0553 — Zakas - 2010 - High Performance JavaScript.pdf | 1,779.173 ms | 7.912 ms, 7.812 ms, 7.744 ms | 7.812 ms |

The following serial threshold breaches did not repeat in any of the three
fresh-process samples. They remain in the serial counts. The repeated checks
do not establish a persistent page cost above 500 ms for these cases.

| File and page | Serial sample | Three fresh-process samples |
| --- | --- | --- |
| 0464, page 106 | 1,618.183 ms | 107.310 ms, 107.578 ms, 111.579 ms |
| 0464, page 105 | 1,021.798 ms | 228.916 ms, 232.931 ms, 235.113 ms |
| 0209, page 193 | 837.756 ms | 279.438 ms, 286.124 ms, 286.557 ms |

### Paired measurements

The baseline and final executables ran in alternating order. Each case has
three fresh processes per executable, with profiling off. The table gives
medians. Each speed ratio is the baseline median divided by the final median.
These selected checks do not give a collection-wide speed ratio.

| File and operation | Baseline | Final | Speed ratio |
| --- | --- | --- | --- |
| C++ draft (0496), document load | 4,388.553 ms | 319.616 ms | 13.73x |
| TikZ manual (0489), document load | 558.334 ms | 458.744 ms | 1.22x |
| GPU algorithm thesis (0342), page 123 | 10,737.506 ms | 415.650 ms | 25.83x |
| Joulaian moment fitting (0251), page 1 | 4,084.101 ms | 278.822 ms | 14.65x |
| Hibbeler structural analysis (0215), page 134 | 3,953.721 ms | 356.325 ms | 11.10x |
| Nam thesis (0370), page 52 | 4,912.291 ms | 1,648.265 ms | 2.98x |
| TikZ manual (0489), page 342 | 397.328 ms | 266.293 ms | 1.49x |
| Hayek (0210), page 258 | 660.780 ms | 264.924 ms | 2.49x |
| GPU Computing Gems (0529), page 196 | 3,427.830 ms | 129.549 ms | 26.46x |
| Deflections table (0132), page 53 | 1,126.156 ms | 88.439 ms | 12.73x |
| Mothes mesh generation (0359), page 103 | 1,396.045 ms | 812.321 ms | 1.72x |
| Abert NURBS ray tracing (0004), page 6 | 588.022 ms | 289.490 ms | 2.03x |
| jQuery Novice to Ninja (0090), page 1 | 544.369 ms | 285.871 ms | 1.90x |
| Nguyen patch coupling (0373), page 24 | 1,045.153 ms | 484.905 ms | 2.16x |
| Multi-GPU programming (0347), page 1 | 8,289.824 ms | 377.815 ms | 21.94x |

### Remaining slow work

Profiles show two main costs. Dense vector pages spend time reading commands,
changing clips, and drawing paths. Large image pages spend time decoding rows,
converting colors, and applying masks. Profile timers have overhead and nested
totals. They are not added to the timing table.

| File and page | Final serial time | Fresh-process median | Main work in the profile |
| --- | --- | --- | --- |
| Remonato (0413), page 71 | 12,149.179 ms | 14,603.430 ms | Command parsing, vector paths, and repeated form decoding |
| Dynamics of structures (0110), page 579 | 3,804.939 ms | 4,151.440 ms | Dense vector paths and command interpretation |
| CAD-CAE integration (0108), page 53 | 3,357.013 ms | 3,530.028 ms | Dense vector paths and command parsing |
| Kuru (0288), page 1 | 1,118.673 ms | 1,122.022 ms | Large RGB images, soft masks, and page composition |
| Nam (0370), page 52 | 1,458.075 ms | 1,490.293 ms | Many clip changes, vector paths, and command parsing |
| Mothes (0359), page 103 | 745.260 ms | 834.614 ms | Large RGB images, row decoding, and color conversion |

Remonato page 71 and Dynamics of structures page 579 failed to render in the
baseline. Their final times show the cost of rendering content that was previously
rejected; they are not comparable before-and-after timing samples.

The Nam page contains about 15,000 clipping commands. Correct fractional clip
coverage costs more than the earlier incorrect clip mode. A further clip-mode
trial preserved the sampled pixels but took more than twice as long on three
Nam pages. That trial was removed. The accepted rendering remains correct at
the checked borders.

Further improvements could reuse parsed page and form commands, reduce repeated
vector submission, and decode large images nearer the display resolution.
These are remaining design tasks. They are not completed optimizations.

The complete serial slow-page list is below. Use the file table to map IDs to
full file names. Fresh-process slow cases are recorded separately.

| File ID | PDF pages above 500 ms | Maximum |
| --- | --- | --- |
| 0108 | 53, 72, 82 | 3,357.013 ms |
| 0110 | 115, 118, 153, 579, 588, 589 | 3,804.939 ms |
| 0190 | 13 | 642.319 ms |
| 0209 | 193 | 837.756 ms |
| 0256 | 21, 28 | 772.016 ms |
| 0277 | 7 | 1,169.200 ms |
| 0280 | 2 | 645.388 ms |
| 0288 | 1 | 1,118.673 ms |
| 0299 | 12 | 766.558 ms |
| 0318 | 4 | 680.116 ms |
| 0359 | 37, 95, 97, 103, 104, 105, 108, 109, 110, 111 | 745.260 ms |
| 0360 | 20 | 1,511.121 ms |
| 0370 | 52, 56, 59 | 1,458.075 ms |
| 0413 | 46, 48, 52, 66, 71 | 12,149.179 ms |
| 0464 | 105, 106 | 1,618.183 ms |
| 0472 | 15 | 617.691 ms |
| 0480 | 7 | 602.607 ms |
| 0489 | 695, 696 | 1,115.530 ms |
| 0490 | 35 | 590.112 ms |
| 0529 | 86, 426, 643 | 858.433 ms |

## Rendering verification

All 53,535 pages have complete comparison coverage, with no comparison errors.
The final check reuses 52,784 earlier comparisons only where
pixel SHA-256 hashes and dimensions match exactly. It renders and compares the
other 751 pages again. Every replay hash matches the final
measured run. Replay batches contain at most 32 pages per worker.

Image comparison permits a two-pixel rasterizer displacement. It measures
missing and extra ink, blurred color error, and large color differences. A flag
requests review; it is not itself a rendering failure. A small raster difference
can produce a large ink ratio on a nearly blank page.

Review used representative flagged pages for each affected file, plus targeted
checks for new flags and material increases. This was not a manual reading of
all 53,535 pages. Earlier and final review records are retained. The final
register accounts for every remaining flag.

Two earlier verification attempts reported five batch hash mismatches in file
0341. The reported pages were 54, 115, 138, 175, and 200. Direct PNG and BMP
checks matched the measured pixels. Sixteen concurrent batches also matched
all 512 rendered pages. An instrumented full verification and three additional
full-document checks then matched all 265 pages each time. The final accepted
comparison has no errors.

The cause of the earlier mismatches was not determined. This remains an
unresolved verification observation. The original failure logs are retained.
The audit tool now saves any future mismatching bitmap and both pixel records
before another batch can overwrite them. Its acceptance checks are unchanged.

### Remaining image differences

The remaining 201 flagged pages are in 62 files. Reviewed differences
include thin line and text weight, antialiasing, image sampling, near-blank page
metrics, and color-profile differences. The corrected gray mask background in
file 0267 is present; its journal-cover images retain color differences from
MuPDF. Small differences in stroke darkness remain in some technical figures.

The renderer still reports display approximations for overprint, knockout
groups, and some transfer or interpolation functions. Full per-file warnings
are in the file table. The audit checks this collection at 96 dpi. It does not
establish complete PDF-standard conformance or identical output at all zoom levels.

The last image decoding optimizations were also checked by exact pixels:
all 53,535 pages in runs 3 and 4 match. A separate reference comparison then
found a thin stencil-border fault from an earlier change. The fix restored
fractional stencil and vector coverage. Final review also found the scanned-page
fringe and added the boundary-pixel correction. A later full run found that
this correction made some Type 3 bitmap glyphs too dark. The final change keeps
fractional coverage inside glyph programs and restores the normal text weight.
All three corrections are included here.

## Fixes and optimizations

| Change | Reason and result | Commit |
| --- | --- | --- |
| Resumable page audit | Save each page and its timing. Continue after a failed page. | `9c79343` |
| Shared font data | Reuse glyph-name lookup data and shared Type 1 programs. | `a12c3ee` |
| Image color mapping | Apply PDF Decode and color-space rules to gray JPEG and indexed JPEG 2000 samples. | `2036605` |
| JBIG2 context reuse | Reuse rolling pixel contexts in arithmetic-coded generic regions. | `6539456` |
| Font and stream recovery | Use native Type 1/CFF encodings, apply encoding differences in order, and avoid heavy CFF hinting. Do not decrypt inline images twice. Recover a truncated Flate suffix only when its checksum is valid. | `374867f` |
| Paths, stencils, and gradients | Handle degenerate paths, reuse brushes and stroke styles, cache Type 3 programs and stencil bitmaps, and limit gradient rasterization to the visible region. | `374867f` |
| Fax and CMYK JPEG | Decode byte-aligned Group 4 fax rows. Restore PDF sample polarity before CMYK JPEG color conversion. | `84d705f` |
| Outline lookup | Build a named-destination index once instead of searching the name tree for each outline entry. | `b91603d` |
| DEFLATE stack use | Build large Huffman tables on the heap. This removes a sanitizer stack overflow on damaged streams. | `42d6996` |
| Display colors and symbols | Apply bilevel image palettes, map symbolic TrueType glyphs from PDF character codes, and use the installed Windows SWOP profile for Device CMYK display colors. | `8e844c7` |
| Dense vector pages | Reuse numeric operand storage, parse common tokens directly, reuse tint-function gradient ramps, and use native rectangular clipping. Keep very thin strokes visible. | `8e844c7` |
| Audit integrity | Check executable and input identities before resuming. Reuse reference images only at the same scale and reference version. | `81ae2b9` |
| Image masks and JPEG 2000 tints | Give image masks priority over the graphics soft mask. Cache single-channel JPEG 2000 tint colors. | `430ed5d` |
| Pixel verification | Store a SHA-256 hash of the rendered pixels outside the page timer. | `06108ca` |
| Image conversion and checksums | Use SSE2 for Adler-32 and CMYK table interpolation. Cache calibrated RGB gamma samples. | `fbbdb6c` |
| Transparency groups | Retain the backdrop for non-normal group blends. Calculate group alpha before backdrop removal. | `9ed2477` |
| Macintosh font maps | Read embedded symbolic TrueType Macintosh cmap tables directly. Keep ToUnicode separate from glyph selection. | `c760acf` |
| Font regression data | Use different glyph heights so the tests can detect a wrong character map. | `eb0467e` |
| Image strips and page blending | Remove white seams between axis-aligned image strips. Blend the page on a transparent surface, then place it on white. This restores missing blend-mode examples. | `0e0adc3` |
| Image-strip regression data | Check fractional strip edges at several pixel phases. | `5ba359b` |
| Fax sample columns | Decode the fax stream as packed bytes before applying image bit depth, color maps, and masks. This restores equations in both Intel MKL manuals. | `3e939b6` |
| Scanned stencil edges | Cover the full boundary pixels of large stencil images. This removes gray frames around scanned pages. Keep fractional coverage for stencils narrower than one display pixel. | `06d3741` |
| Type 3 bitmap coverage | Keep fractional bitmap coverage inside glyph programs. This prevents the scan-edge correction from making text too dark. Restore the image state after each glyph. | `23b77b0` |
| Repeated image tiles | Reuse stencil brushes. Skip clip-mode changes when the image is fully inside the clip. This removes most clip resets on a page with more than 30,000 small stencil tiles. | `23b77b0` |
| Thin stencils and clip borders | Preserve fractional coverage for one-pixel stencil rules and vector clip edges. Use whole-pixel clip edges only while drawing ordinary image tiles. This restores table and figure borders without bringing back strip seams. | `015d0e1` |
| Image conversion and prediction | Reuse repeated CMYK conversions, bypass identity RGB lookups, and copy unchanged Paeth blocks exactly. Use compact Huffman tables and direct stored-block copies. | `72dbc4f` |
| Bounded image replay | Replay at most 32 pages per worker and verify every replayed pixel hash before comparison. | `426a306` |
| Hash comparison across builds | Compare complete render hashes to prove that the last optimizations preserve page pixels. | `fc1451b` |
| Replay failure evidence | Keep the actual bitmap, expected and actual hashes, dimensions, and executable identity when a replay differs. Reject the replay as before. | `3982068` |
| Final verification and reports | Compare exact pixel hashes, check changed pages again, and export complete coverage and timing tables. | `5fcfb29` |

The baseline has 44 failed pages and 1,856 image-comparison flags, including the
failed pages. Its timing records contain 1,436 pages above 500 ms and two opens
above 500 ms.

The corrected cases include black fax charts, inverted CMYK covers, wrong
grayscale tint colors, garbled symbolic fonts, missing thin plot lines, encrypted
inline images, truncated compressed streams, and large vector pages rejected by
the old parser budget. Later review also found image-mask priority errors, white
seams between image strips, missing shapes in blend-mode examples, and equations
replaced by black bars in an eight-bit fax image.

The mask and transparency rules were checked against
[PDF 32000-2, sections 11.4 and 11.6](https://developer.adobe.com/document-services/docs/assets/5b15559b96303194340b99820d3a70fa/PDF_ISO_32000-2.pdf).
The font-map reader follows the
[OpenType cmap formats](https://learn.microsoft.com/en-us/typography/opentype/spec/cmap).



## Selected rendering checks

File IDs refer to the complete file table in the local evidence folder.

| File and page | Fault found | Check after the fix |
| --- | --- | --- |
| 0258 page 8; 0259 page 25 | A symbolic font used the wrong character map. Text was garbled. | Read the Macintosh cmap and check owned fonts with distinct glyph heights. |
| 0267 page 17 | A graphics soft mask removed the gray image background. | Apply the image mask first and compare the page with the reference. |
| 0414 and 0415 page 223 | White horizontal seams split adjacent image strips. | Use consistent pixel coverage at strip edges. Missing-ink error fell from about 11.4% to 0.004%. |
| 0489 page 343 | Page blending hid example shapes. | Blend on a transparent page before adding white paper. Missing-ink error fell from about 30.1% to zero. MuPDF and PDFium show the same shapes. |
| 0237 and 0333 page 494 | Eight-bit fax samples became black bars over equations. | Decode packed fax bytes before the image color map. All 44 flagged pages in the two manuals passed a new comparison. |
| 0048 and 0049 page 64; 0084 pages 35 and 124; 0092 and 0093 page 26 | Whole-pixel image edges removed thin stencil rules. | Restore fractional stencil coverage. All table and figure borders reappear. Eight new fixtures also check vector clipping, form bounds, opacity, and nested image tiles. |
| 0044 and 0045, including pages 4, 11, 219, 232, and 811 | A white stencil left a gray frame over its black base. | Extend the stencil sampling to the full boundary pixels. All five checked pages have zero extra-ink error. Six phase, reflection, and rotation fixtures fail before the fix and pass after it. MuPDF agrees. |
| 0036 page 4; 0341 page 250 | Full bitmap-edge coverage made Type 3 text too dark. | Preserve fractional glyph coverage. Check all 539 affected pages and the new glyph fixture. |
| 0347 page 1 | More than 30,000 stencil tiles caused repeated clip resets. | Skip interior clip resets and reuse brushes. Three quiet samples give a median of 376.738 ms, down from 1,092.034 ms in the preceding scan-edge build. The output pixels are identical to that build. |

The measurements in this table are targeted checks. The final full-page run
provides the acceptance counts. Original fixtures also check normal and reversed
fax samples, row sizes, RGB samples, indexed colors, masks, and Decode arrays.



## Validation

The final Release build completed without compiler warnings. All nine CTest
groups passed in 125.82 seconds. All nine AddressSanitizer groups
passed in 179.87 seconds. The generated PDF suite contains
132 cases, including the new stencil edge, thin-rule, glyph-coverage, and brush-reuse tests.

The final stencil correction also passed eight real-page AddressSanitizer checks.
Earlier sanitizer checks covered the large vector page in file 0413, encrypted
inline images, repaired compressed streams, fax images, symbolic fonts, page
blends, masks, and the large C++ outline. Sanitizer times are not performance results.

A deliberate wrong-hash check verified the new audit failure capture. The tool
rejected the bad measurement, saved the matching bitmap evidence, accepted the
correct measurement, and retained the earlier failure record.

Additional validation includes 1,000 PNG predictor byte cases, 28 exact-pixel
CMYK/RGB/zlib checks, 45 truncated-stream warning and pixel checks, and owned
font and image fixtures for the faults found in this collection. The eight
border fixtures were checked with MuPDF. Six new scanned-stencil fixtures fail
on the preceding executable and pass after the final correction; MuPDF confirms
their boundary pixels. The seventh new fixture preserves a narrow multi-pixel stencil.
Two further fixtures check Type 3 glyph coverage and reuse of stencil brushes with different
colors, opacity values, and rotations.

The Type 3 follow-up checked 539 affected pages. On 458 pages, the final pixels
match the earlier text rendering exactly. The other 81 pages have no comparison
flags and no material increase in error. The full final run checks all pages again.

Input SHA-256 and directory-membership checks completed at
`2026-10-22T08:27:24+02:00`. All 562 hashes match the original inventory.
There are no added or missing PDFs.

## Build identity and local evidence

Final runtime source commit: `23b77b045dcf840e4066da5f86f0eabb212204a0`.
Final audit-tool commit: `3982068`.
Frozen executable: `artifacts/articles-run/final7-pdfprobe.exe`.
Executable SHA-256:
`c52606e4e903c00b5e855f6d570df678b4e1565405a7b182e63bdc4995cb7d9a`.

The report is committed. Generated images, executables, and measurement data
remain in the ignored local evidence folder. The source PDFs are not copied
into the repository.

- [All 562 files and load times](../artifacts/articles-run/report/files.csv)
- [All 53,535 page times and comparison metrics](../artifacts/articles-run/report/pages.csv)
- [Serial pages above 500 ms](../artifacts/articles-run/report/slow-pages.csv)
- [All repeated timing samples and profiles](../artifacts/articles-run/timing-v7/repeated-times.json)
- [Paired baseline and final measurements](../artifacts/articles-run/paired-times-v7.json)
- [Final coverage and statistics](../artifacts/articles-run/report/summary.json)
- [Final review register](../artifacts/articles-run/final-review-v7/review-register.json)
- [Remaining flagged pages](../artifacts/articles-run/report/flagged-pages.csv)
- [Source-file integrity check](../artifacts/articles-run/input-integrity-final.json)
- [Release test log](../artifacts/articles-run/stencil-final-build.log)
- [Sanitizer test log](../artifacts/articles-run/stencil-final-asan-build.log)
- [Scanned-stencil regression evidence](../artifacts/articles-run/stencil-bounds-check/fixtures-results.json)
- [Scanned page comparison](../artifacts/articles-run/stencil-bounds-check/0044-0232-pair.png)
- [Replay mismatch investigation](../artifacts/articles-run/final-review-v7/replay-investigation/summary.json)
- [Replay failure-capture test](../artifacts/articles-run/replay-capture-test/results.json)
- [Type 3 pixel recovery](../artifacts/articles-run/stencil-final-check/type3-recovery.json)
- [Type 3 remaining page comparisons](../artifacts/articles-run/stencil-final-check/remaining-comparisons.json)
- [Final real-page sanitizer results](../artifacts/articles-run/stencil-final-asan-corpus.json)
- [Stencil tile timing checks](../artifacts/articles-run/stencil-tile-paired.json)
- [Discarded clip optimization measurements](../artifacts/articles-run/interior-clips-paired.json)

## Reproduce the audit

These commands use the retained inventory, frozen executable, and development
reference package. Use a new output folder for a new executable. Use `--resume`
only with the same executable, inventory, and render scale.

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\build.ps1 -Configuration Release -Sanitize
python scripts/audit-articles.py --inventory artifacts/articles-run/inventory.json --exe artifacts/articles-run/final7-pdfprobe.exe --reference-path artifacts/oracle-mupdf --output artifacts/articles-run/final-v7 --hash-only --skip-reference --resume
python scripts/verify-audit-images.py --inventory artifacts/articles-run/inventory.json --previous artifacts/articles-run/final-v6 --current artifacts/articles-run/final-v7 --exe artifacts/articles-run/final7-pdfprobe.exe --reference-path artifacts/oracle-mupdf --reuse-reference artifacts/articles-run/baseline --workers 8 --resume
python scripts/check-audit-times.py --inventory artifacts/articles-run/inventory.json --pages artifacts/articles-run/timing-v7/recheck-cases.csv --exe artifacts/articles-run/final7-pdfprobe.exe --output artifacts/articles-run/timing-v7/repeated-times.json --repeats 3
python scripts/summarize-articles.py --inventory artifacts/articles-run/inventory.json --baseline artifacts/articles-run/baseline-compact.json --final artifacts/articles-run/final-v7 --output artifacts/articles-run/report
```
