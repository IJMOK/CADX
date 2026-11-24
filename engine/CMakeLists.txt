cmake_minimum_required(VERSION 3.24)
project(cadx_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCASCADE REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

add_executable(cadx-engine
  src/main.cpp
  src/mesh.cpp
)

target_include_directories(cadx-engine SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_include_directories(cadx-engine PRIVATE src)

# Debian/Ubuntu OCCT 7.6.3 packages omit NCollection_AliasedArray.hxx.
# Fall back to the upstream copy (LGPL, unmodified) only when it is missing.
if(NOT EXISTS "${OpenCASCADE_INCLUDE_DIR}/NCollection_AliasedArray.hxx")
  message(STATUS "OCCT header NCollection_AliasedArray.hxx missing; using third_party/occt-compat")
  target_include_directories(cadx-engine SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/occt-compat)
endif()
target_link_libraries(cadx-engine PRIVATE
  TKernel TKMath TKBRep TKTopAlgo TKPrim TKMesh TKG3d TKGeomBase
  nlohmann_json::nlohmann_json
)

if(MSVC)
  target_compile_options(cadx-engine PRIVATE /W4 /permissive-)
else()
  target_compile_options(cadx-engine PRIVATE -Wall -Wextra)
endif()
