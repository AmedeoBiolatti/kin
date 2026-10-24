include_guard(GLOBAL)

# A browser bundle has no native runtime dependencies or desktop release ABI.
# Asset preloading is performed by the host Emscripten tool, never a Wasm kin_pack.
function(kin_package_web_game target)
    cmake_parse_arguments(WEB "" "CONTENT;NAME" "" ${ARGN})
    set_target_properties(${target} PROPERTIES SUFFIX ".html")
    target_link_options(${target} PRIVATE
        "--shell-file=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../web/shell.html" --emit-symbol-map)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../web/shell.html")
    if(KIN_WEB_PERSISTENCE)
        set(persistent "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../web/persistent.js")
        target_link_options(${target} PRIVATE "--pre-js=${persistent}" -lidbfs.js -sFORCE_FILESYSTEM=1)
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${persistent}")
    endif()
    if(WEB_CONTENT AND IS_DIRECTORY "${WEB_CONTENT}")
        target_link_options(${target} PRIVATE "SHELL:--preload-file \"${WEB_CONTENT}@/${WEB_NAME}\"")
    endif()
    set(package "${CMAKE_BINARY_DIR}/packages/${target}-webgl2.zip")
    set(stage "${CMAKE_BINARY_DIR}/web/${target}")
    get_target_property(extra_licenses ${target} KIN_WEB_EXTRA_LICENSES)
    set(component "kin-web-${target}")
    _kin_install_package_licenses(${component})
    if(WEB_CONTENT)
        file(GLOB_RECURSE content_licenses CONFIGURE_DEPENDS RELATIVE "${WEB_CONTENT}"
            "${WEB_CONTENT}/*[Ll]icense*" "${WEB_CONTENT}/*OFL*.txt" "${WEB_CONTENT}/*COPYING*")
        foreach(license IN LISTS content_licenses)
            if(NOT IS_DIRECTORY "${WEB_CONTENT}/${license}")
                get_filename_component(parent "${license}" DIRECTORY)
                install(FILES "${WEB_CONTENT}/${license}" DESTINATION "licenses/game/${parent}"
                    COMPONENT ${component} EXCLUDE_FROM_ALL)
            endif()
        endforeach()
    endif()
    if(extra_licenses)
        install(FILES ${extra_licenses} DESTINATION licenses/game COMPONENT ${component} EXCLUDE_FROM_ALL)
    endif()
    add_custom_target(${target}_package
        COMMAND "${CMAKE_COMMAND}"
            "-DHTML=$<TARGET_FILE:${target}>" "-DSTAGE=${stage}" "-DARCHIVE=${package}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DCONFIG=$<CONFIG>" "-DCOMPONENT=${component}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package_web.cmake"
        DEPENDS ${target} VERBATIM)
endfunction()
