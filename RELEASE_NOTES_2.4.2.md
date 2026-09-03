# TorReader PDF 2.4.2

A correctness release for annotations. Text you add is now a real PDF comment, Vietnamese can be
typed into it, and several rendering and stability defects are fixed.

## Text is a real PDF comment

The Text tool used to draw its own text onto the page. It now writes a genuine FreeText
annotation, so what you add is a proper PDF comment.

- Acrobat and other PDF software read, select and edit the text you add.
- Text wraps inside its box instead of running off the page.
- The box can be resized by dragging a corner, and the text reflows to fit.

## Typing Vietnamese into annotations

Vietnamese now types correctly into annotations with any input method, including those that send
characters as key packets rather than as keystrokes.

## Sharper and faster pages, in both view modes

Pages that carry comments used to be locked to a raster image for the rest of the session, so they
stayed soft when zoomed and were slow to redraw. They now use the vector page, in single-page mode
and in continuous mode alike.

- Dense drawings — site plans, master plans, planning sheets — stay crisp at any zoom instead of
  blurring, and page flips and zoom steps are noticeably faster on them.
- The vector page and the comment layer are now separate, so a page no longer has to choose
  between showing comments and staying sharp.

## Insert Image is now in the markup grid

Insert Image moved out of the Comments tab into the markup grid, as the tenth tool where the Note
button used to be. The stamp can still be resized by its four corner handles.

## Fixes

- Text drawn over a vector page no longer appears striped.
- Closing a tab no longer crashes the app.
- The annotation store and the vector layer store were merged, so a page is parsed once instead
  of twice.

## Interface

The Note button was removed and the markup tools rearranged into a 5 by 2 grid.
