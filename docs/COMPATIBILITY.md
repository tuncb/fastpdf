# Compatibility status

This file describes implemented behavior and known gaps. Opening a file successfully does not prove that every mark on every page is correct. `pdfprobe` reports detected rendering gaps in its JSON output; `pdfprobe --all --strict` turns recorded compatibility warnings into a nonzero exit code.

## Implemented

- PDF lexical objects, indirect references, classic xrefs, xref streams, hybrid xrefs, incremental updates, object streams, linearized files through their ordinary xrefs, and recovery scanning for damaged xrefs.
- Inherited page resources, MediaBox/CropBox, page rotation, UserUnit, page trees, outlines, named/internal destinations and external URI links.
- Standard encryption revisions 2–6: user and owner passwords, 40/128-bit RC4, AES-128, AES-256, crypt filters and encrypted object streams. R6 includes password mapping, normalization, prohibited-character and bidirectional checks. Legacy PDFDocEncoding passwords and UTF-8 fallback are tried.
- Flate/DEFLATE, LZW with EarlyChange 0/1, ASCII85, ASCIIHex, RunLength, TIFF predictors and PNG predictors, including packed/16-bit samples.
- Main PDF path, graphics-state, clipping, text and XObject operators; embedded TrueType/OpenType/CFF; Type 1 and Type 3; CID fonts, ToUnicode maps, predefined CJK CMaps and vertical metrics.
- Raw/inline images, packed samples, indexed palettes, image stencils, color-key masks, image soft masks and Matte; JPEG and CCITT through Windows codecs.
- Owned JPEG 2000 decoding: JP2/raw codestreams, reversible 5/3 and irreversible 9/7 wavelets, MQ codeblocks, tiling, quality layers, all five progression orders, component transforms, subsampling and embedded alpha.
- Owned JBIG2 decoding: embedded/global segments, arithmetic and Huffman dictionaries, standard/custom Huffman tables, text/aggregation/refinement, generic regions, pattern dictionaries, halftones, typical prediction and combination operators. MMR substreams use the Windows CCITT codec.
- DeviceGray/RGB/CMYK, Indexed, calibrated spaces, Lab, ICC alternate spaces, Separation and DeviceN tint transforms; function types 0/2/3/4; tiling patterns; shading types 1–7, including Coons and tensor patches.
- Alpha, image/graphics soft masks, the 16 standard blend modes, isolated transparency groups, and non-isolated groups with normal group composition, including group opacity and masks.
- Optional-content default visibility, annotation appearance streams, link/highlight/underline/strikeout/square fallback rendering, and generated basic text, choice and button field appearances.

## Remaining differences and unsupported features

- **Universal conformance has not been established.** The exercised corpus is finite. Unusual fonts, malformed producer output, exotic color profiles and less common codecs can still fail or render incorrectly.
- **JPEG 2000:** bypass/terminated codeblock styles, ROI shifts, progression-change markers, packed packet headers, JP2 palette/channel-mapping boxes, HTJ2K and Part 2 extensions remain unsupported. Signed and high-bit-depth images have less validation than ordinary 8-bit unsigned images. Detected unsupported codestream features report a warning instead of invoking another PDF engine.
- **JBIG2:** extended generic templates remain unsupported. Embedded sequential streams, standalone sequential/random-access headers, and unknown-length generic-region row counts are handled. Some damaged streams may be rejected. The arithmetic/Huffman/refinement/halftone paths have been exercised against the standard's examples, but not every legal parameter combination.
- **Color:** embedded ICC profiles use their alternate/device color space. Device CMYK uses the installed Windows SWOP profile and a cached display table when that profile is available; otherwise it uses a simple RGB approximation. Full embedded ICC transforms, calibrated white-point handling and overprint remain incomplete. Graphics transfer functions are not applied.
- **Transparency:** non-isolated groups with a non-normal group blend mode use an isolated backdrop, and knockout groups use non-knockout composition. These cases emit notes. Non-isolated groups with normal group composition preserve the actual backdrop and apply group opacity/masks to the resulting premultiplied color and alpha. Other PDF group shape/alpha/backdrop semantics need further work.
- **Functions and meshes:** sampled functions with Order 3 use linear interpolation and report a note. Patch meshes use bounded tessellation, not an exact analytic surface rasterizer.
- **Fonts and text:** Base14 substitution uses installed Windows fonts. Rare Type 1/CFF operators and unusual encodings need broader validation. Text without useful Unicode information may be inferred or unavailable. Extraction preserves content-stream order rather than reconstructing a tagged reading order; selection is based on text spans, not individual glyphs.
- **Forms and annotations:** existing appearances are preferred. Generated appearances use a system font and cover common values; complex comb/list layouts, rich text, JavaScript calculations and interactive editing/saving are not implemented. Some annotation types require a stored appearance. Search can find stored/generated widget text, but has no OCR.
- **XFA and active content:** dynamic XFA layout, JavaScript, multimedia, 3D, launch actions and embedded-file execution are not implemented. XFA files display any available stored page representation and report a note.
- **Security/signatures:** certificate/public-key security handlers and digital-signature verification are not implemented. Drawing an existing signature appearance does not validate the signature. Password normalization uses Windows NFKC with frozen Unicode 3.2 profile tables; rare historical normalization corrections can differ.
- **Reader:** single-page view; no editing, annotations authoring, document saving, tabs or continuous-page layout. Printing is rasterized and has not been tested against a physical printer. Print-specific annotation visibility and advanced print-production controls are incomplete.
- **Resource limits:** exceptionally large pages, image streams, deeply nested structures or huge object counts may hit documented allocation/work limits. These limits are intentional and adjustable in code, but prevent a claim to open every PDF.

## Highest-value next compatibility work

1. Expand differential tests with representative business, CJK, engineering, scanned and accessibility PDFs, including page-level reference images and expected text.
2. Complete JPEG 2000 codeblock segmentation/ROI/packed headers and JP2 palettes; extend JBIG2 edge-case fixtures and templates.
3. Complete non-normal group-blend/knockout transparency and managed ICC/overprint behavior.
4. Improve annotation/form appearance generation and difficult embedded-font cases.
5. Isolate rendering in a restricted helper process and run sustained coverage-guided fuzzing before treating the reader as production hardened.
