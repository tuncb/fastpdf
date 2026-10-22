# FastPDF

A native Windows PDF reader written in C++20, with a PDF engine implemented in this repository. The code uses structs and free functions; there are no application classes with private or protected state.

The application opens local PDFs, renders pages, navigates outlines and links, searches and copies text, rotates and zooms pages, and prints. Password-protected documents use the standard Windows password dialog supplied by the application.

**This is a working initial implementation, not a claim of universal PDF conformance.** The engine covers a substantial part of PDF 1.7 and several later features. The remaining compatibility limits are recorded in [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md). In particular, dynamic XFA documents and some advanced rendering and image-codec features still need work. Render warnings are available through the probe's JSON output.

## Build and run

Requirements: x64 Windows 10 version 1703 or later, Visual Studio with Desktop development with C++, a Windows SDK, CMake 3.24+, Ninja, and Git. Build from a Git checkout so the executable can embed its source commit. Python is optional for building the app and is used for the integration tests.

```powershell
.\scripts\build.ps1 -Configuration Release
.\build\fastpdf.exe
.\build\fastpdf.exe C:\documents\example.pdf
```

The build script configures the Visual Studio environment, compiles the app and tools, and runs all available CTest checks. The release uses the static Microsoft C/C++ runtime. No third-party C++ packages need to be installed or downloaded.

The reader and console tools support Unicode file names. Paths longer than 260
characters work when the Windows `LongPathsEnabled` setting is enabled. Explicit
extended paths, such as `\\?\C:\documents\example.pdf` or
`\\?\UNC\server\share\example.pdf`, do not require that setting. You can enter
these paths in **File → Open** or pass them on the command line. `pdfprobe` also
supports long paths for PNG and text output. File system limits on each file or
folder name still apply.

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
in the executable and available from **Help → About… → Third-party notices…**.
The local release files are written to `artifacts/release/`.
See [CHANGELOG.md](CHANGELOG.md) for release notes and [AGENTS.md](AGENTS.md) for
the versioning and tag-based release process.

The app icon source is `src/assets/fastpdf.png`. To regenerate the checked-in Windows icon,
install Pillow and run `python scripts/generate-icon.py`. Normal builds use the existing ICO
and do not need Pillow.

## Installation and manual updates

The downloaded `fastpdf.exe` works portably. On the empty welcome screen, choose
**Install FastPDF** or **Continue portable**. Opening a PDF directly goes straight
to the document. Installation is also available from **Help → Installation…** or
`fastpdf.exe --install`.

Installation copies the reader to `%LOCALAPPDATA%\Programs\FastPDF`, optionally
creates a Start menu shortcut, and registers FastPDF in Windows **Installed Apps**.
It needs no administrator privileges and makes FastPDF available in **Open with**.
Select **Use FastPDF as the default PDF reader** to open Windows Default apps after
installation and confirm FastPDF for `.pdf` files. This option starts unchecked;
your existing default is kept until you confirm a change in Windows Settings.
The Installation dialog also lets you change the shortcut option or uninstall. You can
also uninstall through Windows Settings or `fastpdf.exe --uninstall`. Uninstall
removes FastPDF's files and registration and leaves your PDFs alone.

Use **Help → Check for updates…** from the installed reader. FastPDF checks the
latest stable GitHub release only when requested, then asks before downloading.
The download runs separately from document rendering.
FastPDF verifies the executable's SHA-256 checksum, Windows
x64 architecture, version, and required embedded resources.

Once an update is ready, choose **Restart now**, **Install when I close FastPDF**,
or **Later**. Close other FastPDF windows before installing. Restart restores the
current document, page, zoom mode, and rotation; document passwords are never
saved. Choosing Later keeps the download for the current session; closing without
choosing installation discards it.
Portable copies must be installed to use the updater, or replaced with a fresh
download from GitHub Releases.

The executable briefly runs a temporary copy of itself to finish an update or
uninstall after the reader exits. Replacement retains the previous executable until
the new reader starts successfully. Temporary maintenance files are cleaned when
possible, with any still-running helper cleaned on a subsequent FastPDF launch.
There are no automatic update checks, scheduled tasks, login startup entries,
background services, or tray icons.

## Reader controls

- Open: Ctrl+O or drag a PDF into the window.
- Pages: Page Up / Page Down, left / right, Home / End; Ctrl+G for a page number.
- Zoom: Ctrl+mouse wheel or + / −. F fits the page; W fits its width; R rotates it.
- Find: Ctrl+F. Search starts as you type. Enter or F3 selects the next match;
  Shift+Enter or Shift+F3 selects the previous match. Escape closes search.
  Use Results to open the list of matches, or Options to match case, find whole
  words, or search only the current page. Switch between Pages, Bookmarks, and
  Results with the sidebar tabs. Switching tabs keeps the search active.
- Selection: drag over text, then Ctrl+C. Ctrl+A selects the current page's extracted text.
- Pan: middle-button drag. Scroll: mouse wheel, arrows, Space, or scrollbars.
- Bookmarks: B or the bookmark icon. Links support document destinations and HTTP, HTTPS, and mail links.
- Print: Ctrl+P. The print dialog includes a page preview with Previous and Next buttons.
  The preview shows PDF page content and rotation. Printer margins and paper settings are not shown.
- Exit: Alt+F4 or File → Exit.
- About: F1 or the Help menu.
- Installation and manual updates: Help menu.

The compact toolbar groups file, page, zoom, and view controls into small icons. Hover over an icon for
its name and shortcut. Enter a page number or zoom percentage (5–800%) in the toolbar and press Enter;
Escape cancels the edit. F6 focuses the toolbar, arrow keys move between its buttons, and Tab moves
between the toolbar, page and zoom fields, search, sidebar tabs, and visible list.
Use the arrow keys on the tabs to change the sidebar view. The main menus include
any actions hidden when the window is narrow. Document controls are disabled
until a PDF is open.

The reader uses neutral dark surfaces, Segoe UI text, and blue keyboard-focus outlines.
Dialogs use a shared Segoe UI style and follow the Windows light or dark app theme.
Open dialogs update their colors when the Windows theme changes. The page list
keeps focus while you navigate with the arrow keys; Escape returns focus to the document.
Bookmarks use a tree with collapsed branches when you open a document. Click an arrow, or use
the Left and Right keys, to collapse or expand a branch. Drag the right edge of the Bookmarks
or Pages pane to change its width. The default width is 320 pixels at 100% display scaling.
The Results tab is available while Find is open. Closing Find returns to the last Pages or
Bookmarks view. Bookmark branches keep their expanded or collapsed state when you switch tabs.

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

For a long corpus check, use `--render-dir DIR` to save each page and
`--progress FILE` to write an open record and each page result as JSON lines.
Each progress record is flushed to disk. `--render-format bmp` selects fast,
uncompressed image output. Image output time is outside the render timer.
`--last-page N` sets the end of a page range that starts at `--page`.
`--continue-on-error` keeps checking pages after a render error and returns exit
code 2 if a page fails. The summary lists these errors in `samples` and gives
their count in `failed_pages`.

`scripts/audit-articles.py` uses these options to check a local corpus against
PyMuPDF. It requires Pillow, NumPy, SciPy and PyMuPDF as development tools. It
records every page result, warnings, timing, and image differences. The image
comparison identifies pages for review; it is not a proof of PDF conformance.
Its measurements use the existing file cache and do not measure cold-disk
latency or the full time from GUI launch to display.

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
