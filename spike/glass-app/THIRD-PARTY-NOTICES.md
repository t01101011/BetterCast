# BetterCast — Third-Party Notices (Windows)

BetterCast is free software under the GNU GPL v3. The source lives at
https://github.com/StephenLovino/BetterCast, and this build's own license
text is `licenses/BetterCast-LICENSE.txt`. The components below ship inside
or beside it, each under its author's license. Full texts are in the
`licenses/` folder next to the executable.

## The interface

- **liquidDX11** — MIT License, Copyright (c) 2026 poncipp
  (https://github.com/Pondot). The liquid-glass UI kit this app's whole
  interface is built on: BetterCast overlays its pages onto liquidDX11's
  render loop and widget library rather than rebuilding them. Upstream:
  https://github.com/Pondot/liquidDX11, built from the pinned fork
  https://github.com/StephenLovino/liquidDX11.
  Full text: `licenses/liquidDX11-LICENSE.txt`.
- **Dear ImGui** — MIT License, Copyright (c) 2014-2026 Omar Cornut. The
  immediate-mode GUI framework liquidDX11 is built on.
  Full text: `licenses/DearImGui-LICENSE.txt`.
- **FreeType** — FreeType License (FTL). Portions of this software are
  copyright © The FreeType Project (https://www.freetype.org). All rights
  reserved.
- **Fonts and icons** — Font Awesome Free (icons CC BY 4.0, fonts SIL OFL
  1.1, code MIT; attribution: Font Awesome — https://fontawesome.com),
  Inter (SIL OFL 1.1), Sacramento (SIL OFL 1.1), and UIcons by Flaticon
  (https://www.flaticon.com/uicons — attribution required). Details and
  full texts: `licenses/THIRD-PARTY-NOTICES.md`, liquidDX11's own notices
  file, shipped verbatim.

## Bundled tools and drivers

- **scrcpy** and **adb** — Apache License 2.0, by Genymobile and the
  Android Open Source Project. Bundled for wired Android mirroring; the
  bundled release is recorded in `scrcpy/BUNDLED_VERSION.txt`.
- **Virtual Display Driver** — MIT License,
  https://github.com/VirtualDrivers/Virtual-Display-Driver. Optionally
  installed so the desktop can extend onto devices without a physical
  screen.

## Runtime libraries

- **Qt 6** — LGPL v3, The Qt Company Ltd. and contributors. Dynamically
  linked; the Qt runtime ships beside the executable.
- **FFmpeg** — LGPL v2.1 or later (some optional components GPL), the
  FFmpeg developers. Dynamically linked; the FFmpeg DLLs ship beside the
  executable.

---

The glass visual style follows Apple's "Liquid Glass" design language as an
homage. BetterCast is not affiliated with, endorsed by, or sponsored by
Apple Inc. See `licenses/DISCLAIMER.md` (from liquidDX11) for the full
notice.
