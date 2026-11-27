cmake_minimum_required(VERSION 3.16)
project(Framegrabber VERSION 0.4.0 LANGUAGES C CXX)

option(FRAMEGRABBER_BUILD_PLAYGROUND_ADAPTER
    "Build the optional Framegrabber Playground adapter target."
    OFF
)
option(FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN
    "Build the Framegrabber Playground device plugin MODULE."
    OFF
)
option(FRAMEGRABBER_BUILD_TESTS "Build focused Framegrabber adapter tests." OFF)
option(FRAMEGRABBER_BUILD_HARDWARE_TESTS
    "Build the opt-in FrameGrabberTest hardware smoke test."
    OFF)
option(FRAMEGRABBER_BUILD_PLAYGROUND_LOOPBACK_TESTS
    "Build the Playground parameter loopback test for the Framegrabber controller."
    OFF
)
set(FRAMEGRABBER_PLAYGROUND_SOURCE_DIR "" CACHE PATH
    "Playground checkout used only by the opt-in loopback test.")

if(FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN)
    set(FRAMEGRABBER_BUILD_PLAYGROUND_ADAPTER ON)
endif()
if(PROJECT_NAME STREQUAL "Framegrabber" AND PROJECT_IS_TOP_LEVEL AND
   (FRAMEGRABBER_BUILD_PLAYGROUND_ADAPTER OR FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN))
    find_package(PlaygroundPluginSDK 0.3 CONFIG REQUIRED)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT BASLER_FG_SDK_DIR AND DEFINED ENV{BASLER_FG_SDK_DIR})
    set(BASLER_FG_SDK_DIR "$ENV{BASLER_FG_SDK_DIR}")
endif()

find_path(BASLER_FG_SDK_DIR
    NAMES cmake/modules/FindFgLib5.cmake
    HINTS
        "${BASLER_FG_SDK_DIR}"
        "/opt/Basler/FramegrabberSDK"
        "C:/Program Files/Basler/FramegrabberSDK"
)

if(NOT BASLER_FG_SDK_DIR)
    message(FATAL_ERROR
        "[Framegrabber] Basler Frame Grabber SDK not found. "
        "Set BASLER_FG_SDK_DIR to the SDK installation root."
    )
endif()

file(TO_CMAKE_PATH "${BASLER_FG_SDK_DIR}" BASLER_FG_SDK_DIR)
set(ENV{BASLER_FG_SDK_DIR} "${BASLER_FG_SDK_DIR}")
list(PREPEND CMAKE_MODULE_PATH "${BASLER_FG_SDK_DIR}/cmake/modules")
set(SISOPLATFORM_DIR "${BASLER_FG_SDK_DIR}/include")

find_package(FgLib5 REQUIRED)
find_package(ClSerSis REQUIRED)
find_package(SisoIoLib REQUIRED)
find_package(SisoPlatform REQUIRED)
find_package(SisoGenicamLib REQUIRED)

add_library(Framegrabber STATIC
    Framegrabber.h
    Framegrabber.cpp
    FramegrabberCameraGeometry.h
    FramegrabberCaptureTransaction.h
    FramegrabberOwnedBufferPool.h
    FramegrabberSystem.h
    FramegrabberSystem.cpp
)
add_library(Framegrabber::Framegrabber ALIAS Framegrabber)

target_include_directories(Framegrabber
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${FgLib5_INCLUDE_DIR}"
        "${ClSerSis_INCLUDE_DIR}"
        "${SisoIoLib_INCLUDE_DIR}"
        "${SisoPlatform_INCLUDE_DIR}"
        "${SisoGenicamLib_INCLUDE_DIR}"
        "${GENICAM_INCLUDE_DIR}"
)

target_link_libraries(Framegrabber
    PUBLIC
        ${FgLib5_LIBRARIES}
        ${ClSerSis_LIBRARIES}
        ${SisoIoLib_LIBRARIES}
        ${SisoGenicamLib_LIBRARIES}
)

if(FRAMEGRABBER_BUILD_TESTS)
    enable_testing()
    add_executable(framegrabber_producer_runtime_counters_test Tests/ProducerRuntimeCountersTest.cpp)
    target_link_libraries(framegrabber_producer_runtime_counters_test PRIVATE Framegrabber::Framegrabber)
    add_test(NAME framegrabber_producer_runtime_counters_test COMMAND framegrabber_producer_runtime_counters_test)
    add_executable(framegrabber_camera_geometry_sync_test
        Tests/CameraGeometrySyncTest.cpp
    )
    target_include_directories(framegrabber_camera_geometry_sync_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
        NAME framegrabber_camera_geometry_sync_test
        COMMAND framegrabber_camera_geometry_sync_test
    )
    add_executable(framegrabber_owned_buffer_pool_test
        Tests/OwnedBufferPoolTest.cpp
    )
    target_include_directories(framegrabber_owned_buffer_pool_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
        NAME framegrabber_owned_buffer_pool_test
        COMMAND framegrabber_owned_buffer_pool_test
    )
    add_executable(framegrabber_resource_claim_test
        Tests/ResourceClaimTest.cpp
    )
    target_link_libraries(framegrabber_resource_claim_test PRIVATE
        Framegrabber::Framegrabber
    )
    add_test(
        NAME framegrabber_resource_claim_test
        COMMAND framegrabber_resource_claim_test
    )
    add_executable(framegrabber_callback_reentry_test Tests/CallbackReentryTest.cpp)
    target_link_libraries(framegrabber_callback_reentry_test PRIVATE Framegrabber::Framegrabber)
    add_test(NAME framegrabber_callback_reentry_test COMMAND framegrabber_callback_reentry_test)
    add_executable(framegrabber_image_layout_test Tests/ImageLayoutTest.cpp)
    target_include_directories(framegrabber_image_layout_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME framegrabber_image_layout_test COMMAND framegrabber_image_layout_test)
endif()

if(SISOPLATFORM_DEFINITIONS)
    target_compile_definitions(Framegrabber PRIVATE ${SISOPLATFORM_DEFINITIONS})
endif()

if(FRAMEGRABBER_BUILD_PLAYGROUND_ADAPTER)
    if(NOT TARGET GraphicsEngine::Contracts)
        message(FATAL_ERROR
            "[Framegrabber] FRAMEGRABBER_BUILD_PLAYGROUND_ADAPTER=ON requires "
            "GraphicsEngine::Contracts. Add GraphicsEngine first or disable the adapter."
        )
    endif()

    add_library(FramegrabberPlaygroundAdapter STATIC
        Utility/PlaygroundAdapter/FramegrabberGraphicsFrameAdapter.h
        Utility/PlaygroundAdapter/FramegrabberGraphicsFrameAdapter.cpp
        Utility/PlaygroundAdapter/FramegrabberGraphicsFrameStream.h
        Utility/PlaygroundAdapter/FramegrabberGraphicsFrameStream.cpp
    )
    add_library(
        Framegrabber::PlaygroundAdapter
        ALIAS FramegrabberPlaygroundAdapter
    )
    target_compile_features(FramegrabberPlaygroundAdapter PUBLIC cxx_std_17)
    target_compile_definitions(FramegrabberPlaygroundAdapter PUBLIC
        $<$<PLATFORM_ID:Windows>:NOMINMAX>)
    target_include_directories(FramegrabberPlaygroundAdapter
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter"
    )
    target_link_libraries(FramegrabberPlaygroundAdapter
        PUBLIC
            Framegrabber::Framegrabber
            GraphicsEngine::Contracts
    )
    message(STATUS "[Framegrabber] Framegrabber::PlaygroundAdapter enabled.")

    if(FRAMEGRABBER_BUILD_TESTS)
        enable_testing()
        add_executable(framegrabber_graphics_image_adapter_test
            Utility/PlaygroundAdapter/Tests/GraphicsFrameAdapterTest.cpp
        )
        target_link_libraries(framegrabber_graphics_image_adapter_test
            PRIVATE
                Framegrabber::PlaygroundAdapter
        )
        add_test(
            NAME framegrabber_graphics_image_adapter_test
            COMMAND framegrabber_graphics_image_adapter_test
        )
    endif()
endif()

if(FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN)
    if(NOT FRAMEGRABBER_BUILD_PLAYGROUND_ADAPTER)
        message(FATAL_ERROR
            "[Framegrabber] FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "FRAMEGRABBER_BUILD_PLAYGROUND_ADAPTER."
        )
    endif()
    if(NOT TARGET Playground::DeviceRuntimeContract)
        message(FATAL_ERROR
            "[Framegrabber] FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "Playground::DeviceRuntimeContract."
        )
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Xml)

    add_library(FramegrabberPlugin MODULE
        Utility/PlaygroundAdapter/Plugin/FramegrabberPlugin.cpp
        Utility/PlaygroundAdapter/Source/FramegrabberSourceController.h
        Utility/PlaygroundAdapter/Source/FramegrabberSourceController.cpp
        Utility/PlaygroundAdapter/Source/FramegrabberParameterControl.cpp
        Utility/PlaygroundAdapter/Source/FramegrabberParameterNotifications.h
        Utility/PlaygroundAdapter/Source/FramegrabberParameterNotifications.cpp
    )
    playground_sdk_add_runtime_contract(FramegrabberPlugin)
    set_target_properties(FramegrabberPlugin PROPERTIES
        PREFIX ""
        OUTPUT_NAME FramegrabberPlugin
        AUTOMOC ON
        PLAYGROUND_PLUGIN_PACKAGE
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/Package.cmake"
    )
    target_compile_features(FramegrabberPlugin PRIVATE cxx_std_17)
    target_include_directories(FramegrabberPlugin PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source"
        "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Plugin"
    )
    target_link_libraries(FramegrabberPlugin PRIVATE
        Playground::DeviceRuntimeContract
        Framegrabber::PlaygroundAdapter
        Qt6::Core
        Qt6::Concurrent
        Qt6::Xml
    )
    include("${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/RuntimeStaging.cmake")
    framegrabber_prepare_playground_plugin_runtime(FramegrabberPlugin)
    if(PROJECT_NAME STREQUAL "Framegrabber" AND PROJECT_IS_TOP_LEVEL)
        if(APPLE)
            set_target_properties(FramegrabberPlugin PROPERTIES BUILD_WITH_INSTALL_RPATH ON
                INSTALL_RPATH "@loader_path/runtime;@executable_path/../Frameworks")
        elseif(UNIX)
            set_target_properties(FramegrabberPlugin PROPERTIES BUILD_WITH_INSTALL_RPATH ON
                INSTALL_RPATH "$ORIGIN/runtime")
        endif()
        playground_sdk_package_plugin(FramegrabberPlugin
            PACKAGE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/Package.cmake"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/icon.png")
    endif()
    message(STATUS "[Framegrabber] FramegrabberPlugin enabled.")
    if(FRAMEGRABBER_BUILD_TESTS)
        find_package(Qt6 REQUIRED COMPONENTS Test Network)
        add_executable(FramegrabberControllerTests
            Tests/FramegrabberControllerTests.cpp
            Utility/PlaygroundAdapter/Source/FramegrabberSourceController.cpp
            Utility/PlaygroundAdapter/Source/FramegrabberSourceController.h
            Utility/PlaygroundAdapter/Source/FramegrabberParameterControl.cpp
            Utility/PlaygroundAdapter/Source/FramegrabberParameterNotifications.cpp
            Utility/PlaygroundAdapter/Source/FramegrabberParameterNotifications.h
        )
        set_target_properties(FramegrabberControllerTests PROPERTIES AUTOMOC ON)
        playground_sdk_add_runtime_contract(FramegrabberControllerTests)
        target_include_directories(FramegrabberControllerTests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter"
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source"
        )
        target_link_libraries(FramegrabberControllerTests PRIVATE
            Playground::DeviceRuntimeContract
            Framegrabber::PlaygroundAdapter
            Qt6::Test
            Qt6::Concurrent
            Qt6::Xml
        )
        add_test(NAME FramegrabberControllerTests COMMAND FramegrabberControllerTests)
        set_tests_properties(FramegrabberControllerTests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        if(FRAMEGRABBER_BUILD_PLAYGROUND_LOOPBACK_TESTS)
            if(NOT FRAMEGRABBER_PLAYGROUND_SOURCE_DIR OR NOT EXISTS
               "${FRAMEGRABBER_PLAYGROUND_SOURCE_DIR}/src/DeviceRuntime/Protocol/DeviceParameterLoopback.cpp")
                message(FATAL_ERROR
                    "[Framegrabber] FRAMEGRABBER_BUILD_PLAYGROUND_LOOPBACK_TESTS=ON requires "
                    "FRAMEGRABBER_PLAYGROUND_SOURCE_DIR to name a Playground checkout."
                )
            endif()
            add_executable(FramegrabberParameterLoopbackTests
                Tests/FramegrabberParameterLoopbackTests.cpp
                Utility/PlaygroundAdapter/Source/FramegrabberSourceController.cpp
                Utility/PlaygroundAdapter/Source/FramegrabberSourceController.h
                Utility/PlaygroundAdapter/Source/FramegrabberParameterControl.cpp
                Utility/PlaygroundAdapter/Source/FramegrabberParameterNotifications.cpp
                Utility/PlaygroundAdapter/Source/FramegrabberParameterNotifications.h
                "${FRAMEGRABBER_PLAYGROUND_SOURCE_DIR}/src/DeviceRuntime/Protocol/DeviceParameterLoopback.cpp"
                "${FRAMEGRABBER_PLAYGROUND_SOURCE_DIR}/src/DeviceRuntime/Protocol/DeviceParameterService.cpp"
                "${FRAMEGRABBER_PLAYGROUND_SOURCE_DIR}/src/DeviceRuntime/Protocol/DeviceRuntimeProtocol.cpp"
                "${FRAMEGRABBER_PLAYGROUND_SOURCE_DIR}/src/DeviceRuntime/Contract/AbstractSourceController.h"
            )
            set_target_properties(FramegrabberParameterLoopbackTests PROPERTIES AUTOMOC ON)
            target_include_directories(FramegrabberParameterLoopbackTests PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter"
                "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source"
                "${FRAMEGRABBER_PLAYGROUND_SOURCE_DIR}/src/Tools"
                "${FRAMEGRABBER_PLAYGROUND_SOURCE_DIR}/src/Api"
                "${FRAMEGRABBER_PLAYGROUND_SOURCE_DIR}/src"
            )
            target_link_libraries(FramegrabberParameterLoopbackTests PRIVATE
                Framegrabber::PlaygroundAdapter
                Qt6::Test
                Qt6::Concurrent
                Qt6::Xml
                Qt6::Network
            )
            add_test(NAME FramegrabberParameterLoopbackTests COMMAND FramegrabberParameterLoopbackTests)
            set_tests_properties(FramegrabberParameterLoopbackTests PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endif()
    endif()
endif()

if(FRAMEGRABBER_BUILD_HARDWARE_TESTS)
    enable_testing()
    add_executable(FramegrabberHardwareSmokeTest Tests/HardwareSmokeTest.cpp)
    target_link_libraries(FramegrabberHardwareSmokeTest PRIVATE Framegrabber::Framegrabber)
    add_test(NAME FramegrabberHardwareSmokeTest COMMAND FramegrabberHardwareSmokeTest)
    set_tests_properties(FramegrabberHardwareSmokeTest PROPERTIES
        LABELS "hardware;framegrabber"
        SKIP_RETURN_CODE 77
        TIMEOUT 180)
endif()

message(STATUS "[Framegrabber] Basler Frame Grabber SDK root: ${BASLER_FG_SDK_DIR}")

# Keep contract assertions active in Release validation.
foreach(module_test IN ITEMS framegrabber_graphics_image_adapter_test)
    if(TARGET ${module_test})
        target_compile_options(${module_test} PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
    endif()
endforeach()
