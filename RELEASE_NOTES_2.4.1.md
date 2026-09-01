# TorReader PDF 2.4.1

A performance and correctness release for large CAD drawings and for documents that carry
comments made in other PDF software.

## Markup appears instantly

Drawing a markup used to re-render the whole page. It no longer does — markups are painted on
their own layer above the page image.

- Committing a rectangle on a heavy drawing: **about 2.3 s → 0 ms**.
- Deleting a markup disappears immediately, without changing page or zoom first.
- Text and Note tools show their result straight away instead of waiting for the next redraw.
- Markups now stay on screen through zoom, and are drawn in continuous mode as well as in
  single-page mode.

## Comments from other PDF software

Notes, free text and stamps written by other applications are now drawn by the PDF engine itself,
so they keep their own font and background instead of being redrawn approximately.

- First display of a page carrying such comments: **about 2.5 s → 0 ms**.
- Pages with comments stay sharp when you zoom in — the visible region is redrawn at the zoom
  level you are actually looking at.

## Faster on heavy drawings

On an A0 drawing with 2.5 million objects:

- Reopening a page already visited: **95.7 s → 0.2 s**.
- The longest freeze of the window: **20.8 s → 0.58 s**.
- Thumbnails are rendered from the vector layer on the GPU: **5.4 s → 0.44 s** per batch.
- Thumbnails of tabs you are not looking at are no longer rendered at all.

Two documents open side by side also use markedly less memory than in 2.4.0.

## Fixed

- Double-clicking a PDF whose name contains Vietnamese letters with two diacritics (for example
  `Ể ề ộ`) opened an empty tab when TorReader PDF was the default PDF application.
- The thumbnail of the current page was not highlighted.
- Opening a second tab could crash the app.
- Embedded logos and images could appear as noise blocks on merged documents.
- Continuous mode showed blank pages while scrolling fast, and lost sharpness after zooming.
- Switching between View Fast and View Quality & Edit no longer loses your place in the document.

---

Windows: portable ZIP, no installer, no admin rights.
Linux: AppImage, `.deb` via the APT repository, and Snap.
