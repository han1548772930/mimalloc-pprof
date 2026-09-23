cmake_minimum_required(VERSION 3.16)
project(arena_reclaim_bench C CXX)

# Keep this out of the allocator's default build. The Python benchmark driver
# configures this small wrapper and delegates the platform compiler setup to
# CMake (especially native MSVC, whose `cl` is not on Git Bash's PATH).
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." mimalloc)
set(BENCH_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../bench_arena_reclaim.c")
set_source_files_properties("${BENCH_SOURCE}" PROPERTIES LANGUAGE CXX)
add_executable(bench-arena-reclaim "${BENCH_SOURCE}")
target_compile_definitions(bench-arena-reclaim PRIVATE MI_STATIC_LIB)
if(MI_USE_CXX)
  # internal.h declarations use C++ linkage when mimalloc itself is built as C++
  # (native MSVC/clang-cl). The benchmark must agree for its internal stats hook.
  target_compile_definitions(bench-arena-reclaim PRIVATE MI_USE_CXX)
endif()
target_include_directories(bench-arena-reclaim PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../include")
target_link_libraries(bench-arena-reclaim PRIVATE mimalloc-static ${mi_libraries})
if(WIN32)
  target_link_libraries(bench-arena-reclaim PRIVATE psapi)
endif()
