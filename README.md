# Framegrabber

Basler frame grabber module for board discovery, applet/MCF configuration,
multi-DMA acquisition, transport-aware camera control, and Qt feature editing.

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
- Applets are initialized first through `loadApplet()`/`Fg_Init()`. VisualApplets HAP
  files and wrapped DLL/SO applets must be deployed independently of MCF snapshots.
- MCF files are applied only to an initialized applet through `Fg_loadConfig()`.
  Live feature edits mark the snapshot dirty; Save and Save As export the current
  board configuration.
- `cameraControlCapabilities()` reports only camera-control transports exposed by the
  opened board and applet. The current backend reports CoaXPress when Siso GenICam
  initialization succeeds; Camera Link remains an explicit transport extension point.
- `QFramegrabberWidget` creates transport camera tabs from those capabilities after
  the board opens and removes them when it closes.
- The Setup tab contains applet selection and initialization followed by the
  DMA-channel selector and applet feature editor. MCF and DMA-buffer APIs remain
  module-level capabilities and are not exposed in this compact widget.
- Applet feature XML is interpreted by a GenApi node map. `Framegrabber` exposes a
  Qt-neutral hierarchy with category, display-name, tooltip, enum, and access metadata;
  each feature's GenApi register address is bound to the corresponding frame grabber
  SDK parameter ID for live values. The Qt widget falls back to its legacy XML renderer
  if a deployed applet cannot produce a node map.
- Applet writes are read back when the parameter is readable. Every applet node update
  invalidates the selected DMA hierarchy and rebuilds it from current XML because one
  feature may create, remove, move, or change the value/access state of other features;
  tree expansion and selection remain UI state.
- Dynamic access uses the SDK's `PROP_ID_ACCESS_ID` virtual parameter when present and
  falls back to the GenApi register `AccessMode` when the applet reports access ID zero.
  Live write/lock/modify flags are enforced again immediately before an SDK write.

## Build

```bash
cmake -S C++ -B build -DFRAMEGRABBER_BUILD_QT_WIDGET=ON
cmake --build build --config Debug
```

Set `BASLER_FG_SDK_DIR` when the SDK is not installed in a default location.
