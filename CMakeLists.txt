cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0077 NEW)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
project(ToolSmith VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build all static libraries with -fPIC so they can be linked into shared libs (toolsmith_capi)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Generate Version.h from template
configure_file(
  "${PROJECT_SOURCE_DIR}/include/core/Version.h.in"
  "${CMAKE_BINARY_DIR}/generated/core/Version.h"
  @ONLY
)

option(USE_UWS "Use uWebSockets (async) server" OFF)
option(BUILD_CSHARP "Build C# wrapper (requires dotnet SDK)" ON)
option(UNITY_BUILD "Enable CMake unity build for the core library" OFF)
set(TEST_FRAMEWORK "GTest" CACHE STRING "GTest or Catch2")

include(FetchContent)
include(CMakeFindDependencyMacro)

# ------------------------------------------------------------------------------
# json-schema-validator
# ------------------------------------------------------------------------------
set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  jsv
  GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
  GIT_TAG 2.3.0
)

# ------------------------------------------------------------------------------
# nlohmann json
# ------------------------------------------------------------------------------
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json jsv)

# ------------------------------------------------------------------------------
# subprocess.h (single-header cross-platform subprocess library)
# ------------------------------------------------------------------------------
FetchContent_Declare(
  subprocess_h
  GIT_REPOSITORY https://github.com/sheredom/subprocess.h.git
  GIT_TAG b0c3e4f6491da6aef71ffa3beaa083e22f6855f5
)
FetchContent_MakeAvailable(subprocess_h)

# ------------------------------------------------------------------------------
# cpp-httplib
# ------------------------------------------------------------------------------
# Disable httplib optional dependency auto-detection.
# The _IF_AVAILABLE flags cause httplib to find & link zstd/brotli/zlib/openssl
# when headers happen to be present on the system — but the CMake targets may
# not exist, leading to "target not found" errors (e.g. zstd::libzstd on CI).
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.26.0
)
FetchContent_MakeAvailable(httplib)

# ------------------------------------------------------------------------------
# uWebSockets (optional)
# ------------------------------------------------------------------------------
if(USE_UWS)
  message(STATUS "Configuring uWebSockets support...")
  FetchContent_Declare(
    uwebsockets
    GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
    GIT_TAG v20.47.0
  )
  FetchContent_MakeAvailable(uwebsockets)

  add_library(uwebsockets INTERFACE)
  target_include_directories(uwebsockets INTERFACE
    ${uwebsockets_SOURCE_DIR}/src
    ${uwebsockets_SOURCE_DIR}/uSockets/src
  )
  target_compile_definitions(uwebsockets INTERFACE UWS_NO_ZLIB)
  if(NOT MSVC)
    target_compile_options(uwebsockets INTERFACE -Wall -O3)
  endif()
endif()

# ------------------------------------------------------------------------------
# googletest / catch2
# ------------------------------------------------------------------------------
if(TEST_FRAMEWORK STREQUAL "GTest")
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  )
  FetchContent_MakeAvailable(googletest)
else()
  FetchContent_Declare(
    catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
  )
  FetchContent_MakeAvailable(catch2)
endif()

# ------------------------------------------------------------------------------
# Compiler options
# ------------------------------------------------------------------------------
if(MSVC)
  add_compile_options(/W4 /WX /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror -fdiagnostics-color=always -pipe)
endif()

# ------------------------------------------------------------------------------
# Core static library
# ------------------------------------------------------------------------------
set(CORE_SOURCES
    src/core/ProtocolHandler.cpp
    src/core/Config.cpp
    src/core/Logger.cpp
    src/core/ThreadPool.cpp
    src/core/Hashline.cpp
    src/core/WorkspaceJail.cpp
    src/core/ToolProfile.cpp
    src/validation/JsonSchemaValidator.cpp
    src/validation/InputSanitizer.cpp
    src/commands/CommandRegistry.cpp
    src/commands/EchoCommand.cpp
    src/commands/CoreTools.cpp
    src/llm/LiteLLMProvider.cpp
    src/llm/LLMCommand.cpp
    src/discovery/McpServerRegistry.cpp
    src/discovery/CompositeCommand.cpp
    src/skills/SkillEngine.cpp
    src/skills/SkillCommand.cpp
    src/skills/SkillToolAdapter.cpp
    src/skills/PluginLoader.cpp
    src/skills/YamlFrontmatter.cpp
    src/skills/AgentSkillLoader.cpp
    src/plugins/DlPlugin.cpp
    src/plugins/NativePluginAdapter.cpp
    src/plugins/NativePluginLoader.cpp
    src/plugins/ScriptPluginAdapter.cpp
    src/plugins/ScriptPluginLoader.cpp
    src/plugins/SubprocessPipe.cpp
    src/plugins/StdioMCPAdapter.cpp
    src/http/HttplibClient.cpp
    src/security/RateLimiter.cpp
    src/security/ApiKeyValidator.cpp
    src/security/SecurityHeaders.cpp
    src/server/HttplibServer.cpp
    src/server/StdioTransport.cpp
)

if(USE_UWS)
  list(APPEND CORE_SOURCES src/server/UwsServer.cpp)
endif()

add_library(core STATIC ${CORE_SOURCES})
if(UNITY_BUILD)
  set_target_properties(core PROPERTIES UNITY_BUILD ON)
endif()
target_include_directories(core PUBLIC
  ${PROJECT_SOURCE_DIR}/include
  "${CMAKE_BINARY_DIR}/generated"
)
# SYSTEM: vendor headers (macOS 26 deprecates posix_spawn_file_actions_addchdir_np
# inside subprocess.h; -Werror must not fire on that).
target_include_directories(core SYSTEM PRIVATE ${subprocess_h_SOURCE_DIR})
if(NOT MSVC)
  set_source_files_properties(src/plugins/SubprocessPipe.cpp PROPERTIES
    COMPILE_FLAGS "-Wno-deprecated-declarations -Wno-unused-function -Wno-pedantic")
endif()
target_link_libraries(core PUBLIC
  nlohmann_json::nlohmann_json
  nlohmann_json_schema_validator
  httplib::httplib
)

if(USE_UWS)
  target_link_libraries(core PUBLIC uwebsockets)
  target_compile_definitions(core PUBLIC USE_UWS=1)
endif()

# Native plugin DL loading
if(WIN32)
  # LoadLibrary / GetProcAddress are in kernel32, always linked automatically.
  # No extra target needed.
elseif(APPLE)
  # dlopen is in libSystem on macOS — no extra link needed.
else()
  target_link_libraries(core PUBLIC dl)
endif()

# std::filesystem requires explicit linking on GCC < 9 and Clang < 9
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
  target_link_libraries(core PUBLIC stdc++fs)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
  target_link_libraries(core PUBLIC c++fs)
endif()

# pthreads on Linux/Unix
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(core PUBLIC Threads::Threads)
endif()

# macOS: httplib uses CoreFoundation + CFNetwork for async DNS resolution
if(APPLE)
  target_link_libraries(core PUBLIC "-framework CoreFoundation" "-framework CFNetwork")
endif()

# ------------------------------------------------------------------------------
# Server executable
# ------------------------------------------------------------------------------
add_executable(toolsmith src/server_entry.cpp)
target_link_libraries(toolsmith PRIVATE core)

# ------------------------------------------------------------------------------
# Demo client
# ------------------------------------------------------------------------------
add_executable(demo_client src/demo_client.cpp)
target_link_libraries(demo_client PRIVATE core)

# ------------------------------------------------------------------------------
# C API shared library
# ------------------------------------------------------------------------------
add_library(toolsmith_capi SHARED capi/src/mcp_capi.cpp)
target_include_directories(toolsmith_capi
  PUBLIC capi/include
  PRIVATE ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(toolsmith_capi PRIVATE core)
target_compile_definitions(toolsmith_capi PRIVATE MCP_CAPI_EXPORTS)

# ------------------------------------------------------------------------------
# C# wrapper (built via dotnet, triggered from CMake)
# ------------------------------------------------------------------------------
if(BUILD_CSHARP)
  find_program(DOTNET_EXE dotnet)
  if(DOTNET_EXE)
    add_custom_target(mcp_csharp ALL
      COMMAND ${DOTNET_EXE} build ${CMAKE_SOURCE_DIR}/csharp/McpClient.csproj
              -c $<IF:$<CONFIG:Debug>,Debug,Release>
              -o ${CMAKE_BINARY_DIR}/csharp
      DEPENDS toolsmith_capi
      COMMENT "Building C# wrapper (McpClient)"
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/csharp
    )
  else()
    message(STATUS "dotnet SDK not found, skipping C# wrapper build")
  endif()
endif()

# ------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------
enable_testing()

set(TEST_SOURCES
    tests/ProtocolHandler_test.cpp
    tests/CommandRegistry_test.cpp
    tests/InputSanitizer_test.cpp
    tests/RateLimiter_test.cpp
    tests/Config_test.cpp
    tests/SkillEngine_test.cpp
    tests/SkillToolAdapter_test.cpp
    tests/PluginLoader_test.cpp
    tests/McpServerRegistry_test.cpp
    tests/StdioTransport_test.cpp
    tests/NativePlugin_test.cpp
    tests/ScriptPluginAdapter_test.cpp
    tests/ScriptPluginLoader_test.cpp
    tests/ScriptPlugin_Integration_test.cpp
    tests/StdioMCPAdapter_test.cpp
    tests/Hashline_test.cpp
    tests/WorkspaceJail_test.cpp
    tests/ToolProfile_test.cpp
    tests/YamlFrontmatter_test.cpp
    tests/AgentSkillLoader_test.cpp
    tests/CoreTools_test.cpp
    tests/Env_test.cpp
)

# Desktop notification plugin tests — cross-platform
# NotificationBackend.cpp compiles cleanly on all platforms via #ifdef guards.
list(APPEND TEST_SOURCES
    tests/DesktopNotification_test.cpp
    plugins/desktop-notification/src/NotificationBackend.cpp
)

if(TEST_FRAMEWORK STREQUAL "GTest")
  add_executable(unit_tests ${TEST_SOURCES})
  target_link_libraries(unit_tests PRIVATE core gtest_main)
  add_test(NAME unit_tests COMMAND unit_tests)
else()
  add_executable(unit_tests ${TEST_SOURCES})
  target_link_libraries(unit_tests PRIVATE core Catch2::Catch2WithMain)
  add_test(NAME unit_tests COMMAND unit_tests)
endif()

# Include path for desktop-notification backend (needed on all platforms)
target_include_directories(unit_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/plugins/desktop-notification/src"
)

# Test plugin directory — used by ScriptPlugin tests to locate echo-plugin
target_compile_definitions(unit_tests PRIVATE
    TEST_PLUGIN_DIR="${PROJECT_SOURCE_DIR}/tests/test_plugins"
    SCRIPT_PLUGIN_DIR="${PROJECT_SOURCE_DIR}/plugins"
)
# runtimeobject.lib only needed on Windows (WRL COM for WinRT)
if(WIN32)
  target_link_libraries(unit_tests PRIVATE runtimeobject)
endif()

# ------------------------------------------------------------------------------
# Desktop notification plugin (all platforms — builds into plugins/desktop-notification/bin/)
# ------------------------------------------------------------------------------
option(BUILD_DESKTOP_NOTIFICATION_PLUGIN "Build the desktop-notification native plugin" ON)
if(BUILD_DESKTOP_NOTIFICATION_PLUGIN)
  add_library(desktop-notification SHARED
    plugins/desktop-notification/src/desktop-notification.cpp
    plugins/desktop-notification/src/NotificationBackend.cpp
  )
  target_include_directories(desktop-notification PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/plugins/desktop-notification/src"
  )
  if(WIN32)
    target_link_libraries(desktop-notification PRIVATE runtimeobject)
  endif()
  set_target_properties(desktop-notification PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/plugins/desktop-notification/bin"
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/plugins/desktop-notification/bin"
  )
  if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
    target_compile_options(desktop-notification PRIVATE -fvisibility=hidden)
  endif()
  message(STATUS "Building desktop-notification -> plugins/desktop-notification/bin/")
endif()

# ------------------------------------------------------------------------------
# Example native plugin (optional — builds into plugins/example-plugin/bin/)
# ------------------------------------------------------------------------------
option(BUILD_EXAMPLE_PLUGIN "Build the example native plugin shared library" ON)
if(BUILD_EXAMPLE_PLUGIN)
  add_library(example-plugin SHARED
    plugins/example-plugin/src/example-plugin.cpp
  )
  target_include_directories(example-plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
  set_target_properties(example-plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/plugins/example-plugin/bin"
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/plugins/example-plugin/bin"
  )
  if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
    target_compile_options(example-plugin PRIVATE -fvisibility=hidden)
  endif()
  message(STATUS "Building example-plugin -> plugins/example-plugin/bin/")
endif()

# ------------------------------------------------------------------------------
# Unreal Engine tools native plugin (builds into plugins/unreal-tools/bin/)
# ------------------------------------------------------------------------------
option(BUILD_UNREAL_TOOLS_PLUGIN "Build the unreal-tools native plugin" ON)
if(BUILD_UNREAL_TOOLS_PLUGIN)
  add_library(unreal-tools SHARED
    plugins/unreal-tools/src/unreal-tools.cpp
  )
  target_include_directories(unreal-tools PRIVATE ${PROJECT_SOURCE_DIR}/include)
  set_target_properties(unreal-tools PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/plugins/unreal-tools/bin"
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/plugins/unreal-tools/bin"
  )
  if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
    target_compile_options(unreal-tools PRIVATE -fvisibility=hidden)
  endif()
  message(STATUS "Building unreal-tools -> plugins/unreal-tools/bin/")
endif()

# ------------------------------------------------------------------------------
# Install targets
# ------------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS toolsmith RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS toolsmith_capi
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES capi/include/mcp_capi.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/toolsmith
)
install(FILES include/plugins/PluginABI.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/toolsmith
)
install(FILES "${CMAKE_BINARY_DIR}/generated/core/Version.h"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/toolsmith
)

install(DIRECTORY config/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/toolsmith/config
  FILES_MATCHING PATTERN "*.example"
)

install(DIRECTORY plugins/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/toolsmith/plugins
  PATTERN "bin" EXCLUDE
  PATTERN "__pycache__" EXCLUDE
  PATTERN "*.pyc" EXCLUDE
  PATTERN "data" EXCLUDE
)

if(BUILD_DESKTOP_NOTIFICATION_PLUGIN)
  install(TARGETS desktop-notification
    RUNTIME DESTINATION ${CMAKE_INSTALL_DATADIR}/toolsmith/plugins/desktop-notification/bin
    LIBRARY DESTINATION ${CMAKE_INSTALL_DATADIR}/toolsmith/plugins/desktop-notification/bin
  )
endif()

if(BUILD_EXAMPLE_PLUGIN)
  install(TARGETS example-plugin
    RUNTIME DESTINATION ${CMAKE_INSTALL_DATADIR}/toolsmith/plugins/example-plugin/bin
    LIBRARY DESTINATION ${CMAKE_INSTALL_DATADIR}/toolsmith/plugins/example-plugin/bin
  )
endif()

if(BUILD_UNREAL_TOOLS_PLUGIN)
  install(TARGETS unreal-tools
    RUNTIME DESTINATION ${CMAKE_INSTALL_DATADIR}/toolsmith/plugins/unreal-tools/bin
    LIBRARY DESTINATION ${CMAKE_INSTALL_DATADIR}/toolsmith/plugins/unreal-tools/bin
  )
endif()
