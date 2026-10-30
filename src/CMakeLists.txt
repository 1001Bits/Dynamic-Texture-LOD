# ── CommonLibF4 multi-runtime source build ───────────────────────────────────

macro(find_commonlib_path)
    if(CommonLibName AND NOT ${CommonLibName} STREQUAL "")
        find_path(CommonLibPath
            include/REL/Relocation.h
            PATHS ${ROOT_DIR}/external/${CommonLibName}/${CommonLibName}/)
        if(${CommonLibPath} STREQUAL "CommonLibPath-NOTFOUND")
            set_from_environment(${CommonLibName}Path)
            set(CommonLibPath ${${CommonLibName}Path})
        endif()
    endif()
endmacro()

set(CommonLibName "CommonLibF4")
set_root_directory()

find_commonlib_path()
message(STATUS "Building ${PROJECT_NAME} ${PROJECT_VERSION} with ${CommonLibName} at ${CommonLibPath}.")

set(SOURCE_DIR "${ROOT_DIR}/src")
set(SOURCE_FILES
    "${SOURCE_DIR}/AddressLibrary.h"
    "${SOURCE_DIR}/Addresses.cpp"
    "${SOURCE_DIR}/Addresses.h"
    "${SOURCE_DIR}/Budget.cpp"
    "${SOURCE_DIR}/Budget.h"
    "${SOURCE_DIR}/Controls.cpp"
    "${SOURCE_DIR}/Controls.h"
    "${SOURCE_DIR}/FaultReport.cpp"
    "${SOURCE_DIR}/FaultReport.h"
    "${SOURCE_DIR}/FramePump.cpp"
    "${SOURCE_DIR}/FramePump.h"
    "${SOURCE_DIR}/Fo4Mesh.cpp"
    "${SOURCE_DIR}/Fo4Mesh.h"
    "${SOURCE_DIR}/Fo4MeshData.cpp"
    "${SOURCE_DIR}/Fo4MeshData.h"
    "${SOURCE_DIR}/Fo4MeshHeap.cpp"
    "${SOURCE_DIR}/Fo4MeshHeap.h"
    "${SOURCE_DIR}/Fo4MeshHeapHooks.cpp"
    "${SOURCE_DIR}/Fo4MeshHeapHooks.h"
    "${SOURCE_DIR}/Fo4MeshLifetime.cpp"
    "${SOURCE_DIR}/Fo4MeshLifetime.h"
    "${SOURCE_DIR}/Fo4MeshLifetimeDiagnostics.cpp"
    "${SOURCE_DIR}/Fo4MeshLifetimeDiagnostics.h"
    "${SOURCE_DIR}/Fo4MeshOwners.h"
    "${SOURCE_DIR}/Fo4MeshDetach.h"
    "${SOURCE_DIR}/Fo4MeshPass.h"
    "${SOURCE_DIR}/Fo4MeshPilot.h"
    "${SOURCE_DIR}/Fo4MeshResources.cpp"
    "${SOURCE_DIR}/Fo4MeshResources.h"
    "${SOURCE_DIR}/Fo4MeshPolicy.cpp"
    "${SOURCE_DIR}/Fo4MeshPolicy.h"
    "${SOURCE_DIR}/Fo4MeshNative.h"
    "${SOURCE_DIR}/Fo4MeshCountStub.h"
    "${SOURCE_DIR}/Fo4MeshRetireStub.h"
    "${SOURCE_DIR}/GpuBounds.h"
    "${SOURCE_DIR}/HostFloor.h"
    "${SOURCE_DIR}/GpuGather.cpp"
    "${SOURCE_DIR}/GpuGather.h"
    "${SOURCE_DIR}/GpuGatherDispatch.h"
    "${SOURCE_DIR}/GpuFeedback.cpp"
    "${SOURCE_DIR}/GpuFeedback.h"
    "${SOURCE_DIR}/TextureFeedback.cpp"
    "${SOURCE_DIR}/TextureFeedback.h"
    "${SOURCE_DIR}/RequestDemand.h"
    "${SOURCE_DIR}/NativeTextureLevels.h"
    "${SOURCE_DIR}/Lod.cpp"
    "${SOURCE_DIR}/Lod.h"
    "${SOURCE_DIR}/Logging.cpp"
    "${SOURCE_DIR}/Logging.h"
    "${SOURCE_DIR}/main.cpp"
    "${SOURCE_DIR}/PCH.cpp"
    "${SOURCE_DIR}/PluginIni.cpp"
    "${SOURCE_DIR}/PluginIni.h"
    "${SOURCE_DIR}/PCH.h"
    "${SOURCE_DIR}/TextureLifecycle.cpp"
    "${SOURCE_DIR}/TextureLifecycle.h"
    "${SOURCE_DIR}/Vram.cpp"
    "${SOURCE_DIR}/Vram.h"
)

# The game-independent core, shared with the Skyrim plugin. Built as part of
# this target so it gets the same flags and precompiled header.
include("${ROOT_DIR}/external/DynamicTextureLODCore/DynamicTextureLODCore.cmake")
list(APPEND SOURCE_FILES ${DTL_CORE_SOURCES})

source_group(TREE "${ROOT_DIR}" FILES ${SOURCE_FILES})

set(VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/src/Plugin.h")
configure_file(
    "${ROOT_DIR}/cmake/Plugin.h.in"
    "${VERSION_HEADER}"
    @ONLY
)
source_group("src" FILES "${VERSION_HEADER}")

configure_file(
    "${ROOT_DIR}/cmake/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY
)

if(MSVC)
    set(NATIVE_CALLBACK_SOURCES "${SOURCE_DIR}/Fo4Mesh.cpp" "${SOURCE_DIR}/Fo4MeshHeapHooks.cpp")
    list(REMOVE_ITEM SOURCE_FILES ${NATIVE_CALLBACK_SOURCES})
endif()

add_library(
    "${PROJECT_NAME}"
    SHARED
    ${SOURCE_FILES}
    "${VERSION_HEADER}"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
)

target_compile_features("${PROJECT_NAME}" PRIVATE cxx_std_23)

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    # Its zero-reference native callback must not swallow asynchronous engine
    # faults. Parse the common header textually with this source's /EHsc mode.
    set_source_files_properties("${SOURCE_DIR}/Fo4Mesh.cpp" "${SOURCE_DIR}/Fo4MeshHeapHooks.cpp" PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "/EHsc;/FI${SOURCE_DIR}/PCH.h"
        COMPILE_DEFINITIONS "NOMINMAX;WIN32_LEAN_AND_MEAN")
    target_compile_options("${PROJECT_NAME}" PRIVATE
        "/EHa" "/sdl" "/utf-8" "/Zi"
        "/permissive-" "/Zc:preprocessor"
        "/wd4324"
        "$<$<CONFIG:DEBUG>:>"
        "$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
    )
    target_link_options("${PROJECT_NAME}" PRIVATE
        "$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
        "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:NOICF;/DEBUG:FULL>"
    )
endif()

target_include_directories("${PROJECT_NAME}" PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/src"
    "${SOURCE_DIR}"
    "${DTL_CORE_INCLUDE_DIR}"
)

if(DEFINED CommonLibPath AND NOT ${CommonLibPath} STREQUAL "" AND IS_DIRECTORY ${CommonLibPath})
    add_subdirectory(${CommonLibPath} ${CommonLibName})
else()
    message(FATAL_ERROR "Variable ${CommonLibName}Path is not set or in external/.")
endif()

# CommonLib keeps its original directory's compiler defaults. This plugin sets
# exception handling and release inlining explicitly, without conflicting flags.
if(MSVC)
    string(REGEX REPLACE "(^|[ \t])[-/]EHsc([ \t]|$)" "\\1\\2" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "(^|[ \t])[-/]Ob[0123]([ \t]|$)" "\\1\\2" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()

find_package(fmt REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)
find_package(mmio REQUIRED CONFIG)
find_package(xbyak REQUIRED CONFIG)
find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h")

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
add_compile_definitions(_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)

target_include_directories("${PROJECT_NAME}" PRIVATE "${SIMPLEINI_INCLUDE_DIRS}")

target_link_libraries("${PROJECT_NAME}" PRIVATE
    CommonLibF4::CommonLibF4
    fmt::fmt
    mmio::mmio
    spdlog::spdlog
    xbyak::xbyak
    dxgi
    d3d11
    d3dcompiler
)

target_compile_definitions("${PROJECT_NAME}" PRIVATE F4SE_SUPPORT_XBYAK)
target_compile_definitions(CommonLibF4 PUBLIC F4SE_SUPPORT_XBYAK _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)

# Native zero-reference callbacks use /EHsc so a bookkeeping catch cannot swallow
# an engine fault. Compile them separately from the plugin's /EHa precompiled
# header, with the same includes, definitions, optimization and dependencies.
if(MSVC)
    set(NATIVE_CALLBACK_TARGET "${PROJECT_NAME}NativeCallbacks")
    add_library("${NATIVE_CALLBACK_TARGET}" OBJECT ${NATIVE_CALLBACK_SOURCES})
    target_compile_features("${NATIVE_CALLBACK_TARGET}" PRIVATE cxx_std_23)
    target_compile_options("${NATIVE_CALLBACK_TARGET}" PRIVATE
        "$<FILTER:$<TARGET_PROPERTY:${PROJECT_NAME},COMPILE_OPTIONS>,EXCLUDE,^/EHa$>" "/EHsc")
    target_compile_definitions("${NATIVE_CALLBACK_TARGET}" PRIVATE
        "$<TARGET_PROPERTY:${PROJECT_NAME},COMPILE_DEFINITIONS>")
    target_include_directories("${NATIVE_CALLBACK_TARGET}" PRIVATE
        "$<TARGET_PROPERTY:${PROJECT_NAME},INCLUDE_DIRECTORIES>")
    get_target_property(NATIVE_CALLBACK_LINKS "${PROJECT_NAME}" LINK_LIBRARIES)
    target_link_libraries("${NATIVE_CALLBACK_TARGET}" PRIVATE ${NATIVE_CALLBACK_LINKS})
    target_sources("${PROJECT_NAME}" PRIVATE "$<TARGET_OBJECTS:${NATIVE_CALLBACK_TARGET}>")
endif()

target_precompile_headers("${PROJECT_NAME}" PRIVATE "${SOURCE_DIR}/PCH.h")

handle_data_files(
    FILES
    "$<TARGET_FILE:${PROJECT_NAME}>"
    "${ROOT_DIR}/package/${PROJECT_NAME}.ini"
    DESTINATION "F4SE/Plugins"
)

# ── MO2 auto-deploy ─────────────────────────────────────────────────────────
# Per-project deploy targets. Hardcoded so this DLL doesn't leak into other
# projects' mod folders via the shared $FalloutPluginTargets env var.
#
# Only the DLL and its PDB are copied. The plugin writes its own ini when none
# exists, so an edited one in the mod folder is never overwritten by a build.
# Configure with -DDEPLOY_PLUGIN=OFF to build without touching any game folder.
option(DEPLOY_PLUGIN "Copy the plugin into the deploy targets after each build" ON)

set(__DEPLOY_TARGETS
    "C:/Games/MO2/mods/Dynamic Texture LOD"
    "C:/Games/Fallout.4 1.10.163/Data"
)
if(DEPLOY_PLUGIN)
    foreach(DEPLOY_TARGET ${__DEPLOY_TARGETS})
        if(EXISTS "${DEPLOY_TARGET}")
            message("Adding deployment target ${DEPLOY_TARGET}.")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_TARGET}/F4SE/Plugins/"
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${DEPLOY_TARGET}/F4SE/Plugins/"
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${DEPLOY_TARGET}/F4SE/Plugins/")
        else()
            message("Skipping deployment target ${DEPLOY_TARGET} (does not exist).")
        endif()
    endforeach()
else()
    message("Deployment disabled (DEPLOY_PLUGIN=OFF).")
endif()

# ── Zip packaging ────────────────────────────────────────────────────────────
# The DLL and the default ini, laid out as they go into Data.
set(ZIP_DIR "${CMAKE_CURRENT_BINARY_DIR}/zip")
file(MAKE_DIRECTORY "${ZIP_DIR}/F4SE/Plugins")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${ZIP_DIR}/F4SE/Plugins/"
    COMMAND ${CMAKE_COMMAND} -E copy "${ROOT_DIR}/package/${PROJECT_NAME}.ini" "${ZIP_DIR}/F4SE/Plugins/"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-${PROJECT_VERSION}.7z" --format=7zip -- .
    WORKING_DIRECTORY "${ZIP_DIR}")
