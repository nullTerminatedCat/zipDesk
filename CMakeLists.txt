cmake_minimum_required(VERSION 3.21)

project(ZipDesk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

if(NOT DEFINED CMAKE_PREFIX_PATH)
    list(APPEND CMAKE_PREFIX_PATH "F:/Qt/6.9.1/msvc2022_64")
endif()

include(FetchContent)
FetchContent_Declare(
    bit7z
    GIT_REPOSITORY https://github.com/rikyoz/bit7z.git
    GIT_TAG v4.0.12
    GIT_SHALLOW TRUE
)
set(BIT7Z_AUTO_FORMAT ON CACHE BOOL "" FORCE)
set(BIT7Z_USE_NATIVE_STRING ON CACHE BOOL "" FORCE)
set(BIT7Z_PATH_SANITIZATION ON CACHE BOOL "" FORCE)
set(BIT7Z_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BIT7Z_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BIT7Z_REGEX_MATCHING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(bit7z)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

set(ZIPDESK_7Z_DLL "C:/Program Files/7-Zip/7z.dll" CACHE FILEPATH "7-Zip 官方 7z.dll")
if(NOT EXISTS "${ZIPDESK_7Z_DLL}")
    message(FATAL_ERROR "找不到 7z.dll：${ZIPDESK_7Z_DLL}")
endif()

function(zipdesk_copy_7z target_name)
    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ZIPDESK_7Z_DLL}"
                "$<TARGET_FILE_DIR:${target_name}>/7z.dll"
        VERBATIM
    )
endfunction()

add_library(zipdesk_core STATIC
    src/ArchiveService.cpp
    src/Launch.cpp
)
target_include_directories(zipdesk_core PUBLIC src)
target_link_libraries(zipdesk_core PUBLIC bit7z)
target_compile_features(zipdesk_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(zipdesk_core PRIVATE /utf-8 /W3 /EHsc)
endif()

qt_add_executable(ZipDesk WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.hpp
    src/ItemCommands.cpp
    src/ArchiveJob.cpp
    src/ArchiveJob.hpp
    src/ProgressDialog.cpp
    src/ProgressDialog.hpp
    src/CompressDialog.cpp
    src/CompressDialog.hpp
    src/ShellIntegration.cpp
    src/ShellIntegration.hpp
    src/Cli.cpp
    src/Cli.hpp
    src/Lang.cpp
    src/Lang.hpp
    resources/app.rc
)
qt_add_resources(ZipDesk app_assets
    PREFIX "/assets"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/empty.png
)
target_link_libraries(ZipDesk PRIVATE zipdesk_core Qt6::Widgets shell32 dwmapi)
if(MSVC)
    target_compile_options(ZipDesk PRIVATE /utf-8 /W3 /EHsc)
endif()
zipdesk_copy_7z(ZipDesk)
if(EXISTS "C:/Program Files/7-Zip/7z.sfx")
    add_custom_command(
        TARGET ZipDesk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "C:/Program Files/7-Zip/7z.sfx"
                "$<TARGET_FILE_DIR:ZipDesk>/7z.sfx"
        VERBATIM
    )
endif()

if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt
        HINTS "${Qt6_DIR}/../../../bin" "${Qt6_DIR}/../../bin")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(
            TARGET ZipDesk POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations "$<TARGET_FILE:ZipDesk>"
            COMMENT "Deploying Qt runtime"
            VERBATIM
        )
    endif()
endif()

add_executable(zipdesk_smoke tests/smoke.cpp)
target_link_libraries(zipdesk_smoke PRIVATE zipdesk_core)
if(MSVC)
    target_compile_options(zipdesk_smoke PRIVATE /utf-8 /W3 /EHsc)
endif()
zipdesk_copy_7z(zipdesk_smoke)
