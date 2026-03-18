cmake_minimum_required(VERSION 3.16)
project(Framegrabber LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(HEADERS
    Framegrabber.h
    FramegrabberSystem.h
)
set(SOURCES
    Framegrabber.cpp
    FramegrabberSystem.cpp
)

set(QT_HEADERS
    Utility/Qt/QGrabberWidget.h
)
set(QT_SOURCES
    Utility/Qt/QGrabberWidget.cpp
)

add_library(${PROJECT_NAME} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(QT NAMES Qt6 Qt5 QUIET)
if(QT_FOUND)
    find_package(Qt${QT_VERSION_MAJOR} QUIET COMPONENTS Core Gui Widgets Xml)
    message(STATUS "[${PROJECT_NAME}] Configuring for Qt ${QT_VERSION_MAJOR}")
    target_sources(${PROJECT_NAME} PRIVATE ${QT_HEADERS} ${QT_SOURCES})
    target_link_libraries(${PROJECT_NAME} PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Xml
    )
message(STATUS "[${PROJECT_NAME}] QtConverter.h features will be enabled for users")
else()
    message(STATUS "[${PROJECT_NAME}] Qt Not Found")
    message(STATUS "[${PROJECT_NAME}] QtConverter.h features will be disabled")
endif()

find_path(BASLER_FG_SDK_DIR
    NAMES cmake/modules/FindFgLib5.cmake
    HINTS
    "/opt/Basler/FramegrabberSDK"
    "C:/Program Files/Basler/FramegrabberSDK"
)
if(NOT DEFINED BASLER_FG_SDK_DIR AND DEFINED ENV{BASLER_FG_SDK_DIR})
    set(BASLER_FG_SDK_DIR $ENV{BASLER_FG_SDK_DIR})
endif()

if(BASLER_FG_SDK_DIR)
    set(ENV{BASLER_FG_SDK_DIR} "${BASLER_FG_SDK_DIR}")
    set(Fg_cmake "${BASLER_FG_SDK_DIR}/cmake/modules" "${CMAKE_INSTALL_PREFIX}/cmake/modules")
    list(APPEND CMAKE_MODULE_PATH ${Fg_cmake})

    find_package(FgLib5 REQUIRED)
    if(FgLib5_FOUND)
        add_definitions(${SISOPLATFORM_DEFINITIONS})
        message(STATUS "[${PROJECT_NAME}] Framegrabber SDK ${FgLib5_VERSION_STRING} found")
        target_include_directories(${PROJECT_NAME} PUBLIC ${FgLib5_INCLUDE_DIR})
        target_link_libraries(${PROJECT_NAME} PUBLIC ${FgLib5_LIBRARY})

        find_package(ClSerSis REQUIRED)
        if(ClSerSis_FOUND)
            target_include_directories(${PROJECT_NAME} PUBLIC ${ClSerSis_INCLUDE_DIR})
            target_link_libraries(${PROJECT_NAME} PUBLIC ${ClSerSis_LIBRARY})
        endif()

        find_package(SisoIoLib REQUIRED)
        if(SisoIoLib_FOUND)
            target_include_directories(${PROJECT_NAME} PUBLIC ${SisoIoLib_INCLUDE_DIR})
            target_link_libraries(${PROJECT_NAME} PUBLIC ${SisoIoLib_LIBRARY})
        endif()

        find_package(SisoPlatform REQUIRED)
        if(SisoPlatform_FOUND)
            target_include_directories(${PROJECT_NAME} PUBLIC ${SisoPlatform_INCLUDE_DIR})
            target_link_libraries(${PROJECT_NAME} PUBLIC ${SisoPlatform_LIBRARY})
        endif()

        find_package(SisoGenicamLib REQUIRED)
        if(SisoGenicamLib_FOUND)
            target_include_directories(${PROJECT_NAME} PUBLIC ${SisoGenicamLib_INCLUDE_DIR})
            target_link_libraries(${PROJECT_NAME} PUBLIC ${SisoGenicamLib_LIBRARY})
        endif()
    endif()
else()
    message(FATAL_ERROR "[${PROJECT_NAME}] FramegrabberSDK not found! Set BASLER_FG_SDK_DIR manually.")
endif()
