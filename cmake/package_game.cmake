cmake_minimum_required(VERSION 3.22)
if(KIN_PACKAGE_PGO_MODE STREQUAL "generate")
    message(FATAL_ERROR "Instrumented PGO binaries are training tools; rebuild with KIN_PGO_MODE=use or off before packaging")
endif()

if(NOT KIN_PACKAGE_CONFIG MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    message(FATAL_ERROR "Desktop packages require Release, RelWithDebInfo or MinSizeRel, not '${KIN_PACKAGE_CONFIG}'")
endif()
set(output "${KIN_PACKAGE_BUILD_DIR}/packages")
set(stage "${output}/${KIN_PACKAGE_NAME}")
file(MAKE_DIRECTORY "${output}")
file(LOCK "${output}/${KIN_PACKAGE_NAME}.lock" GUARD PROCESS TIMEOUT 60)
# Only this target/configuration's generated staging directory is replaced.
file(REMOVE_RECURSE "${stage}")
file(REMOVE "${stage}.zip" "${stage}.size.json" "${stage}-symbols.zip" "${stage}.assets.json" "${stage}.cook.json")
if(NOT KIN_PACKAGE_DEBUG_SYMBOLS)
    file(REMOVE_RECURSE "${stage}-symbols")
endif()
# Developer install settings must not turn the package into links to the build
# tree or redirect its files under an unrelated DESTDIR.
set(strip_argument)
if(KIN_PACKAGE_STRIP AND NOT KIN_PACKAGE_DEBUG_SYMBOLS)
    set(strip_argument --strip)
endif()
if(KIN_PACKAGE_COOK_TOOL)
    execute_process(COMMAND "${KIN_PACKAGE_ASSET_PYTHON}" "${KIN_PACKAGE_COOK_TOOL}"
        --assets "${KIN_PACKAGE_COOK_SOURCE}" --manifest "${KIN_PACKAGE_ASSET_MANIFEST}"
        --output "${KIN_PACKAGE_COOK_OUTPUT}" --cache "${KIN_PACKAGE_COOK_CACHE}"
        --report "${KIN_PACKAGE_COOK_REPORT}" --target "${KIN_PACKAGE_COOK_TARGET}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DESTDIR "CMAKE_INSTALL_MODE=COPY"
    "${CMAKE_COMMAND}" --install "${KIN_PACKAGE_BUILD_DIR}"
    --config "${KIN_PACKAGE_CONFIG}" --component "${KIN_PACKAGE_COMPONENT}" --prefix "${stage}" ${strip_argument}
    COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${stage}/${KIN_PACKAGE_EXECUTABLE}")
    message(FATAL_ERROR "Package install did not produce ${KIN_PACKAGE_EXECUTABLE}")
endif()
if(KIN_PACKAGE_ASSET_MANIFEST)
    execute_process(COMMAND "${KIN_PACKAGE_ASSET_PYTHON}" "${KIN_PACKAGE_ASSET_TOOL}"
        --assets "${stage}/assets/${KIN_PACKAGE_ASSET_NAME}"
        --manifest "${KIN_PACKAGE_ASSET_MANIFEST}" --output "${stage}.assets.json"
        COMMAND_ERROR_IS_FATAL ANY)
endif()
if(KIN_PACKAGE_COOK_TOOL)
    # Ensure the cooked provenance describes exactly what was installed, even
    # if a separate cooker invocation updated its output during installation.
    file(READ "${KIN_PACKAGE_COOK_REPORT}" cook_report)
    string(JSON cooked_count LENGTH "${cook_report}" files)
    if(cooked_count GREATER 0)
        math(EXPR cooked_last "${cooked_count} - 1")
        foreach(index RANGE 0 ${cooked_last})
            string(JSON path GET "${cook_report}" files ${index} path)
            string(JSON expected GET "${cook_report}" files ${index} sha256)
            file(SHA256 "${stage}/assets/${KIN_PACKAGE_ASSET_NAME}/${path}" actual)
            if(NOT actual STREQUAL expected)
                message(FATAL_ERROR "Cooked asset changed during packaging: ${path}")
            endif()
        endforeach()
    endif()
endif()
if(KIN_PACKAGE_DEBUG_SYMBOLS)
    include("${CMAKE_CURRENT_LIST_DIR}/package_symbols.cmake")
endif()
file(GLOB_RECURSE files LIST_DIRECTORIES FALSE RELATIVE "${stage}" "${stage}/*")
list(SORT files)
set(manifest "")
foreach(path IN LISTS files)
    file(SHA256 "${stage}/${path}" digest)
    string(APPEND manifest "${digest}  ${path}\n")
endforeach()
file(WRITE "${stage}/SHA256SUMS" "${manifest}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${KIN_PACKAGE_NAME}.zip" --format=zip "${KIN_PACKAGE_NAME}"
    WORKING_DIRECTORY "${output}" COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "Desktop package: ${output}/${KIN_PACKAGE_NAME}.zip")

if(KIN_PACKAGE_ASSET_MANIFEST)
    file(READ "${stage}.assets.json" asset_report)
    file(SHA256 "${stage}.zip" package_sha)
    string(JSON asset_report SET "${asset_report}" package_sha256 "\"${package_sha}\"")
    file(WRITE "${stage}.assets.json" "${asset_report}\n")
    message(STATUS "Asset audit: ${stage}.assets.json (no pruning)")
endif()
if(KIN_PACKAGE_COOK_TOOL)
    file(READ "${KIN_PACKAGE_COOK_REPORT}" cook_report)
    file(SHA256 "${stage}.zip" package_sha)
    string(JSON cook_report SET "${cook_report}" package_sha256 "\"${package_sha}\"")
    file(WRITE "${stage}.cook.json" "${cook_report}\n")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/package_size.cmake")
if(KIN_PACKAGE_DEBUG_SYMBOLS)
    # Hash the final ZIP too: symbols must identify the exact player artifact.
    file(SHA256 "${stage}.zip" package_sha)
    kin_json_string(quoted "${package_sha}")
    string(JSON symbol_manifest SET "${symbol_manifest}" package_sha256 "${quoted}")
    file(WRITE "${symbols}/manifest.json" "${symbol_manifest}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${KIN_PACKAGE_NAME}-symbols.zip"
        --format=zip "${KIN_PACKAGE_NAME}-symbols"
        WORKING_DIRECTORY "${output}" COMMAND_ERROR_IS_FATAL ANY)
    message(STATUS "Debug symbols: ${symbols}.zip")
endif()
