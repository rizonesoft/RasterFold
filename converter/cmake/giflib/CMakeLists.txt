cmake_minimum_required(VERSION 3.20)
project(giflib_static LANGUAGES C)

if(NOT DEFINED VENDOR_SOURCE_DIR OR NOT EXISTS "${VENDOR_SOURCE_DIR}/gif_lib.h")
  message(FATAL_ERROR "VENDOR_SOURCE_DIR must point to pinned giflib source tree")
endif()

set(_gif_sources
  dgif_lib.c
  egif_lib.c
  gifalloc.c
  gif_err.c
  gif_font.c
  gif_hash.c
  openbsd-reallocarray.c
  quantize.c
)
list(TRANSFORM _gif_sources PREPEND "${VENDOR_SOURCE_DIR}/")

add_library(gif STATIC ${_gif_sources})
set_target_properties(gif PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS ON
  OUTPUT_NAME gif
)
target_include_directories(gif PUBLIC "${VENDOR_SOURCE_DIR}")

include(GNUInstallDirs)
install(TARGETS gif ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${VENDOR_SOURCE_DIR}/gif_lib.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
