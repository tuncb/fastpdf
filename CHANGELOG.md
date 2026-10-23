# FastPDF changelog

Notable changes by release tag, newest first.

## v0.4.1

Maintenance release. No notable application changes.

## v0.4.0

- Show surrounding text in search results and display each match in bold.
- Make large search result lists available through Windows UI Automation.
- Use GPU rendering to keep PDF pages responsive when resizing the window or sidebar.
- Open PDFs with the standard Windows file picker.

## v0.3.0

- Preview pages in the print dialog before printing.
- Resize sidebars and collapse the bookmarks pane.
- Use consistent theme-aware styles in dialogs.
- Search as you type, use regular expressions, and get responsive results on large queries.

## v0.2.0

- Blue lightning F app icon for the window, taskbar, executable, and Start menu shortcut.
- Compact dark toolbar with editable page and zoom fields, clearer control states, and improved sidebar navigation.
- Open the bookmark sidebar when a document has bookmarks. Keep its selection in step with the current page, and load large bookmark lists faster.
- Dark menu bar and drop-down menus with readable shortcuts, selection highlights, and disabled commands.
- Compact dark About dialog with the app version, GitHub link, and a button for third-party notices.
- Open PDFs with Unicode names and long file paths, including explicit Windows extended paths.
- Correct embedded Type 1 and CFF fonts, Type 3 character spacing and bitmap coverage, and symbolic TrueType character maps.
- Correct JPEG and JPEG 2000 image colors, CMYK conversion, and fax-compressed scans and equations.
- Correct transparency group blending and image mask priority. Preserve thin rules, clipped borders, and image tile edges without gaps or dark fringes.
- Render dense page content and recover usable data from truncated Flate streams. Prevent a stack overflow during image decompression.
- Reduce page render time through faster image decoding and color conversion, shared font data, and reuse of image tile drawing resources.
- Install FastPDF for your Windows account from a compact dark dialog, with an optional Start menu shortcut and uninstall support in Windows Installed Apps.
- Choose FastPDF as the default PDF reader during installation through Windows Default apps, and open PDFs from File Explorer's Open with menu.
- Check for updates manually from Help. Downloads are verified before installation, with options to restart now or install when the reader closes. No automatic checks, startup app, or tray icon are added.
- Reopen the current PDF at the same page, zoom mode, and rotation after an update. Passwords are not saved; protected documents may ask again.
- Continue using the single executable portably without installing it.
- Download and run `fastpdf.exe` directly. Releases now contain only the standalone Windows x64 reader and `SHA256SUMS.txt`, replacing the ZIP bundle.
- View bundled data licenses in Help → Third-party notices without separate documentation files.
- Remove the reader's PNG export and Document information commands. PNG export and diagnostic information remain available through `pdfprobe`.
- The `pdfprobe` command-line diagnostic tool remains available in source builds and is no longer included in release downloads.
- Add render stage timings, page range rendering, pixel hashes, and resumable collection audits to the source tools.

## v0.1.0

- Initial portable release for Windows x64.
- Native PDF reader with page navigation, outlines, links, zoom, rotation, text search and selection, PNG export, and printing.
- Built-in PDF engine with password-protected document support, embedded fonts, transparency, and JPEG 2000 and JBIG2 decoding.
- Command-line PDF inspection, text extraction, rendering, and validation with `pdfprobe`.

See [COMPATIBILITY.md](docs/COMPATIBILITY.md) for known PDF compatibility limits.
