# TorReader PDF 2.4.0

## New interface

The whole app moved to a high-contrast theme in both light and dark mode. Borders, hover and
selection states are consistent everywhere, the page sits on a neutral grey desk so its edge is
always visible, and the sidebar no longer competes with the toolbar for attention.

## OCR for scanned documents — 10 languages

Scanned PDFs have no text layer, so you cannot search, select or copy anything in them.
TorReader PDF now recognises the text and makes it work like a normal document.

- **Bundled, no download, no account, fully offline.** The language data ships inside the app.
- **10 languages:** Vietnamese, English, Chinese (Simplified), Chinese (Traditional), Japanese,
  Korean, French, Russian, Portuguese, Spanish. Vietnamese and English use the highest-accuracy
  models.
- **Your file is never modified.** The recognised text lives in memory only — the file on disk
  keeps the same checksum.
- A new **OCR** tab in the sidebar: recognise the whole document or just the current page, pick the
  language, watch progress, cancel any time.

## Select, find and copy text — in PDFs and in scans

- **Select text** works by character, the way a text editor does: drag from the middle of one word
  to the middle of another, across lines and across pages. Double-click selects a word, triple-click
  a line, `Ctrl+A` the page.
- **Find** ignores diacritics by default, so typing `MAT` also finds `MẶT` — useful when a scan
  loses a mark here and there. Turn on *Match diacritics* for exact matching.
- Search results now scroll straight to the match instead of only jumping to the page, and
  highlights stay visible as you scroll through the document.
- **Copy** with `Ctrl+C` or right-click → *Copy text*.

## Links

Links inside a PDF now work. Internal links jump to the target and briefly outline it; external
links ask for confirmation and show the full address before your browser opens.

## Faster

Opening and flipping pages no longer waits on background work. A single shared page cache means a
page is parsed once instead of several times, and neighbouring pages are prepared while you read.
On a large CAD drawing the per-flip cost dropped from about **850 ms to 1–2 ms** after the first
visit.

## Fixed

- Comments landed in the wrong place on pages rotated 270°.
- Fill-only shapes were drawn with an extra outline.
- Search highlights disappeared when scrolling in continuous mode.
- Opening a second document kept the first document's search results in the sidebar.
- Licence text in the About dialog showed mojibake on Windows.

---

Windows: portable ZIP, no installer, no admin rights.
Linux: AppImage, `.deb` via the APT repository, Snap and Flatpak.
