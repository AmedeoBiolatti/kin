file(GLOB KIN_GAME_DIRS CONFIGURE_DEPENDS LIST_DIRECTORIES true
    "${CMAKE_CURRENT_SOURCE_DIR}/*"
)

foreach(KIN_GAME_DIR IN LISTS KIN_GAME_DIRS)
    if(NOT IS_DIRECTORY "${KIN_GAME_DIR}")
        continue()
    endif()

    if(NOT EXISTS "${KIN_GAME_DIR}/CMakeLists.txt")
        continue()
    endif()

    get_filename_component(KIN_GAME_NAME "${KIN_GAME_DIR}" NAME)
    if(KIN_GAME_NAME MATCHES "^\\.")
        continue()
    endif()

    string(MAKE_C_IDENTIFIER "${KIN_GAME_NAME}" KIN_GAME_ID)
    string(TOUPPER "${KIN_GAME_ID}" KIN_GAME_ID)
    set(KIN_GAME_OPTION "KIN_BUILD_GAME_${KIN_GAME_ID}")

    option(${KIN_GAME_OPTION} "Build the ${KIN_GAME_NAME} sample game" ON)
    if(${KIN_GAME_OPTION})
        add_subdirectory("${KIN_GAME_NAME}")
    endif()
endforeach()
