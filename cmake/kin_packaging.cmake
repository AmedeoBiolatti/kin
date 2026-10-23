include_guard(GLOBAL)
include(FetchContent)

function(_kin_install_header_notice dependency relative marker name component)
    FetchContent_GetProperties(${dependency} SOURCE_DIR source)
    if(NOT source)
        return()
    endif()
    file(READ "${source}/${relative}" header)
    string(FIND "${header}" "${marker}" start REVERSE)
    if(start LESS 0)
        message(FATAL_ERROR "License marker missing from ${dependency}/${relative}")
    endif()
    string(SUBSTRING "${header}" ${start} -1 notice)
    string(FIND "${notice}" "*/" end)
    if(end LESS 0)
        message(FATAL_ERROR "License comment not terminated in ${dependency}/${relative}")
    endif()
    string(SUBSTRING "${notice}" 0 ${end} notice)
    string(STRIP "${notice}" notice)
    set(path "${CMAKE_CURRENT_BINARY_DIR}/kin-package-notices/${dependency}/${name}")
    file(CONFIGURE OUTPUT "${path}" CONTENT "@notice@\n" @ONLY)
    install(FILES "${path}" DESTINATION "licenses/${dependency}"
        COMPONENT ${component} EXCLUDE_FROM_ALL)
endfunction()

function(_kin_install_package_licenses component)
    set(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    install(FILES "${root}/LICENSE" DESTINATION licenses/kin COMPONENT ${component} EXCLUDE_FROM_ALL)
    install(FILES "${root}/docs/third_party_notices.md"
        DESTINATION licenses COMPONENT ${component} EXCLUDE_FROM_ALL)
    foreach(entry IN LISTS KIN_THIRD_PARTY_LICENSES)
        string(REPLACE "|" ";" entry "${entry}")
        list(GET entry 0 name)
        list(GET entry 1 path)
        install(FILES "${path}" DESTINATION licenses RENAME "${name}" COMPONENT ${component} EXCLUDE_FROM_ALL)
    endforeach()
    # Include the notices from the exact dependency sources used by this build.
    # Some optional codecs are not linked, but retaining their notices is harmless.
    foreach(entry
        "SDL3|LICENSE.txt" "flecs|LICENSE" "box2d|LICENSE"
        "lua|doc/readme.html" "sol2|LICENSE.txt" "earcut|LICENSE"
        "SDL3_image|LICENSE.txt" "SDL3_image|external/libpng/LICENSE"
        "SDL3_image|external/zlib/LICENSE"
        "SDL3_ttf|LICENSE.txt" "SDL3_ttf|external/freetype/docs/FTL.TXT"
        "SDL3_ttf|external/harfbuzz/COPYING" "SDL3_ttf|external/plutosvg/LICENSE"
        "SDL3_ttf|external/plutovg/LICENSE"
        "SDL3_ttf|external/plutovg/source/FTL.TXT")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 dependency)
        list(GET parts 1 relative)
        unset(dependency_source)
        FetchContent_GetProperties(${dependency} SOURCE_DIR dependency_source)
        if(NOT dependency_source)
            continue()
        endif()
        get_filename_component(directory "${relative}" DIRECTORY)
        install(FILES "${dependency_source}/${relative}"
            DESTINATION "licenses/${dependency}/${directory}"
            COMPONENT ${component} EXCLUDE_FROM_ALL)
    endforeach()
    if(TARGET SDL3::SDL3-static AND NOT KIN_ENABLE_SDL_DYNAMIC_API)
        FetchContent_GetProperties(SDL3 BINARY_DIR sdl_binary)
        install(FILES "${sdl_binary}/../sdl3-direct-src/KIN-SDL-CHANGES.txt"
            DESTINATION licenses/SDL3 COMPONENT ${component} EXCLUDE_FROM_ALL)
    endif()
    # Keep the complete license comments, without shipping the implementation
    # headers merely to include their notices. Fail if upstream moves them.
    if(TARGET SDL3::SDL3-static AND KIN_ENABLE_IMAGE_IO)
        _kin_install_header_notice(SDL3_image src/stb_image.h
            "This software is available under 2 licenses" stb-LICENSE.txt ${component})
        _kin_install_header_notice(SDL3 src/video/stb_image.h
            "This software is available under 2 licenses" stb-LICENSE.txt ${component})
        _kin_install_header_notice(SDL3 src/video/miniz.h
            "This is free and unencumbered software released into the public domain."
            miniz-LICENSE.txt ${component})
    endif()
endfunction()

# Creates <target>_package and an isolated install component, kin-<target>.
# Call after kin_configure_game_assets if the game has external resources.
function(kin_package_game target)
    cmake_parse_arguments(PACKAGE "" "VERSION" "RUNTIME_TARGETS;LICENSE_FILES;HOST_LIBRARY_REGEXES" ${ARGN})
    if(PACKAGE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown kin_package_game arguments: ${PACKAGE_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Windows|Linux)$")
        message(STATUS "${target}: desktop packaging currently supports Windows and Linux")
        return()
    endif()
    get_target_property(type ${target} TYPE)
    if(NOT type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR "kin_package_game requires an executable target")
    endif()
    # Also handle games created in a parent project, outside Kin's directory
    # scope where the CMAKE_INTERPROCEDURAL_OPTIMIZATION defaults are set.
    foreach(binary IN ITEMS ${target} ${PACKAGE_RUNTIME_TARGETS})
        kin_apply_release_policy(${binary})
    endforeach()
    if(NOT PACKAGE_VERSION)
        set(PACKAGE_VERSION "${PROJECT_VERSION}")
    endif()
    if(NOT PACKAGE_VERSION)
        set(PACKAGE_VERSION "0.0.0")
    endif()
    if(NOT PACKAGE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+[A-Za-z0-9.+-]*$")
        message(FATAL_ERROR "Invalid package VERSION: ${PACKAGE_VERSION}")
    endif()
    get_target_property(component ${target} KIN_PACKAGE_INSTALL_COMPONENT)
    if(NOT component)
        set(component "kin-${target}")
    endif()
    set(runtime_set "${component}-runtime")
    set(configurations Release RelWithDebInfo MinSizeRel)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "$ORIGIN/lib")
        # DT_RPATH also finds transitive dependencies of bundled third-party
        # libraries that have no loader path of their own (DT_RUNPATH does not).
        target_link_options(${target} PRIVATE "LINKER:--disable-new-dtags")
        foreach(library IN LISTS PACKAGE_RUNTIME_TARGETS)
            set_property(TARGET ${library} APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
        endforeach()
        # The system C/C++ runtime and graphics/audio drivers remain host
        # prerequisites. Do not copy glibc or a loader from the build machine.
        set(exclude "^ld-linux.*" "^lib(c|m|dl|pthread|rt|resolv|util)\\.so\\..*"
            "^libstdc\\+\\+\\.so\\..*" "^libgcc_s\\.so\\..*")
        list(APPEND exclude ${PACKAGE_HOST_LIBRARY_REGEXES})
        set(system_paths)
    else()
        set(exclude "[Aa][Pp][Ii]-[Mm][Ss]-.*" "[Ee][Xx][Tt]-[Mm][Ss]-.*")
        set(system_paths ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss]([Tt][Ee][Mm]32|[Ww][Oo][Ww]64)[/\\\\].*")
        if(MSVC AND NOT KIN_SHIPPING)
            set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
            include(InstallRequiredSystemLibraries)
            if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
                message(FATAL_ERROR "MSVC redistributable runtime files were not found")
            endif()
            install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
                DESTINATION . CONFIGURATIONS ${configurations}
                COMPONENT ${component} EXCLUDE_FROM_ALL)
        endif()
    endif()
    install(TARGETS ${target} ${PACKAGE_RUNTIME_TARGETS}
        RUNTIME_DEPENDENCY_SET ${runtime_set}
        RUNTIME DESTINATION . CONFIGURATIONS ${configurations}
            COMPONENT ${component} EXCLUDE_FROM_ALL
        LIBRARY DESTINATION lib CONFIGURATIONS ${configurations}
            COMPONENT ${component} EXCLUDE_FROM_ALL)
    set(runtime_directories "$<TARGET_FILE_DIR:${target}>")
    foreach(library IN LISTS PACKAGE_RUNTIME_TARGETS)
        list(APPEND runtime_directories "$<TARGET_FILE_DIR:${library}>")
    endforeach()
    install(RUNTIME_DEPENDENCY_SET ${runtime_set}
        DIRECTORIES ${runtime_directories}
        PRE_EXCLUDE_REGEXES ${exclude}
        POST_EXCLUDE_REGEXES ${system_paths}
        RUNTIME DESTINATION . CONFIGURATIONS ${configurations}
            COMPONENT ${component} EXCLUDE_FROM_ALL
        LIBRARY DESTINATION lib CONFIGURATIONS ${configurations}
            COMPONENT ${component} EXCLUDE_FROM_ALL)

    get_target_property(assets ${target} KIN_GAME_ASSETS_DIR)
    get_target_property(content_pack ${target} KIN_GAME_CONTENT_PACK)
    get_target_property(content_directory ${target} KIN_GAME_CONTENT_DIRECTORY)
    get_target_property(content_component ${target} KIN_PACKAGE_INSTALL_COMPONENT)
    if(assets)
        get_target_property(asset_name ${target} KIN_GAME_ASSETS_NAME)
        get_target_property(cook_output ${target} KIN_GAME_COOK_OUTPUT)
        set(install_assets "${assets}")
        if(cook_output)
            set(install_assets "${cook_output}")
        endif()
        if(NOT content_component)
            install(DIRECTORY "${install_assets}/" DESTINATION "assets/${asset_name}"
                COMPONENT ${component} EXCLUDE_FROM_ALL)
        endif()
    endif()
    _kin_install_package_licenses(${component})
    if(PACKAGE_LICENSE_FILES)
        install(FILES ${PACKAGE_LICENSE_FILES} DESTINATION licenses/game
            COMPONENT ${component} EXCLUDE_FROM_ALL)
    endif()

    get_property(component_manifest GLOBAL PROPERTY KIN_COMPONENT_MANIFEST)
    set(content_settings "")
    if(content_pack)
        string(APPEND content_settings "set(KIN_PACKAGE_CONTENT_PACK [==[${content_pack}]==])\n")
        string(APPEND content_settings "set(KIN_PACKAGE_PACK_TOOL [==[$<TARGET_FILE:kin_pack>]==])\n")
    elseif(content_directory)
        string(APPEND content_settings "set(KIN_PACKAGE_CONTENT_DIRECTORY [==[${content_directory}]==])\n")
    endif()
    set(asset_settings "${content_settings}set(KIN_PACKAGE_COMPONENT_MANIFEST [==[${component_manifest}]==])\n")
    get_target_property(asset_manifest ${target} KIN_GAME_ASSET_MANIFEST)
    if(asset_manifest)
        get_target_property(asset_python ${target} KIN_GAME_ASSET_REPORT_PYTHON)
        get_target_property(asset_tool ${target} KIN_GAME_ASSET_REPORT_TOOL)
        string(APPEND asset_settings "set(KIN_PACKAGE_ASSET_MANIFEST [==[${asset_manifest}]==])\n")
        string(APPEND asset_settings "set(KIN_PACKAGE_ASSET_NAME [==[${asset_name}]==])\n")
        string(APPEND asset_settings "set(KIN_PACKAGE_ASSET_PYTHON [==[${asset_python}]==])\n")
        string(APPEND asset_settings "set(KIN_PACKAGE_ASSET_TOOL [==[${asset_tool}]==])\n")
    endif()

    if(cook_output)
        string(APPEND asset_settings "set(KIN_PACKAGE_COOK_SOURCE [==[${assets}]==])\n")
        foreach(property OUTPUT CACHE REPORT TOOL TARGET)
            get_target_property(value ${target} KIN_GAME_COOK_${property})
            string(APPEND asset_settings "set(KIN_PACKAGE_COOK_${property} [==[${value}]==])\n")
        endforeach()
    endif()

    set(architecture "${CMAKE_SYSTEM_PROCESSOR}")
    if(CMAKE_VS_PLATFORM_NAME)
        set(architecture "${CMAKE_VS_PLATFORM_NAME}")
    endif()
    # Report settings from this configuration, alongside the actual byte counts.
    set(build_settings "set(KIN_PACKAGE_RENDERER ${KIN_RENDERER})\n")
    string(APPEND build_settings "set(KIN_PACKAGE_HOST_LIBRARY_REGEXES [==[${PACKAGE_HOST_LIBRARY_REGEXES}]==])\n")
    string(APPEND build_settings "set(KIN_PACKAGE_LINUX_VIDEO ${KIN_LINUX_VIDEO})\n")
    string(APPEND build_settings "set(KIN_PACKAGE_SDL_VIDEO_DRIVERS ${KIN_SDL_VIDEO_DRIVERS})\n")
    foreach(feature SCRIPTING PHYSICS AUDIO SCENE_SERVER DEBUG_OVERLAY RENDER_PROBE DETERMINISM_CHECK IMAGE_IO TTF RUNTIME_PROFILING RUNTIME_ASSETS SDL_DYNAMIC_API SDL_GAMEPAD SDL_DESKTOP_SERVICES)
        string(APPEND build_settings "set(KIN_SIZE_${feature} $<BOOL:${KIN_ENABLE_${feature}}>)\n")
    endforeach()
    string(APPEND build_settings "set(KIN_SIZE_PROFILING $<OR:$<BOOL:${KIN_ENABLE_PROFILING}>,$<AND:$<CONFIG:Release>,$<BOOL:${KIN_ENABLE_RELEASE_PROFILING}>>>)\n")
    string(APPEND build_settings "set(KIN_SIZE_LTO $<BOOL:${KIN_ENABLE_LTO}>)\n")
    string(APPEND build_settings "set(KIN_SIZE_DEAD_STRIPPING $<BOOL:${KIN_ENABLE_SIZE_OPTIMIZATIONS}>)\n")
    # PDBs belong in the separate symbols archive, never the player package.
    set(strip_package OFF)
    if(KIN_PACKAGE_STRIP AND NOT MSVC)
        if(NOT CMAKE_STRIP)
            message(FATAL_ERROR "KIN_PACKAGE_STRIP requires a strip tool for this toolchain")
        endif()
        set(strip_package ON)
    endif()
    set(source_revision "unknown")
    set(source_dirty "unknown")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse HEAD
            OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE result)
        if(result EQUAL 0)
            set(source_revision "${revision}")
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" status --porcelain --untracked-files=normal
                OUTPUT_VARIABLE changes ERROR_QUIET RESULT_VARIABLE result)
            if(NOT result EQUAL 0)
                set(source_dirty "unknown")
            elseif(changes)
                set(source_dirty "true")
            else()
                set(source_dirty "false")
            endif()
        endif()
    endif()
    set(symbol_settings "set(KIN_PACKAGE_DEBUG_SYMBOLS ${KIN_PACKAGE_DEBUG_SYMBOLS})\n")
    if(KIN_PACKAGE_DEBUG_SYMBOLS)
        if(NOT MSVC AND (NOT CMAKE_OBJCOPY OR NOT CMAKE_OBJDUMP OR NOT CMAKE_STRIP))
            message(FATAL_ERROR "Separate debug symbols require objcopy, objdump and strip from the target toolchain")
        endif()
        foreach(tool OBJCOPY OBJDUMP STRIP)
            string(APPEND symbol_settings "set(KIN_PACKAGE_${tool}_TOOL [==[${CMAKE_${tool}}]==])\n")
        endforeach()
        foreach(binary IN ITEMS ${target} ${PACKAGE_RUNTIME_TARGETS})
            string(APPEND symbol_settings "list(APPEND KIN_PACKAGE_OWN_BINARIES [==[$<TARGET_FILE_NAME:${binary}>]==])\n")
            if(MSVC)
                string(APPEND symbol_settings "list(APPEND KIN_PACKAGE_PDBS [==[$<TARGET_PDB_FILE:${binary}>]==])\n")
            endif()
        endforeach()
    endif()
    set(package_name "${target}-${PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${architecture}-$<CONFIG>")
    get_target_property(package_basename ${target} KIN_PACKAGE_BASENAME)
    if(package_basename)
        set(package_name "${package_basename}")
    endif()
    get_target_property(archive_extension ${target} KIN_PACKAGE_ARCHIVE_EXTENSION)
    if(NOT archive_extension)
        set(archive_extension zip)
    endif()
    set(package_script "${CMAKE_CURRENT_BINARY_DIR}/package-${target}-$<CONFIG>.cmake")
    file(GENERATE OUTPUT "${package_script}" CONTENT
"${build_settings}
${asset_settings}
${symbol_settings}
set(KIN_PACKAGE_ARCHIVE_EXTENSION [==[${archive_extension}]==])
set(KIN_PACKAGE_STATIC_RUNTIME ${KIN_SHIPPING})
set(KIN_PACKAGE_EXPERIMENTAL_3D $<BOOL:${KIN_EXPERIMENTAL_3D}>)
set(KIN_PACKAGE_VERSION [==[${PACKAGE_VERSION}]==])
set(KIN_PACKAGE_RELEASE_CONTRACT ${KIN_RELEASE_CONTRACT})
set(KIN_PACKAGE_CONTRACT_FILE [==[${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../release/desktop-v1.json]==])
set(KIN_PACKAGE_MSVC ${MSVC})
set(KIN_PACKAGE_PGO_MODE [==[${KIN_PGO_MODE}]==])
set(KIN_PACKAGE_PGO_MANIFEST_SHA256 [==[${KIN_PGO_MANIFEST_SHA256}]==])
set(KIN_PACKAGE_GIT [==[${GIT_EXECUTABLE}]==])
set(KIN_PACKAGE_SOURCE_ROOT [==[${CMAKE_SOURCE_DIR}]==])
set(KIN_PACKAGE_SOURCE_REVISION [==[${source_revision}]==])
set(KIN_PACKAGE_SOURCE_DIRTY [==[${source_dirty}]==])
set(KIN_PACKAGE_STRIP ${strip_package})
set(KIN_PACKAGE_TARGET [==[${target}]==])
set(KIN_PACKAGE_BUILD_EXECUTABLE [==[$<TARGET_FILE:${target}>]==])
set(KIN_PACKAGE_SYSTEM [==[${CMAKE_SYSTEM_NAME}]==])
set(KIN_PACKAGE_ARCHITECTURE [==[${architecture}]==])
set(KIN_PACKAGE_COMPILER [==[${CMAKE_CXX_COMPILER_ID}]==])
set(KIN_PACKAGE_COMPILER_VERSION [==[${CMAKE_CXX_COMPILER_VERSION}]==])
set(KIN_PACKAGE_NAME [==[${package_name}]==])
set(KIN_PACKAGE_BUILD_DIR [==[${CMAKE_BINARY_DIR}]==])
set(KIN_PACKAGE_COMPONENT [==[${component}]==])
set(KIN_PACKAGE_CONFIG [==[$<CONFIG>]==])
set(KIN_PACKAGE_EXECUTABLE [==[$<TARGET_FILE_NAME:${target}>]==])
include([==[${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package_game.cmake]==])
")
    add_custom_target(${target}_package
        COMMAND "${CMAKE_COMMAND}" -P "${package_script}"
        DEPENDS ${target} ${PACKAGE_RUNTIME_TARGETS}
        COMMENT "Packaging ${target} for ${CMAKE_SYSTEM_NAME}"
        VERBATIM USES_TERMINAL)
    set_target_properties(${target} PROPERTIES
        KIN_PACKAGE_SCRIPT "${package_script}"
        KIN_PACKAGE_NAME "${package_name}")
endfunction()
