# Framegrabber

Framegrabber is a C++17 acquisition library for Basler frame grabber boards. It owns SDK handles, applet and configuration loading, multi-DMA acquisition, buffer lifetime, and optional CoaXPress camera control.

## Capabilities

- Discover boards and create independently owned `Framegrabber` instances.
- Enumerate the applets the SDK can load on a board and load them by name or path.
- Load and save MCF configuration files.
- Start every configured DMA channel and identify each image by `dmaIndex`.
- Expose applet features and supported camera features through typed APIs.
- Build an optional Qt control widget and an optional external GraphicsFrame adapter.

## Requirements

- CMake 3.16 or newer and a C++17 compiler.
- A Basler Frame Grabber SDK installation containing the FgLib5, ClSerSis, SisoIoLib, SisoPlatform, and SisoGenicamLib CMake packages.
- `BASLER_FG_SDK_DIR` when the SDK is not installed in a searched default location.
- Qt Widgets only when `FRAMEGRABBER_BUILD_QT_WIDGET=ON`.

Operating-system and board support are defined by the installed SDK and driver. Do not infer hardware support from compilation alone.

## Integration

The core library is the default and does not discover or link Qt:

```cmake
add_subdirectory(path/to/Framegrabber/C++ Framegrabber-build)
target_link_libraries(consumer PRIVATE Framegrabber::Framegrabber)
```

To use the Qt control panel, opt in before adding the module and link only the
widget target; it supplies the core library and required Qt components:

```cmake
set(FRAMEGRABBER_BUILD_QT_WIDGET ON CACHE BOOL "" FORCE)
add_subdirectory(path/to/Framegrabber/C++ Framegrabber-build)
target_link_libraries(qt_consumer PRIVATE Framegrabber::QtWidget)
```

Enabling `FRAMEGRABBER_BUILD_QT_WIDGET` requires Qt Core, Gui, Widgets, and Xml.
Configuration fails when the requested Qt components are unavailable.

The optional Playground adapter is disabled by default. Enable `FRAMEGRABBER_BUILD_PLAYGROUND_ADAPTER` only when the neutral GraphicsEngine contract target is available. Consumers that want owned frames include `Utility/PlaygroundAdapter/FramegrabberGraphicsFrameStream.h` and link `Framegrabber::PlaygroundAdapter`; pixel-format conversion stays in `FramegrabberGraphicsFrameAdapter`. The stream publishes one single-image GraphicsFrame for each accepted DMA image, using the DMA index as the source index. It does not assemble frames across DMA channels; the consumer owns multi-source presentation.

The optional Playground device plugin is a separate MODULE. Enable `FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN` only when `Playground::DevicePlugin`, the adapter, `Framegrabber::QtWidget`, and `Resources::Dialogs` are available. The plugin passes `ThemedFileDialog` through `QFramegrabberWidget::setFileDialogPresenter()` for Load, Save and missing-applet selection; without a presenter the widget uses Qt's non-native file dialog and needs no theme. Package identity lives in `Utility/PlaygroundAdapter/Package/Package.cmake` as quoted `set(PLAYGROUND_PLUGIN_* ...)` assignments; the MODULE must set `PLAYGROUND_PLUGIN_PACKAGE` and include `DevicePlugin.h`. The host emits `plugin.json` from that identity plus `DevicePluginPackage.h`.

## Acquisition Contract

```cpp
#include "Framegrabber.h"
#include "FramegrabberSystem.h"

FramegrabberSystem system;
system.updateFramegrabberList();
Framegrabber* board = system.addFramegrabber();

const auto callbackId = board->registerGrabCallback(
    [board](const Framegrabber::Image& image, std::size_t sequence) {
        if (image.isValid()) {
            // Consume or retain image.storage as needed.
        }
    });
```

Each accepted SDK frame is copied once into a leased pool buffer because DMA memory is owned and reused by the SDK. The pool is coordinated with a mutex; it is not lock-free. Copying an `Image` retains its `shared_ptr` storage and can keep a pool slot occupied, so bound retention in consumers. `OwnedBufferPool::acquire()` never waits: when every pool buffer is retained by a consumer the worker drops that frame before the copy and counts it as `poolExhausted`; the acquisition worker never waits for a consumer.

Acquisition uses `Fg_AcquireEx(..., ACQ_STANDARD, ...)` with `setDMABufferCount()` frame buffers (default 6, minimum 2; smaller values are raised to 2) and waits with `Fg_getLastPicNumberBlockingEx`. In this model the driver never blocks a buffer, so the worker delivers the newest completed frame and handles backpressure as follows:

- Frames that completed while the previous frame was still being processed are counted as `skipped`.
- After each copy the worker reads `NUMBER_OF_GRABBED_IMAGES`. The buffer of frame `n` is reused by frame `n + bufferCount`. If that transfer may have started (`grabbed - n >= bufferCount - 1`), the copy is dropped and counted as `overwritten`, because it can contain data from the later frame.
- A frame whose owned pool buffer is still retained by a consumer is dropped before the copy and counted as `poolExhausted`.
- `acquisitionStatistics()` reports `delivered`, `skipped`, `overwritten`, `rejected` (invalid pointer or transfer length) and `poolExhausted` for each DMA during and after a run.
- A wait result other than the short wait timeout (for example `FG_TRANSFER_NOT_ACTIVE` after the driver's `FG_TIMEOUT`) ends that DMA worker.

Stopping follows the SDK order: the CXP camera stream (`Sgc_stopAcquisition`) stops first, then the DMA (`Fg_stopAcquireEx`). After that the buffers are freed (`Fg_FreeMemEx`).

Callbacks run on acquisition workers. Keep them bounded, do not access GUI objects directly, and stop acquisition and join workers before closing the board or destroying callbacks. A callback return is not an admission signal: the worker already moved on to the newest frame and never waits for downstream processing.

## Applet and Configuration Contract

All `FramegrabberSystem` instances in a process share one `Fg_InitLibraries(nullptr)` / `Fg_FreeLibraries()` reference. Discovery can run while a session holds an open board.

- **Enumeration.** `refreshAppletCatalog(board)` lists the applets the SDK can load on the board: `Fg_getAppletIterator(FG_AIS_FILESYSTEM, FG_AF_IS_LOADABLE)`, including the `FG_AF_IS_ACTIVE` applet. It runs only while this instance has no open applet; `appletCatalog()` returns the last result.
- **Applet loading.** `loadApplet(reference, board)` accepts a file path, a catalog path, or a unique applet name or file name. Use a full path when names are ambiguous or the applet is outside the SDK standard locations (`dll/<board>`, `Hardware Applets/<board>`).
  - Validation failures (unknown board or applet, acquisition running) leave the loaded applet untouched.
  - Otherwise the current applet is released and the board is initialized with `Fg_Init` (`Fg_InitEx` with `FG_INIT_FLAG_DEFAULT`).
  - If initialization fails, the previous applet and configuration are restored.
  - `lastError()` returns the SDK code and description of the failed request.
  - Each successful open increments `connectionGeneration()`.
  - Slave initialization (`FG_INIT_FLAG_SLAVE`) is not supported.
- **Configuration files.** `loadAppletConfiguration("", mcf, board)` reads the applet from the `.mcf` file (`[fglib5] FileName`).
  - If that applet is already loaded, only `Fg_loadConfig` runs.
  - Otherwise the board is initialized with `Fg_InitConfig`. If the SDK cannot locate the MCF applet itself, the locally resolved applet is used with `Fg_Init` + `Fg_loadConfig`.
  - A non-empty applet argument replaces the MCF reference.
  - `saveConfiguration()` writes the `.mcf` with `Fg_saveConfig`; field parameters go to the `.mfs` file with the same stem.
- **CoaXPress.** Applets tagged `interface=cxp` (except test applets) initialize `Sgc_initBoard`.
  - Discovery scans every physical port reported by the board (four or five CXP ports depending on the model) for 10 s. `refreshCameras()` restarts discovery with `SGC_SP_FLAG_RESET_DISCOVERY_INFOS`.
  - Opening an applet, loading a configuration, refreshing cameras, or connecting a newly discovered camera aligns each connected CXP camera's `Width`/`Height` with the applet's `FG_WIDTH`/`FG_HEIGHT`. Refresh synchronizes every camera that connected, even when another channel failed. A manual connect is refused while acquisition is running, returns only its DMA's geometry synchronization result, and records a failure in `lastError`; a manual disconnect is also refused while acquisition runs. `lastError` is refreshed by these commands. A failed SDK disconnect leaves the cached camera connected and records the SDK error. For camera-control applets, a camera startup failure stops that DMA worker instead of leaving it in the frame wait loop; test and non-CXP applets continue without a camera. Camera startup is serialized with the stop phase, so a stop request cannot be followed by a new camera acquisition start. Missing applet parameters are not applicable, while unreadable geometry, a single missing parameter, write failures and read-back mismatches are failures that retain `lastError`; the previous pair is restored when the rollback read-back verifies it.
  - `ERR_SGC_TOPOLOGY_MISMATCH` is reported through `lastCameraDiscovery()`: the cameras answer, but the applet layout cannot transfer their images.
  - Cameras without an applet operator index (`CAM_PROP_APPLETOPERATORINDEX`) are not bound to a DMA.
- **Control gate.** `tryBeginControl()` / `endControl()` serialize lifecycle commands from every control surface. Status callbacks report the gate as `ControlStatus`, and parameter replacement without a reopen as `ConfigurationStatus`.

## Validation

Validate applet loading, every active DMA channel, payload size and pixel format, timeout handling, camera discovery, feature access, shutdown, and sustained copy/pool pressure on the intended board and camera. A configure or link result is not a hardware acceptance test.

## Installed consumer SDK plugin build

Build the existing plugin project using this repository, an installed Plugin
SDK, the matching Qt kit and its vendor SDK. No consumer source tree or
renderer is needed. The plugin switch enables its owning widget/adapter
(and vendor SDK implementation where required). Theme installation remains
consumer-owned.

```sh
cmake -S C++ -B build/plugin -DCMAKE_PREFIX_PATH="<PluginSDK>;<Qt-prefix>" -DCMAKE_CONFIGURATION_TYPES=Release -DCMAKE_BUILD_TYPE=Release -DFRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN=ON -DFRAMEGRABBER_BUILD_TESTS=ON
cmake --build build/plugin --config Release --parallel 4
ctest --test-dir build/plugin -C Release --output-on-failure
cmake --build build/plugin --config Release --target FramegrabberPluginPackageStage
```

Supply the existing vendor SDK options when it is not in the documented default
location. The generated package is `build/plugin/packages/Release/plugins/<id>/current`
on multi-configuration generators and retains the module-owned runtime layout.
It includes the manifest, icon and SDK identity. Staging reconciles only this
generated package. Use a fresh build directory or explicitly update
`PlaygroundPluginSDK_DIR` when selecting another SDK. This Release software
gate does not establish physical-device, Debug or cross-platform acceptance.
Set `CMAKE_BUILD_TYPE=Release` for a single-configuration generator.

## Qt presentation

`C++/Utility/Qt` provides the SDK-independent `FramegrabberPresentation` target.
The runtime Qt widget and isolated presentation consumers use the same
`FramegrabberControlUi` composition; device discovery, feature access and
acquisition remain in the runtime widget. The presentation target does not
install an application theme.

The Setup tab contains:

- a loadable-applet selector with refresh, load and release commands;
- file-based loading of an applet or `.mcf` file, and configuration saving;
- the applet feature tree for each DMA.

Applet and camera numeric features use native spin editors that preserve signed
and unsigned integer widths and floating-point round trips. Applet bounds and
increments come from `getAppletParameterLimits`; camera bounds come from the live
`Sgc_getIntegerValueLimits` / `Sgc_getFloatValueLimits` calls through
`getCameraFeatureLimits`. Missing limits remain unspecified (the scalar type's
representable range still applies). Positive increments are enforced relative to
a reported minimum. Invalid or off-grid input restores the previous value without
a write. Writes run asynchronously, disable the editor while pending, and restore
the accepted value and refresh the tree on rejection. Read-only editors are
disabled. Commands use Execute buttons, including write-only commands; only
string features use free-text editors. Unknown types are shown as unsupported.

On creation the widget loads the board's active applet (or else its power-up applet). Every lifecycle command takes the module control gate. While another surface holds the gate the commands are disabled, but Live can still be stopped. `setFileDialogPresenter()` lets a consumer supply themed file dialogs. Without one, the widget uses plain Qt dialogs.

## Declared device and parameter contracts

`FramegrabberSourceController` implements `IDeviceConnectionControl/1.0`, `IToolProvider/1.0` and `IDeviceParameterControl/1.0` over the same `Framegrabber` authority as the widget.

Playground declares the standard `device.*` lifecycle tools from `IDeviceConnectionControl`:

| Tool | Framegrabber behavior |
| --- | --- |
| `device.connection` | Canonical state; `targetId` is the loaded board and `displayName` names board and applet |
| `device.disconnect` | Stops acquisition and releases the board (`Fg_FreeGrabber`) |
| `device.single` | Waits up to 30 s for one frame on every DMA |
| `device.targets`, `device.discover`, `device.connect` | Unavailable: a connection is made by loading an applet |
| `device.live-start`, `device.live-stop` | Unavailable: Live uses the common `source.*` tools |

The module declares its own board and applet tools. Observe tools complete synchronously:

| Tool | Result |
| --- | --- |
| `framegrabber.state` | Board, applet, configuration, DMA count, `connectionId`, last SDK error, camera discovery and per-DMA counters |
| `framegrabber.boards` | Discovered boards |
| `framegrabber.applets` | Catalog with active/power-up/loaded flags |
| `framegrabber.cameras` | CXP cameras from the last discovery |

Other tools, and the lifecycle disconnect and Single, run on one worker and hold the control gate:

| Tool | Effect |
| --- | --- |
| `framegrabber.applets-refresh` | Re-enumerates applets and replaces the cached catalog; only while released (device) |
| `framegrabber.applet-load` | Loads `{board, applet}` (device) |
| `framegrabber.configuration-load` | Loads `{board, configuration, applet?}` (device) |
| `framegrabber.configuration-save` | Writes `{connectionId, configuration}` to a host file (device) |
| `framegrabber.cameras-refresh` | Restarts CXP discovery (device) |

Admission rules:

- Replacing a loaded applet requires its current `connectionId`.
- Switching boards requires a release first.
- Acquisition is never stopped implicitly.
- A busy gate is reported as `busy`. A stale `connectionId` is reported as `conflict`.

Applet parameters use the scope `Applet:<dma>` and their SDK access names (for example `FG_WIDTH`). They have no selectors.

- Values carry their exact types. Enumerations are written by entry name. Limits come from `PROP_ID_MIN`, `PROP_ID_MAX` and `PROP_ID_STEP`.
- Revisions change only when an observation changes.
- Each write is revalidated against a fresh observation and reports the actual readback and effect.
- Commands are executed from the parameter's declared SDK type, so write-only commands work.

CXP camera features remain available through the widget and the C++ API. They are not part of the parameter contract.
