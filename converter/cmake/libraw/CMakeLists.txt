cmake_minimum_required(VERSION 3.20)
project(libraw_static LANGUAGES CXX)

if(NOT DEFINED VENDOR_SOURCE_DIR OR NOT EXISTS "${VENDOR_SOURCE_DIR}/libraw/libraw.h")
  message(FATAL_ERROR "VENDOR_SOURCE_DIR must point to pinned libraw source tree")
endif()

set(_libraw_sources
  src/libraw_c_api.cpp
  src/libraw_datastream.cpp
  src/decoders/canon_600.cpp
  src/decoders/crx.cpp
  src/decoders/pana8.cpp
  src/decoders/decoders_dcraw.cpp
  src/decoders/sonycc.cpp
  src/decompressors/losslessjpeg.cpp
  src/decoders/decoders_libraw_dcrdefs.cpp
  src/decoders/olympus14.cpp
  src/decoders/decoders_libraw.cpp
  src/decoders/dng.cpp
  src/decoders/fp_dng.cpp
  src/decoders/fuji_compressed.cpp
  src/decoders/generic.cpp
  src/decoders/kodak_decoders.cpp
  src/decoders/load_mfbacks.cpp
  src/decoders/smal.cpp
  src/decoders/unpack_thumb.cpp
  src/decoders/unpack.cpp
  src/demosaic/aahd_demosaic.cpp
  src/demosaic/ahd_demosaic.cpp
  src/demosaic/dcb_demosaic.cpp
  src/demosaic/dht_demosaic.cpp
  src/demosaic/misc_demosaic.cpp
  src/demosaic/xtrans_demosaic.cpp
  src/integration/dngsdk_glue.cpp
  src/integration/rawspeed_glue.cpp
  src/metadata/adobepano.cpp
  src/metadata/canon.cpp
  src/metadata/ciff.cpp
  src/metadata/cr3_parser.cpp
  src/metadata/epson.cpp
  src/metadata/exif_gps.cpp
  src/metadata/fuji.cpp
  src/metadata/identify_tools.cpp
  src/metadata/identify.cpp
  src/metadata/kodak.cpp
  src/metadata/leica.cpp
  src/metadata/makernotes.cpp
  src/metadata/mediumformat.cpp
  src/metadata/minolta.cpp
  src/metadata/misc_parsers.cpp
  src/metadata/nikon.cpp
  src/metadata/normalize_model.cpp
  src/metadata/olympus.cpp
  src/metadata/hasselblad_model.cpp
  src/metadata/p1.cpp
  src/metadata/pentax.cpp
  src/metadata/samsung.cpp
  src/metadata/sony.cpp
  src/metadata/tiff.cpp
  src/postprocessing/aspect_ratio.cpp
  src/postprocessing/dcraw_process.cpp
  src/postprocessing/mem_image.cpp
  src/postprocessing/postprocessing_aux.cpp
  src/postprocessing/postprocessing_utils_dcrdefs.cpp
  src/postprocessing/postprocessing_utils.cpp
  src/preprocessing/ext_preprocess.cpp
  src/preprocessing/raw2image.cpp
  src/preprocessing/subtract_black.cpp
  src/tables/cameralist.cpp
  src/tables/colorconst.cpp
  src/tables/colordata.cpp
  src/tables/wblists.cpp
  src/utils/curves.cpp
  src/utils/decoder_info.cpp
  src/utils/init_close_utils.cpp
  src/utils/open.cpp
  src/utils/phaseone_processing.cpp
  src/utils/read_utils.cpp
  src/utils/thumb_utils.cpp
  src/utils/utils_dcraw.cpp
  src/utils/utils_libraw.cpp
  src/write/apply_profile.cpp
  src/write/file_write.cpp
  src/write/tiff_writer.cpp
  src/x3f/x3f_parse_process.cpp
  src/x3f/x3f_utils_patched.cpp
)
list(TRANSFORM _libraw_sources PREPEND "${VENDOR_SOURCE_DIR}/")

find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)

add_library(raw STATIC ${_libraw_sources})
set_target_properties(raw PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  OUTPUT_NAME raw
)
target_compile_definitions(raw
  PUBLIC LIBRAW_NODLL
  PRIVATE USE_LCMS2 USE_JPEG USE_ZLIB
)
target_compile_options(raw PRIVATE -w)
target_include_directories(raw PUBLIC
  "${VENDOR_SOURCE_DIR}"
  "${RF_DEPS_PREFIX}/include"
)
target_link_libraries(raw PUBLIC ZLIB::ZLIB JPEG::JPEG "${RF_DEPS_PREFIX}/lib/liblcms2.a")
if(WIN32)
  target_link_libraries(raw PUBLIC ws2_32)
endif()

include(GNUInstallDirs)
install(TARGETS raw ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES
  "${VENDOR_SOURCE_DIR}/libraw/libraw.h"
  "${VENDOR_SOURCE_DIR}/libraw/libraw_alloc.h"
  "${VENDOR_SOURCE_DIR}/libraw/libraw_const.h"
  "${VENDOR_SOURCE_DIR}/libraw/libraw_datastream.h"
  "${VENDOR_SOURCE_DIR}/libraw/libraw_internal.h"
  "${VENDOR_SOURCE_DIR}/libraw/libraw_types.h"
  "${VENDOR_SOURCE_DIR}/libraw/libraw_version.h"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/libraw"
)
