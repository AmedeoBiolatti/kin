# Shaders compiled at build time, and small files built into a binary.
#
#   kin_compile_glsl(<out_var> OUTPUT_DIR <dir> SOURCES <a.frag.glsl> ...)
#     Compiles each GLSL source to <dir>/<name>.spv with glslc, the stage taken
#     from the name (.vert, .frag or .comp before .glsl), and sets <out_var> to
#     the .spv files. With KIN_SPIRV_DIR it copies <name>.spv from there
#     instead (made by cmake/kin_spirv.cmake where glslc runs), for build
#     machines without glslc. With neither, it skips the shaders and <out_var>
#     lists only those it has.
#
#   kin_compile_surface_shader(<out_var> OUTPUT_DIR <dir> SOURCE <name>.surface.glsl [CUTOUTS])
#     A 3D surface shader (Material3D::surface; docs/rendering_3d.md, Surface
#     shaders): its eight variants, each kin's mesh fragment shader with the
#     source's kin_surface() between its halves, compiled with that variant's
#     defines, as <dir>/<name>.<variant>.spv. <out_var> lists them in
#     SurfaceVariant3D's order, as Renderer3D::create_surface_shader takes them
#     (the merged draws' four after the eight); with CUTOUTS (a surface
#     defining kin_surface_alpha) the cut-out passes' five after those.
#     Needs glslc (no KIN_SPIRV_DIR form yet).
#
#   kin_planar_surfaces(<target>)
#     kin's planar surfaces (mirrors and portals: engine/shaders/planar_reflection.surface.glsl and
#     planar_window.surface.glsl) compiled as kin_compile_surface_shader compiles a surface and built into <target>
#     (kin_embed_files, NAME planar_surfaces): create_planar_surfaces(r3d, planar_surfaces) makes them
#     (kin/scene/planar_views3d.hpp). About 2 MB, which is why the engine does not carry them. Needs glslc; sets
#     KIN_PLANAR_SURFACES on <target> when it built them.
#
#   kin_cook_material_graph(<out_var> SOURCE <graph> [OUTPUT <file>])
#     A material graph (kin.material-graph/1) cooked at build time by the
#     kin_material_graph tool (glslc): its layout and every variant's SPIR-V in
#     one file, by default <binary dir>/<graph's name>.spv. A game ships it
#     beside the graph in its content as <graph>.spv, where
#     Assets3D::load_material_graph looks.
#
#   kin_cook_material_graphs(<out_var> CONTENT <dir> [MISSING <var>])
#     Every material graph in a content folder (*.kingraph, in its subfolders
#     too) cooked as kin_cook_material_graph cooks one, each beside its graph
#     as <graph>.spv: where Assets3D::load_material_graph looks, so whatever
#     ships the folder ships them (its content pack, the folder installed, an
#     Android or web package). <out_var>: the cooked files. Where it cannot
#     cook (no kin_material_graph tool or glslc, or a cross-compile: Android's,
#     whose tools don't run here) it cooks none, and MISSING lists the graphs
#     with no cooked file as new as they are. kin_game cooks its content's.
#
#   kin_embed_files(<target> NAME <function> [NAMESPACE <ns>] FILES <file> ...)
#     Builds the files into <target>, readable by file name through
#       #include "<function>.hpp"
#       std::span<const unsigned char> <ns>::<function>(std::string_view file_name);
#       std::span<const std::string_view> <ns>::<function>_files();
#     The span is empty for a name not built in. For files of a few hundred KB
#     at most (shaders, small tables); a game's content goes in its pack.

find_program(KIN_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
set(KIN_SPIRV_DIR "" CACHE PATH "Already-compiled .spv files to use instead of glslc (cmake/kin_spirv.cmake makes them)")

# Shaders may #include the engine's (kin_material.inc.glsl: custom 3D material
# shaders): glslc finds them here, and a rebuild follows their change.
set(KIN_SHADER_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../engine/shaders" CACHE INTERNAL "")

function(kin_compile_glsl out_var)
    cmake_parse_arguments(ARG "" "OUTPUT_DIR" "SOURCES" ${ARGN})
    set(depfile_tool "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/kin_shader_depfile.cmake")
    set(outputs)
    if(KIN_SPIRV_DIR)
        file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")
        foreach(source IN LISTS ARG_SOURCES)
            get_filename_component(name "${source}" NAME)
            string(REGEX REPLACE "\\.glsl$" ".spv" spv_name "${name}")
            set(prebuilt "${KIN_SPIRV_DIR}/${spv_name}")
            if(NOT EXISTS "${prebuilt}")
                message(WARNING "kin_compile_glsl: ${spv_name} is not in KIN_SPIRV_DIR (${KIN_SPIRV_DIR})")
                continue()
            endif()
            set(output "${ARG_OUTPUT_DIR}/${spv_name}")
            add_custom_command(
                OUTPUT "${output}"
                COMMAND "${CMAKE_COMMAND}" -E copy "${prebuilt}" "${output}"
                DEPENDS "${prebuilt}"
                COMMENT "SPIR-V: ${spv_name} from KIN_SPIRV_DIR"
                VERBATIM)
            list(APPEND outputs "${output}")
        endforeach()
    elseif(KIN_GLSLC)
        file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")
        foreach(source IN LISTS ARG_SOURCES)
            get_filename_component(source "${source}" ABSOLUTE)
            get_filename_component(name "${source}" NAME)
            if(name MATCHES "\\.vert\\.glsl$")
                set(stage vertex)
            elseif(name MATCHES "\\.frag\\.glsl$")
                set(stage fragment)
            elseif(name MATCHES "\\.comp\\.glsl$")
                set(stage compute)
            else()
                message(FATAL_ERROR "kin_compile_glsl: ${name} is not .vert.glsl, .frag.glsl or .comp.glsl")
            endif()
            string(REGEX REPLACE "\\.glsl$" ".spv" spv_name "${name}")
            set(output "${ARG_OUTPUT_DIR}/${spv_name}")
            set(depfile "${output}.d")
            add_custom_command(
                OUTPUT "${output}"
                COMMAND "${KIN_GLSLC}" -fshader-stage=${stage} -I "${KIN_SHADER_INCLUDE_DIR}"
                    -MD -MF "${depfile}" -MT "${output}" -o "${output}" "${source}"
                COMMAND "${CMAKE_COMMAND}" "-DDEPFILE=${depfile}" "-DOUTPUT=${output}"
                    -P "${depfile_tool}"
                DEPENDS "${source}" "${depfile_tool}"
                DEPFILE "${depfile}"
                COMMENT "glslc: ${name} -> ${spv_name}"
                VERBATIM)
            list(APPEND outputs "${output}")
        endforeach()
    endif()
    set(${out_var} "${outputs}" PARENT_SCOPE)
endfunction()

# The variants (SurfaceVariant3D) and their defines, as surface_variant_defines (render3d/environment.cpp) gives them.
set(_KIN_SURFACE_VARIANTS plain fog lights lights_fog velocity velocity_fog velocity_lights velocity_lights_fog
    material material_fog material_lights material_lights_fog)
set(_KIN_SURFACE_DEFINES_plain "")
set(_KIN_SURFACE_DEFINES_fog KIN_FOG)
set(_KIN_SURFACE_DEFINES_lights KIN_LIGHTS KIN_GI KIN_LIGHT_GRID)
set(_KIN_SURFACE_DEFINES_lights_fog KIN_LIGHTS KIN_GI KIN_LIGHT_GRID KIN_FOG)
set(_KIN_SURFACE_DEFINES_velocity KIN_VELOCITY)
set(_KIN_SURFACE_DEFINES_velocity_fog KIN_VELOCITY KIN_FOG)
set(_KIN_SURFACE_DEFINES_velocity_lights KIN_VELOCITY KIN_LIGHTS KIN_GI KIN_LIGHT_GRID)
set(_KIN_SURFACE_DEFINES_velocity_lights_fog KIN_VELOCITY KIN_LIGHTS KIN_GI KIN_LIGHT_GRID KIN_FOG)
set(_KIN_SURFACE_DEFINES_material KIN_DRAW_MATERIALS)
set(_KIN_SURFACE_DEFINES_material_fog KIN_DRAW_MATERIALS KIN_FOG)
set(_KIN_SURFACE_DEFINES_material_lights KIN_DRAW_MATERIALS KIN_LIGHTS KIN_GI KIN_LIGHT_GRID)
set(_KIN_SURFACE_DEFINES_material_lights_fog KIN_DRAW_MATERIALS KIN_LIGHTS KIN_GI KIN_LIGHT_GRID KIN_FOG)
# The cut-outs' (CUTOUTS: a surface defining kin_surface_alpha), each its pass's body (kin_<pass>.inc.glsl).
set(_KIN_SURFACE_MASKS shadow_mask pick_mask prepass_mask prepass_velocity_mask visibility_mask)

function(kin_compile_surface_shader out_var)
    cmake_parse_arguments(ARG "CUTOUTS" "OUTPUT_DIR;SOURCE" "" ${ARGN})
    set(depfile_tool "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/kin_shader_depfile.cmake")
    if(NOT ARG_OUTPUT_DIR OR NOT ARG_SOURCE)
        message(FATAL_ERROR "kin_compile_surface_shader: OUTPUT_DIR and SOURCE are required")
    endif()
    if(NOT KIN_GLSLC)
        message(WARNING "kin_compile_surface_shader: no glslc for ${ARG_SOURCE}")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    get_filename_component(source "${ARG_SOURCE}" ABSOLUTE)
    get_filename_component(name "${source}" NAME)
    get_filename_component(folder "${source}" DIRECTORY)
    string(REGEX REPLACE "(\\.surface)?\\.glsl$" "" stem "${name}")
    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")
    # The same shader as surface_shader_source() writes, the surface included rather than pasted in.
    set(wrapper "${ARG_OUTPUT_DIR}/${stem}.surface.frag.glsl")
    file(CONFIGURE OUTPUT "${wrapper}" CONTENT
"#version 450 core
#define KIN_SURFACE kin_surface
#include \"kin_mesh_surface.inc.glsl\"
#include \"${name}\"
#include \"kin_mesh_main.inc.glsl\"
")
    set(outputs)
    foreach(variant IN LISTS _KIN_SURFACE_VARIANTS)
        set(defines)
        foreach(define IN LISTS _KIN_SURFACE_DEFINES_${variant})
            list(APPEND defines "-D${define}")
        endforeach()
        set(output "${ARG_OUTPUT_DIR}/${stem}.${variant}.spv")
        set(depfile "${output}.d")
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${KIN_GLSLC}" -fshader-stage=fragment -I "${KIN_SHADER_INCLUDE_DIR}" -I "${folder}" ${defines}
                    -MD -MF "${depfile}" -MT "${output}" -o "${output}"
                    "${wrapper}"
            COMMAND "${CMAKE_COMMAND}" "-DDEPFILE=${depfile}" "-DOUTPUT=${output}" -P "${depfile_tool}"
            DEPENDS "${source}" "${wrapper}" "${depfile_tool}"
            DEPFILE "${depfile}"
            COMMENT "glslc: ${name} (${variant}) -> ${stem}.${variant}.spv"
            VERBATIM)
        list(APPEND outputs "${output}")
    endforeach()
    if(ARG_CUTOUTS)
        foreach(mask IN LISTS _KIN_SURFACE_MASKS)
            set(mask_wrapper "${ARG_OUTPUT_DIR}/${stem}.${mask}.frag.glsl")
            file(CONFIGURE OUTPUT "${mask_wrapper}" CONTENT
"#version 450 core
#define KIN_SURFACE_MASK
#define KIN_SURFACE_ALPHA kin_surface_alpha
#include \"kin_${mask}.inc.glsl\"
#include \"${name}\"
")
            set(output "${ARG_OUTPUT_DIR}/${stem}.${mask}.spv")
            set(depfile "${output}.d")
            add_custom_command(
                OUTPUT "${output}"
                COMMAND "${KIN_GLSLC}" -fshader-stage=fragment -I "${KIN_SHADER_INCLUDE_DIR}" -I "${folder}"
                        -MD -MF "${depfile}" -MT "${output}" -o "${output}" "${mask_wrapper}"
                COMMAND "${CMAKE_COMMAND}" "-DDEPFILE=${depfile}" "-DOUTPUT=${output}" -P "${depfile_tool}"
                DEPENDS "${source}" "${mask_wrapper}" "${depfile_tool}"
                DEPFILE "${depfile}"
                COMMENT "glslc: ${name} (${mask}) -> ${stem}.${mask}.spv"
                VERBATIM)
            list(APPEND outputs "${output}")
        endforeach()
    endif()
    set(${out_var} "${outputs}" PARENT_SCOPE)
endfunction()

function(kin_cook_material_graph out_var)
    cmake_parse_arguments(ARG "" "SOURCE;OUTPUT" "" ${ARGN})
    if(NOT ARG_SOURCE)
        message(FATAL_ERROR "kin_cook_material_graph: SOURCE is required")
    endif()
    if(NOT TARGET kin_material_graph OR NOT KIN_GLSLC)
        message(WARNING "kin_cook_material_graph: no kin_material_graph tool or glslc for ${ARG_SOURCE}")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    get_filename_component(source "${ARG_SOURCE}" ABSOLUTE)
    get_filename_component(name "${source}" NAME)
    set(output "${ARG_OUTPUT}")
    if(NOT output)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/${name}.spv")
    endif()
    # The tool writes its shaders to temporary files and has glslc read the engine's
    # includes from KIN_SHADER_INCLUDE_DIR, so there is no depfile: it follows them all.
    file(GLOB includes CONFIGURE_DEPENDS "${KIN_SHADER_INCLUDE_DIR}/*.inc.glsl")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E env "KIN_GLSLC=${KIN_GLSLC}" $<TARGET_FILE:kin_material_graph> cook "${source}"
                "${output}"
        DEPENDS "${source}" kin_material_graph ${includes}
        COMMENT "kin_material_graph: ${name} -> ${name}.spv"
        VERBATIM)
    set(${out_var} "${output}" PARENT_SCOPE)
endfunction()

function(kin_cook_material_graphs out_var)
    cmake_parse_arguments(ARG "" "CONTENT;MISSING" "" ${ARGN})
    if(NOT ARG_CONTENT)
        message(FATAL_ERROR "kin_cook_material_graphs: CONTENT is required")
    endif()
    get_filename_component(content "${ARG_CONTENT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    file(GLOB_RECURSE graphs CONFIGURE_DEPENDS "${content}/*.kingraph")
    set(cooked "")
    set(missing "")
    if(graphs AND TARGET kin_material_graph AND KIN_GLSLC AND NOT CMAKE_CROSSCOMPILING)
        foreach(graph IN LISTS graphs)
            kin_cook_material_graph(spv SOURCE "${graph}" OUTPUT "${graph}.spv")
            list(APPEND cooked "${spv}")
        endforeach()
    else()
        foreach(graph IN LISTS graphs)
            if(NOT EXISTS "${graph}.spv" OR "${graph}" IS_NEWER_THAN "${graph}.spv")
                list(APPEND missing "${graph}")
            endif()
        endforeach()
    endif()
    set(${out_var} "${cooked}" PARENT_SCOPE)
    if(ARG_MISSING)
        set(${ARG_MISSING} "${missing}" PARENT_SCOPE)
    endif()
endfunction()

function(kin_embed_files target)
    cmake_parse_arguments(ARG "" "NAME;NAMESPACE" "FILES" ${ARGN})
    set(generator "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/kin_embed_generate.cmake")
    if(NOT ARG_NAME)
        message(FATAL_ERROR "kin_embed_files: NAME is required")
    endif()
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/kin_embed/${target}")
    set(header "${dir}/${ARG_NAME}.hpp")
    set(source "${dir}/${ARG_NAME}.cpp")
    set(open_ns "")
    set(close_ns "")
    if(ARG_NAMESPACE)
        set(open_ns "namespace ${ARG_NAMESPACE} {\n\n")
        set(close_ns "\n} // namespace ${ARG_NAMESPACE}\n")
    endif()
    file(CONFIGURE OUTPUT "${header}" CONTENT
"// Generated by kin_embed_files; do not edit.
#pragma once

#include <span>
#include <string_view>

${open_ns}// A built-in file's bytes by name; empty if there is no such file.
std::span<const unsigned char> ${ARG_NAME}(std::string_view file_name);
// The names of the built-in files, sorted.
std::span<const std::string_view> ${ARG_NAME}_files();
${close_ns}")
    # The files' paths go in a list file, not on the command line: many long absolute paths pass the 8191 characters
    # Windows' cmd.exe takes for a command (the built-in shader list did). Written only when it changes.
    set(list_file "${dir}/${ARG_NAME}.files")
    string(JOIN "\n" files_text ${ARG_FILES})
    file(CONFIGURE OUTPUT "${list_file}" CONTENT "${files_text}\n" @ONLY)
    add_custom_command(
        OUTPUT "${source}"
        COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${source}" "-DHEADER=${ARG_NAME}.hpp" "-DNAME=${ARG_NAME}"
                "-DNAMESPACE=${ARG_NAMESPACE}" "-DFILES_LIST=${list_file}" -P "${generator}"
        DEPENDS ${ARG_FILES} "${generator}" "${list_file}"
        COMMENT "Embedding ${ARG_NAME} in ${target}"
        VERBATIM)
    target_sources(${target} PRIVATE "${source}")
    target_include_directories(${target} PRIVATE "${dir}")
endfunction()

function(kin_planar_surfaces target)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/kin_planar_surfaces/${target}")
    kin_compile_surface_shader(reflection OUTPUT_DIR "${dir}" SOURCE "${KIN_SHADER_INCLUDE_DIR}/planar_reflection.surface.glsl")
    kin_compile_surface_shader(window OUTPUT_DIR "${dir}" SOURCE "${KIN_SHADER_INCLUDE_DIR}/planar_window.surface.glsl")
    if(reflection AND window)
        kin_embed_files(${target} NAME planar_surfaces FILES ${reflection} ${window})
        target_compile_definitions(${target} PRIVATE KIN_PLANAR_SURFACES)
    endif()
endfunction()
