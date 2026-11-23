cmake_minimum_required(VERSION 3.16)
project(zephium C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Keep a compile_commands.json next to the build for IDE / clang-tidy users.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT WIN32)
    add_compile_definitions(_DEFAULT_SOURCE _GNU_SOURCE)
endif()

option(ZEPHIUM_WERROR "Treat compiler warnings as errors (used by CI)" OFF)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wstrict-prototypes
        -Wformat=2
        -Wno-deprecated-declarations
    )
    # Never embed the checkout location in the binaries: -ffile-prefix-map
    # rewrites the source/build paths in debug info and in __FILE__.
    add_compile_options(
        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
        "-ffile-prefix-map=${CMAKE_BINARY_DIR}=."
    )
    # GCC-only diagnostics that guard the bounded string helpers. Clang does
    # not know these flag names and would reject them under -Werror.
    if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wstringop-overflow -Wstringop-truncation)
    endif()
    if (ZEPHIUM_WERROR)
        add_compile_options(-Werror)
    endif()
elseif (MSVC)
    add_compile_options(/W4)
    if (ZEPHIUM_WERROR)
        add_compile_options(/WX)
    endif()
endif()

# Core sources
set(ZEPHIUM_SOURCES
    main.c
    zoom/port.c
    geometry/port.c
    ids/port.c
    navigation/port.c
    blocker/port.c
    store/port.c
    browser_core/port.c
)

add_executable(zephium ${ZEPHIUM_SOURCES})

target_include_directories(zephium PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Keep the binary at <build>/zephium[.exe] for single- and multi-config
# generators so `make run` and the documented paths stay stable.
set_target_properties(zephium PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
foreach(_zephium_config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(zephium PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${_zephium_config}" "${CMAKE_BINARY_DIR}"
    )
endforeach()

# SQLite3: use CMake's package module first, then pkg-config. Never assume a
# toolchain-specific install prefix (pass CMAKE_PREFIX_PATH instead).
find_package(SQLite3 QUIET)
if (NOT SQLite3_FOUND)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(SQLite3 QUIET sqlite3)
    endif()
endif()

if (NOT SQLite3_FOUND)
    message(FATAL_ERROR
        "SQLite3 development files were not found. Install libsqlite3-dev "
        "(Debian/Ubuntu), sqlite3 (MSYS2/vcpkg), or set CMAKE_PREFIX_PATH.")
endif()

if (TARGET SQLite3::SQLite3)
    target_link_libraries(zephium PRIVATE SQLite3::SQLite3)
elseif (TARGET SQLite::SQLite3)
    # CMake <= 4.3 FindSQLite3 name.
    target_link_libraries(zephium PRIVATE SQLite::SQLite3)
else()
    target_include_directories(zephium PRIVATE ${SQLite3_INCLUDE_DIRS})
    target_link_directories(zephium PRIVATE ${SQLite3_LIBRARY_DIRS})
    target_link_libraries(zephium PRIVATE ${SQLite3_LIBRARIES})
endif()

# Platform specifics
if (WIN32)
    target_link_libraries(zephium PRIVATE bcrypt)
endif()

# Linux WebKitGTK (4.1 or 4.0) Shell support. pkg-config is the only portable
# way to discover the GTK/WebKit headers, so the headless core is used when it
# is missing.
if (UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(WEBKIT QUIET webkit2gtk-4.1)
        if (NOT WEBKIT_FOUND)
            pkg_check_modules(WEBKIT QUIET webkit2gtk-4.0)
        endif()
    endif()

    if (WEBKIT_FOUND)
        message(STATUS "Found WebKitGTK ${WEBKIT_VERSION}: enabling Linux graphical browser shell")
        target_compile_definitions(zephium PRIVATE ZEPHIUM_ENABLE_WEBKITGTK=1)
        target_include_directories(zephium PRIVATE ${WEBKIT_INCLUDE_DIRS})
        target_compile_options(zephium PRIVATE ${WEBKIT_CFLAGS_OTHER})
        target_link_directories(zephium PRIVATE ${WEBKIT_LIBRARY_DIRS})
        target_link_libraries(zephium PRIVATE ${WEBKIT_LIBRARIES} m pthread)
    else()
        message(STATUS "WebKitGTK not found: building standalone headless/CLI core")
        target_link_libraries(zephium PRIVATE m pthread)
    endif()
endif()

# Tests
enable_testing()
add_test(NAME zephium_batch_test COMMAND zephium --batch)
set_tests_properties(zephium_batch_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
