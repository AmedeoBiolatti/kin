include_guard(GLOBAL)

# Emscripten's default stack (64 KiB) is less than flecs needs to make a world:
# past it the stack runs over the game's memory (ecs_init itself fails).
set(KIN_WEB_STACK_SIZE 1048576 CACHE STRING "Stack of a kin game in a browser, in bytes (a desktop main thread's, Windows')")

# A browser bundle has no native runtime dependencies or desktop release ABI.
# Asset preloading is performed by the host Emscripten tool, never a Wasm kin_pack.
function(kin_package_web_game target)
    cmake_parse_arguments(WEB "" "CONTENT;NAME" "" ${ARGN})
    set_target_properties(${target} PROPERTIES SUFFIX ".html")
    target_link_options(${target} PRIVATE
        "--shell-file=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../web/shell.html" --emit-symbol-map
        "-sSTACK_SIZE=${KIN_WEB_STACK_SIZE}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../web/shell.html")
    if(KIN_WEB_PERSISTENCE)
        set(persistent "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../web/persistent.js")
        target_link_options(${target} PRIVATE "--pre-js=${persistent}" -lidbfs.js -sFORCE_FILESYSTEM=1)
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${persistent}")
    endif()
    if(WEB_CONTENT AND IS_DIRECTORY "${WEB_CONTENT}")
        target_link_options(${target} PRIVATE "SHELL:--preload-file \"${WEB_CONTENT}@/${WEB_NAME}\"")
        # The link writes the content's .data: it links again when a file of
        # it changes (a file added or taken away configures again).
        file(GLOB_RECURSE content_files CONFIGURE_DEPENDS "${WEB_CONTENT}/*")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${content_files})
    endif()
    set(ui_fonts "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../examples/content/lang/fonts")
    if(KIN_ENABLE_TTF)
        # A browser has no system fonts: the UI's (themes' system_font, ui2's
        # own) is Noto Sans' Latin subset, here (else kin's bitmap font,
        # capitals only). Fonts of the game's content are its own.
        target_link_options(${target} PRIVATE "SHELL:--preload-file \"${ui_fonts}/latin.ttf@/kin/fonts/ui.ttf\"")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${ui_fonts}/latin.ttf")
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
    if(KIN_ENABLE_TTF)
        install(FILES "${ui_fonts}/core-OFL.txt" DESTINATION licenses/kin-ui-font
            RENAME OFL.txt COMPONENT ${component} EXCLUDE_FROM_ALL)
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
