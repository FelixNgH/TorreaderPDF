# TorReader PDF

[![Release](https://img.shields.io/github/v/release/FelixNgH/TorreaderPDF)](https://github.com/FelixNgH/TorreaderPDF/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/FelixNgH/TorreaderPDF/total)](https://github.com/FelixNgH/TorreaderPDF/releases)
[![License: MIT](https://img.shields.io/github/license/FelixNgH/TorreaderPDF)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue)

Portable PDF reader and editor for **Windows** and **Linux**. Open
200MB files in seconds, merge/split while keeping bookmarks intact, insert
pages Adobe-style, translate inline, and digitally sign — all processed
**locally on your machine**, with no installation required.

### ⬇ Download

| Platform | Download |
|----------|----------|
| **Windows** (x64) | [`TorReader-PDF-Portable-v2.1.3.zip`](https://github.com/FelixNgH/TorreaderPDF/releases/latest) — unzip & run `TorReader.exe` |
| **Linux** (x86_64) | [`TorReaderPDF-2.1.3-x86_64.AppImage`](https://github.com/FelixNgH/TorreaderPDF/releases/latest) — `chmod +x` & run |

See all versions on the **[Releases page](https://github.com/FelixNgH/TorreaderPDF/releases)**.

<!-- 📸 TODO: add a screenshot or GIF here — biggest single boost to first-impression.
     Chụp app đang mở 1 file, hoặc GIF thao tác merge giữ bookmark. Đặt vào docs/screenshot.png rồi:
     ![TorReader PDF](docs/screenshot.png) -->

<!-- Prebuilt binaries and the web version are also at torreader.cloud -->

## Features

- **Fast rendering** — memory-mapped loading, opens 200MB files in 1–2s
- **Continuous scroll** — for long technical drawings, RAM-efficient (off-screen pages are evicted)
- **Sharp at any zoom** — mixed page sizes (A4 + A0–A3 drawings) in one merged file render crisply at every zoom level
- **Merge PDFs** — keeps each source file's bookmarks, remapped to the new page numbers; unbookmarked pages get an auto "Page N" entry
- **Insert Pages from File** — Adobe-style, right-click a thumbnail to pull pages from another PDF, no drag-and-drop
- **Extract / Split / Delete / Reorder pages**, reorder bookmarks
- **Digital signature** (optional feature) — PKCS#7 detached, SHA-256, via your own `.pfx`/`.p12` certificate
- **Inline translation** — Ctrl+drag over text to translate in place, with local caching
- **Annotations** — sticky notes, highlight, underline, strikethrough, shapes, freehand, stamp
- **Dark mode**, full keyboard shortcuts (press **F1** in-app for the full list)
- **Save model**: edits (insert/delete/reorder/merge) apply to an in-memory working copy first — your original file is only overwritten when you press **Ctrl+S**

## Building from source

### Requirements (both platforms)
- CMake ≥ 3.25
- Qt 6 (Core, Widgets, Gui, Concurrent, PrintSupport, OpenGL, OpenGLWidgets, Network)
- [Rust](https://rustup.rs/) + Cargo (builds the `formibpdf` preview-rendering helper)
- QPDF ≥ 11 (dev headers)
- **PDFium** prebuilt binaries — download from
  [bblanchon/pdfium-binaries releases](https://github.com/bblanchon/pdfium-binaries/releases)
  and extract into `third_party/pdfium/` (must contain `include/`, `lib/`,
  and on Windows `bin/pdfium.dll`)
- *(optional)* OpenSSL dev headers — enables the "Sign PDF" feature. Skipped
  cleanly if not found.
- *(optional)* Tesseract OCR dev headers — enables OCR search on scanned pages.

### Windows
```powershell
# Requires Visual Studio Build Tools 2022 + vcpkg (for QPDF/OpenSSL)
vcpkg install qpdf openssl
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
# → build/bin/Release/TorReader.exe
```

### Linux
```bash
# Debian/Ubuntu example
sudo apt install cmake qt6-base-dev libqt6opengl6-dev libqt6openglwidgets6 \
                  libqpdf-dev liblcms2-dev libssl-dev
cmake -B build
cmake --build build -j$(nproc)
# → build/bin/TorReader
```

Building without `libssl-dev` installed works fine — the Sign PDF feature is
simply disabled at configure time (see the CMake status message).

## Architecture

- **Rendering & structural editing**: [PDFium](https://pdfium.googlesource.com/pdfium/) (BSD 3-Clause) — merge, split, insert, extract, reorder, rotate all go through `FPDF_ImportPagesByIndex` and friends.
- **Bookmark/outline writing + compression**: [QPDF](https://github.com/qpdf/qpdf) (Apache-2.0) — object-stream compression on save, lossless.
- **UI**: Qt 6 Widgets + OpenGL (`PdfGpuView` single-page view, `ContinuousView` scroll strip).
- **Preview helper**: a small Rust engine (`src/formibpdf`) renders low-resolution thumbnails/previews (scale ≤ 0.16) in parallel; the main view always uses PDFium for full-resolution, correctly laid-out rendering.
- All `FPDF_*` calls are serialized behind a global mutex — PDFium is not thread-safe.

See `THIRD_PARTY_NOTICES.md` for the full list of dependencies and their licenses.

## License

TorReader's own source code is [MIT-licensed](LICENSE). Third-party
dependencies keep their own licenses — see `THIRD_PARTY_NOTICES.md`.

## Author

Built by **Loc Nguyen Huy ([@FelixNgH](https://github.com/FelixNgH))**.

- Web: [torreader.cloud](https://torreader.cloud)
- Also building: [BIMServer.cloud](https://bimserver.cloud) — BIM infrastructure for architecture firms
- Twitter: [@FelixNgHuy](https://twitter.com/FelixNgHuy)

Issues and pull requests welcome.
