cmake_minimum_required(VERSION 3.21)

project(ZipDesk VERSION 0.2.16 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

include(CTest)
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
find_package(Qt6 QUIET COMPONENTS Test)
qt_standard_project_setup()

set(ZIPDESK_7Z_DLL "C:/Program Files/7-Zip/7z.dll" CACHE FILEPATH "7-Zip 官方 7z.dll")
if(NOT EXISTS "${ZIPDESK_7Z_DLL}")
    message(FATAL_ERROR "找不到 7z.dll：${ZIPDESK_7Z_DLL}。请通过 -DZIPDESK_7Z_DLL=... 指定路径。")
endif()
get_filename_component(ZIPDESK_7Z_DIR "${ZIPDESK_7Z_DLL}" DIRECTORY)
set(ZIPDESK_7Z_SFX "${ZIPDESK_7Z_DIR}/7z.sfx" CACHE FILEPATH "7-Zip 官方 7z.sfx（可选）")

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
    src/ConflictDialog.cpp
    src/ConflictDialog.hpp
    src/MultiDropDialog.cpp
    src/MultiDropDialog.hpp
    src/ShellIntegration.cpp
    src/ShellIntegration.hpp
    src/ClassicContextMenu.cpp
    src/ClassicContextMenu.hpp
    src/Cli.cpp
    src/Cli.hpp
    src/Lang.cpp
    src/Lang.hpp
    resources/app.rc
)
target_include_directories(ZipDesk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources")
target_compile_definitions(ZipDesk PRIVATE
    ZIPDESK_VERSION="${PROJECT_VERSION}"
    ZIPDESK_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    ZIPDESK_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    ZIPDESK_VERSION_PATCH=${PROJECT_VERSION_PATCH})
qt_add_resources(ZipDesk app_assets
    PREFIX "/assets"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/empty.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/conflict-copy.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/conflict-skip.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/conflict-refresh.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/conflict-warning.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/conflict-file-common.png
)
target_link_libraries(ZipDesk PRIVATE zipdesk_core Qt6::Widgets shell32 dwmapi)
if(MSVC)
    target_compile_options(ZipDesk PRIVATE /utf-8 /W3 /EHsc)
endif()
zipdesk_copy_7z(ZipDesk)
if(EXISTS "${ZIPDESK_7Z_SFX}")
    add_custom_command(
        TARGET ZipDesk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ZIPDESK_7Z_SFX}"
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

add_executable(zipdesk_smoke tests/smoke.cpp src/ClassicContextMenu.cpp)
target_link_libraries(zipdesk_smoke PRIVATE zipdesk_core)
if(WIN32)
    target_link_libraries(zipdesk_smoke PRIVATE advapi32 shell32)
endif()
if(MSVC)
    target_compile_options(zipdesk_smoke PRIVATE /utf-8 /W3 /EHsc)
endif()
zipdesk_copy_7z(zipdesk_smoke)

if(TARGET Qt6::Test)
    add_executable(zipdesk_progress_smoke tests/progress_smoke.cpp src/ProgressDialog.cpp src/ProgressDialog.hpp)
    target_include_directories(zipdesk_progress_smoke PRIVATE src)
    target_link_libraries(zipdesk_progress_smoke PRIVATE Qt6::Widgets Qt6::Test)

    add_executable(zipdesk_ui_smoke
        tests/ui_smoke.cpp
        src/CompressDialog.cpp
        src/CompressDialog.hpp
    )
    target_include_directories(zipdesk_ui_smoke PRIVATE src)
    target_link_libraries(zipdesk_ui_smoke PRIVATE Qt6::Widgets Qt6::Test)
    if(MSVC)
        target_compile_options(zipdesk_ui_smoke PRIVATE /utf-8 /W3 /EHsc)
    endif()

    add_executable(zipdesk_main_ui_smoke
        tests/main_ui_smoke.cpp
        src/MainWindow.cpp
        src/MainWindow.hpp
        src/ItemCommands.cpp
        src/ArchiveJob.cpp
        src/ArchiveJob.hpp
        src/ProgressDialog.cpp
        src/ProgressDialog.hpp
        src/CompressDialog.cpp
        src/CompressDialog.hpp
        src/ConflictDialog.cpp
        src/ConflictDialog.hpp
        src/MultiDropDialog.cpp
        src/MultiDropDialog.hpp
        src/ShellIntegration.cpp
        src/ShellIntegration.hpp
        src/ClassicContextMenu.cpp
        src/ClassicContextMenu.hpp
        src/Lang.cpp
        src/Lang.hpp
    )
    qt_add_resources(zipdesk_main_ui_smoke smoke_assets
        PREFIX "/assets"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/empty.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/conflict-copy.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/conflict-skip.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/conflict-refresh.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/conflict-warning.png
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/conflict-file-common.png
    )
    target_include_directories(zipdesk_main_ui_smoke PRIVATE src)
    target_link_libraries(zipdesk_main_ui_smoke PRIVATE zipdesk_core Qt6::Widgets Qt6::Test shell32 dwmapi)
    if(MSVC)
        target_compile_options(zipdesk_main_ui_smoke PRIVATE /utf-8 /W3 /EHsc)
    endif()
    zipdesk_copy_7z(zipdesk_main_ui_smoke)
endif()

if(BUILD_TESTING)
    add_test(NAME archive_service COMMAND zipdesk_smoke)
    if(TARGET Qt6::Test)
        function(zipdesk_add_ui_test name target)
            add_test(NAME ${name}
                COMMAND ${CMAKE_COMMAND} -E env
                    "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}"
                    "QT_QPA_PLATFORM=windows"
                    $<TARGET_FILE:${target}> ${ARGN})
        endfunction()
        zipdesk_add_ui_test(compression_dialog zipdesk_ui_smoke)
        zipdesk_add_ui_test(progress_dialog zipdesk_progress_smoke)
        zipdesk_add_ui_test(archive_window zipdesk_main_ui_smoke)
        foreach(scenario IN ITEMS overwrite cancel failure decline)
            zipdesk_add_ui_test(move_${scenario} zipdesk_main_ui_smoke --move ${scenario})
        endforeach()
        foreach(scenario IN ITEMS success skip keep pick failure)
            zipdesk_add_ui_test(copy_${scenario} zipdesk_main_ui_smoke --copy ${scenario})
        endforeach()
        foreach(scenario IN ITEMS success skip keep pick cancel failure blocked smart-directory smart-success smart-skip smart-keep smart-pick smart-cancel smart-failure smart-blocked smart-empty smart-window smart-directory-skip)
            zipdesk_add_ui_test(extract_${scenario} zipdesk_main_ui_smoke --extract ${scenario})
        endforeach()
    endif()
    get_property(zipdesk_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${zipdesk_tests} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT 120)
endif()
