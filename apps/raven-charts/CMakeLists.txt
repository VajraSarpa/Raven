# Raven Charts: CSV / JSON files to Vega-Lite charts, in a Raven window.
#
# Inside the Raven tree (apps/raven-charts):
#   cmake -S apps/raven-charts -B build-charts -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-charts --parallel
#
# Elsewhere: -DRAVEN_SOURCE_DIR=/path/to/Raven
cmake_minimum_required(VERSION 3.16)
project(raven_charts C)

if(NOT TARGET raven::raven)
	set(RAVEN_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Folder of the Raven sources")
	set(RAVEN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(RAVEN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(RAVEN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
	set(RAVEN_BUILD_WEB ON CACHE BOOL "" FORCE)
	add_subdirectory("${RAVEN_SOURCE_DIR}" raven)
endif()

add_executable(raven_charts src/main.c ui/main.xml web/chart.html web/chart.js)
target_link_libraries(raven_charts PRIVATE raven::raven raven::web)
set_target_properties(raven_charts PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
# where ui/, web/ and data/ are; RAVEN_CHARTS_DIR overrides it at run time
target_compile_definitions(raven_charts PRIVATE RAVEN_CHARTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
	target_compile_definitions(raven_charts PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
	target_link_libraries(raven_charts PRIVATE m)
endif()
