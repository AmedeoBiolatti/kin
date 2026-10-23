include_guard(GLOBAL)

# SDL intentionally rejects -DSDL_DYNAMIC_API=0. Patch only a private copy of
# the pinned release, leaving fetched/shared source overrides untouched.
function(kin_prepare_sdl_direct source destination)
    get_filename_component(destination "${destination}" ABSOLUTE)
    set(relative "src/dynapi/SDL_dynapi.h")
    file(READ "${source}/${relative}" header)
    # Git may check out CRLF on Windows; pin content independently of line endings.
    string(REPLACE "\r\n" "\n" header "${header}")
    string(SHA256 digest "${header}")
    if(NOT digest STREQUAL "fb36fb0560027d83410c3762fc6e437ef7aeac9bbf08082f80f6363456aca09d")
        message(FATAL_ERROR
            "KIN_ENABLE_SDL_DYNAMIC_API=OFF requires the unmodified SDL release-3.4.10 "
            "dynamic API header. Review the private patch before upgrading SDL.")
    endif()
    string(REPLACE "#define SDL_DYNAMIC_API 1"
        "// Modified by Kin: direct calls in this experimental static build.\n#define SDL_DYNAMIC_API 0"
        header "${header}")
    # COPY updates changed sources on reconfigure. The patched header is handled
    # separately so an unchanged configuration does not rewrite its timestamp.
    file(COPY "${source}/" DESTINATION "${destination}"
        PATTERN ".git" EXCLUDE PATTERN "SDL_dynapi.h" EXCLUDE)
    file(CONFIGURE OUTPUT "${destination}/${relative}" CONTENT "@header@" @ONLY)
    file(CONFIGURE OUTPUT "${destination}/KIN-SDL-CHANGES.txt" CONTENT
        "Kin experimental SDL variant, based on SDL release-3.4.10.\nThe default SDL_DYNAMIC_API definition in src/dynapi/SDL_dynapi.h is changed from 1 to 0.\nThis allows unused API implementations to be discarded by the static linker.\nSDL3_DYNAMIC_API runtime library replacement is unavailable; SDL fixes require rebuilding the game.\n" @ONLY)
    message(STATUS "Kin: experimental SDL direct API source: ${destination}")
    set(KIN_SDL_DIRECT_SOURCE_DIR "${destination}" PARENT_SCOPE)
endfunction()
