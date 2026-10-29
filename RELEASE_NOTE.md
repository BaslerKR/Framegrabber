## v0.2.1 (2026-10-20)

- Match applet open/save controls to the shared neutral icons.

- Use the shared neutral network-card artwork for the package icon.

- Use the shared refresh, single-grab, and live glyphs on the device toolbar.

- Publish the independently built device package with installed SDK validation.

## Unreleased
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
