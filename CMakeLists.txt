cmake_minimum_required(VERSION 2.4.6)
include(CheckIncludeFile)
include(CheckLibraryExists)

if(COMMAND cmake_policy)
  # Quash warnings about mixing library search paths
  cmake_policy(SET CMP0003 NEW)
  # Quash warning about pre-processor symbol escaping (which it doesn't
  # seem to actually do)
  cmake_policy(SET CMP0005 OLD)
endif(COMMAND cmake_policy)

# Not every platform has argp.h
check_include_file(argp.h HAVE_ARGP_H)
# Not every platform has (or needs) librt
check_library_exists(rt clock_gettime "" HAVE_LIBRT)
if(HAVE_LIBRT)
  set(LIBRT "rt")
endif()

set(CMAKE_BUILD_TYPE Release)

set(VERSION 0.20110913)
# TODO: not all compilers accept --std=gnu99
add_definitions(--std=gnu99 -DACH_VERSION_STRING=\\"${VERSION}\\")
add_definitions(-Wc++-compat -Wall -Wextra -Wconversion -Wpointer-arith -Wfloat-equal -Wshadow -Wwrite-strings)

if(APPLE)
  add_definitions(-DHAVE_STRLEN)
endif()

include_directories(include)
add_library(ach src/ach.c src/ach_stream.c)
target_link_libraries(ach pthread ${LIBRT})

if(HAVE_ARGP_H)
  add_executable(achcat src/achcat.c)
  target_link_libraries(achcat ach pthread ${LIBRT})

  add_executable(achpipe.bin src/achpipe.c)
  target_link_libraries(achpipe.bin ach pthread ${LIBRT})

  add_executable(ach-bench src/ach-bench.c)
  target_link_libraries(ach-bench ach pthread ${LIBRT} m)
endif()

if(HAVE_LIBRT)
  add_executable(ach-example src/ach-example.c)
  target_link_libraries(ach-example ach pthread ${LIBRT} m)
endif()

