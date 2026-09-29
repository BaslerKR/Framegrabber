# Device-owned Playground plugin identity.
# Allowed commands are quoted `set(PLAYGROUND_PLUGIN_* ...)` assignments.
# Host schema, ABI, and icon names come from DevicePluginPackage.h.
# Repeat NAME=value to declare multiple environment paths for one variable.
set(PLAYGROUND_PLUGIN_ID "framegrabber")
set(PLAYGROUND_PLUGIN_VERSION "0.1.2")
set(PLAYGROUND_PLUGIN_DISPLAY_NAME "Frame Grabber")
# Optional presentation metadata; consumers must export it to manifests/catalogs.
set(PLAYGROUND_PLUGIN_SUBTITLE "Basler Frame Grabber SDK")
set(PLAYGROUND_PLUGIN_DESCRIPTION "Discover Basler frame grabbers, load applets, and acquire images from multiple DMA channels.")
set(PLAYGROUND_PLUGIN_ADD_ACTION_TEXT "Frame Grabber")
set(PLAYGROUND_PLUGIN_SESSION_TYPE "Frame Grabber")
set(PLAYGROUND_PLUGIN_MENU_ORDER 200)
set(PLAYGROUND_PLUGIN_LOAD_ORDER 100)
set(PLAYGROUND_PLUGIN_LIBRARY_WINDOWS "FramegrabberPlugin.dll")
set(PLAYGROUND_PLUGIN_LIBRARY_LINUX "FramegrabberPlugin.so")
set(PLAYGROUND_PLUGIN_LIBRARY_MACOS "FramegrabberPlugin.so")
set(PLAYGROUND_PLUGIN_LIBRARY_DIRECTORIES
    "runtime/bin"
    "runtime/bin/plugins"
    "runtime/lib")
set(PLAYGROUND_PLUGIN_ENVIRONMENT_PATHS
    "BASLER_FG_SDK_DIR=runtime")
set(PLAYGROUND_PLUGIN_ENVIRONMENT_FILES
    "BASLER_FGSDK_LOGGING_CONFIG=runtime/bin/BaslerFgSdkLogging.properties")
