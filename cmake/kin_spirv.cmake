# cmake -DOUTPUT_DIR=<dir> [-DGLSLC=<glslc>] [-DSOURCES=<file or folder>;...] -P cmake/kin_spirv.cmake
#
# Compiles kin's shaders (the engine's and its demos') and any SOURCES to SPIR-V
# in OUTPUT_DIR, named as kin_compile_glsl names them. SPIR-V is the same on
# every machine, so a build where glslc does not run (the Steam Runtime) can use
# this folder instead: -DKIN_SPIRV_DIR=<dir> (docs/shipping.md).

if(NOT OUTPUT_DIR)
    message(FATAL_ERROR "usage: cmake -DOUTPUT_DIR=<dir> [-DGLSLC=<glslc>] [-DSOURCES=...] -P kin_spirv.cmake")
endif()
if(NOT GLSLC)
    find_program(GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
endif()
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found: install it, or pass -DGLSLC=<path>")
endif()

get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(GLOB sources "${root}/engine/shaders/*.glsl" "${root}/games/*/shaders/*.glsl")
foreach(source IN LISTS SOURCES)
    if(IS_DIRECTORY "${source}")
        file(GLOB more "${source}/*.glsl")
        list(APPEND sources ${more})
    else()
        list(APPEND sources "${source}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(count 0)
foreach(source IN LISTS sources)
    get_filename_component(name "${source}" NAME)
    if(name MATCHES "\\.vert\\.glsl$")
        set(stage vertex)
    elseif(name MATCHES "\\.frag\\.glsl$")
        set(stage fragment)
    elseif(name MATCHES "\\.comp\\.glsl$")
        set(stage compute)
    else()
        message(FATAL_ERROR "${name} is not .vert.glsl, .frag.glsl or .comp.glsl")
    endif()
    string(REGEX REPLACE "\\.glsl$" ".spv" spv_name "${name}")
    execute_process(COMMAND "${GLSLC}" -fshader-stage=${stage} -o "${OUTPUT_DIR}/${spv_name}" "${source}"
                    RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "glslc failed on ${source}")
    endif()
    math(EXPR count "${count} + 1")
endforeach()
message(STATUS "kin_spirv: ${count} shaders in ${OUTPUT_DIR}")
