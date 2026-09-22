# GoogleTest is test-only; no production library links against it.
# Prefer a system/local install when available, otherwise download once at configure time.
find_package(GTest 1.15.2 CONFIG QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    # On Windows, avoid changing the parent project's CRT linkage.
    set(gtest_force_shared_crt ON CACHE BOOL "Use shared CRT for GoogleTest" FORCE)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(ddse_tests
    unit/core_version_tests.cpp
    unit/application_info_tests.cpp
    unit/application_status_tests.cpp
    unit/platform_info_tests.cpp
    unit/error_tests.cpp
    unit/logger_tests.cpp
    unit/file_system_tests.cpp
    unit/sqlite_tests.cpp
    unit/dson_reader_tests.cpp
    unit/dson_writer_tests.cpp
    unit/save_profile_tests.cpp
    unit/content_scanner_tests.cpp
    unit/stage7_resource_mapping_tests.cpp
    integration/campaign_model_tests.cpp
    integration/save_commit_tests.cpp
    unit/campaign_edit_session_tests.cpp
    integration/base_content_database_tests.cpp
    integration/mod_environment_tests.cpp
    integration/fixture_layout_tests.cpp
)
target_link_libraries(ddse_tests PRIVATE
    ddse_application
    ddse_infrastructure
    ddse_project_options
    ddse_project_warnings
    GTest::gtest_main
)
target_include_directories(ddse_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/helpers")
target_compile_definitions(ddse_tests PRIVATE
    DDSE_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    DDSE_TEST_SAVE_PROFILE_DIR="${PROJECT_SOURCE_DIR}/test_save_profile/profile_0"
    DDSE_TEST_SAVE_BACKUP_DIR="${PROJECT_SOURCE_DIR}/test_save_profile/backup/profile_0"
)
ddse_copy_mingw_runtime(ddse_tests)

include(GoogleTest)
gtest_discover_tests(ddse_tests
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "ddse."
)

if(DDSE_BUILD_CLI)
    add_test(NAME ddse.cli_smoke COMMAND ddse_cli --version)
    set_tests_properties(ddse.cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "DDSE 0.1.0")
endif()
