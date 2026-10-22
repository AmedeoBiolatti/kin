include_guard(GLOBAL)

function(_kin_install_package_licenses component)
    set(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    install(FILES "${root}/LICENSE" DESTINATION licenses/kin COMPONENT ${component} EXCLUDE_FROM_ALL)
    install(FILES "${root}/docs/third_party_notices.md"
        DESTINATION licenses COMPONENT ${component} EXCLUDE_FROM_ALL)
    # Include the notices from the exact dependency sources used by this build.
    # Some optional codecs are not linked, but retaining their notices is harmless.
    foreach(entry
        "SDL3|LICENSE.txt" "flecs|LICENSE" "box2d|LICENSE"
        "lua|doc/readme.html" "sol2|LICENSE.txt" "earcut|LICENSE"
        "SDL3_image|LICENSE.txt" "SDL3_image|external/libpng/LICENSE"
        "SDL3_image|external/zlib/LICENSE" "SDL3_image|src/stb_image.h"
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
endfunction()

# Creates <target>_package and an isolated install component, kin-<target>.
# Call after kin_configure_game_assets if the game has external resources.
function(kin_package_game target)
    if(TARGET ${target}_package)
        return() # kin_game already owns this game
    endif()
    cmake_parse_arguments(PACKAGE "" "" "RUNTIME_TARGETS;LICENSE_FILES" ${ARGN})
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
    if(KIN_ENABLE_LTO)
        foreach(config RELEASE RELWITHDEBINFO MINSIZEREL)
            set_property(TARGET ${target} ${PACKAGE_RUNTIME_TARGETS}
                PROPERTY INTERPROCEDURAL_OPTIMIZATION_${config} ON)
        endforeach()
    endif()
    set(component "kin-${target}")
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
        set(system_paths)
    else()
        set(exclude "[Aa][Pp][Ii]-[Mm][Ss]-.*" "[Ee][Xx][Tt]-[Mm][Ss]-.*")
        set(system_paths ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss]([Tt][Ee][Mm]32|[Ww][Oo][Ww]64)[/\\\\].*")
        if(MSVC)
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
    install(RUNTIME_DEPENDENCY_SET ${runtime_set}
        PRE_EXCLUDE_REGEXES ${exclude}
        POST_EXCLUDE_REGEXES ${system_paths}
        RUNTIME DESTINATION . CONFIGURATIONS ${configurations}
            COMPONENT ${component} EXCLUDE_FROM_ALL
        LIBRARY DESTINATION lib CONFIGURATIONS ${configurations}
            COMPONENT ${component} EXCLUDE_FROM_ALL)

    get_target_property(assets ${target} KIN_GAME_ASSETS_DIR)
    if(assets)
        get_target_property(asset_name ${target} KIN_GAME_ASSETS_NAME)
        install(DIRECTORY "${assets}/" DESTINATION "assets/${asset_name}"
            COMPONENT ${component} EXCLUDE_FROM_ALL)
    endif()
    _kin_install_package_licenses(${component})
    if(PACKAGE_LICENSE_FILES)
        install(FILES ${PACKAGE_LICENSE_FILES} DESTINATION licenses/game
            COMPONENT ${component} EXCLUDE_FROM_ALL)
    endif()

    set(architecture "${CMAKE_SYSTEM_PROCESSOR}")
    if(CMAKE_VS_PLATFORM_NAME)
        set(architecture "${CMAKE_VS_PLATFORM_NAME}")
    endif()
    # Report settings from this configuration, alongside the actual byte counts.
    set(build_settings "")
    foreach(feature SCRIPTING PHYSICS AUDIO SCENE_SERVER DEBUG_OVERLAY RENDER_PROBE DETERMINISM_CHECK)
        string(APPEND build_settings "set(KIN_SIZE_${feature} $<BOOL:${KIN_ENABLE_${feature}}>)\n")
    endforeach()
    string(APPEND build_settings "set(KIN_SIZE_PROFILING $<OR:$<BOOL:${KIN_ENABLE_PROFILING}>,$<AND:$<CONFIG:Release>,$<BOOL:${KIN_ENABLE_RELEASE_PROFILING}>>>)\n")
    string(APPEND build_settings "set(KIN_SIZE_LTO $<BOOL:${KIN_ENABLE_LTO}>)\n")
    string(APPEND build_settings "set(KIN_SIZE_DEAD_STRIPPING $<BOOL:${KIN_ENABLE_SIZE_OPTIMIZATIONS}>)\n")
    # MSVC's PDBs are separate files and are not installed into the package.
    set(strip_package OFF)
    if(KIN_PACKAGE_STRIP AND NOT MSVC)
        if(NOT CMAKE_STRIP)
            message(FATAL_ERROR "KIN_PACKAGE_STRIP requires a strip tool for this toolchain")
        endif()
        set(strip_package ON)
    endif()
    set(package_name "${target}-${CMAKE_SYSTEM_NAME}-${architecture}-$<CONFIG>")
    set(package_script "${CMAKE_CURRENT_BINARY_DIR}/package-${target}-$<CONFIG>.cmake")
    file(GENERATE OUTPUT "${package_script}" CONTENT
"${build_settings}
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
