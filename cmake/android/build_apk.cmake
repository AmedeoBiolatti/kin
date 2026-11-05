# Builds one debug-signed APK (cmake -P; arguments from kin_android_app in
# cmake/kin_android.cmake). Steps: content pack -> Java -> dex -> resources and
# manifest (aapt2) -> native library -> zipalign (16 KB pages) -> apksigner.
cmake_minimum_required(VERSION 3.24)

foreach(required IN ITEMS KIN_APK KIN_APK_WORK KIN_APK_LIBRARY KIN_APK_ABI KIN_APK_NAME KIN_APK_ID KIN_APK_SDK
                          KIN_APK_SDL_JAVA KIN_APK_TEMPLATES)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "build_apk: ${required} is not set")
    endif()
endforeach()

function(_run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        list(JOIN ARGN " " shown)
        message(FATAL_ERROR "build_apk: failed (${result}): ${shown}\n${out}${err}")
    endif()
endfunction()

# Tools: the newest build-tools and the target platform's android.jar.
file(GLOB build_tools_versions LIST_DIRECTORIES true "${KIN_APK_SDK}/build-tools/*")
if(NOT build_tools_versions)
    message(FATAL_ERROR "build_apk: no build-tools in ${KIN_APK_SDK} (sdkmanager \"build-tools;35.0.0\")")
endif()
list(SORT build_tools_versions COMPARE NATURAL ORDER DESCENDING)
list(GET build_tools_versions 0 build_tools)
set(android_jar "${KIN_APK_SDK}/platforms/android-${KIN_APK_TARGET_SDK}/android.jar")
if(NOT EXISTS "${android_jar}")
    message(FATAL_ERROR "build_apk: no ${android_jar} (sdkmanager \"platforms;android-${KIN_APK_TARGET_SDK}\")")
endif()
set(java_bin "")
if(KIN_APK_JAVA_HOME)
    set(java_bin "${KIN_APK_JAVA_HOME}/bin/")
endif()
set(ENV{JAVA_HOME} "${KIN_APK_JAVA_HOME}")

set(work "${KIN_APK_WORK}")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/assets" "${work}/classes" "${work}/dex" "${work}/native/lib/${KIN_APK_ABI}")

# 1. The content, as one pack. A phone has no glslc to compile the content's
#    shaders as the game loads them (docs/rendering.md), so each .frag.glsl and
#    .comp.glsl gets its .spv beside it here, made with the build machine's,
#    unless the content already holds one at least as new.
if(KIN_APK_CONTENT)
    if(NOT KIN_APK_PACK_TOOL OR NOT EXISTS "${KIN_APK_PACK_TOOL}")
        message(FATAL_ERROR "build_apk: kin_pack not found at '${KIN_APK_PACK_TOOL}' (KIN_HOST_TOOLS_DIR)")
    endif()
    set(content "${KIN_APK_CONTENT}")
    file(GLOB_RECURSE shaders RELATIVE "${KIN_APK_CONTENT}" "${KIN_APK_CONTENT}/*.frag.glsl"
        "${KIN_APK_CONTENT}/*.comp.glsl")
    set(to_compile "")
    foreach(shader IN LISTS shaders)
        string(REGEX REPLACE "\\.glsl$" ".spv" spv "${shader}")
        if(NOT EXISTS "${KIN_APK_CONTENT}/${spv}" OR "${KIN_APK_CONTENT}/${shader}" IS_NEWER_THAN "${KIN_APK_CONTENT}/${spv}")
            list(APPEND to_compile "${shader}")
        endif()
    endforeach()
    if(to_compile)
        if(NOT KIN_APK_GLSLC OR NOT EXISTS "${KIN_APK_GLSLC}")
            message(FATAL_ERROR "build_apk: the content's shaders need glslc (KIN_GLSLC): ${to_compile}")
        endif()
        set(content "${work}/content")
        file(COPY "${KIN_APK_CONTENT}/" DESTINATION "${content}")
        foreach(shader IN LISTS to_compile)
            string(REGEX REPLACE "\\.glsl$" ".spv" spv "${shader}")
            set(stage fragment)
            if(shader MATCHES "\\.comp\\.glsl$")
                set(stage compute)
            endif()
            set(include_args "")
            if(KIN_APK_SHADER_INCLUDE_DIR)
                set(include_args -I "${KIN_APK_SHADER_INCLUDE_DIR}")
            endif()
            _run("${KIN_APK_GLSLC}" "-fshader-stage=${stage}" ${include_args} -o "${content}/${spv}"
                "${content}/${shader}")
            message(STATUS "Shader ${shader} -> ${spv}")
        endforeach()
    endif()
    _run("${KIN_APK_PACK_TOOL}" create "${content}" "${work}/assets/${KIN_APK_NAME}.kinpak" --force)
endif()

# 2. Java: SDL's activity and kin's subclass, compiled against android.jar.
file(GLOB_RECURSE java_sources "${KIN_APK_SDL_JAVA}/*.java")
list(APPEND java_sources "${KIN_APK_TEMPLATES}/KinActivity.java")
_run("${java_bin}javac" -nowarn -Xlint:-options -encoding UTF-8 --release 11 -classpath "${android_jar}"
    -d "${work}/classes" ${java_sources})
_run("${java_bin}jar" cf "${work}/classes.jar" -C "${work}/classes" .)
_run("${build_tools}/d8" --release --min-api "${KIN_APK_MIN_SDK}" --lib "${android_jar}"
    --output "${work}/dex" "${work}/classes.jar")

# 3. Manifest and assets. The pack is stored, not compressed, so the app reads
#    it in place from the APK.
set(KIN_APK_TITLE_XML "${KIN_APK_TITLE}")
string(REPLACE "&" "&amp;" KIN_APK_TITLE_XML "${KIN_APK_TITLE_XML}")
string(REPLACE "\"" "&quot;" KIN_APK_TITLE_XML "${KIN_APK_TITLE_XML}")
string(REPLACE "<" "&lt;" KIN_APK_TITLE_XML "${KIN_APK_TITLE_XML}")
set(KIN_APK_TITLE_RAW "${KIN_APK_TITLE}")
set(KIN_APK_TITLE "${KIN_APK_TITLE_XML}")
configure_file("${KIN_APK_TEMPLATES}/AndroidManifest.xml.in" "${work}/AndroidManifest.xml" @ONLY)
set(KIN_APK_TITLE "${KIN_APK_TITLE_RAW}")
_run("${build_tools}/aapt2" link -o "${work}/base.apk" --manifest "${work}/AndroidManifest.xml"
    -I "${android_jar}" --min-sdk-version "${KIN_APK_MIN_SDK}" --target-sdk-version "${KIN_APK_TARGET_SDK}"
    --version-code "${KIN_APK_VERSION_CODE}" --version-name "${KIN_APK_VERSION}" --debug-mode
    -A "${work}/assets" -0 kinpak)

# 4. Code: the dex, and the stripped native library (stored: Android maps it
#    from the APK, extractNativeLibs="false").
set(native_lib "${work}/native/lib/${KIN_APK_ABI}/libmain.so")
if(KIN_APK_STRIP)
    _run("${KIN_APK_STRIP}" --strip-unneeded -o "${native_lib}" "${KIN_APK_LIBRARY}")
else()
    file(COPY_FILE "${KIN_APK_LIBRARY}" "${native_lib}")
endif()
find_program(zip_tool zip REQUIRED)
execute_process(COMMAND "${zip_tool}" -q "${work}/base.apk" classes.dex WORKING_DIRECTORY "${work}/dex"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "build_apk: zip classes.dex failed")
endif()
execute_process(COMMAND "${zip_tool}" -q -0 "${work}/base.apk" "lib/${KIN_APK_ABI}/libmain.so"
    WORKING_DIRECTORY "${work}/native" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "build_apk: zip libmain.so failed")
endif()

# 5. Align (native libraries to 16 KB pages, the rest to 4 bytes) and sign.
_run("${build_tools}/zipalign" -P 16 -f 4 "${work}/base.apk" "${work}/aligned.apk")
if(DEFINED ENV{HOME})
    set(keystore "$ENV{HOME}/.android/debug.keystore")
else()
    set(keystore "${work}/../debug.keystore")
endif()
if(NOT EXISTS "${keystore}")
    get_filename_component(keystore_dir "${keystore}" DIRECTORY)
    file(MAKE_DIRECTORY "${keystore_dir}")
    _run("${java_bin}keytool" -genkeypair -keystore "${keystore}" -storepass android -alias androiddebugkey
        -keypass android -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US")
endif()
_run("${build_tools}/apksigner" sign --ks "${keystore}" --ks-pass pass:android --key-pass pass:android
    --ks-key-alias androiddebugkey --out "${KIN_APK}" "${work}/aligned.apk")
file(SIZE "${KIN_APK}" apk_size)
math(EXPR apk_kib "${apk_size} / 1024")
message(STATUS "APK ${KIN_APK} (${apk_kib} KiB, ${KIN_APK_ID})")
