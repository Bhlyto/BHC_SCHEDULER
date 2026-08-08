cmake_minimum_required(VERSION 3.16)
project(orchestrator C)

include(CTest)

option(ORCH_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The scheduler, HTTP server, launcher and watcher threads share the SQLite
# connection. Keep serialized mode enabled consistently for every target that
# compiles the bundled sqlite3.c, including tests.
add_compile_definitions(SQLITE_THREADSAFE=1)

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
    src/transfer/artifacts.c
    src/transfer/upload.c
    src/transfer/download.c
    src/http/auth.c
    src/http/routes.c
    src/http/batches.c
    src/http/artifacts.c
    src/http/response.c
    src/http/httpd.c
    src/http/events.c
    src/cloud/cloud.c
    src/decision_core_default.c
    src/solvers/thermal/thermal_solver.c
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

add_executable(dc_test
    src/tools/dc_test.c
    ${CORE_SOURCES}
    ${VENDOR_SOURCES}
)
target_include_directories(dc_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dc_test ${PLATFORM_LIBS})

target_link_libraries(orchestrator ${PLATFORM_LIBS})

add_executable(presim_runner
    src/tools/presim_runner.c
    src/solvers/thermal/thermal_solver.c
)
target_include_directories(presim_runner PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(UNIX)
    target_link_libraries(presim_runner pthread dl m)
endif()

add_executable(presim_e2e_test
    src/tools/presim_e2e_test.c
    ${CORE_SOURCES}
    ${VENDOR_SOURCES}
)
target_include_directories(presim_e2e_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(presim_e2e_test ${PLATFORM_LIBS})

add_executable(lifecycle_test
    src/tools/lifecycle_test.c
    ${CORE_SOURCES}
    ${VENDOR_SOURCES}
)
target_include_directories(lifecycle_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(lifecycle_test ${PLATFORM_LIBS})

add_executable(allocator_test
    src/tools/allocator_test.c
    ${CORE_SOURCES}
    ${VENDOR_SOURCES}
)
target_include_directories(allocator_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(allocator_test ${PLATFORM_LIBS})

add_executable(recovery_test
    src/tools/recovery_test.c
    ${CORE_SOURCES}
    ${VENDOR_SOURCES}
)
target_include_directories(recovery_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(recovery_test ${PLATFORM_LIBS})

add_executable(queue_test
    src/tools/queue_test.c
    src/core/queue.c
    src/persistence/log.c
)
target_include_directories(queue_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(queue_test ${PLATFORM_LIBS})

add_executable(batch_test
    src/tools/batch_test.c
    ${CORE_SOURCES}
    ${VENDOR_SOURCES}
)
target_include_directories(batch_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(batch_test ${PLATFORM_LIBS})

add_executable(artifact_test
    src/tools/artifact_test.c
    ${CORE_SOURCES}
    ${VENDOR_SOURCES}
)
target_include_directories(artifact_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(artifact_test ${PLATFORM_LIBS})

add_executable(retry_test
    src/tools/retry_test.c
    ${CORE_SOURCES}
    ${VENDOR_SOURCES}
)
target_include_directories(retry_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(retry_test ${PLATFORM_LIBS})

add_executable(scale_test
    src/tools/scale_test.c
    ${CORE_SOURCES}
    ${VENDOR_SOURCES}
)
target_include_directories(scale_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(scale_test ${PLATFORM_LIBS})

if(BUILD_TESTING)
    add_test(NAME decision_core COMMAND $<TARGET_FILE:dc_test>)
    add_test(NAME presim_e2e COMMAND $<TARGET_FILE:presim_e2e_test>)
    add_test(NAME lifecycle COMMAND $<TARGET_FILE:lifecycle_test>)
    add_test(NAME allocator COMMAND $<TARGET_FILE:allocator_test>)
    add_test(NAME recovery COMMAND $<TARGET_FILE:recovery_test>)
    add_test(NAME queue COMMAND $<TARGET_FILE:queue_test>)
    add_test(NAME batch COMMAND $<TARGET_FILE:batch_test>)
    add_test(NAME artifact COMMAND $<TARGET_FILE:artifact_test>)
    add_test(NAME retry COMMAND $<TARGET_FILE:retry_test>)
    add_test(NAME scale_1000 COMMAND $<TARGET_FILE:scale_test>)

    set_tests_properties(decision_core presim_e2e lifecycle allocator recovery queue batch artifact retry scale_1000 PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(scale_1000 PROPERTIES TIMEOUT 120 LABELS "scale")

    add_executable(unit_tests
        tests/unit_tests.c
        ${CORE_SOURCES}
        ${VENDOR_SOURCES}
    )
    target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(unit_tests ${PLATFORM_LIBS})
    target_compile_definitions(unit_tests PRIVATE ORCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME unit_tests COMMAND unit_tests)
    set_tests_properties(unit_tests PROPERTIES TIMEOUT 30)
    set_tests_properties(decision_core presim_e2e PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()

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
)

if(WIN32)
    target_compile_definitions(orchestrator PRIVATE MG_ARCH=2 _WIN32_WINNT=0x0601)  # MG_ARCH_WIN32
else()
    target_compile_definitions(orchestrator PRIVATE MG_ARCH=1)  # MG_ARCH_UNIX
endif()

set(ORCH_FULL_TARGETS orchestrator dc_test presim_e2e_test)
if(BUILD_TESTING)
    list(APPEND ORCH_FULL_TARGETS unit_tests)
endif()

foreach(target_name IN LISTS ORCH_FULL_TARGETS)
    target_compile_definitions(${target_name} PRIVATE MG_TLS=0 SQLITE_THREADSAFE=1)
    if(WIN32)
        target_compile_definitions(${target_name} PRIVATE MG_ARCH=2 _WIN32_WINNT=0x0601)
    else()
        target_compile_definitions(${target_name} PRIVATE MG_ARCH=1)
    endif()
endforeach()

if(ORCH_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target_name IN LISTS ORCH_FULL_TARGETS)
        target_compile_options(${target_name} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE -fsanitize=address,undefined)
    endforeach()
    target_compile_options(presim_runner PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(presim_runner PRIVATE -fsanitize=address,undefined)
endif()

install(TARGETS orchestrator presim_runner RUNTIME DESTINATION bin)
install(DIRECTORY config web DESTINATION bin)
install(FILES orchestrator.service DESTINATION share/bhc-scheduler OPTIONAL)

set(CPACK_PACKAGE_NAME "bhc-scheduler")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_CONTACT "BHC Scheduler maintainers")
set(CPACK_GENERATOR "ZIP;TGZ")
include(CPack)

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
