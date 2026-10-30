# The game-independent core of Dynamic Texture LOD, shared by the Fallout 4
# and Skyrim plugins. Include this file, then add DTL_CORE_SOURCES to the
# plugin target and DTL_CORE_INCLUDE_DIR to its include directories, so the
# core builds with the plugin's own flags and precompiled header:
#
#   include("${ROOT_DIR}/external/DynamicTextureLODCore/DynamicTextureLODCore.cmake")
#   target_sources(${PROJECT_NAME} PRIVATE ${DTL_CORE_SOURCES})
#   target_include_directories(${PROJECT_NAME} PRIVATE ${DTL_CORE_INCLUDE_DIR})
#
# The core uses spdlog (the plugin's default logger), SimpleIni and DXGI.

set(DTL_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(DTL_CORE_INCLUDE_DIR "${DTL_CORE_DIR}/src")
set(DTL_CORE_SOURCES
    "${DTL_CORE_DIR}/src/core/BudgetMode.cpp"
    "${DTL_CORE_DIR}/src/core/BudgetMode.h"
    "${DTL_CORE_DIR}/src/core/DdsHeader.h"
    "${DTL_CORE_DIR}/src/core/Frustum.h"
    "${DTL_CORE_DIR}/src/core/LodPolicy.h"
    "${DTL_CORE_DIR}/src/core/Observation.h"
    "${DTL_CORE_DIR}/src/core/Planner.cpp"
    "${DTL_CORE_DIR}/src/core/Planner.h"
    "${DTL_CORE_DIR}/src/core/StarfieldPlanner.cpp"
    "${DTL_CORE_DIR}/src/core/StarfieldPlanner.h"
    "${DTL_CORE_DIR}/src/core/StarfieldPolicy.h"
    "${DTL_CORE_DIR}/src/core/FrameBudget.h"
    "${DTL_CORE_DIR}/src/core/GpuProcessMemory.cpp"
    "${DTL_CORE_DIR}/src/core/GpuProcessMemory.h"
    "${DTL_CORE_DIR}/src/core/StreamingLedger.cpp"
    "${DTL_CORE_DIR}/src/core/StreamingLedger.h"
    "${DTL_CORE_DIR}/src/core/Settings.cpp"
    "${DTL_CORE_DIR}/src/core/Settings.h"
    "${DTL_CORE_DIR}/src/core/TextureFormat.h"
    "${DTL_CORE_DIR}/src/core/VramMonitor.cpp"
    "${DTL_CORE_DIR}/src/core/VramMonitor.h"
)
