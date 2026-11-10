include_guard(GLOBAL)

option(KIN_BUILD_ON_DEMAND "Build tools, samples and tests only when their targets are requested" OFF)

# Exclude targets rather than directories so component install rules remain
# available to explicitly requested <game>_package targets.
function(kin_exclude_directory_targets directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    if(targets)
        set_target_properties(${targets} PROPERTIES
            EXCLUDE_FROM_ALL TRUE EXCLUDE_FROM_DEFAULT_BUILD TRUE)
    endif()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        kin_exclude_directory_targets("${child}")
    endforeach()
endfunction()

# Keep targets and CTest registration available without adding every executable
# to the default build. Dependencies of an explicitly requested target still build.
macro(kin_add_optional_subdirectory directory)
    add_subdirectory("${directory}")
    if(KIN_BUILD_ON_DEMAND)
        kin_exclude_directory_targets("${CMAKE_CURRENT_SOURCE_DIR}/${directory}")
    endif()
endmacro()

# Keep developer build settings separate from the shipping/release contract.
option(KIN_ENABLE_PCH "Precompile stable headers for the engine and kin_game targets" OFF)
option(KIN_ENABLE_WIDGET_PCH "Use a private PCH for the six UI2 widget implementation files (requires KIN_ENABLE_PCH)" OFF)
option(KIN_DEV_FAST_SCRIPTING "Use -O1 for scene/entity Lua bindings in Release development builds (GCC/Clang)" OFF)
set(KIN_COMPILER_CACHE "off" CACHE STRING "Compiler cache: off, auto, ccache or sccache")
set_property(CACHE KIN_COMPILER_CACHE PROPERTY STRINGS off auto ccache sccache)
if(NOT KIN_COMPILER_CACHE MATCHES "^(off|auto|ccache|sccache)$")
    message(FATAL_ERROR "KIN_COMPILER_CACHE must be off, auto, ccache or sccache")
endif()
if(NOT KIN_COMPILER_CACHE STREQUAL "off")
    if(KIN_COMPILER_CACHE STREQUAL "auto")
        set(_kin_cache_names ccache sccache)
    else()
        set(_kin_cache_names "${KIN_COMPILER_CACHE}")
    endif()
    find_program(_kin_cache_program NAMES ${_kin_cache_names} NO_CACHE)
    if(_kin_cache_program)
        set_property(GLOBAL PROPERTY KIN_COMPILER_CACHE_PROGRAM "${_kin_cache_program}")
        foreach(language C CXX)
            # Respect explicit launchers, including the launchers CI sets in
            # the environment. Do not persist auto-detection in the cache:
            # selecting "off" on the next configure must undo it.
            if(NOT DEFINED CMAKE_${language}_COMPILER_LAUNCHER AND
               NOT DEFINED ENV{CMAKE_${language}_COMPILER_LAUNCHER})
                set(CMAKE_${language}_COMPILER_LAUNCHER "${_kin_cache_program}")
            endif()
        endforeach()
    elseif(NOT KIN_COMPILER_CACHE STREQUAL "auto")
        message(FATAL_ERROR "KIN_COMPILER_CACHE=${KIN_COMPILER_CACHE}, but the program was not found")
    endif()
endif()

# Applied after the target's normal optimization settings, and also used by its
# PCH. Keep the override private: games, the engine and Lua keep their settings.
function(kin_configure_scripting_build target)
    if(NOT KIN_DEV_FAST_SCRIPTING)
        return()
    endif()
    if(KIN_SHIPPING)
        message(FATAL_ERROR "KIN_DEV_FAST_SCRIPTING is for development; disable it for KIN_SHIPPING builds")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        message(FATAL_ERROR "KIN_DEV_FAST_SCRIPTING requires GCC or Clang")
    endif()
    target_compile_options(${target} PRIVATE "$<$<CONFIG:Release,RelWithDebInfo>:-O1>")
endfunction()

# Games defined in a parent project do not inherit directory-scoped launchers
# from add_subdirectory(kin). Apply the selected cache without replacing theirs.
function(kin_configure_game_build target)
    get_property(cache_program GLOBAL PROPERTY KIN_COMPILER_CACHE_PROGRAM)
    if(cache_program)
        foreach(language C CXX)
            get_property(has_launcher TARGET ${target} PROPERTY ${language}_COMPILER_LAUNCHER SET)
            if(NOT has_launcher)
                set_property(TARGET ${target} PROPERTY ${language}_COMPILER_LAUNCHER "${cache_program}")
            endif()
        endforeach()
    endif()
    kin_precompile_target(${target} GAME)
endfunction()

# Target-private PCHs preserve each target's defines and compiler settings.
# Engine targets exclude Kin headers, so API edits do not invalidate their PCH.
# Game targets precompile the engine's runtime API, which is stable while a
# game's own sources change. No game-owned headers are included.
# Small adapter libraries and third-party targets do not need their own PCH.
function(kin_precompile_target target)
    if(NOT KIN_ENABLE_PCH)
        return()
    endif()
    cmake_parse_arguments(PCH "ECS;SCRIPTING;GAME" "" "" ${ARGN})
    get_target_property(existing ${target} PRECOMPILE_HEADERS)
    get_target_property(reuse ${target} PRECOMPILE_HEADERS_REUSE_FROM)
    if(existing OR reuse)
        return() # A game may provide its own PCH before calling kin_game.
    endif()
    set(headers algorithm array cmath cstdint functional memory optional span
        string string_view unordered_map utility vector filesystem)
    if(PCH_ECS)
        list(APPEND headers flecs.h)
    endif()
    if(PCH_SCRIPTING)
        list(APPEND headers sol/sol.hpp)
    endif()
    if(PCH_GAME)
        list(APPEND headers kin/runtime/scene_app.hpp)
    endif()
    foreach(header IN LISTS headers)
        target_precompile_headers(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:<${header}$<ANGLE-R>>")
    endforeach()
    get_target_property(sources ${target} SOURCES)
    foreach(source IN LISTS sources)
        foreach(property COMPILE_OPTIONS COMPILE_DEFINITIONS COMPILE_FLAGS INCLUDE_DIRECTORIES)
            get_source_file_property(value "${source}" ${property})
            if(value)
                set_source_files_properties("${source}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
                break()
            endif()
        endforeach()
    endforeach()
    # Also lets ccache's preprocessing mode recognize GCC's PCH usage.
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fpch-preprocess>")
endfunction()
