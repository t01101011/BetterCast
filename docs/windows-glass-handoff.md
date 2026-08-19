# Windows glass UI — where this is up to

Written to be picked up on another machine. The code is all on
`fix/windows-sender-latency-topology`; this is the part that is not in the code.

Last updated at commit `d6ee3d2`, 19 Aug 2026.

## What this is

`spike/` holds a second Windows front end, built on
[liquidDX11](https://github.com/StephenLovino/liquidDX11) — a Dear ImGui + D3D11
glass UI. It is not a rewrite. liquidDX11's own tree is copied in CI and only
the page bodies, the nav labels and a handful of anchored lines in its
`main.cpp` are changed, each patch asserting that it applied. The BetterCast
core is compiled into that same target and driven in-process:

- `spike/glass-app/BetterCastBridge.{h,cpp}` — the seam. Creates a real
  `QApplication`, pumps it once per render frame, and exposes the core through
  plain types so the ImGui page fragments never see Qt.
- `spike/glass-ui/pages/*.inc` — page bodies overlaid onto liquidDX11's.
- `.github/workflows/build-glass-spike.yml` — the overlay, the patches, the
  CMake append, and now the installer.

Two things had to be true first, and both are: the core is widget-free
(`LogManager` was lifted out of `MainWindow.h`), and a full `QApplication` keeps
the existing `VideoWindow` working, so the receive path needed no porting.

The Qt app in `Sources/BetterCastReceiverDesktop` still builds and ships
unchanged. Both are installed side by side.

## Confirmed working on real hardware

Display pool with three simultaneous receivers; Mobile Hotspot and QR pairing
(all three receivers plus the Mac's scanner); per-device resolution; the update
checker; language switching; the glass front end streaming at 4% integrated GPU
/ 0% discrete, down from ~50%; DXGI duplication coexisting with the Qt app; the
receive path.

## Open work

**Needs a macOS session.** Two things cannot be finished from Windows:

1. *"Extend Mac here"* — a button on Windows asking the Mac to start sending.
   The Mac has no listener for it. Its only three `NWListener`s are in
   `ReceiverNetworkListener.swift` and all of them are for receiving video, and
   nothing in the repo advertises or browses an invite service. Before building
   a protocol, try the Mac sender's existing **Auto-Connect** toggle
   (`BetterCastSenderApp.swift`, `autoConnect`) — it may already start streaming
   to a discovered receiver, which would make the Windows button unnecessary.
2. Audio playback on the Mac side.

`InviteListener` (on `fix/android-windows-discovery`, unmerged) is the *opposite*
half: Windows accepting invites from phones. It is not what "Extend Mac here"
needs.

**Windows-side, not started.**

- No outgoing audio at all. `NetworkSender::sendAudio()` exists and nothing
  calls it; Windows has no loopback capture. Needs WASAPI plus an AAC encoder.
  Incoming audio from the Mac does work.
- Hotspot and Receive pages are still liquidDX11's placeholders. Devices,
  Settings and the profile page are real.
- Double-logging in the glass build: every line appears twice, and the bridge's
  own lines appear once. Probably two `ServiceDiscovery`/`VirtualDisplayVDD`
  instances, or the Qt app running alongside and sharing `bettercast.log`.
  Observed, never investigated.

**Branch hygiene.** `fix/windows-sender-latency-topology` is ~72 commits ahead
of `main` and has never been merged. Five more commits are stranded on
`chore/security-policy`, `chore/windows-build-hardening` and
`fix/android-windows-discovery`.

## Things that have bitten more than once

- **Write C++ and YAML with an editor, not a shell heredoc.** Backslashes and
  non-ASCII have been corrupted in transit at least six times — `'\\'` becoming
  `'\'`, literal `\n` landing in YAML, Spanish accents stripped out of
  translations twice.
- **Assert observable state, not return codes.** `Backdrop::Init` ignoring
  `CreateDuplication()`'s result is the archetype, and every CI patch now
  throws if its target string was not found.
- **`QFileInfo::isWritable()` lies on Windows.** It reads the read-only
  attribute, not the ACL, so the elevated settings write never ran. Attempt the
  write and fall back on failure.
- **`actions/checkout` gives CRLF on Windows.** Normalise before matching or
  any multi-line pattern silently fails.
- **A PowerShell here-string terminator sits at column 0**, and a column-0 line
  inside a YAML block scalar ends the block. Use an array joined with newlines.
- **`qttools` is not an aqt module** for Qt 6 desktop. `qtmultimedia` is.
- **Growing the virtual display pool restarts the driver** and kills every live
  capture, so the pool is built once, up front, while nothing is streaming.
