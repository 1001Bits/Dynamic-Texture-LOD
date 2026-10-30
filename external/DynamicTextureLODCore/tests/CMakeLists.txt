# Unit tests for the game-independent core. Standalone, no CommonLib or vcpkg:
#   cmake -S tests -B build-tests -G Ninja
#   cmake --build build-tests
#   build-tests\core_tests.exe
cmake_minimum_required(VERSION 3.23)
project(DynamicTextureLODCoreTests LANGUAGES CXX)

add_executable(core_tests
    CoreTests.cpp
    ../src/core/Planner.cpp
    ../src/core/StarfieldPlanner.cpp
    ../src/core/StreamingLedger.cpp
)
target_include_directories(core_tests PRIVATE ../src)
target_compile_features(core_tests PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(core_tests PRIVATE /W4 /permissive- /utf-8)
endif()

enable_testing()
add_test(NAME core_tests COMMAND core_tests)

add_executable(starfield_tests StarfieldTests.cpp
    ../src/core/Planner.cpp ../src/core/StarfieldPlanner.cpp ../src/core/StreamingLedger.cpp)
target_include_directories(starfield_tests PRIVATE ../src)
target_compile_features(starfield_tests PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(starfield_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME starfield_tests COMMAND starfield_tests)

add_executable(pressure_extras_tests PressureExtrasTests.cpp)
target_include_directories(pressure_extras_tests PRIVATE ../src)
target_compile_features(pressure_extras_tests PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(pressure_extras_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME pressure_extras_tests COMMAND pressure_extras_tests)

# Optional integration of the actual INI reader/budget service using an
# existing dependency installation. Hardware monitor is replaced by a stub.
set(DTL_TEST_DEPS "" CACHE PATH "Optional x64-windows-static-md dependency prefix")
if(DTL_TEST_DEPS)
    add_executable(settings_budget_tests SettingsBudgetTests.cpp
        ../src/core/Settings.cpp ../src/core/BudgetMode.cpp ../src/core/Planner.cpp
        ../src/core/StarfieldPlanner.cpp ../src/core/StreamingLedger.cpp ../src/core/GpuProcessMemory.cpp)
    target_include_directories(settings_budget_tests PRIVATE ../src "${DTL_TEST_DEPS}/include")
    target_compile_features(settings_budget_tests PRIVATE cxx_std_23)
    target_compile_definitions(settings_budget_tests PRIVATE SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL)
    target_compile_options(settings_budget_tests PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(settings_budget_tests PRIVATE "${DTL_TEST_DEPS}/lib/spdlog.lib" "${DTL_TEST_DEPS}/lib/fmt.lib")
    add_test(NAME settings_budget_tests COMMAND settings_budget_tests)
endif()
