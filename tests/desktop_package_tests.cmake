cmake_minimum_required(VERSION 3.22)

# Generate the package twice: stale files from an older export must not survive.
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${PACKAGE_SCRIPT}" COMMAND_ERROR_IS_FATAL ANY)
set(stage "${BUILD_DIR}/packages/${PACKAGE_NAME}")
file(WRITE "${stage}/stale-from-previous-export.txt" "must not ship")
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${PACKAGE_SCRIPT}" COMMAND_ERROR_IS_FATAL ANY)
if(EXISTS "${stage}/stale-from-previous-export.txt")
    message(FATAL_ERROR "Package kept a stale file")
endif()

set(scratch "${TEST_DIR}/desktop-package-${CONFIG}")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/unpacked" "${scratch}/unrelated cwd")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${stage}.zip"
    WORKING_DIRECTORY "${scratch}/unpacked" COMMAND_ERROR_IS_FATAL ANY)
set(relocated "${scratch}/relocated game")
file(RENAME "${scratch}/unpacked/${PACKAGE_NAME}" "${relocated}")
foreach(required SHA256SUMS licenses/kin/LICENSE assets/package_fixture/marker.txt)
    if(NOT EXISTS "${relocated}/${required}")
        message(FATAL_ERROR "Missing package content: ${required}")
    endif()
endforeach()
file(STRINGS "${relocated}/SHA256SUMS" manifest)
foreach(entry IN LISTS manifest)
    string(SUBSTRING "${entry}" 0 64 expected)
    string(SUBSTRING "${entry}" 66 -1 path)
    file(SHA256 "${relocated}/${path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Package checksum mismatch: ${path}")
    endif()
endforeach()
set(backend sdl)
set(arguments)
if(DEFINED ENV{KIN_REQUIRE_GPU_TESTS} AND NOT "$ENV{KIN_REQUIRE_GPU_TESTS}" STREQUAL "0"
    AND NOT "$ENV{KIN_REQUIRE_GPU_TESTS}" STREQUAL "")
    set(backend gpu)
    set(arguments --gpu)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KIN_RENDER_BACKEND=${backend}"
    "${relocated}/${EXECUTABLE}" ${arguments}
    WORKING_DIRECTORY "${scratch}/unrelated cwd"
    COMMAND_ERROR_IS_FATAL ANY TIMEOUT 60)
# Losing an asset must be an observable failure, even on the build machine.
file(REMOVE "${relocated}/assets/package_fixture/marker.txt")
execute_process(COMMAND "${relocated}/${EXECUTABLE}"
    WORKING_DIRECTORY "${scratch}/unrelated cwd" RESULT_VARIABLE missing TIMEOUT 20)
if(missing STREQUAL "0")
    message(FATAL_ERROR "Package unexpectedly succeeded without its assets")
endif()
