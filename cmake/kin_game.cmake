function(kin_configure_game_assets target game_name)
    option(KIN_COPY_GAME_ASSETS "Copy game assets beside built binaries after build" OFF)

    kin_enable_release_optimizations(${target})
    kin_enable_release_link_optimizations(${target})

    set(options)
    set(one_value_args SOURCE_DIR)
    set(multi_value_args)
    cmake_parse_arguments(KIN_GAME "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT KIN_GAME_SOURCE_DIR)
        set(KIN_GAME_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    endif()

    target_compile_definitions(${target} PRIVATE
        "$<$<NOT:$<CONFIG:Release>>:KIN_ASSETS_ROOT=\"${KIN_GAME_SOURCE_DIR}\">"
    )

    if(KIN_COPY_GAME_ASSETS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${KIN_GAME_SOURCE_DIR}"
                "$<TARGET_FILE_DIR:${target}>/assets/${game_name}"
            COMMENT "Copying ${game_name} assets"
        )
    endif()
endfunction()
