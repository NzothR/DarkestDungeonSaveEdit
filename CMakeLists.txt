cmake_minimum_required(VERSION 3.20)

project(ddse_backend VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

option(DDSE_BUILD_CLI "Build the command-line composition root" ON)
option(DDSE_ENABLE_WARNINGS "Enable compiler warnings for project targets" ON)

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
add_library(ddse_core STATIC src/core/version.cpp)
add_library(ddse::core ALIAS ddse_core)
ddse_set_project_defaults(ddse_core)

# Application may depend on core contracts, but never on concrete adapters.
add_library(ddse_application STATIC src/application/application_info.cpp)
add_library(ddse::application ALIAS ddse_application)
ddse_set_project_defaults(ddse_application)
target_link_libraries(ddse_application PUBLIC ddse_core)

# Adapters depend inward; application must never link this target.
add_library(ddse_infrastructure STATIC src/infrastructure/platform_info.cpp)
add_library(ddse::infrastructure ALIAS ddse_infrastructure)
ddse_set_project_defaults(ddse_infrastructure)
target_link_libraries(ddse_infrastructure PUBLIC ddse_core)

# Concrete implementations are assembled only at the executable boundary.
if(DDSE_BUILD_CLI)
    add_executable(ddse_cli src/composition_root/main.cpp)
    target_link_libraries(ddse_cli PRIVATE
        ddse_application ddse_infrastructure ddse_project_options ddse_project_warnings)
endif()

if(BUILD_TESTING)
    add_executable(ddse_tests tests/smoke_tests.cpp)
    target_link_libraries(ddse_tests PRIVATE
        ddse_application ddse_infrastructure ddse_project_options ddse_project_warnings)
    add_test(NAME ddse.smoke COMMAND ddse_tests)
    if(DDSE_BUILD_CLI)
        add_test(NAME ddse.cli_smoke COMMAND ddse_cli --version)
        set_tests_properties(ddse.cli_smoke PROPERTIES PASS_REGULAR_EXPRESSION "DDSE 0.1.0")
    endif()
endif()
