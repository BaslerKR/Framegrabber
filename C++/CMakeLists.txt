cmake_minimum_required(VERSION 3.16)
project(Framegrabber LANGUAGES C CXX)

option(FRAMEGRABBER_BUILD_QT_WIDGET "Build the optional Qt control widget." OFF)
option(FRAMEGRABBER_BUILD_PLAYGROUND_ADAPTER
    "Build the optional Framegrabber Playground adapter target."
    OFF
)
option(FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN
    "Build the Framegrabber Playground device plugin MODULE."
    OFF
)
option(FRAMEGRABBER_BUILD_TESTS "Build focused Framegrabber adapter tests." OFF)

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

if(SISOPLATFORM_DEFINITIONS)
    target_compile_definitions(Framegrabber PRIVATE ${SISOPLATFORM_DEFINITIONS})
endif()

if(FRAMEGRABBER_BUILD_QT_WIDGET)
    if(TARGET Qt6::Core)
        set(FRAMEGRABBER_QT_VERSION_MAJOR 6)
    elseif(TARGET Qt5::Core)
        set(FRAMEGRABBER_QT_VERSION_MAJOR 5)
    else()
        find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Xml)
        set(FRAMEGRABBER_QT_VERSION_MAJOR "${QT_VERSION_MAJOR}")
    endif()
    find_package(
        Qt${FRAMEGRABBER_QT_VERSION_MAJOR}
        REQUIRED
        COMPONENTS Core Gui Widgets Xml
    )

    add_library(FramegrabberQtWidget STATIC
        Utility/Qt/QFramegrabberWidget.h
        Utility/Qt/QFramegrabberWidget.cpp
    )
    add_library(Framegrabber::QtWidget ALIAS FramegrabberQtWidget)
    set_target_properties(FramegrabberQtWidget PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        AUTOUIC ON
    )
    target_compile_features(FramegrabberQtWidget PUBLIC cxx_std_17)
    target_include_directories(FramegrabberQtWidget
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(FramegrabberQtWidget
        PUBLIC
            Framegrabber::Framegrabber
            Qt${FRAMEGRABBER_QT_VERSION_MAJOR}::Core
            Qt${FRAMEGRABBER_QT_VERSION_MAJOR}::Gui
            Qt${FRAMEGRABBER_QT_VERSION_MAJOR}::Widgets
            Qt${FRAMEGRABBER_QT_VERSION_MAJOR}::Xml
    )
    message(STATUS "[Framegrabber] Framegrabber::QtWidget enabled.")
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
    if(NOT FRAMEGRABBER_BUILD_QT_WIDGET)
        message(FATAL_ERROR
            "[Framegrabber] FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "FRAMEGRABBER_BUILD_QT_WIDGET."
        )
    endif()
    if(NOT TARGET Playground::DevicePlugin)
        message(FATAL_ERROR
            "[Framegrabber] FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "Playground::DevicePlugin."
        )
    endif()
    if(NOT TARGET Resources::Resources)
        message(FATAL_ERROR
            "[Framegrabber] FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "Resources::Resources."
        )
    endif()
    if(NOT TARGET Qt6::Widgets)
        message(FATAL_ERROR
            "[Framegrabber] FRAMEGRABBER_BUILD_PLAYGROUND_PLUGIN=ON requires Qt 6."
        )
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Concurrent)

    add_library(FramegrabberPlugin MODULE
        Utility/PlaygroundAdapter/Plugin/FramegrabberPlugin.cpp
        Utility/PlaygroundAdapter/Source/FramegrabberSourceController.h
        Utility/PlaygroundAdapter/Source/FramegrabberSourceController.cpp
    )
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
        Playground::DevicePlugin
        Framegrabber::QtWidget
        Framegrabber::PlaygroundAdapter
        Resources::Resources
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        Qt6::Concurrent
    )
    include("${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/RuntimeStaging.cmake")
    framegrabber_prepare_playground_plugin_runtime(FramegrabberPlugin)
    message(STATUS "[Framegrabber] FramegrabberPlugin enabled.")
endif()

message(STATUS "[Framegrabber] Basler Frame Grabber SDK root: ${BASLER_FG_SDK_DIR}")
