# Corpus regression data

The following files are original test assets. They contain no pages or font
programs copied from the article collection.

- `symbol-subset.ttf` contains two rectangular glyphs. Its Windows symbol cmap
  maps codes `F001` and `F041` to different rectangles. This tests a collision
  between a PDF character code and a glyph-name Unicode value.
- `mac-subset-0.ttf`, `mac-subset-4.ttf`, and `mac-subset-6.ttf` use the same
  rectangles with Macintosh cmap formats 0, 4, and 6. PDF code 1 must select
  the left rectangle, even when its ToUnicode value is A (code 65).
- `thin-stem.cff` contains simple test outlines. It tests the native encoding and
  the display weight of a small CFF glyph.
- `cmyk-panels.jpg` contains four constant CMYK panels. It tests JPEG sample
  polarity, the PDF Decode array, and display color conversion.
- `gray-tints.jpg` contains grayscale samples. It tests PDF tint and Decode
  processing after JPEG decoding.
- `fax-aligned.json` contains ten generated T.6 streams with byte-aligned rows.
  Expected pixel hashes were checked with MuPDF. The codes follow the tables in
  [ITU-T T.4](https://www.itu.int/rec/T-REC-T.4/en).
- `jbig2-generic.json` contains generated arithmetic-coded generic regions.
  Expected pixel hashes were checked with MuPDF.

Reference decoders are development tools. FastPDF does not link to them.
