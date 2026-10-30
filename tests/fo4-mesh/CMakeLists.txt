cmake_minimum_required(VERSION 3.23)
project(Fo4MeshTests LANGUAGES CXX)
enable_testing()
add_executable(fo4_mesh_tests Tests.cpp ../../src/Fo4MeshData.cpp ../../src/Fo4MeshResources.cpp
    ../../src/Fo4MeshLifetime.cpp)
target_compile_features(fo4_mesh_tests PRIVATE cxx_std_23)
target_compile_definitions(fo4_mesh_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(fo4_mesh_tests PRIVATE ../../src)
target_link_libraries(fo4_mesh_tests PRIVATE d3d11 dxgi)
add_test(NAME fo4_mesh_tests COMMAND fo4_mesh_tests)
add_executable(fo4_mesh_retire_tests RetireTests.cpp)
target_compile_features(fo4_mesh_retire_tests PRIVATE cxx_std_23)
target_compile_definitions(fo4_mesh_retire_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(fo4_mesh_retire_tests PRIVATE ../../src)
add_test(NAME fo4_mesh_retire_tests COMMAND fo4_mesh_retire_tests)
set(DTL_CORE ../../external/DynamicTextureLODCore/src)
add_executable(fo4_mesh_policy_tests PolicyTests.cpp ../../src/Fo4MeshPolicy.cpp
    ${DTL_CORE}/core/Planner.cpp ${DTL_CORE}/core/StarfieldPlanner.cpp ${DTL_CORE}/core/StreamingLedger.cpp)
target_compile_features(fo4_mesh_policy_tests PRIVATE cxx_std_23)
target_include_directories(fo4_mesh_policy_tests PRIVATE ../../src ${DTL_CORE})
add_test(NAME fo4_mesh_policy_tests COMMAND fo4_mesh_policy_tests)
add_executable(fo4_mesh_heap_tests HeapTests.cpp ../../src/Fo4MeshHeap.cpp)
target_compile_features(fo4_mesh_heap_tests PRIVATE cxx_std_23)
target_compile_definitions(fo4_mesh_heap_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(fo4_mesh_heap_tests PRIVATE ../../src)
add_test(NAME fo4_mesh_heap_tests COMMAND fo4_mesh_heap_tests)
add_executable(fo4_mesh_pilot_tests PilotTests.cpp ../../src/Fo4MeshData.cpp)
target_compile_features(fo4_mesh_pilot_tests PRIVATE cxx_std_23)
target_compile_definitions(fo4_mesh_pilot_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0601 _WIN32_WINNT=0x0601)
target_include_directories(fo4_mesh_pilot_tests PRIVATE ../../src)
target_link_libraries(fo4_mesh_pilot_tests PRIVATE bcrypt)
add_test(NAME fo4_mesh_pilot_tests COMMAND fo4_mesh_pilot_tests)
add_executable(fo4_mesh_owner_tests OwnerTests.cpp)
target_compile_features(fo4_mesh_owner_tests PRIVATE cxx_std_23)
target_include_directories(fo4_mesh_owner_tests PRIVATE ../../src)
add_test(NAME fo4_mesh_owner_tests COMMAND fo4_mesh_owner_tests)
add_executable(fo4_mesh_detach_tests DetachTests.cpp)
target_compile_features(fo4_mesh_detach_tests PRIVATE cxx_std_23)
target_include_directories(fo4_mesh_detach_tests PRIVATE ../../src)
add_test(NAME fo4_mesh_detach_tests COMMAND fo4_mesh_detach_tests)

# Fake COM buffers only; this target creates no D3D device or tracing session.
add_executable(fo4_mesh_lifetime_tests LifetimeTests.cpp ../../src/Fo4MeshLifetime.cpp)
target_compile_features(fo4_mesh_lifetime_tests PRIVATE cxx_std_23)
target_compile_definitions(fo4_mesh_lifetime_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    WINVER=0x0601 _WIN32_WINNT=0x0601)
target_include_directories(fo4_mesh_lifetime_tests PRIVATE ../../src)
if(MSVC)
    target_compile_options(fo4_mesh_lifetime_tests PRIVATE /W4 /WX /EHsc /utf-8)
endif()
add_test(NAME fo4_mesh_lifetime_tests COMMAND fo4_mesh_lifetime_tests)

# Compile the native adapter without linking it into the production plugin.
# This target performs no engine calls and does not launch a game.
add_library(fo4_mesh_runtime_compile OBJECT ../../src/Fo4Mesh.cpp ../../src/Fo4MeshHeapHooks.cpp)
target_compile_features(fo4_mesh_runtime_compile PRIVATE cxx_std_23)
target_compile_definitions(fo4_mesh_runtime_compile PRIVATE ENABLE_FALLOUT_F4=1 ENABLE_FALLOUT_NG=1
    ENABLE_FALLOUT_VR=1 F4SE_SUPPORT_XBYAK SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)
target_include_directories(fo4_mesh_runtime_compile PRIVATE ../../src ${DTL_CORE}
    ../../build/src/src ../../external/CommonLibF4/CommonLibF4/include
    ../../build/vcpkg_installed/x64-windows-static-md/include
    ../../build/vcpkg_installed/x64-windows-static-md/include/xbyak)
target_compile_options(fo4_mesh_runtime_compile PRIVATE /EHsc /utf-8 /Zc:preprocessor /wd4324)
target_precompile_headers(fo4_mesh_runtime_compile PRIVATE ../../src/PCH.h)
