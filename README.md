# Framegrabber

Basler frame grabber module for board discovery, applet/MCF configuration,
multi-DMA acquisition, transport-aware camera control, and Qt feature editing.

## Boundaries

- The module owns all Basler Frame Grabber SDK handles and DMA memory.
- `Framegrabber::Image` owns copied frame bytes; consumers never retain an SDK buffer.
- Each DMA channel preallocates an owned byte-buffer pool with the configured DMA
  buffer count. Frames copy once from the SDK ring into a leased pool slot, and the
  last consumer reference returns that slot for reuse. Pool exhaustion waits rather
  than overwriting bytes still visible to a consumer.
- Legacy SDK `FG_COL24` and `FG_COL48` output is BGR-ordered. The public pixel
  format preserves that identity so Qt uses `Format_BGR888` for 8-bit data and
  swaps the 16-bit BGR channels only while reducing them for 8-bit display.
- DMA output `FG_FORMAT` constants are mapped explicitly. Supported display paths
  include Mono 8/10/12/14/16/32-bit output, 1-bit binary output, BGR/RGB/RGBA/BGRA
  8/10/12/14/16-bit output, RGBX 8/10/12/14/16-bit output, YUV422/YCbCr422 8-bit
  output, Bayer GR/RG/GB/BG 8/10/12/14/16-bit output, and BiColor RGBG/GRGB/BGRG/GBGR
  8/10/12-bit output. DMA buffers use the SDK bit depth for conservative allocation,
  while each delivered image uses `FG_TRANSFER_LEN` for its actual payload size and
  stride. The GraphicsEngine adapter maps the core `Framegrabber::PixelFormat` and
  bit-alignment metadata without interpreting SDK constants a second time.
- `FG_RAW` and `FG_JPEG` are recognized identities but are not accepted by generic
  acquisition because they do not provide the fixed pixel layout and SDK bit depth
  needed to derive a safe DMA allocation size.
- Bayer DMA output is displayed through a lightweight 2x2 Bayer-cell RGB
  reconstruction. The host does not expose raw Bayer samples as artificial
  per-pixel RGB checker data in the live viewer.
- `FG_YUV422_8` and `FG_YCBCR422_8` DMA output both use two bytes per pixel.
  `YUV422_8` and `FG_YUV422_8` use Y0-Cb-Y1-Cr; `FG_YCBCR422_8` uses
  Cb-Y0-Cr-Y1. Camera input `FG_PIXELFORMAT` and DMA output `FG_FORMAT` remain
  separate applet settings.
- Acquisition sizes and displays frames from DMA output `FG_FORMAT` only. If
  `FG_PIXELFORMAT` is YUV422 but `FG_FORMAT` is `FG_GRAY`, the host receives and
  displays Mono8 output.
- The core module does not depend on Camera, Gocator, Resources, Playground, or
  GraphicsEngine. Set `FRAMEGRABBER_BUILD_GRAPHICSENGINE_ADAPTER=ON` after adding
  GraphicsEngine to create `Framegrabber::GraphicsEngineAdapter`; hosts link that
  target explicitly without adding GraphicsEngine/VTK to `Framegrabber::Framegrabber`.
- `QFramegrabberWidget` remains usable with plain Qt when the host does not install Resources.

## Runtime Contract

- `FramegrabberSystem` initializes the SDK before board discovery and releases it after all wrappers.
- `Framegrabber` follows the Camera lifecycle shape: callback registration, `open`, `close`,
  `grab`, `requestStop`, `stop`, and status notifications.
- Continuous acquisition uses DMA-specific `ready(dmaIndex)` admission.
- Open, grab, stop, and close operations are serialized. Loading a replacement applet
  closes the current SDK handle under the same lifecycle lock before initializing the
  new handle, so UI refresh, camera discovery, and acquisition cannot observe a
  half-released board.
- A new grab joins and clears every stopped DMA worker before allocating replacement
  channels, and channel registration publishes its worker atomically so concurrent
  lifecycle calls cannot miss a thread.
- DMA stop/free operations are synchronized per channel, `ready(dmaIndex)` is a binary
  admission signal, and exceptions from consumer callbacks do not escape into SDK workers.
- Worker frame waits time out after one second. A stop request also wakes the local
  admission and buffer-pool waits before issuing the SDK stop, bounding no-frame
  shutdown latency without treating an idle trigger source as an acquisition error.
- Consumer callbacks run on the DMA worker and must return; `stop()` joins that worker
  before releasing DMA memory and SDK handles, so a blocking consumer intentionally
  keeps shutdown pending rather than risking use-after-free of a live DMA buffer.
- Applets are initialized first through `loadApplet()`/`Fg_Init()`. VisualApplets HAP
  files and wrapped DLL/SO applets must be deployed independently of MCF snapshots.
- Before manual selection, the Qt widget queries the selected board's active applet and
  then its power-up applet through the SDK board iterator. It matches that board identity
  by applet UID against the SDK's loadable filesystem iterator and initializes the matched
  path automatically; no application-side recent-applet cache is used.
- MCF files are applied only to an initialized applet through `Fg_loadConfig()`.
  Live feature edits mark the snapshot dirty; Save and Save As export the current
  board configuration.
- `cameraControlCapabilities()` reports only camera-control transports exposed by the
  opened board and applet. The current backend reports CoaXPress when Siso GenICam
  initialization succeeds; Camera Link remains an explicit transport extension point.
- Siso GenICam initialization is attempted only for applets whose SDK metadata declares
  a CoaXPress interface and excludes the `family=test` diagnostic family.
  `FrameGrabberTest` therefore keeps its Fg/DMA controls without entering the SDK camera
  control path, which is not valid for that applet.
- CXP discovery immediately connects every discovered camera so GenICam features are
  available without a separate user action. The camera-page refresh action remains only
  for an explicit rescan after hardware or link changes.
- `QFramegrabberWidget` creates transport camera tabs from those capabilities after
  the board opens and removes them when it closes.
- Camera feature trees are rebuilt automatically after board connection, camera combo
  changes, and explicit rescans. During acquisition, camera feature editors are disabled
  and the existing tree remains visible.
- Camera feature XML is cached during camera connect/rescan from `CAM_PROP_XML_DATA`
  and the widget reparses that cache for tree refreshes. The UI refresh path does not
  call `Sgc_getGenICamXML()`, because that SDK path can log access errors for optional
  or dynamically unreadable camera nodes before the widget asks for live values.
- Camera feature trees hide non-readable leaf nodes before asking the SDK for a live
  value. Write-only commands remain visible as executable buttons; other write-only or
  unavailable nodes stay out of the value tree because reading them can make Siso
  GenICam log access exceptions.
- A session-level read-only information field above the tabs shows the loaded applet
  file name and runtime version; its tooltip preserves the full local applet path.
  This identity is not repeated inside the Setup tab because one applet owns the
  whole board session. If the loaded-handle version query is empty, the version
  falls back to the SDK filesystem iterator metadata for the same applet path.
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
- Camera feature writes are read back only when the UI path has a readable current
  value. Write-only or dynamically unreadable camera writes rely on the SDK write
  result. Failed feature writes refresh the current tree instead of locally reverting
  one editor, because the rejected write can also change dependent access or value state.
- The Qt widget runs feature writes on a short-lived worker thread and returns cleanup
  to the GUI thread through callable objects with explicit shared lifetime, so stateful
  completion handlers compile consistently across MSVC and other C++17 toolchains.
- Dynamic access uses the SDK's `PROP_ID_ACCESS_ID` virtual parameter when present and
  falls back to the GenApi register `AccessMode` when the applet reports access ID zero.
  Live write/lock/modify flags are enforced again immediately before an SDK write.

## Follow-up

- Validate Sgc camera event coverage on CXP hardware before replacing explicit camera
  refresh with camera-side node event updates.
- Profile the remaining safe DMA-to-owned-buffer copy before considering an SDK-buffer
  lease or zero-copy frame contract.
- Validate host `DeviceSession` DMA-specific display routing on multi-DMA hardware
  before presenting simultaneous multi-camera display as supported.
- Split DMA acquisition channel ownership from CXP camera lifecycle ownership into
  dedicated classes only when further hot-path optimization requires the extra structure.

## Build

```bash
cmake -S C++ -B build -DFRAMEGRABBER_BUILD_QT_WIDGET=ON
cmake --build build --config Debug
```

Set `BASLER_FG_SDK_DIR` when the SDK is not installed in a default location.
