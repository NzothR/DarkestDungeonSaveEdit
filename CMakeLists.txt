cmake_minimum_required(VERSION 3.20)

project(ddse_backend VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

option(DDSE_BUILD_CLI "Build the command-line composition root" ON)
option(DDSE_ENABLE_WARNINGS "Enable compiler warnings for project targets" ON)

find_package(SQLite3 REQUIRED)
find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    set(JSON_BuildTests OFF CACHE INTERNAL "Disable nlohmann/json tests")
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()
if(TARGET SQLite3::SQLite3)
    set(DDSE_SQLITE_TARGET SQLite3::SQLite3)
else()
    # SQLite3::SQLite3 is an alias added after the project's CMake 3.20 minimum.
    set(DDSE_SQLITE_TARGET SQLite::SQLite3)
endif()

# No global compiler flags or include paths: keep each target's contract explicit.
add_library(ddse_project_options INTERFACE)
target_compile_features(ddse_project_options INTERFACE cxx_std_20)
set_target_properties(ddse_project_options PROPERTIES
    INTERFACE_POSITION_INDEPENDENT_CODE ON)

add_library(ddse_project_warnings INTERFACE)
if(DDSE_ENABLE_WARNINGS)
    if(MSVC)
        target_compile_options(ddse_project_warnings INTERFACE /W4 /permissive-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(ddse_project_warnings INTERFACE -Wall -Wextra -Wpedantic)
    endif()
endif()

function(ddse_set_project_defaults target)
    target_link_libraries(${target} PRIVATE ddse_project_options ddse_project_warnings)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
endfunction()

# Core contains no platform I/O, SQLite or HTTP dependencies.
add_library(ddse_core STATIC
    src/core/version.cpp
    src/core/error.cpp
    src/core/dson/dson_document.cpp
    src/core/dson/dson_reader.cpp
    src/core/dson/dson_writer.cpp
)
add_library(ddse::core ALIAS ddse_core)
ddse_set_project_defaults(ddse_core)

# Semantic types are header-only and have no application or adapter dependencies.
add_library(ddse_domain INTERFACE)
add_library(ddse::domain ALIAS ddse_domain)
target_compile_features(ddse_domain INTERFACE cxx_std_20)
target_include_directories(ddse_domain INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# Application may depend on core contracts, but never on concrete adapters.
add_library(ddse_application STATIC
    src/application/application_info.cpp
    src/application/logger.cpp
    src/application/save_profile.cpp
    src/application/content_scanner.cpp
    src/application/mod_environment.cpp
    src/application/campaign_mappings.cpp
    src/application/campaign_model_builder.cpp
)
add_library(ddse::application ALIAS ddse_application)
ddse_set_project_defaults(ddse_application)
target_link_libraries(ddse_application PUBLIC ddse_core ddse_domain)
target_link_libraries(ddse_application PRIVATE nlohmann_json::nlohmann_json)

# Adapters depend inward; application must never link this target.
add_library(ddse_infrastructure STATIC
    src/infrastructure/platform_info.cpp
    src/infrastructure/console_logger.cpp
    src/infrastructure/native_file_system.cpp
    src/infrastructure/sqlite/database.cpp
    src/infrastructure/sqlite/statement.cpp
    src/infrastructure/sqlite/transaction.cpp
    src/infrastructure/sqlite/migration_runner.cpp
    src/infrastructure/base_content_database.cpp
    src/infrastructure/mod_environment_database.cpp
    src/infrastructure/sqlite_content_environment.cpp
)
add_library(ddse::infrastructure ALIAS ddse_infrastructure)
ddse_set_project_defaults(ddse_infrastructure)
target_link_libraries(ddse_infrastructure PUBLIC ddse_core)
target_link_libraries(ddse_infrastructure
    PUBLIC ddse_application
    PRIVATE ${DDSE_SQLITE_TARGET})

# Concrete implementations are assembled only at the executable boundary.
if(DDSE_BUILD_CLI)
    add_executable(ddse_cli src/composition_root/main.cpp)
    target_link_libraries(ddse_cli PRIVATE
        ddse_application ddse_infrastructure ddse_project_options ddse_project_warnings)
    if(WIN32)
        target_link_libraries(ddse_cli PRIVATE shell32)
    endif()
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
