# Framegrabber

Basler frame grabber module for board discovery, applet/MCF configuration,
multi-DMA acquisition, CXP camera control, and Qt feature editing.

## Boundaries

- The module owns all Basler Frame Grabber SDK handles and DMA memory.
- `Framegrabber::Image` owns copied frame bytes; consumers never retain an SDK buffer.
- The module does not depend on Camera, GraphicsEngine, Gocator, Resources, or Playground.
- `QFramegrabberWidget` remains usable with plain Qt when the host does not install Resources.

## Runtime Contract

- `FramegrabberSystem` initializes the SDK before board discovery and releases it after all wrappers.
- `Framegrabber` follows the Camera lifecycle shape: callback registration, `open`, `close`,
  `grab`, `requestStop`, `stop`, and status notifications.
- Continuous acquisition uses DMA-specific `ready(dmaIndex)` admission.
- MCF files are configuration snapshots. Live feature edits mark the snapshot dirty;
  Save and Save As export the current board configuration.
- Applet and CXP GenICam feature trees share a Qt renderer, while SDK access stays in
  `Framegrabber`.

## Build

```bash
cmake -S C++ -B build -DFRAMEGRABBER_BUILD_QT_WIDGET=ON
cmake --build build --config Debug
```

Set `BASLER_FG_SDK_DIR` when the SDK is not installed in a default location.
