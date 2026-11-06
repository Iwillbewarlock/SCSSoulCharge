cmake_minimum_required(VERSION 3.21)

project(SCSSoulCharge VERSION 0.1.0 LANGUAGES CXX)

# CommonLibSSE-NG = alandtse/CommonLibVR 10.1.0 (39f9d07), cloned into extern/ (not tracked).
# Chosen for AE 1.7.104 (address library format 5) support. GPL-3.0 with modding exceptions.
set(ENABLE_SKYRIM_SE ON  CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON  CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)

add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)

# Game-independent rules, shared by the plugin and the offline tests.
add_library(scssc_core STATIC
    src/core/charge_rules.cpp
    src/core/pool_table.cpp
    src/core/staff_filter.cpp
)
target_include_directories(scssc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(scssc_core PUBLIC cxx_std_23)

add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE scssc_core)
enable_testing()
add_test(NAME core_tests COMMAND core_tests)

add_library(${PROJECT_NAME} SHARED
    src/plugin.cpp
    src/settings.cpp
    src/keywords.cpp
    src/selftest.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE scssc_core)
