## Unreleased

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
