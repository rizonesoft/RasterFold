cmake_minimum_required(VERSION 3.20)
project(lcms2_static LANGUAGES C)

if(NOT DEFINED VENDOR_SOURCE_DIR OR NOT EXISTS "${VENDOR_SOURCE_DIR}/include/lcms2.h")
  message(FATAL_ERROR "VENDOR_SOURCE_DIR must point to pinned lcms2 source tree")
endif()

set(_lcms2_sources
  cmscnvrt.c
  cmserr.c
  cmsgamma.c
  cmsgmt.c
  cmsintrp.c
  cmsio0.c
  cmsio1.c
  cmslut.c
  cmsplugin.c
  cmssm.c
  cmsmd5.c
  cmsmtrx.c
  cmspack.c
  cmspcs.c
  cmswtpnt.c
  cmsxform.c
  cmssamp.c
  cmsnamed.c
  cmscam02.c
  cmsvirt.c
  cmstypes.c
  cmscgats.c
  cmsps2.c
  cmsopt.c
  cmshalf.c
  cmsalpha.c
)
list(TRANSFORM _lcms2_sources PREPEND "${VENDOR_SOURCE_DIR}/src/")

add_library(lcms2 STATIC ${_lcms2_sources})
set_target_properties(lcms2 PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  OUTPUT_NAME lcms2
)
target_compile_definitions(lcms2 PRIVATE CMS_NO_REGISTER_KEYWORD=1)
target_include_directories(lcms2 PUBLIC "${VENDOR_SOURCE_DIR}/include")

include(GNUInstallDirs)
install(TARGETS lcms2 ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES
  "${VENDOR_SOURCE_DIR}/include/lcms2.h"
  "${VENDOR_SOURCE_DIR}/include/lcms2_plugin.h"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
