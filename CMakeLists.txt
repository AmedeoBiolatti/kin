cmake_minimum_required(VERSION 3.22)

project(kin VERSION 0.2.1 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

function(kin_enable_release_optimizations target)
    target_compile_definitions(${target} PRIVATE
        "$<$<CONFIG:Release>:NDEBUG>"
    )

    target_compile_options(${target} PRIVATE
        "$<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2;/Ob2;/Oi;/Gy;/EHsc>"
        "$<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang>>:-O2>"
    )
endfunction()

function(kin_enable_release_link_optimizations target)
    target_link_options(${target} PRIVATE
        "$<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/OPT:REF;/OPT:ICF>"
    )
endfunction()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(dependencies)
include(kin_game)

# Built by default only when kin is the top-level project: a game that adds
# kin with add_subdirectory gets the engine alone.
option(KIN_BUILD_GAMES "Build sample games" ${PROJECT_IS_TOP_LEVEL})
option(KIN_BUILD_EXAMPLES "Build interactive performance examples" ${PROJECT_IS_TOP_LEVEL})
option(KIN_BUILD_BENCHMARKS "Build benchmark executables" ${PROJECT_IS_TOP_LEVEL})
option(KIN_BUILD_TESTS "Build kin's tests (with BUILD_TESTING)" ${PROJECT_IS_TOP_LEVEL})
option(KIN_ENABLE_PROFILING "Enable Kin manual profiling macros" OFF)
option(KIN_ENABLE_RELEASE_PROFILING "Enable Kin manual profiling macros in Release builds" ON)

add_subdirectory(engine)

if(KIN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(KIN_BUILD_GAMES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/CMakeLists.txt")
    add_subdirectory(games)
endif()

if(BUILD_TESTING AND KIN_BUILD_TESTS)
    add_subdirectory(tests)
endif()
