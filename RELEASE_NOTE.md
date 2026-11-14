## Unreleased

- Add `Framegrabber::setFrameStorage()`: the consumer may supply the storage delivered frames are copied into. With it set, the grabber allocates no buffer pool of its own and the copy out of the DMA buffer goes straight into that storage. The Playground source controller forwards host-provided storage (`IFrameStorageClient`), which needs a Playground release that ships `FrameStorage.h`.

## v0.3.0 (2026-11-13)

- Require Playground 0.6.6 or later; this package uses the device plugin interface introduced in that release.

- Claim the host-owned `siso-board:<serial>` resource immediately before `Fg_Init`/`Fg_InitConfig` and hold it until the public close completes. Release the claim when an open or previous-applet restore fails without an adopted handle, so a failed open cannot block another plugin. A board already owned by another Playground plugin is rejected with that plugin's package id and display name instead of attempting a competing native open. The core claim callback remains optional for standalone consumers; the Playground plugin rejects a missing host service. Requires the ABI 7.0 host resource-claim contract.

- Ignore wheel input on unfocused numeric editors so scrolling a feature tree does
  not write device parameters. Clamp floating-point multi-step input to the last
  complete step within the SDK range; use the same tolerance for arrow/key availability.

- Use numeric spin editors for applet and camera integer/float features, with SDK
  bounds and increments, exact 64-bit integers, floating-point round trips and
  rejected-write restoration. Keep commands as Execute buttons and stop treating
  unknown feature types as strings. Add hardware-independent editor regressions.

- Use one tested wait-result classification for DMA loop actions and failure accounting, including idle polls and shutdown.

- Keep one warning per adapter failure at the DMA callback boundary and directly test producer counters, idle-poll exclusion and per-DMA aggregation.

- Add per-run producer diagnostics and Live-only FPS/failure bubbles. Exclude idle polls and DMA policy drops; expose the same counters through the source diagnostics query.

- Remove host-coupled acquisition pacing: the DMA worker no longer waits for a consumer `ready()` credit. The owned buffer pool is non-blocking and a frame whose pool buffer is still retained is dropped before the copy and counted as `poolExhausted`; the ACQ_STANDARD newest-frame and overwrite checks are unchanged. The default DMA buffer count is 6, which raises the per-channel DMA plus owned-pool memory at 127 MP to about 1.53 GB (two 127 MB rings of six buffers).

- Scan every CXP port reported by the board instead of a fixed four-port mask, so Penta boards discover cameras on port 4. After an applet open, configuration load, camera refresh, or newly connected camera, align each connected CXP camera's `Width`/`Height` with the applet's `FG_WIDTH`/`FG_HEIGHT`. Refresh synchronizes every connected camera even on partial discovery. Manual connect and disconnect are refused while acquisition runs, connect returns only its DMA geometry synchronization result, and camera startup is serialized with stop so no camera starts after a stop request. The connect/disconnect commands refresh `lastError` on success. A failed SDK disconnect leaves the cached camera connected and records the error, and a camera-control startup failure stops its DMA worker instead of spinning the frame wait loop; test and non-CXP applets continue without a camera. Missing applet parameters are not applicable; unreadable geometry, write failures and read-back mismatches retain `lastError` and restore the previous pair when the rollback read-back verifies it.

- Report the frame grabber connection, release it and acquire one frame per DMA through Playground's standard device connection tools. Loading an applet still makes the connection, so the standard target and connect tools are shown as unavailable with that reason. The board, applet, configuration and camera tools are renamed from `device.*` to `framegrabber.*`, and the detailed board/applet/counter state moves to `framegrabber.state`. Builds against Plugin SDK 0.2 and requires a Playground release with the standard device connection interface.
- Declare what kind of data each automation tool returns, so Playground can limit what connected assistants receive. Requires a Playground release with the updated automation interface; earlier releases do not list this package's automation tools.
- Take the binary display name from the package definition (`Frame Grabber`).

## v0.2.1 (2026-10-20)

- Match applet open/save controls to the shared neutral icons.

- Use the shared neutral network-card artwork for the package icon.

- Use the shared refresh, single-grab, and live glyphs on the device toolbar.

- Publish the independently built device package with installed SDK validation.

## v0.2.2 (2026-10-29)

- Stop the missing-applet flow when shutdown begins while its picker is open.

- Show the next Live grab command with cameras/stop artwork in all pointer and focus states, and keep its tooltip and accessible name synchronized with acquisition state.

- Raise the DMA buffer count to at least 2; with one buffer every copied frame was discarded as overwritten, so Live delivered nothing and Single timed out.
- Report the SDK result of rejected applet commands instead of `FG_OK`; `executeAppletCommandById()` returns it through an optional output and reports `FG_INVALID_PARAMETER` for unavailable or non-integral command types.
- Route missing-applet selection through the consumer `FileDialogPresenter` used by Load and Save, and remove the separate missing-applet resolver. Without a presenter the widget uses Qt's non-native file dialog.

- Declare semantic layout, device-panel and action-density scopes through Qt properties; preserve independent plain-Qt builds without a theme dependency.

- Share the SDK-independent Qt control composition between the runtime widget and isolated presentation consumers.

- Enumerate loadable applets with the documented SDK iterator (`FG_AIS_FILESYSTEM`, `FG_AF_IS_LOADABLE`). The Setup tab can select, refresh, load and release them, and the session starts with the board's active applet.
- Load applets by unique name or path. Validation failures leave the loaded applet untouched, and SDK error codes and descriptions are reported.
- Initialize from MCF files with `Fg_InitConfig`. When the MCF applet is already loaded, use `Fg_loadConfig`.
- Share one process-wide `Fg_InitLibraries` reference, so discovery no longer releases the SDK under an open session.
- Restart CXP discovery on refresh (`SGC_SP_FLAG_RESET_DISCOVERY_INFOS`), scan all four ports, report applet/camera topology mismatches, and skip cameras without an applet operator index. A missing camera no longer fails configuration loading.
- Stop the CXP camera before the DMA. End a DMA worker when the driver ends its acquisition, instead of retrying. Count skipped, overwritten and rejected frames, and drop copies whose `ACQ_STANDARD` buffer may have been reused during the copy.
- Read `FG_NR_OF_DMAS` per applet process, read string parameters without a fixed buffer, and execute write-only applet commands.
- Declare `device.*` lifecycle tools and `IDeviceParameterControl` applet parameters (`Applet:<dma>`). They share the widget's control gate.
- Let consumers supply themed file dialogs to the control widget.
- Require Core 0.6.0, the first Core that provides the tool and parameter interfaces.

## v0.2.0 (2026-10-08)

- Declare package version 0.2.0, matching the existing project version and release.
- Stage the installed Frame Grabber SDK `bin` directory, and bundle applet directories only for iF and iW CXP boards.
- Package the current GenICam 3.5 runtime without requiring retired GenICam 3.1 serial-protocol DLLs.
- Document explicit Release-only installed SDK configuration.
- Build the standalone device plugin against an installed consumer SDK and stage its module-owned runtime package.

- Report acquisition lifecycle to consumers and retain opt-in stopped-device feature snapshots at the current selector state; expose parameter requests and operation outcomes through diagnostics without changing capture policy.

- Declare the Frame Grabber display name, SDK subtitle, and multi-DMA acquisition summary in package metadata.

- Register stable `dma.N` presentation sources and publish one single-image GraphicsFrame per DMA channel so GraphicsEngine owns automatic single-view/Tabs/Grid presentation without cross-DMA frame assembly.

- Publish DMA images through the identified GraphicsFrame resource collection contract.

- Limit the control statusbar to Idle, Connected, and Live; route operation messages to application logs.

- Publish the frame-grabber controls through the generic host-managed plugin dock contract.

- Link `FramegrabberPlugin` only through `Framegrabber::QtWidget` and `Framegrabber::PlaygroundAdapter` so the core static library is not passed twice to the linker.

- Require ABI / Qt IID 5.0 and recompilation; older packages are rejected. Publish source descriptors through the neutral controller contract.

- Link `Playground::DevicePlugin` for the `IDevicePlugin` MODULE.
- Own package identity in `Utility/PlaygroundAdapter/Package/Package.cmake`; the host emits `plugin.json` from `DevicePluginPackage.h`.
- Keep the session source controller in `Utility/PlaygroundAdapter/Source` as `FramegrabberSourceController`.
- Declare the Playground plugin runtime payload from this module; the host copies it into the package.
- Move the GraphicsFrame stream/adapter into `Utility/PlaygroundAdapter` as `Framegrabber::PlaygroundAdapter`, and rename the stream/converter to the GraphicsFrame contract.
- Publish GraphicsFrame through `FramegrabberGraphicsFrameStream`; the converter header stays in the adapter translation unit.

- Drain in-flight GraphicsFrame adapter callbacks before stream destruction and cover the shared callback gate contract.
- Move DMA callback registration, ownership return, and GraphicsFrame conversion into the module adapter stream; the parent receives only owned GraphicsFrame values.
- Keep the GraphicsFrame adapter target independent of Qt GUI.
- Keep frame-grabber output on the canonical `GraphicsFrame` host boundary.
- Split the opt-in Qt control panel into `Framegrabber::QtWidget`, leaving the default `Framegrabber::Framegrabber` target free of Qt dependencies.
- Updated the optional scene adapter to consume a neutral scene-contract target without inheriting the visualization runtime; image conversion output is unchanged.
- Replace the corrupted host-layout README with a standalone acquisition contract and correct the buffer pool description.
