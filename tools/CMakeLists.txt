# kin_pack: makes, lists, checks and unpacks .kinpak content packs
# (docs/shipping.md). kin_game's <game>_content targets run it.
add_executable(kin_pack kin_pack/main.cpp)
target_link_libraries(kin_pack PRIVATE kin::engine)
target_compile_options(kin_pack PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
kin_enable_release_optimizations(kin_pack)
