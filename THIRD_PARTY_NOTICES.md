# Third-Party Notices

TorReader's own code is MIT-licensed (see `LICENSE`). It links against the
following third-party components, each under its own license:

| Component | License | Used for |
|---|---|---|
| [PDFium](https://pdfium.googlesource.com/pdfium/) (bundled, via [bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries)) | BSD 3-Clause | PDF rendering, editing, text extraction — see `third_party/pdfium/LICENSE` |
| [QPDF](https://github.com/qpdf/qpdf) | Apache License 2.0 | Bookmark/outline writing, object-stream compression |
| [Qt 6](https://www.qt.io/) | LGPL v3 | UI framework (dynamically linked — see note below) |
| [OpenSSL](https://www.openssl.org/) (optional) | Apache License 2.0 | Digital signature (PKCS#7 detached) — only if built with `TORREADER_ENABLE_SIGNER` |
| [Corrosion](https://github.com/corrosion-rs/corrosion) | MIT / Apache-2.0 | CMake↔Rust build bridge (build-time only) |
| Rust crates (`src/formibpdf`): rayon, flate2, fontdue, image, lcms2 | MIT / Apache-2.0 | Low-resolution preview rendering helper |
| [LittleCMS (lcms2)](https://www.littlecms.com/) | MIT | ICC → sRGB color conversion (non-Windows builds) |
| [Tesseract OCR](https://github.com/tesseract-ocr/tesseract) (optional) | Apache License 2.0 | OCR on scanned pages — only if built with `FELIXPDF_ENABLE_OCR` |
| [Noto Sans](https://fonts.google.com/noto/specimen/Noto+Sans) (Google) | SIL Open Font License 1.1 | Default UI font — free for commercial use |

**Qt 6 (LGPL v3):** TorReader links Qt **dynamically** (shared libraries), as
required by LGPL v3 for proprietary or differently-licensed applications to
remain compliant without having to release their own source under LGPL. Since
TorReader's own code is MIT-licensed anyway, this is a non-issue in practice,
but the dynamic-linking choice is intentional and should not be changed to
static linking without re-checking LGPL obligations.

No proprietary, cracked, or license-incompatible code is bundled with this
repository.
