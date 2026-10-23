# Reproduce a dependency that resets CMP0069 through an older minimum version.
cmake_minimum_required(VERSION 3.5)
add_library(legacy_release_library SHARED marker.c)
