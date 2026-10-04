include(FetchContent)

set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.10
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(SDL3)

set(FLECS_STATIC ON CACHE BOOL "" FORCE)
set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
set(FLECS_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(flecs
    GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
    GIT_TAG v4.0.5
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(flecs)

if(TARGET flecs_static)
    get_target_property(_flecs_include_dirs flecs_static INTERFACE_INCLUDE_DIRECTORIES)
    if(_flecs_include_dirs)
        set_property(TARGET flecs_static PROPERTY
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_flecs_include_dirs}"
        )
    endif()
    target_compile_options(flecs_static INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/external:W0>
    )
endif()

set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BOX2D_USER_SETTINGS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(box2d
    GIT_REPOSITORY https://github.com/erincatto/box2d.git
    GIT_TAG v2.4.1
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(box2d)

FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.4.6.tar.gz
    URL_HASH SHA256=7d5ea1b9cb6aa0b59ca3dde1c6adcb57ef83a1ba8e5432c0ecd06bf439b3ad88
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_GetProperties(lua)
if(NOT lua_POPULATED)
    FetchContent_Populate(lua)
    add_library(lua_static STATIC
        "${lua_SOURCE_DIR}/src/lapi.c"
        "${lua_SOURCE_DIR}/src/lauxlib.c"
        "${lua_SOURCE_DIR}/src/lbaselib.c"
        "${lua_SOURCE_DIR}/src/lcode.c"
        "${lua_SOURCE_DIR}/src/lcorolib.c"
        "${lua_SOURCE_DIR}/src/lctype.c"
        "${lua_SOURCE_DIR}/src/ldblib.c"
        "${lua_SOURCE_DIR}/src/ldebug.c"
        "${lua_SOURCE_DIR}/src/ldo.c"
        "${lua_SOURCE_DIR}/src/ldump.c"
        "${lua_SOURCE_DIR}/src/lfunc.c"
        "${lua_SOURCE_DIR}/src/lgc.c"
        "${lua_SOURCE_DIR}/src/linit.c"
        "${lua_SOURCE_DIR}/src/liolib.c"
        "${lua_SOURCE_DIR}/src/llex.c"
        "${lua_SOURCE_DIR}/src/lmathlib.c"
        "${lua_SOURCE_DIR}/src/lmem.c"
        "${lua_SOURCE_DIR}/src/loadlib.c"
        "${lua_SOURCE_DIR}/src/lobject.c"
        "${lua_SOURCE_DIR}/src/lopcodes.c"
        "${lua_SOURCE_DIR}/src/loslib.c"
        "${lua_SOURCE_DIR}/src/lparser.c"
        "${lua_SOURCE_DIR}/src/lstate.c"
        "${lua_SOURCE_DIR}/src/lstring.c"
        "${lua_SOURCE_DIR}/src/lstrlib.c"
        "${lua_SOURCE_DIR}/src/ltable.c"
        "${lua_SOURCE_DIR}/src/ltablib.c"
        "${lua_SOURCE_DIR}/src/ltm.c"
        "${lua_SOURCE_DIR}/src/lundump.c"
        "${lua_SOURCE_DIR}/src/lutf8lib.c"
        "${lua_SOURCE_DIR}/src/lvm.c"
        "${lua_SOURCE_DIR}/src/lzio.c"
    )
    add_library(Lua::Lua ALIAS lua_static)
    target_include_directories(lua_static PUBLIC "${lua_SOURCE_DIR}/src")
    target_compile_definitions(lua_static PUBLIC LUA_COMPAT_5_3)
endif()

FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG v3.3.1
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(sol2)

set(SDLIMAGE_VENDORED ON CACHE BOOL "" FORCE)
set(SDLIMAGE_DEPS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)

set(_sdl3_shim "${CMAKE_BINARY_DIR}/sdl3_shim")
file(MAKE_DIRECTORY "${_sdl3_shim}")
file(WRITE "${_sdl3_shim}/SDL3Config.cmake" [=[
set(SDL3_FOUND TRUE)
set(SDL3_VERSION "3.4.10")
set(SDL3_Headers_FOUND TRUE)
set(SDL3_SDL3-static_FOUND TRUE)
set(SDL3_SDL3_FOUND TRUE)
if(NOT TARGET SDL3::Headers)
    add_library(SDL3_Headers_shim INTERFACE)
    add_library(SDL3::Headers ALIAS SDL3_Headers_shim)
endif()
if(NOT TARGET SDL3::SDL3)
    add_library(SDL3::SDL3 ALIAS SDL3-static)
endif()
if(NOT TARGET SDL3::SDL3-shared)
    add_library(SDL3::SDL3-shared ALIAS SDL3-static)
endif()
]=])
file(WRITE "${_sdl3_shim}/SDL3ConfigVersion.cmake" [=[
set(PACKAGE_VERSION "3.4.10")
if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
]=])
set(SDL3_DIR "${_sdl3_shim}" CACHE PATH "" FORCE)

FetchContent_Declare(SDL3_image
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
    GIT_TAG release-3.4.4
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(SDL3_image)

set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDLTTF_DEPS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG release-3.2.2
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(SDL3_ttf)
