# Android apps from kin games (docs/android.md).
#
# On Android, kin_game(<target> ...) calls kin_android_app for the game's
# executable target: the target is linked as the app's lib/arm64-v8a/libmain.so
# (SDL's Java activity loads it and calls SDL_main), and
#   <target>_apk   builds android/<target>/<name>.apk: the library, SDL's Java
#                  activity, and the game's content as <name>.kinpak, stored
#                  uncompressed in the APK's assets so it is read in place.
# The APK is made with the SDK's build tools directly (javac, d8, aapt2,
# zipalign, apksigner); Gradle is not needed. It is signed with the debug key
# (~/.android/debug.keystore, made if missing): for development, not release.
#
# Settings:
#   KIN_ANDROID_SDK          the Android SDK (default: two folders above the NDK)
#   KIN_ANDROID_JAVA_HOME    a JDK 17 or newer for javac/jar/keytool (default: $JAVA_HOME)
#   KIN_HOST_TOOLS_DIR       a desktop build's bin/ holding kin_pack (needed with content)
#   KIN_ANDROID_ID_PREFIX    the application id is <prefix>.<name> (default: org.kin)
#   KIN_ANDROID_TARGET_SDK   targetSdkVersion and the android.jar compiled against (default: 35)
#   KIN_ANDROID_ORIENTATION  the activity's screenOrientation (default: unspecified)



if(ANDROID)
    get_filename_component(_kin_android_sdk_default "${CMAKE_ANDROID_NDK}/../.." ABSOLUTE)
    set(KIN_ANDROID_SDK "${_kin_android_sdk_default}" CACHE PATH "The Android SDK (platforms/, build-tools/)")
    set(KIN_ANDROID_JAVA_HOME "$ENV{JAVA_HOME}" CACHE PATH "A JDK (17 or newer) for building APKs")
    set(KIN_HOST_TOOLS_DIR "" CACHE PATH "A desktop kin build's bin/ folder holding kin_pack")
    set(KIN_ANDROID_ID_PREFIX "org.kin" CACHE STRING "Android application ids are <prefix>.<game name>")
    set(KIN_ANDROID_TARGET_SDK "35" CACHE STRING "Android targetSdkVersion (and the android.jar used)")
    set(KIN_ANDROID_ORIENTATION "unspecified" CACHE STRING "The activity's android:screenOrientation")
endif()

function(_kin_android_version_code out_var version)
    set(code 1)
    if(version MATCHES "^([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?")
        set(major "${CMAKE_MATCH_1}")
        set(minor 0)
        set(patch 0)
        if(CMAKE_MATCH_3)
            set(minor "${CMAKE_MATCH_3}")
        endif()
        if(CMAKE_MATCH_5)
            set(patch "${CMAKE_MATCH_5}")
        endif()
        math(EXPR code "${major} * 10000 + ${minor} * 100 + ${patch}")
        if(code LESS 1)
            set(code 1)
        endif()
    endif()
    set(${out_var} "${code}" PARENT_SCOPE)
endfunction()

function(kin_android_app target)
    cmake_parse_arguments(ARG "" "CONTENT;NAME;TITLE;VERSION" "" ${ARGN})
    if(NOT ANDROID)
        message(FATAL_ERROR "kin_android_app(${target}) needs the Android NDK toolchain")
    endif()
    get_target_property(type ${target} TYPE)
    if(NOT type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR "kin_android_app(${target}): ${target} must be an executable target")
    endif()
    if(NOT ARG_NAME)
        set(ARG_NAME "${target}")
    endif()
    if(NOT ARG_TITLE)
        set(ARG_TITLE "${ARG_NAME}")
    endif()
    if(NOT ARG_VERSION)
        set(ARG_VERSION "0.0.0")
    endif()

    # (The function's own folder: kin_game may be called from a game outside kin.)
    set(templates "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/android")

    # The game's executable target becomes the app's shared library. Its own
    # main() stays; the shim's SDL_main calls it.
    set(app_dir "${CMAKE_BINARY_DIR}/android/${target}")
    set(lib_dir "${app_dir}/lib/${CMAKE_ANDROID_ARCH_ABI}")
    target_sources(${target} PRIVATE "${templates}/sdl_main_shim.c")
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME main PREFIX lib SUFFIX .so
        RUNTIME_OUTPUT_DIRECTORY "${lib_dir}"
        # An executable's "position independent" is -fPIE, which a shared
        # library cannot use: its own sources are compiled -fPIC instead.
        POSITION_INDEPENDENT_CODE OFF)
    target_compile_options(${target} PRIVATE -fPIC)
    foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${lib_dir}")
    endforeach()
    # 16 KB pages (required by Android 15+ devices that use them; NDK r28+
    # defaults to it, kept explicit). Undefined symbols fail the link, not the
    # app's start.
    target_link_options(${target} PRIVATE -shared -Wl,-z,max-page-size=16384 -Wl,--no-undefined)

    string(MAKE_C_IDENTIFIER "${ARG_NAME}" id_name)
    string(TOLOWER "${id_name}" id_name)
    _kin_android_version_code(version_code "${ARG_VERSION}")

    set(pack_tool "")
    if(ARG_CONTENT AND IS_DIRECTORY "${ARG_CONTENT}")
        if(NOT KIN_HOST_TOOLS_DIR)
            message(WARNING "kin_android_app(${target}): set KIN_HOST_TOOLS_DIR to a desktop build's bin/ "
                "(with kin_pack) to put the game's content in its APK")
        else()
            set(pack_tool "${KIN_HOST_TOOLS_DIR}/kin_pack${CMAKE_HOST_EXECUTABLE_SUFFIX}")
        endif()
    else()
        set(ARG_CONTENT "")
    endif()

    # SDL's Java activity, from the SDL source kin builds (looked up, as a game
    # outside kin's tree does not see kin's own variables).
    include(FetchContent)
    FetchContent_GetProperties(SDL3)
    set(sdl_java "${sdl3_SOURCE_DIR}/android-project/app/src/main/java")
    if(NOT IS_DIRECTORY "${sdl_java}")
        message(FATAL_ERROR "kin_android_app: SDL's Java sources are not at ${sdl_java}")
    endif()

    set(min_sdk "${ANDROID_PLATFORM_LEVEL}")
    if(NOT min_sdk)
        set(min_sdk "${CMAKE_SYSTEM_VERSION}")
    endif()

    set(apk "${app_dir}/${ARG_NAME}.apk")
    add_custom_target(${target}_apk
        COMMAND "${CMAKE_COMMAND}"
            "-DKIN_APK=${apk}"
            "-DKIN_APK_WORK=${app_dir}/work"
            "-DKIN_APK_LIBRARY=$<TARGET_FILE:${target}>"
            "-DKIN_APK_ABI=${CMAKE_ANDROID_ARCH_ABI}"
            "-DKIN_APK_NAME=${ARG_NAME}"
            "-DKIN_APK_ID=${KIN_ANDROID_ID_PREFIX}.${id_name}"
            "-DKIN_APK_TITLE=${ARG_TITLE}"
            "-DKIN_APK_VERSION=${ARG_VERSION}"
            "-DKIN_APK_VERSION_CODE=${version_code}"
            "-DKIN_APK_ORIENTATION=${KIN_ANDROID_ORIENTATION}"
            "-DKIN_APK_MIN_SDK=${min_sdk}"
            "-DKIN_APK_TARGET_SDK=${KIN_ANDROID_TARGET_SDK}"
            "-DKIN_APK_CONTENT=${ARG_CONTENT}"
            "-DKIN_APK_PACK_TOOL=${pack_tool}"
            "-DKIN_APK_GLSLC=${KIN_GLSLC}"
            "-DKIN_APK_SHADER_INCLUDE_DIR=${KIN_SHADER_INCLUDE_DIR}"
            "-DKIN_APK_SDK=${KIN_ANDROID_SDK}"
            "-DKIN_APK_JAVA_HOME=${KIN_ANDROID_JAVA_HOME}"
            "-DKIN_APK_STRIP=${CMAKE_STRIP}"
            "-DKIN_APK_SDL_JAVA=${sdl_java}"
            "-DKIN_APK_TEMPLATES=${templates}"
            -P "${templates}/build_apk.cmake"
        DEPENDS ${target}
        BYPRODUCTS "${apk}"
        COMMENT "Android APK: ${apk}"
        VERBATIM)
    # games/CMakeLists.txt makes a desktop package for games without one.
    if(NOT TARGET ${target}_package)
        add_custom_target(${target}_package DEPENDS ${target}_apk)
    endif()
    set_target_properties(${target} PROPERTIES KIN_ANDROID_APK "${apk}" KIN_ANDROID_ID "${KIN_ANDROID_ID_PREFIX}.${id_name}")
endfunction()
