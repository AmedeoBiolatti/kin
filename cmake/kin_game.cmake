include("${CMAKE_CURRENT_LIST_DIR}/kin_build.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/kin_packaging.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/kin_android.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/kin_web.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/kin_import_2d.cmake")

function(kin_configure_asset_pipeline target game_name KIN_GAME_SOURCE_DIR KIN_GAME_MANIFEST)
    option(KIN_ENABLE_ASSET_COOKING "Cook lossless game assets for shipping" OFF)
    set_target_properties(${target} PROPERTIES KIN_GAME_ASSETS_DIR "${KIN_GAME_SOURCE_DIR}" KIN_GAME_ASSETS_NAME "${game_name}")
    if(KIN_GAME_MANIFEST)
        get_filename_component(manifest "${KIN_GAME_MANIFEST}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${manifest}")
            message(FATAL_ERROR "Asset manifest does not exist: ${manifest}")
        endif()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(tool "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/report_game_assets.py")
        set_target_properties(${target} PROPERTIES
            KIN_GAME_ASSET_MANIFEST "${manifest}"
            KIN_GAME_ASSET_REPORT_PYTHON "${Python3_EXECUTABLE}"
            KIN_GAME_ASSET_REPORT_TOOL "${tool}")
        add_custom_target(${target}_asset_report
            COMMAND "${Python3_EXECUTABLE}" "${tool}"
                --assets "${KIN_GAME_SOURCE_DIR}" --manifest "${manifest}"
                --output "${CMAKE_BINARY_DIR}/asset-reports/${target}-$<CONFIG>.assets.json"
            COMMENT "Auditing ${game_name} assets (dry run; all files kept)"
            VERBATIM)
        if(TARGET ${target}_import_2d)
            add_dependencies(${target}_asset_report ${target}_import_2d)
        endif()
    endif()

    if(KIN_ENABLE_ASSET_COOKING)
        if(NOT KIN_GAME_MANIFEST)
            message(FATAL_ERROR "Asset cooking requires an explicit MANIFEST for ${target}")
        endif()
        set(cook_tool "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/cook_game_assets.py")
        set(cook_output "${CMAKE_BINARY_DIR}/cooked-assets/${target}/$<CONFIG>")
        set(cook_cache "${CMAKE_BINARY_DIR}/asset-cook-cache")
        set(cook_report "${CMAKE_BINARY_DIR}/asset-reports/${target}-$<CONFIG>.cook.json")
        set(cook_target "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
        set_target_properties(${target} PROPERTIES
            KIN_GAME_COOK_OUTPUT "${cook_output}" KIN_GAME_COOK_CACHE "${cook_cache}"
            KIN_GAME_COOK_REPORT "${cook_report}" KIN_GAME_COOK_TOOL "${cook_tool}"
            KIN_GAME_COOK_TARGET "${cook_target}")
        add_custom_target(${target}_cook_assets
            COMMAND "${Python3_EXECUTABLE}" "${cook_tool}"
                --assets "${KIN_GAME_SOURCE_DIR}" --manifest "${manifest}"
                --output "${cook_output}" --cache "${cook_cache}" --report "${cook_report}"
                --target "${cook_target}"
            COMMENT "Cooking ${game_name} assets (lossless; all files kept)" VERBATIM)
        if(TARGET ${target}_import_2d)
            add_dependencies(${target}_cook_assets ${target}_import_2d)
        endif()
    endif()

endfunction()

# Games built with kin, from their content to a package (docs/shipping.md).
#
#   kin_game(<target>
#       [CONTENT <dir>]        # the game's content folder (default: content/ beside the CMakeLists)
#       [IMPORTS <file.json>]  # build authoring sources into native 2D assets (docs/importing_2d.md)
#       [NAME <name>]          # its name beside the executable: <name>.kinpak or <name>/ (default: <target>)
#       [TITLE <text>]         # the product name in the Windows version info (default: <name>)
#       [VERSION <x.y.z>]      # (default: the project's version)
#       [ICON <file.ico>]      # the Windows executable's icon
#       [INFO <project.kin>]   # its game info: TITLE, VERSION and ICON default to its title,
#                              # version and icon (the .ico beside its icon PNG, in the content)
#       [FILES <file> ...]     # more files to ship beside the executable (a readme, ...)
#       [STEAM_APP_ID <id>]    # its Steam app id (default: the game info's steam_app_id)
#       [STEAM_REDIST <dir>]   # Steam's API library to ship beside it: the SDK's
#                              # redistributable_bin folder, a folder holding the
#                              # library, or the library itself (docs/platform_services.md)
#       [NO_CONTENT])          # the game has no content folder
#
# On the target it sets KIN_GAME_TITLE, KIN_GAME_VERSION, KIN_GAME_ICON (the
# game info's icon, else "") and KIN_GAME_CONTENT_NAME and, outside shipping builds,
# KIN_GAME_CONTENT_DIR, so the game finds its content with
#   kin::find_content_root(KIN_GAME_CONTENT, argc, argv)
# in its source folder while it is made, and beside the executable once it
# ships. It also makes:
#   <target>_content   the content pack, <name>.kinpak beside the executable
#                      (with KIN_PACK_CONTENT; built with the game in shipping builds)
#   <target>_package   packages/<name>-<version>-<platform>.zip (.tar.gz on Linux):
#                      the executable, its content, and the license texts it must carry
# With a Steam app id, KIN_GAME_STEAM_APP_ID (a number) and, outside shipping
# builds, a steam_appid.txt beside the executable, so Steam lets the game start
# from the build folder instead of starting it again through Steam.
#
# The store a build is for (KIN_GAME_STORE, a cache variable: one build folder
# for each store the game is sold in, docs/platform_services.md):
#   auto    Steam when the game has a Steam app id (the default);
#   steam   Steam: configuring fails without an app id;
#   none    no store (itch.io, the game's own download): no Steam app id in it,
#           no steam_appid.txt and no Steam library, whatever its info says.
# Its code gets KIN_GAME_STORE: "steam", or "" for none. KIN_STEAM_REDIST (a
# cache variable) is STEAM_REDIST for every Steam build of the folder that
# doesn't give one: the Steamworks SDK's redistributable_bin, say.
# and install rules for the component <target>.

set(_KIN_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

set(KIN_GAME_STORE "auto" CACHE STRING "The store kin_game builds games for: auto (Steam with a Steam app id), steam or none")
set_property(CACHE KIN_GAME_STORE PROPERTY STRINGS auto steam none)
set(KIN_STEAM_REDIST "" CACHE PATH "Steam's API library for Steam builds that give no STEAM_REDIST (the Steamworks SDK's redistributable_bin)")

function(_kin_game_platform out_var)
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" system)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" processor)
    if(processor MATCHES "^(amd64|x86_64|x64)$")
        set(processor x86_64)
    elseif(processor MATCHES "^(arm64|aarch64)$")
        set(processor arm64)
    endif()
    set(${out_var} "${system}-${processor}" PARENT_SCOPE)
endfunction()

function(_kin_game_windows_resource target name title version icon)
    string(REGEX MATCHALL "[0-9]+" numbers "${version}")
    list(APPEND numbers 0 0 0 0)
    list(SUBLIST numbers 0 4 numbers)
    string(JOIN "," numeric ${numbers})
    set(icon_line "")
    if(icon)
        get_filename_component(icon "${icon}" ABSOLUTE)
        set(icon_line "1 ICON \"${icon}\"")
    endif()
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/kin_game/${target}.rc")
    file(CONFIGURE OUTPUT "${rc}" CONTENT
"// Generated by kin_game; do not edit.
#include <winver.h>

${icon_line}

VS_VERSION_INFO VERSIONINFO
FILEVERSION ${numeric}
PRODUCTVERSION ${numeric}
FILEFLAGSMASK VS_FFI_FILEFLAGSMASK
FILEFLAGS 0
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
FILESUBTYPE VFT2_UNKNOWN
BEGIN
    BLOCK \"StringFileInfo\"
    BEGIN
        BLOCK \"040904b0\"
        BEGIN
            VALUE \"FileDescription\", \"${title}\"
            VALUE \"FileVersion\", \"${version}\"
            VALUE \"InternalName\", \"${name}\"
            VALUE \"OriginalFilename\", \"${target}.exe\"
            VALUE \"ProductName\", \"${title}\"
            VALUE \"ProductVersion\", \"${version}\"
        END
    END
    BLOCK \"VarFileInfo\"
    BEGIN
        VALUE \"Translation\", 0x409, 1200
    END
END
")
    target_sources(${target} PRIVATE "${rc}")
endfunction()

# Steam's API library for this platform in `where` (the SDK's
# redistributable_bin, a folder holding it, or the file).
function(_kin_game_steam_library out_var where)
    get_filename_component(where "${where}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(name steam_api64.dll)
            set(sdk_folder win64)
        else()
            set(name steam_api.dll)
            set(sdk_folder .)
        endif()
    elseif(APPLE)
        set(name libsteam_api.dylib)
        set(sdk_folder osx)
    else()
        set(name libsteam_api.so)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
            set(sdk_folder linuxarm64)
        else()
            set(sdk_folder linux64)
        endif()
    endif()
    foreach(candidate IN ITEMS "${where}" "${where}/${name}" "${where}/${sdk_folder}/${name}")
        if(EXISTS "${candidate}" AND NOT IS_DIRECTORY "${candidate}")
            set(${out_var} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "kin_game: no ${name} in ${where} (nor in ${sdk_folder}/)")
endfunction()

function(kin_game target)
    cmake_parse_arguments(ARG "NO_CONTENT" "CONTENT;NAME;TITLE;VERSION;ICON;MANIFEST;INFO;STEAM_APP_ID;STEAM_REDIST;IMPORTS"
        "FILES;RUNTIME_TARGETS" ${ARGN})
    if(NOT ARG_NAME)
        set(ARG_NAME "${target}")
    endif()
    if(NOT ARG_CONTENT)
        set(ARG_CONTENT "${CMAKE_CURRENT_SOURCE_DIR}/content")
    endif()
    get_filename_component(content "${ARG_CONTENT}" ABSOLUTE)
    if(ARG_IMPORTS)
        if(ARG_NO_CONTENT)
            message(FATAL_ERROR "kin_game IMPORTS cannot be combined with NO_CONTENT")
        endif()
        kin_import_2d_assets(${target} MANIFEST "${ARG_IMPORTS}" CONTENT "${content}"
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/imported-content/${target}")
        get_target_property(content ${target} KIN_IMPORTED_CONTENT_DIR)
    endif()
    set(icon_png "")
    if(ARG_INFO)
        # The game info's lines ("title My Game", "version 1.2.0", "icon icon.png"),
        # read again when it changes.
        get_filename_component(info "${ARG_INFO}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${info}")
            message(FATAL_ERROR "kin_game(${target}): no game info at ${info}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${info}")
        file(STRINGS "${info}" info_lines ENCODING UTF-8)
        foreach(line IN LISTS info_lines)
            if(line MATCHES "^title[ \t]+(.+)$" AND NOT ARG_TITLE)
                string(STRIP "${CMAKE_MATCH_1}" ARG_TITLE)
            elseif(line MATCHES "^steam_app_id[ \t]+([0-9]+)" AND NOT ARG_STEAM_APP_ID)
                set(ARG_STEAM_APP_ID "${CMAKE_MATCH_1}")
            elseif(line MATCHES "^version[ \t]+(.+)$" AND NOT ARG_VERSION)
                string(STRIP "${CMAKE_MATCH_1}" ARG_VERSION)
            elseif(line MATCHES "^icon[ \t]+(.+)$")
                string(STRIP "${CMAKE_MATCH_1}" icon_png)
                string(REGEX REPLACE "\\.[^.]*$" ".ico" icon_ico "${content}/${icon_png}")
                if(EXISTS "${icon_ico}" AND NOT ARG_ICON)
                    set(ARG_ICON "${icon_ico}")
                endif()
            endif()
        endforeach()
    endif()
    if(NOT ARG_TITLE)
        set(ARG_TITLE "${ARG_NAME}")
    endif()
    if(NOT ARG_VERSION)
        set(ARG_VERSION "${PROJECT_VERSION}")
    endif()
    if(NOT ARG_VERSION)
        set(ARG_VERSION "0.0.0")
    endif()

    kin_enable_release_optimizations(${target})
    kin_enable_release_link_optimizations(${target})
    kin_configure_game_build(${target})

    target_compile_definitions(${target} PRIVATE KIN_GAME_CONTENT_NAME="${ARG_NAME}")
    # Its title, version and icon (a path in its content, from INFO) for its
    # code: KIN_GAME_TITLE, KIN_GAME_VERSION and KIN_GAME_ICON, string literals.
    foreach(key IN ITEMS TITLE VERSION ICON)
        if(key STREQUAL "ICON")
            set(value "${icon_png}")
        else()
            set(value "${ARG_${key}}")
        endif()
        string(REPLACE "\\" "\\\\" value "${value}")
        string(REPLACE "\"" "\\\"" value "${value}")
        target_compile_definitions(${target} PRIVATE "KIN_GAME_${key}=\"${value}\"")
    endforeach()
    # The store it is built for: none leaves Steam out, whatever the game info says.
    if(NOT KIN_GAME_STORE MATCHES "^(auto|steam|none)$")
        message(FATAL_ERROR "kin_game(${target}): KIN_GAME_STORE is auto, steam or none, not ${KIN_GAME_STORE}")
    endif()
    if(KIN_GAME_STORE STREQUAL "none" OR EMSCRIPTEN)
        set(ARG_STEAM_APP_ID "")
        set(ARG_STEAM_REDIST "")
    elseif(KIN_GAME_STORE STREQUAL "steam" AND NOT ARG_STEAM_APP_ID)
        message(FATAL_ERROR "kin_game(${target}): built for Steam (KIN_GAME_STORE), but the game has no Steam app id: "
            "steam_app_id in its game info, or STEAM_APP_ID")
    endif()
    if(ARG_STEAM_APP_ID AND NOT ARG_STEAM_REDIST AND KIN_STEAM_REDIST)
        set(ARG_STEAM_REDIST "${KIN_STEAM_REDIST}")
    endif()
    if(ARG_STEAM_APP_ID)
        if(NOT ARG_STEAM_APP_ID MATCHES "^[0-9]+$")
            message(FATAL_ERROR "kin_game(${target}): STEAM_APP_ID must be a number: ${ARG_STEAM_APP_ID}")
        endif()
        target_compile_definitions(${target} PRIVATE KIN_GAME_STEAM_APP_ID=${ARG_STEAM_APP_ID} KIN_GAME_STORE="steam")
    else()
        target_compile_definitions(${target} PRIVATE KIN_GAME_STEAM_APP_ID=0 KIN_GAME_STORE="")
    endif()
    if(ARG_STEAM_APP_ID AND NOT KIN_SHIPPING AND NOT ANDROID)
        file(GENERATE OUTPUT "$<TARGET_FILE_DIR:${target}>/steam_appid.txt" CONTENT "${ARG_STEAM_APP_ID}\n")
    else()
        # None from a build before (another store's, or a development one): with it, Steam
        # would let a shipping build start outside Steam, and a build for no store try it.
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE_DIR:${target}>/steam_appid.txt"
            VERBATIM)
    endif()
    if(ARG_STEAM_REDIST AND NOT EMSCRIPTEN AND NOT ANDROID)
        _kin_game_steam_library(steam_library "${ARG_STEAM_REDIST}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${steam_library}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
        install(FILES "${steam_library}" DESTINATION . COMPONENT ${target})
    endif()
    if(ARG_NO_CONTENT)
        set(content "")
    elseif(NOT KIN_SHIPPING AND NOT EMSCRIPTEN AND NOT ANDROID)
        # (An Android app cannot see the build machine's folders.)
        target_compile_definitions(${target} PRIVATE KIN_GAME_CONTENT_DIR="${content}")
    endif()

    if(EMSCRIPTEN)
        kin_package_web_game(${target} CONTENT "${content}" NAME "${ARG_NAME}")
        return()
    endif()
    if(ANDROID)
        kin_android_app(${target} CONTENT "${content}" NAME "${ARG_NAME}" TITLE "${ARG_TITLE}"
            VERSION "${ARG_VERSION}")
        return()
    endif()

    if(WIN32)
        _kin_game_windows_resource(${target} "${ARG_NAME}" "${ARG_TITLE}" "${ARG_VERSION}" "${ARG_ICON}")
        if(KIN_SHIPPING)
            # A windowed app: no console opens beside the game. Its log goes
            # with its saves instead (run_scene_app).
            set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE ON)
            if(MSVC)
                target_link_options(${target} PRIVATE /ENTRY:mainCRTStartup) # keeps int main()
            endif()
        endif()
    endif()

    if(KIN_SHIPPING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # The C++ runtime inside the game, so players need no newer libstdc++
        # than their system has. glibc stays shared: the build machine's
        # version is the oldest the game runs on (docs/shipping.md).
        target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
    endif()

    set_target_properties(${target} PROPERTIES KIN_PACKAGE_INSTALL_COMPONENT "${target}")
    if(content AND IS_DIRECTORY "${content}")
        kin_configure_asset_pipeline(${target} "${ARG_NAME}" "${content}" "${ARG_MANIFEST}")
    endif()
    if(ARG_FILES)
        install(FILES ${ARG_FILES} DESTINATION . COMPONENT ${target})
    endif()
    set(package_depends ${target})
    if(content AND IS_DIRECTORY "${content}")
        if(KIN_PACK_CONTENT)
            # kin_pack leaves an up-to-date pack alone, so this costs a scan of
            # the folder when nothing changed.
            set(pack "$<TARGET_FILE_DIR:${target}>/${ARG_NAME}.kinpak")
            add_custom_target(${target}_content
                COMMAND kin_pack create "${content}" "${pack}"
                COMMENT "Packing ${ARG_NAME} content"
                VERBATIM)
            if(TARGET ${target}_import_2d)
                add_dependencies(${target}_content ${target}_import_2d)
            endif()
            if(KIN_SHIPPING)
                add_dependencies(${target} ${target}_content)
            endif()
            list(APPEND package_depends ${target}_content)
            install(FILES "${pack}" DESTINATION . COMPONENT ${target})
            set_target_properties(${target} PROPERTIES KIN_GAME_CONTENT_PACK "${ARG_NAME}.kinpak")
        else()
            set_target_properties(${target} PROPERTIES KIN_GAME_CONTENT_DIRECTORY "${ARG_NAME}")
            install(DIRECTORY "${content}/" DESTINATION "${ARG_NAME}" COMPONENT ${target}
                    PATTERN ".*" EXCLUDE)
        endif()
    endif()

    _kin_game_platform(platform)
    set_target_properties(${target} PROPERTIES KIN_PACKAGE_BASENAME "${ARG_NAME}-${ARG_VERSION}-${platform}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT KIN_RELEASE_CONTRACT)
        set_target_properties(${target} PROPERTIES KIN_PACKAGE_ARCHIVE_EXTENSION "tar.gz")
    endif()
    kin_package_game(${target} VERSION "${ARG_VERSION}" RUNTIME_TARGETS ${ARG_RUNTIME_TARGETS})
    if(TARGET ${target}_package)
        add_dependencies(${target}_package ${package_depends})
    endif()
endfunction()

# The older way to set up a game's assets, kept for games that use it: bakes
# KIN_ASSETS_ROOT into non-Release builds and, with KIN_COPY_GAME_ASSETS,
# copies the assets beside the binaries. It now also sets the game up with
# kin_game, its assets folder as the content.
function(kin_configure_game_assets target game_name)
    option(KIN_COPY_GAME_ASSETS "Copy legacy game assets beside built binaries" OFF)
    cmake_parse_arguments(ASSETS "" "SOURCE_DIR;MANIFEST" "" ${ARGN})
    if(ASSETS_UNPARSED_ARGUMENTS OR ASSETS_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "Invalid kin_configure_game_assets arguments")
    endif()
    if(NOT ASSETS_SOURCE_DIR)
        set(ASSETS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    endif()
    get_filename_component(source "${ASSETS_SOURCE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT IS_DIRECTORY "${source}" OR NOT game_name MATCHES "^[A-Za-z0-9_-]+$")
        message(FATAL_ERROR "Invalid game assets directory or name: ${source}; ${game_name}")
    endif()
    set(manifest_argument)
    if(ASSETS_MANIFEST)
        set(manifest_argument MANIFEST "${ASSETS_MANIFEST}")
    endif()
    kin_game(${target} CONTENT "${source}" NAME "${game_name}" ${manifest_argument})
    target_compile_definitions(${target} PRIVATE "$<$<CONFIG:Debug>:KIN_ASSETS_ROOT=\"${source}\">")
    if(KIN_COPY_GAME_ASSETS)
        add_custom_target(${target}_assets
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${source}" "$<TARGET_FILE_DIR:${target}>/assets/${game_name}"
            COMMENT "Copying ${game_name} legacy assets" VERBATIM)
        add_dependencies(${target} ${target}_assets)
    endif()
endfunction()
