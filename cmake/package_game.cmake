cmake_minimum_required(VERSION 3.22)

if(NOT KIN_PACKAGE_CONFIG MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    message(FATAL_ERROR "Desktop packages require Release, RelWithDebInfo or MinSizeRel, not '${KIN_PACKAGE_CONFIG}'")
endif()
set(output "${KIN_PACKAGE_BUILD_DIR}/packages")
set(stage "${output}/${KIN_PACKAGE_NAME}")
file(MAKE_DIRECTORY "${output}")
file(LOCK "${output}/${KIN_PACKAGE_NAME}.lock" GUARD PROCESS TIMEOUT 60)
# Only this target/configuration's generated staging directory is replaced.
file(REMOVE_RECURSE "${stage}")
# Developer install settings must not turn the package into links to the build
# tree or redirect its files under an unrelated DESTDIR.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DESTDIR "CMAKE_INSTALL_MODE=COPY"
    "${CMAKE_COMMAND}" --install "${KIN_PACKAGE_BUILD_DIR}"
    --config "${KIN_PACKAGE_CONFIG}" --component "${KIN_PACKAGE_COMPONENT}" --prefix "${stage}"
    COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${stage}/${KIN_PACKAGE_EXECUTABLE}")
    message(FATAL_ERROR "Package install did not produce ${KIN_PACKAGE_EXECUTABLE}")
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
