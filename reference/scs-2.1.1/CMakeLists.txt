# This file configures the "build" of this SKSE plugin
# - how to compile the C++ plugin (in addition to settings found in CMakePresets.json)
# - where to output the resulting .dll file

# It's recommended to set a minimum CMake version.
# If you use CMake features from higher versions, update this to match.
cmake_minimum_required(VERSION 3.21)

# [BEGIN set_project_name]
# Set your project name. This will be the name of your SKSE .dll file.
project(SpellCastingStaff VERSION 2.1.1 LANGUAGES CXX)

# [END set_project_name]

# #
# YOU DO NOT NEED TO EDIT ANYTHING BELOW HERE
# #

# Release 2.1.1: build with bundled 1.7.104-backported CommonLibSSE-NG; package your own DLL.

# Setup your SKSE plugin as an SKSE plugin!
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/CommonLibSSE-NG)
add_library(${PROJECT_NAME} SHARED
    src/PluginInfo.cpp
    src/RuntimeCompatibility.cpp
    src/CompatibilityError.cpp
    src/LogPath.cpp
    src/Plugin.cpp
    src/Controller.cpp
    src/Events.cpp
    src/Equipment.cpp
    src/Charge.cpp
    src/AttackHook.cpp
    src/BoundLifecycleHook.cpp
    src/Casting.cpp
    src/StaffSound.cpp
    src/MenuEquipHook.cpp
    src/FrameHook.cpp
    src/ProjectileDualFlag.cpp
    src/CasterDiagnostics.cpp
    src/PerkEntryProbe.cpp
    src/InventoryNamePreview.cpp
    src/Naming.cpp
    src/Serialization.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE shell32 ole32 uuid)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23) # <--- use C++23 standard
# UI messages are localized in UTF-8 INI files. Force UTF-8 source/execution charsets on MSVC.
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif()
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h) # <--- PCH.h is required!


# Optional release-only path trimming for VS2022. MSVC's /d1trimfile is undocumented.
# After enabling, check the built DLL for absolute C:\\Users\\ paths before publishing.
option(SCS_TRIM_RELEASE_FILE_PATHS "Use MSVC /d1trimfile to trim local paths from __FILE__ in Release" OFF)
if(MSVC AND SCS_TRIM_RELEASE_FILE_PATHS)
    target_compile_options(${PROJECT_NAME} PRIVATE
        "$<$<CONFIG:Release>:/d1trimfile:${CMAKE_CURRENT_SOURCE_DIR}/>")
endif()
