# FastPDF

A native Windows PDF reader written in C++20, with a PDF engine implemented in this repository. The code uses structs and free functions; there are no application classes with private or protected state.

The application opens local PDFs, renders pages, navigates outlines and links, searches and copies text, rotates and zooms pages, exports PNGs, and prints. Password-protected documents use the standard Windows password dialog supplied by the application.

**This is a working initial implementation, not a claim of universal PDF conformance.** The engine covers a substantial part of PDF 1.7 and several later features. The remaining compatibility limits are recorded in [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md). In particular, dynamic XFA documents and some advanced rendering and image-codec features still need work. Render warnings are available under Document information and through the probe's JSON output.

## Build and run

Requirements: x64 Windows 10 version 1703 or later, Visual Studio with Desktop development with C++, a Windows SDK, CMake 3.24+, Ninja, and Git. Build from a Git checkout so the executable can embed its source commit. Python is optional for building the app and is used for the integration tests.

```powershell
.\scripts\build.ps1 -Configuration Release
.\build\fastpdf.exe
.\build\fastpdf.exe C:\documents\example.pdf
```

The build script configures the Visual Studio environment, compiles the app and tools, and runs all available CTest checks. The release uses the static Microsoft C/C++ runtime. No third-party C++ packages need to be installed or downloaded.

To build from an existing x64 Visual Studio developer shell:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To prepare the standalone executable and checksum after a successful build:

```powershell
.\scripts\package.ps1
```

Packaging requires Python 3.10 or newer. Download Windows x64 builds from
[GitHub Releases](https://github.com/tuncb/fastpdf/releases). Each release includes
`fastpdf.exe` and `SHA256SUMS.txt`. Download and run `fastpdf.exe` directly;
there is no installer, extraction step, or companion file required. The checksum
file is optional and lets you verify the download. Third-party notices are embedded
in the executable and available under **Help → Third-party notices**.
The local release files are written to `artifacts/release/`.
See [CHANGELOG.md](CHANGELOG.md) for release notes and [AGENTS.md](AGENTS.md) for
the versioning and tag-based release process.

## Reader controls

- Open: Ctrl+O or drag a PDF into the window.
- Pages: Page Up / Page Down, left / right, Home / End; Ctrl+G for a page number.
- Zoom: Ctrl+mouse wheel or + / −. F fits the page; W fits its width; R rotates it.
- Find: Ctrl+F; Enter searches; F3 starts the next search from the following page.
- Selection: drag over text, then Ctrl+C. Ctrl+A selects the current page's extracted text.
- Pan: middle-button drag. Scroll: mouse wheel, arrows, Space, or scrollbars.
- Bookmarks: B or Outline. Links support document destinations and HTTP, HTTPS, and mail links.
- Export the current page: Ctrl+S. Print: Ctrl+P.
- Document information and compatibility notes: F1 or the File menu.

The UI renders in a worker thread, cancels obsolete render/search requests, caches page bitmaps, and prefetches adjacent pages when idle. Search reads text without rasterizing the page's images and graphics. The current UI displays one page at a time.

## Engine and command line

```powershell
.\build\pdfprobe.exe example.pdf --info
.\build\pdfprobe.exe example.pdf --page 3 --render page3.png --scale 2
.\build\pdfprobe.exe example.pdf --page 3 --text --extract page3.txt
.\build\pdfprobe.exe example.pdf --all --strict
.\build\pdfprobe.exe example.pdf --page 1 --benchmark 20
.\build\pdfprobe.exe example.pdf --page 10 --benchmark 3 --profile
.\build\pdfprobe.exe protected.pdf --password secret --render page.png
```

`pdfprobe` is a developer command-line tool built alongside the reader and used by
the integration tests. The reader does not launch or depend on it. From v0.2.0
onward it is available in source builds, rather than the GitHub release download.

`pdfprobe` writes JSON with page counts, open/render timings, memory use, extracted text when requested, and compatibility warnings. Exit codes: 0 success, 2 error, 3 warnings with `--strict`. `--all` validates every page; PNG and text output refer to the selected `--page`. The benchmark includes the first render and subsequent renders with the engine's resource caches populated; the GUI also has a separate rendered-page cache. `--timings` separates individual samples, and `--profile` breaks down decoding work. `--full-resolution` disables resolution-aware image decoding, and `--image-workers 1` selects serial decoding. See the [measurement notes](docs/ARCHITECTURE.md#measuring-rendering) for the reproducible benchmark harness.

The public procedural API is in [src/pdf.h](src/pdf.h) and [src/render.h](src/render.h). See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for ownership, threading, resource limits, and optimization choices.

## Dependencies and ownership

The parser, object and cross-reference handling, stream filters, PDF encryption logic, content interpreter, font mapping, Type 1 outlines, color/function interpretation, gradients and meshes, transparency composition, JPEG 2000 decoder, and JBIG2 decoder are repository code.

Windows supplies windowing, Direct2D rasterization, DirectWrite font rasterization, Windows Imaging Component JPEG/CCITT/PNG support, and CNG cryptographic primitives. These are operating-system APIs. The shipped reader does not use PDFium, MuPDF, Poppler, Ghostscript, OpenJPEG, libjpeg, zlib, or a web view.

Adobe's published glyph and CMap **data** are embedded to cover legacy and CJK fonts; their licenses are in [docs/FONT-DATA-LICENSE.txt](docs/FONT-DATA-LICENSE.txt). Unicode property data used for password preparation are covered by [docs/UNICODE-LICENSE.txt](docs/UNICODE-LICENSE.txt). Generated test images are project-owned. Development reference decoders and downloaded external PDFs stay under the ignored `artifacts/` directory and are not shipped.

## Verification

```powershell
.\scripts\build.ps1 -Configuration Release -Sanitize
```

CTest checks parser/filter/crypto/function/CMap primitives, generated PDFs with text and pixel assertions, and independently decoded JPEG 2000 pixel fixtures. Tests cover classic, stream, hybrid, and incremental xrefs; encrypted revisions 2–6; embedded fonts; masks; patterns; all seven shading types; JPEG 2000; shared JBIG2 symbols; forms; and malformed inputs. Test generation uses Python's standard library. Regenerating the checked-in JPEG 2000 fixture assets additionally needs Pillow, solely as a development encoder/reference decoder.

[docs/VALIDATION.md](docs/VALIDATION.md) records the actual validation run and its limits. `tests/fuzz_smoke.py` provides deterministic mutation checks, and `tests/jpx_oracle.py` provides a broader optional JPEG 2000 differential test. These are not a claim of exhaustive conformance or a completed security audit.
