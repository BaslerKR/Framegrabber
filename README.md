# Framegrabber

Framegrabber is a C++17 acquisition library for Basler frame grabber boards. It owns SDK handles, applet and configuration loading, multi-DMA acquisition, buffer lifetime, and optional CoaXPress camera control.

## Capabilities

- Discover boards and create independently owned `Framegrabber` instances.
- Enumerate the applets the SDK can load on a board and load them by name or path.
- Load and save MCF configuration files.
- Start every configured DMA channel and identify each image by `dmaIndex`.
- Expose applet features and supported camera features through typed APIs.
- Build an optional external GraphicsFrame adapter and a widget-free Playground device plugin.

## Requirements

- CMake 3.16 or newer and a C++17 compiler.
- A Basler Frame Grabber SDK installation containing the FgLib5, ClSerSis, SisoIoLib, SisoPlatform, and SisoGenicamLib CMake packages.
- `BASLER_FG_SDK_DIR` when the SDK is not installed in a searched default location.
- Qt Core, Concurrent and Xml when `FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN=ON`. The plugin package links no Qt Widgets, Resources widget/dialog library or OpenGL.

Operating-system and board support are defined by the installed SDK and driver. Do not infer hardware support from compilation alone.

## Integration

The core library is the default and does not discover or link Qt:

```cmake
add_subdirectory(path/to/Framegrabber/C++ Framegrabber-build)
target_link_libraries(consumer PRIVATE Framegrabber::Framegrabber)
```

The module ships no Qt widget target. Enable `FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN` to build the widget-free device plugin; the host's common device panel is its only session UI.

The optional Playground adapter is disabled by default. Enable `FRAMEGRABBER_BUILD_PLAYGROUND_ADAPTER` only when the neutral GraphicsEngine contract target is available. Consumers that want owned frames include `Utility/PlaygroundAdapter/FramegrabberGraphicsFrameStream.h` and link `Framegrabber::PlaygroundAdapter`; pixel-format conversion stays in `FramegrabberGraphicsFrameAdapter`. The stream publishes one single-image GraphicsFrame for each accepted DMA image, using the DMA index as the source index. It does not assemble frames across DMA channels; the consumer owns multi-source presentation.

The optional image rejection sink reports the first failed validation rule and numeric layout of an owned DMA image at the adapter. Single reports each rejected DMA image. Live reports the first and every hundredth occurrence per DMA source and reason in an acquisition. A rejected DMA transfer or an unavailable capture buffer is observed earlier by the producer counters.

The optional Playground device plugin is a separate MODULE. Enable `FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN` only when `Playground::DeviceRuntimeContract` and the adapter are available. The plugin creates no dock or session widget and links only the runtime contract, the adapter and Qt Core/Concurrent/Xml; no Qt Widgets, Resources widget/dialog library or OpenGL. Package identity lives in `Utility/PlaygroundAdapter/Package/Package.cmake` as quoted `set(PLAYGROUND_PLUGIN_* ...)` assignments; the MODULE must set `PLAYGROUND_PLUGIN_PACKAGE` and include `DeviceRuntime/Contract/DeviceRuntime.h`. The host emits `plugin.json` from that identity plus `DeviceRuntime.h`.

### Playground session contract

- **Connection model.** One exclusive board connection owns the DMA-channel
  sources (`DeviceConnectionModel::Hierarchical`). `device.targets` copies the
  cached boards as Interfaces, and `device.connect` runs the module's opening
  action: re-enumerate the catalog, then load the applet the SDK reports as
  active, else the power-up applet. The connected state carries the stable
  `dma.<index>` source ids. Board discovery remains a session-creation step, and
  Live stays on `source.start`/`source.stop`.
- **Declared actions.** `framegrabber.applets-refresh`, `framegrabber.applet-load`, `framegrabber.applet-load-file`, `framegrabber.configuration-load`, `framegrabber.configuration-save` and `framegrabber.cameras-refresh` publish semantic action and icon ids from the shared Resources set. Applet load declares an enum prompt over the installed catalog; duplicated applet names fall back to their exact catalog paths so every option loads. A missing or non-catalog applet returns the typed `argument_required` prompt without effects. `framegrabber.applet-load-file` declares a Host-provided `file-open` input with applet name filters; the Host dialog supplies the path and the module validates it before `Fg_Init`. Both `file-open` actions (`applet-load-file` and `configuration-load`) are panel-only: MCP, scripts and `tools.invoke` refuse a path with an explicit reason, while the catalog route remains callable. Configuration load offers only installed applets when the MCF reference is outside the catalog; configuration save declares a file-save prompt. Load actions report bounded progress and answer cancellation with an explicit unsupported reason because `Fg_Init`/`Fg_InitConfig` cannot be cancelled.
- **Parameter scopes.** `Applet:<dma>` exposes the applet access names and
  `Camera:<transport>:<dma>` exposes CoaXPress GenICam features. Descriptors
  carry category paths, display text, units, visibility, representation,
  numeric limits, enum entries and exact revisions. Camera writability is the
  access mode observed now, gated by the current acquisition state plus the
  device's `pIsLocked`/`pIsAvailable` gates, and re-observed after every
  transition. `requestParameterChanges` delivers an
  initial snapshot, revision-stamped deltas after writes/executes and
  acquisition transitions measured against the last published inventory, and
  one resync snapshot when the bounded queue overflows; release waits for
  in-flight delivery. Observation runs on a module thread with its own pass
  serialization, waits for accepted control work to settle without holding the
  shared gate, and caches camera feature XML only while its content is
  unchanged so a refresh or camera reconnect replaces the tree.


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

`FramegrabberSystem::setResourceClaimCallback()` is optional. A standalone consumer that does not need process-wide exclusivity omits it and opens boards without a claim. A consumer that requires an exclusive board claim installs a callback before `addFramegrabber()`; the core calls it with the board serial immediately before `Fg_Init`/`Fg_InitConfig`, holds the returned handle until the public close completes, and refuses the open when the callback returns no handle. The Playground plugin always installs the host claim and rejects a missing host service at the plugin boundary.

Each accepted SDK frame is copied once into a leased pool buffer because DMA memory is owned and reused by the SDK. The pool is coordinated with a mutex; it is not lock-free. Copying an `Image` retains its `shared_ptr` storage and can keep a pool slot occupied, so bound retention in consumers. `OwnedBufferPool::acquire()` never waits: when every pool buffer is retained by a consumer the worker drops that frame before the copy and counts it as `poolExhausted`; the acquisition worker never waits for a consumer.

Acquisition uses `Fg_AcquireEx(..., ACQ_STANDARD, ...)` with `setDMABufferCount()` frame buffers (default 6, minimum 2; smaller values are raised to 2) and waits with `Fg_getLastPicNumberBlockingEx`. In this model the driver never blocks a buffer, so the worker delivers the newest completed frame and handles backpressure as follows:

- Frames that completed while the previous frame was still being processed are counted as `skipped`.
- After each copy the worker reads `NUMBER_OF_GRABBED_IMAGES`. The buffer of frame `n` is reused by frame `n + bufferCount`. If that transfer may have started (`grabbed - n >= bufferCount - 1`), the copy is dropped and counted as `overwritten`, because it can contain data from the later frame.
- A frame whose owned pool buffer is still retained by a consumer is dropped before the copy and counted as `poolExhausted`.
- A custom frame-storage callback that throws or returns empty drops that frame and increments `poolExhausted`; the exception does not terminate the acquisition worker.
- `acquisitionStatistics()` reports `delivered`, `skipped`, `overwritten`, `rejected` (invalid pointer or transfer length) and `poolExhausted` for each DMA during and after a run.
- A wait result other than the short wait timeout (for example `FG_TRANSFER_NOT_ACTIVE` after the driver's `FG_TIMEOUT`) ends that DMA worker.

Stopping follows the SDK order: the CXP camera stream (`Sgc_stopAcquisition`) stops first, then the DMA (`Fg_stopAcquireEx`). After that the buffers are freed (`Fg_FreeMemEx`).

Callbacks run on acquisition workers. Keep them bounded, do not access GUI objects directly, and stop acquisition and join workers before closing the board or destroying callbacks. A callback may call `stop()` or `requestStop()`, but those calls only request acquisition stop and do not join the worker; `isGrabbing()==false` therefore does not prove that callback execution has finished. Applet/configuration/camera lifecycle changes and a new `grab()` from a callback are refused with an explicit reason. Calling `close()` from a callback only requests acquisition stop and records that the board was not closed; close the board from an owning thread after workers finish. `tryBeginControl()` and `endControl()` return false and set `lastError()` when called from a callback, and a callback cannot release another operation's control gate. A node-update callback may write the same value, but the nested node notification is suppressed. A callback return is not an admission signal: the worker already moved on to the newest frame and never waits for downstream processing.

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
- **Control gate.** `tryBeginControl()` / `endControl()` serialize lifecycle commands from every control surface. `endControl()` reports whether it released a gate. Both calls refuse callback/lifecycle re-entry with `lastError()`, and status callbacks report the gate as `ControlStatus`; parameter replacement without a reopen is reported as `ConfigurationStatus`.

## Validation

Validate applet loading, every active DMA channel, payload size and pixel format, timeout handling, camera discovery, feature access, shutdown, and sustained copy/pool pressure on the intended board and camera. A configure or link result is not a hardware acceptance test.

## Installed consumer SDK plugin build

Build the existing plugin project using this repository, an installed Plugin
SDK, the matching Qt kit and its vendor SDK. No consumer source tree or
renderer is needed. The plugin switch enables its owning adapter and vendor SDK
implementation. The package has no theme dependency.

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

The adapter tests include the hardware-free `FramegrabberControllerTests`. The
opt-in loopback target runs the Playground `DeviceParameterLoopback` codec
against the controller and requires a Playground checkout:

```sh
cmake -S C++ -B build/loopback -DCMAKE_PREFIX_PATH="<PluginSDK>;<Qt-prefix>" -DCMAKE_CONFIGURATION_TYPES=Release -DCMAKE_BUILD_TYPE=Release -DFRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN=ON -DFRAMEGRABBER_BUILD_TESTS=ON -DFRAMEGRABBER_BUILD_PLAYGROUND_LOOPBACK_TESTS=ON -DFRAMEGRABBER_PLAYGROUND_SOURCE_DIR=<Playground-root>
cmake --build build/loopback --config Release --target FramegrabberParameterLoopbackTests
```

Set `CMAKE_BUILD_TYPE=Release` for a single-configuration generator.

The opt-in hardware smoke test requires a supported board and the SDK `FrameGrabberTest` applet. It validates DMA layout, Single and sustained Live acquisition, repeated start/stop and applet reopen, owned-pool exhaustion, callback exception containment, frame-storage failure and stop re-entry, callback control-gate protection, lifecycle refusal during acquisition, recursive node-notification protection, concurrent stop/close, and feature-model reads during applet replacement. SDK initialization or board-enumeration failures fail the test; it skips only when no board or test applet is available.

```sh
cmake -S C++ -B build/hardware -DCMAKE_CONFIGURATION_TYPES=Release -DCMAKE_BUILD_TYPE=Release -DFRAMEGRABBER_BUILD_HARDWARE_TESTS=ON
cmake --build build/hardware --config Release --target FramegrabberHardwareSmokeTest
ctest --test-dir build/hardware -C Release -R '^FramegrabberHardwareSmokeTest$' --output-on-failure
```

The test does not establish CXP camera, multi-DMA or sustained-throughput acceptance. MCF save/load runs only on SDK 5.11.9 or later because SDK 5.11.7 crashes inside `Fg_saveConfig` for `FrameGrabberTest` even when called directly.

## Declared device and parameter contracts

`FramegrabberSourceController` implements `IDeviceConnectionControl/1.0`, `IToolProvider/1.0` and `IDeviceParameterControl/1.0` over the same `Framegrabber` authority; the host's common device panel is the session UI.

Playground declares the standard `device.*` lifecycle tools from `IDeviceConnectionControl`:

| Tool | Framegrabber behavior |
| --- | --- |
| `device.connection` | Canonical state; `targetId` is the loaded board and `displayName` names board and applet |
| `device.disconnect` | Stops acquisition and releases the board (`Fg_FreeGrabber`) |
| `device.single` | Waits up to 30 s for one frame on every DMA |
| `device.targets` | Lists the cached boards as connection targets |
| `device.discover` | Unavailable: board discovery runs when the session is created |
| `device.connect` | Loads the selected board's active, else power-up, applet as the opening action |
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
| `framegrabber.applet-load` | Loads `{applet, connectionId?, board?}` (device); an omitted board uses the session's connected board |
| `framegrabber.applet-load-file` | Loads `{appletFile, connectionId?, board?}` from a Host-chosen file (device, panel-only); the plugin validates the path before `Fg_Init` |
| `framegrabber.configuration-load` | Loads `{configuration, applet?, connectionId?, board?}` (device, panel-only); the `file-open` path is chosen in the Host dialog and cannot be supplied through `tools.invoke` |
| `framegrabber.configuration-save` | Writes `{connectionId, configuration}` to a host file (device) |
| `framegrabber.cameras-refresh` | Restarts CXP discovery (device) |

Admission rules:

- Applet and configuration loads default the board to the session's connected board; a session without a connected board refuses them without effect.
- Replacing a loaded applet requires its current `connectionId`.
- Switching boards requires a release first.
- Acquisition is never stopped implicitly.
- A busy gate is reported as `busy`. A stale `connectionId` is reported as `conflict`.

Applet parameters use the scope `Applet:<dma>` and their SDK access names (for example `FG_WIDTH`). They have no selectors.

- Values carry their exact types. Enumerations are written by entry name. Limits come from `PROP_ID_MIN`, `PROP_ID_MAX` and `PROP_ID_STEP`.
- Revisions change only when an observation changes.
- Each write is revalidated against a fresh observation and reports the actual readback and effect.
- Commands are executed from the parameter's declared SDK type, so write-only commands work.

CXP camera features remain available through the C++ API and the `Camera:<transport>:<dma>` parameter scope. A CoaXPress applet loaded from a custom file initializes camera control when the SDK reports its tag; when no tag is available the board opens without CXP discovery and the module logs that limitation.
