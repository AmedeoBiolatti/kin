include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/kin_pgo.cmake")

# Older dependency projects otherwise reset CMP0069 and silently ignore IPO.
# Do not override an upstream project that explicitly selects OLD.
if(KIN_ENABLE_LTO)
    set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
endif()

# MSVC debug information for packaged symbols. /Zi writes every compile to a
# shared PDB, which ccache cannot cache and parallel compiles under Ninja contend
# for; /Z7 keeps it in the object files and the link writes the one PDB that ships.
if(CMAKE_C_COMPILER_LAUNCHER OR CMAKE_CXX_COMPILER_LAUNCHER)
    set(KIN_MSVC_DEBUG_INFORMATION /Z7 CACHE INTERNAL "MSVC debug information format")
else()
    set(KIN_MSVC_DEBUG_INFORMATION /Zi CACHE INTERNAL "MSVC debug information format")
endif()

# With GCC, each link-time-optimized executable re-optimizes the whole engine,
# about two CPU-minutes, and tests are dozens of executables: a CI build of
# them spent 16 minutes linking. Tests gain nothing from it. -ffat-lto-objects
# makes every object hold its normal code beside the LTO form, so a test links as
# an ordinary program (KIN_LINK_WITHOUT_LTO, set by kin_add_test) while the
# executables that ship are still optimized from the same objects. The tests'
# sources are still compiled with -flto, as the release audit checks.
if(KIN_ENABLE_LTO AND KIN_BUILD_TESTS AND NOT KIN_LTO_TESTS AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(KIN_LTO_FAT_OBJECTS ON CACHE INTERNAL "Objects keep their normal code beside the LTO form")
else()
    set(KIN_LTO_FAT_OBJECTS OFF CACHE INTERNAL "Objects keep their normal code beside the LTO form")
endif()

function(kin_apply_release_policy target)
    get_target_property(applied ${target} KIN_RELEASE_POLICY_APPLIED)
    get_target_property(imported ${target} IMPORTED)
    get_target_property(type ${target} TYPE)
    if(applied OR imported OR NOT type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
        return()
    endif()
    set_property(TARGET ${target} PROPERTY KIN_RELEASE_POLICY_APPLIED TRUE)
    set(optimized "$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>")
    if(KIN_PGO_MODE STREQUAL "generate")
        target_compile_options(${target} PRIVATE "${KIN_PGO_GENERATE_FLAG}" -fprofile-update=atomic)
        target_link_options(${target} PRIVATE "${KIN_PGO_GENERATE_FLAG}")
    elseif(KIN_PGO_MODE STREQUAL "use")
        target_compile_options(${target} PRIVATE "${KIN_PGO_USE_FLAG}" ${KIN_PGO_USE_DIAGNOSTICS})
        target_link_options(${target} PRIVATE "${KIN_PGO_USE_FLAG}")
        add_dependencies(${target} kin_pgo_verify)
    endif()
    if(KIN_ENABLE_LTO)
        foreach(config RELEASE RELWITHDEBINFO MINSIZEREL)
            set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_${config} ON)
        endforeach()
        if(KIN_LTO_FAT_OBJECTS)
            target_compile_options(${target} PRIVATE "$<$<AND:${optimized},$<COMPILE_LANGUAGE:C,CXX>>:-ffat-lto-objects>")
            get_target_property(without_lto ${target} KIN_LINK_WITHOUT_LTO)
            if(without_lto)
                # Later than the -flto the IPO property adds to the link line.
                target_link_options(${target} PRIVATE "$<${optimized}:-fno-lto;-fno-use-linker-plugin>")
            endif()
        endif()
    endif()
    if(KIN_ENABLE_SIZE_OPTIMIZATIONS)
        target_compile_options(${target} PRIVATE
            "$<$<AND:${optimized},$<OR:$<COMPILE_LANG_AND_ID:C,GNU,Clang>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>>:-ffunction-sections;-fdata-sections>"
            "$<$<AND:${optimized},$<OR:$<COMPILE_LANG_AND_ID:C,MSVC>,$<COMPILE_LANG_AND_ID:CXX,MSVC>>>:/Gy;/Gw>")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_link_options(${target} PRIVATE "$<${optimized}:LINKER:--gc-sections>")
        elseif(MSVC)
            target_link_options(${target} PRIVATE "$<${optimized}:/OPT:REF;/OPT:ICF>")
        endif()
    endif()
    if(KIN_PACKAGE_DEBUG_SYMBOLS)
        # Add debug information without changing Release/MinSizeRel optimization.
        target_compile_options(${target} PRIVATE
            "$<$<AND:${optimized},$<OR:$<COMPILE_LANG_AND_ID:C,GNU,Clang>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>>:-g>"
            "$<$<AND:${optimized},$<OR:$<COMPILE_LANG_AND_ID:C,MSVC>,$<COMPILE_LANG_AND_ID:CXX,MSVC>>>:${KIN_MSVC_DEBUG_INFORMATION}>")
        if(MSVC)
            target_link_options(${target} PRIVATE "$<${optimized}:/DEBUG:FULL;/OPT:REF;/OPT:ICF>")
        endif()
    endif()
endfunction()

function(kin_apply_release_tree directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        kin_apply_release_policy(${target})
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        kin_apply_release_tree("${child}")
    endforeach()
endfunction()
