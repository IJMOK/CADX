cmake_minimum_required(VERSION 3.24)
project(cadx_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CADX_BUILD_TESTS "Build the engine tests" ON)

find_package(OpenCASCADE REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Eigen3 3.3 REQUIRED NO_MODULE)
find_package(Boost 1.74 REQUIRED)

# PlaneGCS, vendored from FreeCAD (LGPL). Built on its own with warnings off;
# only src/sketch/sketch_solver_planegcs.cpp includes its headers.
set(PLANEGCS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/planegcs)
add_library(planegcs STATIC
  ${PLANEGCS_DIR}/App/planegcs/GCS.cpp
  ${PLANEGCS_DIR}/App/planegcs/Constraints.cpp
  ${PLANEGCS_DIR}/App/planegcs/Geo.cpp
  ${PLANEGCS_DIR}/App/planegcs/SubSystem.cpp
  ${PLANEGCS_DIR}/App/planegcs/qp_eq.cpp
)
target_include_directories(planegcs SYSTEM PUBLIC ${PLANEGCS_DIR} ${PLANEGCS_DIR}/shim)
target_link_libraries(planegcs PUBLIC Eigen3::Eigen Boost::headers)
set_target_properties(planegcs PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(planegcs PRIVATE /w /bigobj)
else()
  target_compile_options(planegcs PRIVATE -w)
endif()

# Core library: everything except the RPC entry point, so tests can link it.
add_library(cadx_core STATIC
  src/mesh.cpp
  src/naming.cpp
  src/part.cpp
  src/profile.cpp
  src/sketch/sketch.cpp
  src/sketch/sketch_solver_planegcs.cpp
)
target_include_directories(cadx_core SYSTEM PUBLIC ${OpenCASCADE_INCLUDE_DIR})
target_include_directories(cadx_core PUBLIC src)

# Debian/Ubuntu OCCT 7.6.3 packages omit NCollection_AliasedArray.hxx.
# Fall back to the upstream copy (LGPL, unmodified) only when it is missing.
if(NOT EXISTS "${OpenCASCADE_INCLUDE_DIR}/NCollection_AliasedArray.hxx")
  message(STATUS "OCCT header NCollection_AliasedArray.hxx missing; using third_party/occt-compat")
  target_include_directories(cadx_core SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/occt-compat)
endif()

target_link_libraries(cadx_core PUBLIC
  TKernel TKMath TKBRep TKTopAlgo TKPrim TKMesh TKG3d TKGeomBase TKGeomAlgo
  TKFillet TKBool TKBO TKShHealing
  nlohmann_json::nlohmann_json
)
target_link_libraries(cadx_core PRIVATE planegcs)

if(MSVC)
  target_compile_options(cadx_core PUBLIC /W4 /permissive-)
else()
  target_compile_options(cadx_core PUBLIC -Wall -Wextra)
endif()

add_executable(cadx-engine src/main.cpp)
target_link_libraries(cadx-engine PRIVATE cadx_core)

if(CADX_BUILD_TESTS)
  find_package(GTest REQUIRED)
  enable_testing()
  add_executable(cadx-tests tests/naming_test.cpp tests/sketch_test.cpp)
  target_link_libraries(cadx-tests PRIVATE cadx_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(cadx-tests)
endif()
