cmake_minimum_required(VERSION 3.20)
project(rasterfold_converter LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Reserved for the C++23 Windows desktop UI target (D10 T03 s1) and C++ static runtime linkage.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED RF_DEPS_PREFIX)
  message(FATAL_ERROR "RF_DEPS_PREFIX must point to the installed converter dependency prefix")
endif()
if(NOT DEFINED RF_LIBRASTERFOLD OR NOT EXISTS "${RF_LIBRASTERFOLD}")
  message(FATAL_ERROR "RF_LIBRASTERFOLD must point to bin/<target>/lib/librasterfold.a")
endif()
if(NOT DEFINED RF_CONVERTER_BIN_DIR)
  set(RF_CONVERTER_BIN_DIR "${CMAKE_SOURCE_DIR}/../bin/converter/windows-x64")
endif()

function(rf_find_static_lib out_var)
  find_library(${out_var}
    NAMES ${ARGN}
    PATHS "${RF_DEPS_PREFIX}/lib"
    NO_DEFAULT_PATH
    REQUIRED
  )
endfunction()

find_package(Imath CONFIG REQUIRED)
find_package(openjph CONFIG REQUIRED)
find_package(OpenEXR CONFIG REQUIRED)

rf_find_static_lib(RF_LIB_Z libz.a z)
rf_find_static_lib(RF_LIB_DEFLATE libdeflate.a deflate)
rf_find_static_lib(RF_LIB_PNG libpng16.a libpng.a png16 png)
rf_find_static_lib(RF_LIB_WEBP libwebp.a webp)
rf_find_static_lib(RF_LIB_WEBPDEMUX libwebpdemux.a webpdemux)
rf_find_static_lib(RF_LIB_WEBPMUX libwebpmux.a webpmux)
rf_find_static_lib(RF_LIB_SHARPYUV libsharpyuv.a sharpyuv)
rf_find_static_lib(RF_LIB_HWY libhwy.a hwy)
rf_find_static_lib(RF_LIB_BROTLICOMMON libbrotlicommon.a brotlicommon)
rf_find_static_lib(RF_LIB_BROTLIDEC libbrotlidec.a brotlidec)
rf_find_static_lib(RF_LIB_BROTLIENC libbrotlienc.a brotlienc)
rf_find_static_lib(RF_LIB_LCMS2 liblcms2.a lcms2)
rf_find_static_lib(RF_LIB_JXL libjxl.a jxl)
rf_find_static_lib(RF_LIB_JXL_THREADS libjxl_threads.a jxl_threads)
rf_find_static_lib(RF_LIB_JXL_CMS libjxl_cms.a jxl_cms)
rf_find_static_lib(RF_LIB_AOM libaom.a aom)
rf_find_static_lib(RF_LIB_JPEG libjpeg.a jpeg)
rf_find_static_lib(RF_LIB_TURBOJPEG libturbojpeg.a turbojpeg)
rf_find_static_lib(RF_LIB_AVIF libavif.a avif)
rf_find_static_lib(RF_LIB_TIFF libtiff.a tiff)
rf_find_static_lib(RF_LIB_GIF libgif.a gif)
rf_find_static_lib(RF_LIB_QOI libqoi.a qoi)
rf_find_static_lib(RF_LIB_OPENJP2 libopenjp2.a openjp2)
rf_find_static_lib(RF_LIB_RAW libraw.a raw)

add_library(rfc_engine STATIC
  "${CMAKE_SOURCE_DIR}/src/registry.c"
  "${CMAKE_SOURCE_DIR}/src/limits.c"
  "${CMAKE_SOURCE_DIR}/src/verify.c"
  "${CMAKE_SOURCE_DIR}/src/metadata.c"
  "${CMAKE_SOURCE_DIR}/src/preview.c"
  "${CMAKE_SOURCE_DIR}/src/job.c"
  "${CMAKE_SOURCE_DIR}/src/scheduler.c"
)
set_target_properties(rfc_engine PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)
target_include_directories(rfc_engine PUBLIC
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/../include"
)
target_compile_options(rfc_engine PRIVATE -Wall -Wextra -Werror)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(rfc_engine PUBLIC Threads::Threads)
endif()

add_executable(rfc_test_jobs
  "${CMAKE_SOURCE_DIR}/../tests/converter/test_jobs.c"
)
set_target_properties(rfc_test_jobs PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY "${RF_CONVERTER_BIN_DIR}"
)
target_compile_options(rfc_test_jobs PRIVATE -Wall -Wextra -Werror)
target_link_libraries(rfc_test_jobs PRIVATE rfc_engine)
if(WIN32)
  target_link_options(rfc_test_jobs PRIVATE -static -Wl,--gc-sections)
endif()

add_executable(rfc_test_exactness
  "${CMAKE_SOURCE_DIR}/../tests/converter/test_exactness.c"
)
set_target_properties(rfc_test_exactness PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY "${RF_CONVERTER_BIN_DIR}"
)
target_compile_options(rfc_test_exactness PRIVATE -Wall -Wextra -Werror)
target_link_libraries(rfc_test_exactness PRIVATE
  rfc_engine
  "${RF_LIBRASTERFOLD}"
)
if(WIN32)
  target_link_options(rfc_test_exactness PRIVATE -static -Wl,--gc-sections)
endif()

add_executable(rfc_bundle_smoke
  "${CMAKE_SOURCE_DIR}/src/bundle_smoke.c"
)
set_target_properties(rfc_bundle_smoke PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  LINKER_LANGUAGE CXX
  RUNTIME_OUTPUT_DIRECTORY "${RF_CONVERTER_BIN_DIR}"
)
target_compile_options(rfc_bundle_smoke PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(rfc_bundle_smoke PRIVATE
  JXL_STATIC_DEFINE
  JXL_THREADS_STATIC_DEFINE
  JXL_CMS_STATIC_DEFINE
  OPJ_STATIC
  LIBRAW_NODLL
  QOI_NO_STDIO
)
target_include_directories(rfc_bundle_smoke PRIVATE
  "${RF_DEPS_PREFIX}/include"
  "${RF_DEPS_PREFIX}/include/openjpeg-2.5"
  "${RF_DEPS_PREFIX}/include/OpenEXR"
  "${RF_DEPS_PREFIX}/include/Imath"
)

target_link_libraries(rfc_bundle_smoke PRIVATE
  rfc_engine
  "${RF_LIBRASTERFOLD}"
  "${RF_LIB_PNG}"
  "${RF_LIB_WEBP}"
  "${RF_LIB_WEBPDEMUX}"
  "${RF_LIB_WEBPMUX}"
  "${RF_LIB_SHARPYUV}"
  "${RF_LIB_JXL}"
  "${RF_LIB_JXL_THREADS}"
  "${RF_LIB_JXL_CMS}"
  "${RF_LIB_HWY}"
  "${RF_LIB_BROTLIENC}"
  "${RF_LIB_BROTLIDEC}"
  "${RF_LIB_BROTLICOMMON}"
  "${RF_LIB_AVIF}"
  "${RF_LIB_AOM}"
  "${RF_LIB_TIFF}"
  "${RF_LIB_TURBOJPEG}"
  "${RF_LIB_JPEG}"
  "${RF_LIB_GIF}"
  "${RF_LIB_QOI}"
  "${RF_LIB_OPENJP2}"
  OpenEXR::OpenEXRCore
  openjph
  Imath::Imath
  "${RF_LIB_RAW}"
  "${RF_LIB_LCMS2}"
  "${RF_LIB_DEFLATE}"
  "${RF_LIB_Z}"
  ws2_32
  bcrypt
)

target_link_options(rfc_bundle_smoke PRIVATE
  -static
  -Wl,--gc-sections
)
