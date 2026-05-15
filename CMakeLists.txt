cmake_minimum_required(VERSION 3.24)

project(chiplets_axi C)

include(FetchContent)
include(ExternalProject)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build/out)
endif()

option(CHIPLETS_FETCH_QEMU "Fetch QEMU source and enable the configured QEMU target" OFF)
set(CHIPLETS_BACKEND_FABRIC "qemu-socket" CACHE STRING "Backend fabric implementation: qemu-socket, linux-devmem, or linux-uio")
set_property(CACHE CHIPLETS_BACKEND_FABRIC PROPERTY STRINGS qemu-socket linux-devmem linux-uio axi devmem uio)


if(CHIPLETS_BACKEND_FABRIC STREQUAL "qemu-socket" OR CHIPLETS_BACKEND_FABRIC STREQUAL "axi")
  set(CHIPLETS_BACKEND_FABRIC_SOURCE src/fabrics/qemu_socket.c)
elseif(CHIPLETS_BACKEND_FABRIC STREQUAL "linux-devmem" OR CHIPLETS_BACKEND_FABRIC STREQUAL "devmem")
  set(CHIPLETS_BACKEND_FABRIC_SOURCE src/fabrics/linux_devmem.c)
elseif(CHIPLETS_BACKEND_FABRIC STREQUAL "linux-uio" OR CHIPLETS_BACKEND_FABRIC STREQUAL "uio")
  set(CHIPLETS_BACKEND_FABRIC_SOURCE src/fabrics/linux_uio.c)
else()
  message(FATAL_ERROR "CHIPLETS_BACKEND_FABRIC must be one of: qemu-socket, linux-devmem, linux-uio")
endif()

FetchContent_Declare(
  ctest
  GIT_REPOSITORY https://github.com/bvdberg/ctest.git
  GIT_TAG master
)
FetchContent_MakeAvailable(ctest)

add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
  -Wall
  -Wextra
  -Werror
  -Wno-missing-braces
  -Wno-unused-function
)

add_library(ctest_header INTERFACE)
target_include_directories(ctest_header INTERFACE ${ctest_SOURCE_DIR})
target_compile_options(ctest_header INTERFACE -Wno-attributes)

add_library(virtio STATIC
  src/virtio.c
)
target_include_directories(virtio PUBLIC src)
target_link_libraries(virtio PRIVATE project_warnings)

add_library(fabric STATIC
  ${CHIPLETS_BACKEND_FABRIC_SOURCE}
)
target_include_directories(fabric PUBLIC src src/fabrics)
target_link_libraries(fabric PRIVATE project_warnings)

add_library(virtio_mmio STATIC
  src/virtio-mmio.c
)
target_include_directories(virtio_mmio PUBLIC src)
target_link_libraries(virtio_mmio PRIVATE project_warnings virtio)

add_executable(virtio-blkd
  src/drivers/virtio-blkd.c
)
target_link_libraries(virtio-blkd PRIVATE project_warnings)
target_link_libraries(virtio-blkd PRIVATE fabric virtio_mmio virtio)

add_executable(virtio-consoled
  src/drivers/virtio-consoled.c
)
target_link_libraries(virtio-consoled PRIVATE project_warnings)
target_link_libraries(virtio-consoled PRIVATE fabric virtio_mmio virtio)

add_executable(uio-membench
  src/tools/uio-membench.c
)
target_link_libraries(uio-membench PRIVATE project_warnings)

add_executable(c-backend-tests
  tests/c-backend-tests.c
  src/drivers/virtio-blkd.c
  src/drivers/virtio-consoled.c
)
target_include_directories(c-backend-tests PRIVATE src src/drivers src/fabrics)
target_compile_definitions(c-backend-tests PRIVATE BACKEND_TEST)
target_link_libraries(c-backend-tests PRIVATE project_warnings ctest_header fabric virtio_mmio virtio)
add_test(NAME c-backend-tests COMMAND c-backend-tests)

install(TARGETS virtio-blkd virtio-consoled uio-membench c-backend-tests RUNTIME DESTINATION bin)

if(CHIPLETS_FETCH_QEMU)
  set(QEMU_SOURCE_DIR ${CMAKE_SOURCE_DIR}/build/external/qemu-src)

  ExternalProject_Add(
    qemu-source
    URL https://download.qemu.org/qemu-10.2.2.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR ${QEMU_SOURCE_DIR}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
  )

  set(QEMU_PATCHED_SRC ${CMAKE_SOURCE_DIR}/build/qemu-src)
  set(QEMU_BUILD_DIR ${CMAKE_SOURCE_DIR}/build/qemu)
  set(QEMU_INSTALL_DIR ${CMAKE_SOURCE_DIR}/build/out/qemu)

  file(GLOB QEMU_PATCHES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/patches/qemu/*.patch)

  ExternalProject_Add(
    qemu
    DEPENDS qemu-source
    SOURCE_DIR ${QEMU_PATCHED_SRC}
    BINARY_DIR ${QEMU_BUILD_DIR}
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND
      ${CMAKE_COMMAND} -E make_directory ${QEMU_PATCHED_SRC}
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${QEMU_PATCHED_SRC}/.git
      COMMAND rsync -a --delete --exclude=build/ ${QEMU_SOURCE_DIR}/ ${QEMU_PATCHED_SRC}/
      COMMAND git -C ${QEMU_PATCHED_SRC} init -q
      COMMAND git -C ${QEMU_PATCHED_SRC} apply ${QEMU_PATCHES}
    CONFIGURE_COMMAND
      ${CMAKE_COMMAND} -E make_directory ${QEMU_INSTALL_DIR}
      COMMAND ${CMAKE_COMMAND} -E env
              CCACHE_DIR=${CMAKE_SOURCE_DIR}/.cache/ccache
              CC=ccache\ gcc
              CXX=ccache\ g++
              ${QEMU_PATCHED_SRC}/configure
              --prefix=${QEMU_INSTALL_DIR}
              --target-list=x86_64-softmmu,aarch64-softmmu
              --with-devices-x86_64=microvm-minimal
              --with-devices-aarch64=virt-minimal
              --without-default-features
              --without-default-devices
              --enable-fdt=internal
              --disable-docs
              --disable-tools
              --disable-guest-agent
              --disable-install-blobs
              --disable-modules
              --disable-slirp
              --disable-vnc
              --disable-sdl
              --disable-gtk
              --disable-curses
              --disable-opengl
              --disable-plugins
              --disable-rust
              --disable-werror
              --disable-vhost-user
              --enable-kvm
              --enable-tcg
              --enable-strip
              --enable-trace-backends=nop
              --audio-drv-list=
    BUILD_COMMAND ninja qemu-system-x86_64 qemu-system-aarch64
    INSTALL_COMMAND
      ${CMAKE_COMMAND} -E make_directory ${QEMU_INSTALL_DIR}/bin ${QEMU_INSTALL_DIR}/share/qemu
      COMMAND ${CMAKE_COMMAND} -E copy ${QEMU_BUILD_DIR}/qemu-system-x86_64 ${QEMU_INSTALL_DIR}/bin/qemu-system-x86_64
      COMMAND strip -s ${QEMU_INSTALL_DIR}/bin/qemu-system-x86_64
      COMMAND ${CMAKE_COMMAND} -E copy ${QEMU_BUILD_DIR}/qemu-system-aarch64 ${QEMU_INSTALL_DIR}/bin/qemu-system-aarch64
      COMMAND strip -s ${QEMU_INSTALL_DIR}/bin/qemu-system-aarch64
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${QEMU_SOURCE_DIR}/pc-bios ${QEMU_INSTALL_DIR}/share/qemu
    BUILD_BYPRODUCTS
      ${QEMU_INSTALL_DIR}/bin/qemu-system-x86_64
      ${QEMU_INSTALL_DIR}/bin/qemu-system-aarch64
  )

  ExternalProject_Add_StepDependencies(qemu patch ${QEMU_PATCHES})
endif()

