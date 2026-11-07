cmake_minimum_required(VERSION 3.13)
project(kmsvnc LANGUAGES C)

IF(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_search_module(LIBDRM REQUIRED libdrm)
# libvncserver: пакет dno-tool встраивает закреплённую версию статически
# (KMSVNC_LIBVNCSERVER_PREFIX — каталог её установки). Системная библиотека МОС 12
# и МОС 13 различается раскладкой структур под одним soname, а 0.9.13 теряет
# буферы Zlib при каждом отключении клиента. Без префикса берётся системная
# (только для локальной сборки).
set(KMSVNC_LIBVNCSERVER_PREFIX "" CACHE PATH "Каталог установки статической libvncserver")
if(KMSVNC_LIBVNCSERVER_PREFIX)
  find_library(LIBVNCSERVER_STATIC NAMES libvncserver.a
    PATHS "${KMSVNC_LIBVNCSERVER_PREFIX}/lib" "${KMSVNC_LIBVNCSERVER_PREFIX}/lib64" NO_DEFAULT_PATH)
  if(NOT LIBVNCSERVER_STATIC)
    message(FATAL_ERROR "libvncserver.a не найдена в ${KMSVNC_LIBVNCSERVER_PREFIX}")
  endif()
  pkg_search_module(ZLIB REQUIRED zlib)
  pkg_search_module(JPEG REQUIRED libjpeg)
  pkg_search_module(PNG REQUIRED libpng)
  set(LIBVNCSERVER_INCLUDE_DIRS "${KMSVNC_LIBVNCSERVER_PREFIX}/include")
  set(LIBVNCSERVER_LIBRARIES "${LIBVNCSERVER_STATIC}" ${JPEG_LIBRARIES} ${PNG_LIBRARIES} ${ZLIB_LIBRARIES} Threads::Threads)
  message(STATUS "libvncserver: встроенная ${LIBVNCSERVER_STATIC}")
else()
  pkg_search_module(LIBVNCSERVER REQUIRED libvncserver)
  message(WARNING "libvncserver: системная (для пакета задайте KMSVNC_LIBVNCSERVER_PREFIX)")
endif()
pkg_search_module(XKBCOMMON REQUIRED xkbcommon)
pkg_search_module(LIBVA REQUIRED libva)
pkg_search_module(LIBVA_DRM REQUIRED libva-drm)
pkg_search_module(EGL REQUIRED egl)
pkg_search_module(GLES REQUIRED glesv2)
pkg_search_module(GBM REQUIRED gbm)
pkg_search_module(JSONC REQUIRED json-c)

add_executable(kmsvnc)
set(kmsvnc_SOURCES kmsvnc.c drm.c input.c keymap.c va.c drm_master.c egl_capture.c screen_layout.c thumbnail.c)

include(CheckIncludeFiles)
CHECK_INCLUDE_FILES("linux/uinput.h;linux/dma-buf.h" HAVE_LINUX_API_HEADERS)
IF(NOT HAVE_LINUX_API_HEADERS)
  message(FATAL_ERROR "linux-api-headers not found")
ENDIF()

include(CheckSymbolExists)
check_symbol_exists(SYS_pidfd_getfd "sys/syscall.h" HAVE_LIBC_SYS_pidfd_getfd)
IF(NOT HAVE_LIBC_SYS_pidfd_getfd)
  message(WARNING "pidfd_getfd syscall not found, the --screen-blank options will be disabled")
  target_compile_options(kmsvnc PUBLIC -DDISABLE_KMSVNC_SCREEN_BLANK)
  list(REMOVE_ITEM kmsvnc_SOURCES drm_master.c)
ENDIF()
include(CMakePushCheckState)
cmake_push_check_state()
set(CMAKE_REQUIRED_INCLUDES ${LIBDRM_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${LIBDRM_LIBRARIES})
check_symbol_exists(drmGetFormatName "xf86drm.h" HAVE_LIBDRM_drmGetFormatName)
cmake_pop_check_state()
IF(NOT HAVE_LIBDRM_drmGetFormatName)
  message(WARNING "drmGetFormatName not found, format name printing will be disabled")
  target_compile_options(kmsvnc PUBLIC -DDISABLE_KMSVNC_drmGetFormatName)
ENDIF()


target_sources(kmsvnc PUBLIC
    ${kmsvnc_SOURCES}
)
target_include_directories(kmsvnc PUBLIC
  ${LIBDRM_INCLUDE_DIRS}
  ${LIBVNCSERVER_INCLUDE_DIRS}
  ${XKBCOMMON_INCLUDE_DIRS}
  ${LIBVA_INCLUDE_DIRS}
  ${LIBVA_DRM_INCLUDE_DIRS}
  ${EGL_INCLUDE_DIRS}
  ${GLES_INCLUDE_DIRS}
  ${GBM_INCLUDE_DIRS}
  ${JSONC_INCLUDE_DIRS}
)
target_link_libraries(kmsvnc PUBLIC
  Threads::Threads
  m
  ${LIBDRM_LIBRARIES}
  ${LIBVNCSERVER_LIBRARIES}
  ${XKBCOMMON_LIBRARIES}
  ${LIBVA_LIBRARIES}
  ${LIBVA_DRM_LIBRARIES}
  ${EGL_LIBRARIES}
  ${GLES_LIBRARIES}
  ${GBM_LIBRARIES}
  ${JSONC_LIBRARIES}
)
install(TARGETS kmsvnc RUNTIME DESTINATION bin)
