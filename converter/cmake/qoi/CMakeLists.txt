cmake_minimum_required(VERSION 3.20)
project(qoi_static LANGUAGES C)

if(NOT DEFINED VENDOR_SOURCE_DIR OR NOT EXISTS "${VENDOR_SOURCE_DIR}/qoi.h")
  message(FATAL_ERROR "VENDOR_SOURCE_DIR must point to pinned qoi source tree containing qoi.h")
endif()

set(_qoi_c "${CMAKE_CURRENT_BINARY_DIR}/qoi_impl.c")
file(WRITE "${_qoi_c}" "#define QOI_IMPLEMENTATION\n#include \"qoi.h\"\n")

add_library(qoi STATIC "${_qoi_c}")
set_target_properties(qoi PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  OUTPUT_NAME qoi
)
target_include_directories(qoi PUBLIC "${VENDOR_SOURCE_DIR}")

include(GNUInstallDirs)
install(TARGETS qoi ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${VENDOR_SOURCE_DIR}/qoi.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
