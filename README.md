# Framegrabber

Framegrabber is a C++17 acquisition library for Basler frame grabber boards. It owns SDK handles, applet and configuration loading, multi-DMA acquisition, buffer lifetime, and optional CoaXPress camera control.

## Capabilities

- Discover boards and create independently owned `Framegrabber` instances.
- Load applets and MCF configuration files.
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

The optional Playground adapter is disabled by default. Enable `FRAMEGRABBER_BUILD_PLAYGROUND_ADAPTER` only when the neutral GraphicsEngine contract target is available. Consumers that want owned frames include `Utility/PlaygroundAdapter/FramegrabberGraphicsFrameStream.h` and link `Framegrabber::PlaygroundAdapter`; pixel-format conversion stays in `FramegrabberGraphicsFrameAdapter`. The stream retains one latest owned image per observed DMA channel and publishes an ordered GraphicsFrame snapshot with stable `dma.N` resources after each accepted callback. This is a latest-per-channel view, not a same-exposure cohort. Its single publication boundary rejects delayed lower global frame sequences and clears retained channels between acquisition runs.

The optional Playground device plugin is a separate MODULE. Enable `FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN` only when `Playground::DevicePlugin`, the adapter, `Framegrabber::QtWidget`, and `Resources::Resources` are available. Package identity lives in `Utility/PlaygroundAdapter/Package/Package.cmake` as quoted `set(PLAYGROUND_PLUGIN_* ...)` assignments; the MODULE must set `PLAYGROUND_PLUGIN_PACKAGE` and include `DevicePlugin.h`. The host emits `plugin.json` from that identity plus `DevicePluginPackage.h`.

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

Callbacks run on acquisition workers. Keep them bounded, do not access GUI objects directly, and return `ready(dmaIndex)` only after the corresponding frame has been accepted by the downstream consumer. Stop acquisition and join workers before closing the board or destroying callbacks.

## Validation

Validate applet loading, every active DMA channel, payload size and pixel format, timeout handling, camera discovery, feature access, shutdown, and sustained copy/pool pressure on the intended board and camera. A configure or link result is not a hardware acceptance test.
