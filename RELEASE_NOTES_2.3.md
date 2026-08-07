# TorReader PDF 2.3 — GPU vector rendering for heavy CAD drawings

**Release date:** 2026-08-02 · **Previous:** 2.2.4 (2026-07-26)

The headline of 2.3: on large CAD drawing sets, **the page is sharp the moment you
zoom or pan** — no more waiting seconds for the picture to catch up. TorReader PDF now
uploads the page's vector geometry to the GPU and draws it directly, instead of
asking the PDF engine to re-rasterise the viewport every time you move.

---

## Why this was needed (measured, not guessed)

The test file is a 62 MB construction set. Page 4 of it contains:

| | Page 4 |
|---|---|
| Vector paths | **2,177,769** |
| Path segments | 4,513,279 |
| Text objects | 2,416 |
| Images | 119 |

Re-rendering that page through the PDF engine costs **~2 seconds and does not get
cheaper at lower resolution** — 200 px wide and 4000 px wide take the same time,
because the cost is the number of paths, not the number of pixels. So every pan at
high zoom used to cost a full 2-second re-render, and repeated pans cancelled and
restarted it, which is why the view could stay blurry indefinitely.

Lowering resolution, tiling, or switching PDF engine cannot fix that. Drawing the
geometry on the GPU can.

---

## What's new

### GPU vector layer (single-page view)

The page's own drawing — not an image of it — is uploaded to the GPU once and drawn
at whatever zoom you're at:

- **Strokes** with their real widths. Line width is expanded to quads in the vertex
  shader; OpenGL core profile does not guarantee `glLineWidth > 1`, so the previous
  1-pixel-everything look is gone and drawings keep their line hierarchy.
- **Sub-pixel line coverage** — a 0.24 pt line at 250 % zoom is 0.6 px wide and is
  drawn with matching opacity, so dense hatching stays light instead of turning into
  a solid mass. Hairlines (width 0) stay fully opaque, per the PDF spec.
- **Dash patterns** honoured (`/D` dash array + phase).
- **Fills** triangulated (ear clipping with a fan fallback), so hatches and solid
  markers are filled, not hollow.
- **Text** rendered per object by the PDF engine at 8 px/pt and drawn as textures —
   which means embedded fonts are used, so **Vietnamese diacritics render exactly
   right**. Textures are mip-mapped, so zooming out stays clean.
- **Images** taken at their native resolution rather than page resolution, so they
  are as sharp as the file allows.
- **Clipping** to the page box and to per-object clip rectangles.
- **Painting order** preserved with a depth buffer keyed to each object's position in
  the document, so a filled shape drawn later correctly covers lines drawn earlier.
- **CropBox origin** handled — pages whose coordinate origin is not at (0, 0), which
  is common in Revit crop views, no longer draw offset from the paper.

### Continuous (scrolling) mode

Continuous scroll reuses the same `VectorGpuRenderer` layer as single-page view,
with a per-page cache that keeps only the visible pages' vector layers in memory.
Raster bitmaps for pages already drawn as vector are dropped, which is what makes
scrolling cheap:

- On a 25-page scroll of the 62 MB test set, resident memory went from
  **2470 MB → 1689 MB** during the scroll, settling at **949 MB** once off-screen
  pages were evicted.
- Markups in Continuous are display-only (popups still work); to select or edit
  markup, switch to single-page view.

### Markup: a comment on every annotation

Every markup type — line, arrow, rectangle, ellipse, cloud, freehand, highlight —
now carries **popup text** (the standard PDF `/Contents` field):

- Type it **inline in the Comments list**: each row is `p.4  Arrow  —  [text field]`.
- It shows as a popup when you select the markup.
- **Undo/redo** covers typing and clearing it.
- Because this writes an annotation field and never touches page content, it is fast
  even on the heaviest pages — a quick alternative to the text tool.

### Smaller changes

- **Fit Page** now centres the page instead of only resetting the zoom level.
- **Undo/redo buttons** are larger and legible.
- **Share app** button with a copyable download link.
- **Translate** region select moved from `Ctrl`+drag to **`Alt`+drag** — `Ctrl`+drag
  was colliding with zoom.

---

## Performance

Measured on page 4 of the test file (2.18 M paths), per markup operation:

| | 2.2.4 | 2.3 |
|---|---|---|
| Move a markup | ~9 s | < 1 s |
| Move a note | 5.1 s | 0.8 s |
| Re-read annotations | 1.3–1.6 s | 0 ms |
| Find note objects to move | 768 ms | 0 ms |
| Content-stream regeneration | — | 79 ms |

Nearly all of the old cost turned out to be the same thing repeated in five different
places: **re-opening or re-scanning a 2.18-million-object page**. The fix in each case
was to remember rather than redo — a pinned page handle for the page being edited, a
separate slot so the document-wide comment scan cannot evict it, and cached object
indices for note geometry.

---

## Known limitations

- **In Continuous mode, markup is display-only.** The vector layer is shared, but
  selecting or editing markup requires switching to single-page view.
- **The sticky-note icon is page content**, so during a drag it does not follow the
  cursor; it lands at the new position once the vector layer is rebuilt (1.5–3 s on the
  heaviest pages). Markup shapes and note text do follow the cursor.
- **Note text is stored as page objects, not as a FreeText appearance stream.** PDFium
  cannot generate an appearance stream that references an embedded Unicode font, and
  the page-object route is what makes Vietnamese text correct. The cost is that editing
  note text regenerates the page content stream (~80 ms).
- Pages whose vector geometry cannot be fully reproduced (unsupported object types, or
  internal caps hit) fall back to the raster path automatically — correctness first.

---

## Upgrading

Nothing to do: it is the same portable build. Windows users unzip and run; Linux users
install the `.deb`/AppImage or `apt install torreader` as before. Files written by 2.3
open in Adobe Acrobat and other viewers unchanged — annotations are written using
standard PDF structures.
