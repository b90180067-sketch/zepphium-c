cmake_minimum_required(VERSION 3.16)
project(zephium C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_definitions(-D_DEFAULT_SOURCE -D_GNU_SOURCE)

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

# SQLite3 dependency
find_package(SQLite3 REQUIRED)
target_link_libraries(zephium PRIVATE SQLite::SQLite3)

# Platform specifics
if (WIN32)
    target_link_libraries(zephium PRIVATE bcrypt)
endif()

# Linux WebKitGTK (4.1 or 4.0) Shell support
if (UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKIT webkit2gtk-4.1)
    if (NOT WEBKIT_FOUND)
        pkg_check_modules(WEBKIT webkit2gtk-4.0)
    endif()

    if (WEBKIT_FOUND)
        message(STATUS "Found WebKit2GTK (${WEBKIT_PACKAGE}): Enabling Linux graphical browser shell")
        target_compile_definitions(zephium PRIVATE ZEPHIUM_ENABLE_WEBKITGTK=1)
        target_include_directories(zephium PRIVATE ${WEBKIT_INCLUDE_DIRS})
        target_compile_options(zephium PRIVATE ${WEBKIT_CFLAGS_OTHER})
        target_link_libraries(zephium PRIVATE ${WEBKIT_LIBRARIES} m pthread)
    else()
        message(WARNING "Neither webkit2gtk-4.1 nor webkit2gtk-4.0 found: building standalone headless/CLI core")
        target_link_libraries(zephium PRIVATE m pthread)
    endif()
endif()

# Tests
enable_testing()
add_test(NAME zephium_batch_test COMMAND zephium --batch)
