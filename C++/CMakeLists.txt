cmake_minimum_required(VERSION 3.16)
project(Framegrabber LANGUAGES C CXX)

option(FRAMEGRABBER_BUILD_QT_WIDGET "Build the optional Qt control widget." ON)
option(FRAMEGRABBER_BUILD_GRAPHICSENGINE_ADAPTER
    "Build the optional Framegrabber-to-GraphicsEngine adapter target."
    OFF
)

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
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)

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

    target_sources(Framegrabber PRIVATE
        Utility/Qt/QFramegrabberWidget.h
        Utility/Qt/QFramegrabberWidget.cpp
    )
    target_link_libraries(Framegrabber
        PUBLIC
            Qt${FRAMEGRABBER_QT_VERSION_MAJOR}::Core
            Qt${FRAMEGRABBER_QT_VERSION_MAJOR}::Gui
            Qt${FRAMEGRABBER_QT_VERSION_MAJOR}::Widgets
            Qt${FRAMEGRABBER_QT_VERSION_MAJOR}::Xml
    )
endif()

if(FRAMEGRABBER_BUILD_GRAPHICSENGINE_ADAPTER)
    if(NOT TARGET GraphicsEngine::GraphicsEngine)
        message(FATAL_ERROR
            "[Framegrabber] FRAMEGRABBER_BUILD_GRAPHICSENGINE_ADAPTER=ON requires "
            "GraphicsEngine::GraphicsEngine. Add GraphicsEngine first or disable the adapter."
        )
    endif()

    add_library(FramegrabberGraphicsEngineAdapter STATIC
        Utility/GraphicsEngine/FramegrabberGraphicsImageAdapter.h
        Utility/GraphicsEngine/FramegrabberGraphicsImageAdapter.cpp
    )
    add_library(
        Framegrabber::GraphicsEngineAdapter
        ALIAS FramegrabberGraphicsEngineAdapter
    )
    target_compile_features(FramegrabberGraphicsEngineAdapter PUBLIC cxx_std_17)
    target_include_directories(FramegrabberGraphicsEngineAdapter
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/GraphicsEngine"
    )
    target_link_libraries(FramegrabberGraphicsEngineAdapter
        PUBLIC
            Framegrabber::Framegrabber
            GraphicsEngine::GraphicsEngine
    )
    message(STATUS "[Framegrabber] Framegrabber::GraphicsEngineAdapter enabled.")
endif()

message(STATUS "[Framegrabber] Basler Frame Grabber SDK root: ${BASLER_FG_SDK_DIR}")
