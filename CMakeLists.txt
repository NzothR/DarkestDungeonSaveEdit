cmake_minimum_required(VERSION 3.20)

project(ddse_backend VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

option(DDSE_BUILD_CLI "Build the command-line composition root" ON)
option(DDSE_BUILD_HTTP "Build the local Drogon frontend host" ON)
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

if(DDSE_BUILD_HTTP)
    find_package(Drogon CONFIG QUIET)
    if(NOT TARGET Drogon::Drogon)
        include(FetchContent)

        # Keep the local HTTP dependency minimal: no ORM, TLS, tools, examples,
        # framework tests, or optional compression/config parsers are needed.
        set(BUILD_CTL OFF CACHE BOOL "Disable Drogon command line tools" FORCE)
        set(BUILD_EXAMPLES OFF CACHE BOOL "Disable Drogon examples" FORCE)
        set(BUILD_ORM OFF CACHE BOOL "Disable Drogon ORM" FORCE)
        set(BUILD_DOC OFF CACHE BOOL "Disable Drogon documentation" FORCE)
        set(BUILD_BROTLI OFF CACHE BOOL "Disable Drogon Brotli support" FORCE)
        set(BUILD_YAML_CONFIG OFF CACHE BOOL "Disable Drogon YAML config" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build Drogon dependencies statically" FORCE)
        set(BUILD_C-ARES OFF CACHE BOOL "Disable optional c-ares support" FORCE)
        set(JSONCPP_WITH_TESTS OFF CACHE BOOL "Disable JsonCpp tests" FORCE)
        set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "Disable JsonCpp post-build tests" FORCE)
        set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "Disable JsonCpp pkg-config files" FORCE)
        set(JSONCPP_WITH_EXAMPLE OFF CACHE BOOL "Disable JsonCpp examples" FORCE)
        set(JSONCPP_WITH_CMAKE_PACKAGE OFF CACHE BOOL "Disable JsonCpp package export" FORCE)
        set(_DDSE_BUILD_TESTING "${BUILD_TESTING}")
        set(BUILD_TESTING OFF CACHE BOOL "Disable third-party framework tests" FORCE)

        # Drogon's FindJsoncpp module requires include/library variables, while
        # JsonCpp itself is built as a normal in-tree target by FetchContent.
        FetchContent_Declare(
            jsoncpp
            GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
            GIT_TAG 1.9.6
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(jsoncpp)
        set(JSONCPP_INCLUDE_DIRS "${jsoncpp_SOURCE_DIR}/include" CACHE PATH
            "JsonCpp headers for Drogon" FORCE)
        set(JSONCPP_LIBRARIES jsoncpp_static CACHE STRING
            "JsonCpp target for Drogon" FORCE)

        # MinGW ships a matching zlib; prefer it over libraries from unrelated
        # package managers in CMAKE_PREFIX_PATH when using the CLion toolchain.
        if(WIN32 AND NOT MSVC)
            get_filename_component(_ddse_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
            get_filename_component(_ddse_mingw_root "${_ddse_compiler_bin}" DIRECTORY)
            if(EXISTS "${_ddse_mingw_root}/include/zlib.h" AND
               EXISTS "${_ddse_mingw_root}/lib/libz.a")
                set(ZLIB_INCLUDE_DIR "${_ddse_mingw_root}/include" CACHE PATH
                    "zlib headers matching the configured MinGW compiler" FORCE)
                set(ZLIB_LIBRARY "${_ddse_mingw_root}/lib/libz.a" CACHE FILEPATH
                    "zlib library matching the configured MinGW compiler" FORCE)
            endif()
        endif()

        set(_DDSE_PREVIOUS_DISABLE_OPENSSL "${CMAKE_DISABLE_FIND_PACKAGE_OpenSSL}")
        set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL TRUE)
        FetchContent_Declare(
            drogon
            GIT_REPOSITORY https://github.com/drogonframework/drogon.git
            GIT_TAG v1.9.13
            GIT_SHALLOW TRUE
            GIT_SUBMODULES_RECURSE TRUE
        )
        FetchContent_MakeAvailable(drogon)
        set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL "${_DDSE_PREVIOUS_DISABLE_OPENSSL}")
        set(BUILD_TESTING "${_DDSE_BUILD_TESTING}" CACHE BOOL "Project test option" FORCE)
    endif()
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
    src/core/dson/dson_document_editor.cpp
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
    src/application/application_status.cpp
    src/application/configuration_store.cpp
    src/application/application_info.cpp
    src/application/logger.cpp
    src/application/save_profile.cpp
    src/application/content_scanner.cpp
    src/application/mod_environment.cpp
    src/application/campaign_mappings.cpp
    src/application/campaign_model_builder.cpp
    src/application/campaign_edit_session.cpp
    src/application/save_commit.cpp
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

if(DDSE_BUILD_HTTP)
    add_library(ddse_http_adapter STATIC
        src/infrastructure/http_drogon_server.cpp
    )
    add_library(ddse::http_adapter ALIAS ddse_http_adapter)
    ddse_set_project_defaults(ddse_http_adapter)
    target_link_libraries(ddse_http_adapter PUBLIC
        ddse_application
        Drogon::Drogon
    )

    add_executable(ddse_http src/composition_root/http_main.cpp)
    target_link_libraries(ddse_http PRIVATE
        ddse_http_adapter
        ddse_infrastructure
        ddse_project_options
        ddse_project_warnings
    )
    if(WIN32)
        target_link_libraries(ddse_http PRIVATE shell32 ws2_32)
    endif()
    add_custom_command(TARGET ddse_http POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/web"
            "$<TARGET_FILE_DIR:ddse_http>/web"
        COMMENT "Copying local editor frontend"
    )
endif()

add_executable(ddse_generate_game_test_saves tools/generate_game_test_saves.cpp)
target_link_libraries(ddse_generate_game_test_saves PRIVATE
    ddse_application ddse_infrastructure nlohmann_json::nlohmann_json
    ddse_project_options ddse_project_warnings)
if(WIN32)
    target_link_libraries(ddse_generate_game_test_saves PRIVATE shell32)
endif()

add_executable(ddse_generate_advanced_game_test_saves tools/generate_advanced_game_test_saves.cpp)
target_link_libraries(ddse_generate_advanced_game_test_saves PRIVATE
    ddse_application ddse_infrastructure nlohmann_json::nlohmann_json
    ddse_project_options ddse_project_warnings)
if(WIN32)
    target_link_libraries(ddse_generate_advanced_game_test_saves PRIVATE shell32)
endif()

add_executable(ddse_generate_stage12_operation_test_saves tools/generate_stage12_operation_test_saves.cpp)
target_link_libraries(ddse_generate_stage12_operation_test_saves PRIVATE
    ddse_application ddse_infrastructure nlohmann_json::nlohmann_json
    ddse_project_options ddse_project_warnings)
if(WIN32)
    target_link_libraries(ddse_generate_stage12_operation_test_saves PRIVATE shell32)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
