# TorReader PDF 2.4.3

A stability release for working with several large drawings at once — heavy CAD sheets, MEP sets
of a few hundred megabytes, and many tabs open side by side.

## Tabs

- Closing a tab no longer crashes the app, including while a very dense page is still being drawn.
- Switching tabs keeps your page, scroll position and zoom, in single-page and continuous mode.
- Markups and comments stay visible when you switch back to a tab.
- A heavy file loading in one tab no longer holds back the pages of the other tabs.

## Responsiveness and memory

- Long freezes when zooming or closing a tab on very dense CAD pages are gone; the longest pause
  measured on a 2-million-object page is now around half a second.
- Memory use with three large drawings open drops to about 1 GB.

## Comments in continuous mode

In continuous mode, clicking a comment selects it and jumps to it in the comment list. Moving,
resizing, deleting and editing comments is done in single-page mode, which avoids accidental
changes while scrolling through a large set.

## Fixes

- Images on pages rotated by 90° are drawn with the correct orientation.
- The "Loading…" text shown while a page loads, and the start screen text, are no longer distorted.
- The start screen shown after closing every tab is drawn clearly in both light and dark themes.

## Downloads

| Platform | File |
|---|---|
| Windows (x64) | `TorReaderPDF-2.4.3-win64.zip` — unzip and run `TorReader.exe` |
| Ubuntu / Debian | `torreader_2.4.3_amd64.deb` |
| Any distro | `TorReaderPDF-2.4.3-x86_64.AppImage` (glibc 2.35+) |
