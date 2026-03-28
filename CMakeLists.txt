cmake_minimum_required(VERSION 3.16)
project(orchestrator C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ──────────────────────────────────────────────
# Output directories
# ──────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ──────────────────────────────────────────────
# Include paths
# ──────────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/vendor
)

# ──────────────────────────────────────────────
# Vendor sources (shared by all targets)
# ──────────────────────────────────────────────
set(VENDOR_SOURCES
    vendor/cJSON.c
    vendor/sqlite3.c
    vendor/mongoose.c
)

# ──────────────────────────────────────────────
# Platform-specific sources
# ──────────────────────────────────────────────
if(WIN32)
    set(PLATFORM_SOURCES src/platform/service_win.c)
    set(PLATFORM_LIBS ws2_32 advapi32 rpcrt4)
else()
    set(PLATFORM_SOURCES src/platform/service_linux.c)
    set(PLATFORM_LIBS pthread dl m)
endif()

# ──────────────────────────────────────────────
# Core sources
# ──────────────────────────────────────────────
set(CORE_SOURCES
    src/persistence/db.c
    src/persistence/log.c
    src/persistence/config.c
    src/resources/registry.c
    src/resources/allocator.c
    src/resources/probe.c
    src/core/job.c
    src/core/queue.c
    src/core/scheduler.c
    src/core/executor.c
    src/transfer/store.c
    src/transfer/upload.c
    src/transfer/download.c
    src/http/auth.c
    src/http/routes.c
    src/http/response.c
    src/http/httpd.c
    src/http/events.c
    src/cloud/cloud.c
)

# ──────────────────────────────────────────────
# Main orchestrator daemon
# ──────────────────────────────────────────────
add_executable(orchestrator
    src/main.c
    ${PLATFORM_SOURCES}
    ${CORE_SOURCES}
    ${VENDOR_SOURCES}
)

target_link_libraries(orchestrator ${PLATFORM_LIBS})

# Copy config/ next to the executable after each build
add_custom_command(TARGET orchestrator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/config"
        "$<TARGET_FILE_DIR:orchestrator>/config"
    COMMENT "Copying config/ to output directory"
)

# Copy web/ next to the executable after each build
add_custom_command(TARGET orchestrator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/web"
        "$<TARGET_FILE_DIR:orchestrator>/web"
    COMMENT "Copying web/ to output directory"
)

# SQLite needs these on Linux
if(UNIX)
    target_link_libraries(orchestrator pthread dl m)
endif()

# Mongoose TLS optional — disabled by default
# MG_ARCH is set explicitly to avoid the conditional #include "mongoose_config.h"
# which would fail on platforms where the file is absent.
target_compile_definitions(orchestrator PRIVATE
    MG_TLS=0
    SQLITE_THREADSAFE=1
)

if(WIN32)
    target_compile_definitions(orchestrator PRIVATE MG_ARCH=2 _WIN32_WINNT=0x0601)  # MG_ARCH_WIN32
else()
    target_compile_definitions(orchestrator PRIVATE MG_ARCH=1)  # MG_ARCH_UNIX
endif()

# ──────────────────────────────────────────────
# Optional: stricter warnings in debug mode
# ──────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(MSVC)
        target_compile_options(orchestrator PRIVATE /W3)
    else()
        target_compile_options(orchestrator PRIVATE -Wall -Wextra)
    endif()
endif()
