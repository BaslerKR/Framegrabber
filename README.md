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
        board->ready(image.dmaIndex);
    });
```

Each accepted SDK frame is copied once into a leased pool buffer because DMA memory is owned and reused by the SDK. The pool is coordinated with a mutex and condition variable; it is not lock-free. Copying an `Image` retains its `shared_ptr` storage and can keep a pool slot occupied, so bound retention in consumers.

Acquisition uses `Fg_AcquireEx(..., ACQ_STANDARD, ...)` with `setDMABufferCount()` frame buffers (default 4, minimum 2; smaller values are raised to 2) and waits with `Fg_getLastPicNumberBlockingEx`. In this model the driver never blocks a buffer, so the worker delivers the newest completed frame and handles backpressure as follows:

- Frames that completed while the previous frame was still being processed are counted as `skipped`.
- After each copy the worker reads `NUMBER_OF_GRABBED_IMAGES`. The buffer of frame `n` is reused by frame `n + bufferCount`. If that transfer may have started (`grabbed - n >= bufferCount - 1`), the copy is dropped and counted as `overwritten`, because it can contain data from the later frame.
- `acquisitionStatistics()` reports `delivered`, `skipped`, `overwritten` and `rejected` (invalid pointer or transfer length) for each DMA during and after a run.
- A wait result other than the short wait timeout (for example `FG_TRANSFER_NOT_ACTIVE` after the driver's `FG_TIMEOUT`) ends that DMA worker.

Stopping follows the SDK order: the CXP camera stream (`Sgc_stopAcquisition`) stops first, then the DMA (`Fg_stopAcquireEx`). After that the buffers are freed (`Fg_FreeMemEx`).

Callbacks run on acquisition workers. Keep them bounded, do not access GUI objects directly, and return `ready(dmaIndex)` only after the corresponding frame has been accepted by the downstream consumer. Stop acquisition and join workers before closing the board or destroying callbacks.

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
  - Discovery scans every port (`0xf`) for 10 s. `refreshCameras()` restarts discovery with `SGC_SP_FLAG_RESET_DISCOVERY_INFOS`.
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

On creation the widget loads the board's active applet (or else its power-up applet). Every lifecycle command takes the module control gate. While another surface holds the gate the commands are disabled, but Live can still be stopped. `setFileDialogPresenter()` lets a consumer supply themed file dialogs. Without one, the widget uses plain Qt dialogs.

## Declared device and parameter contracts

`FramegrabberSourceController` implements `IToolProvider/1.0` and `IDeviceParameterControl/1.0` over the same `Framegrabber` authority as the widget.

Observe tools complete synchronously:

| Tool | Result |
| --- | --- |
| `device.connection` | Board, applet, configuration, DMA count, `connectionId`, last SDK error, camera discovery and per-DMA counters |
| `device.boards` | Discovered boards |
| `device.applets` | Catalog with active/power-up/loaded flags |
| `device.cameras` | CXP cameras from the last discovery |

Other tools run on one worker and hold the control gate:

| Tool | Effect |
| --- | --- |
| `device.applets-refresh` | Re-enumerates applets; only while released (observe) |
| `device.applet-load` | Loads `{board, applet}` (device) |
| `device.configuration-load` | Loads `{board, configuration, applet?}` (device) |
| `device.configuration-save` | Writes `{connectionId, configuration}` to a host file (device) |
| `device.disconnect` | Releases the board (device) |
| `device.cameras-refresh` | Restarts CXP discovery (device) |
| `device.single` | Waits up to 30 s for one frame on every DMA (pipeline) |

Live start/stop uses the common `source.*` tools.

Admission rules:

- Replacing a loaded applet requires its current `connectionId`.
- Switching boards requires a release first.
- Acquisition is never stopped implicitly.
- A busy gate is reported as `busy`. A connection-generation mismatch is reported as `conflict`.

Applet parameters use the scope `Applet:<dma>` and their SDK access names (for example `FG_WIDTH`). They have no selectors.

- Values carry their exact types. Enumerations are written by entry name. Limits come from `PROP_ID_MIN`, `PROP_ID_MAX` and `PROP_ID_STEP`.
- Revisions change only when an observation changes.
- Each write is revalidated against a fresh observation and reports the actual readback and effect.
- Commands are executed from the parameter's declared SDK type, so write-only commands work.

CXP camera features remain available through the widget and the C++ API. They are not part of the parameter contract.
