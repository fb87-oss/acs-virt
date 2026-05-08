cmake_minimum_required(VERSION 3.24)

project(chiplets_axi_bus C)

include(FetchContent)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out)

option(CHIPLETS_FETCH_QEMU "Fetch QEMU source and enable the qemu-x64-minimal target" OFF)

FetchContent_Declare(
  fastoml
  GIT_REPOSITORY https://github.com/EldoDebug/fastoml.git
  GIT_TAG main
)
FetchContent_MakeAvailable(fastoml)

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

add_library(fastoml_header INTERFACE)
target_include_directories(fastoml_header INTERFACE ${fastoml_SOURCE_DIR})

add_library(ctest_header INTERFACE)
target_include_directories(ctest_header INTERFACE ${ctest_SOURCE_DIR})
target_compile_options(ctest_header INTERFACE -Wno-attributes)

add_executable(blkd
  src/bin/blkd.c
  src/bin/blkd-axi.c
  src/bin/blkd-virtio.c
  src/bin/blkd-block.c
)
target_link_libraries(blkd PRIVATE project_warnings fastoml_header)

add_executable(cond
  src/bin/cond.c
  src/bin/cond-axi.c
  src/bin/cond-virtio.c
  src/bin/cond-console.c
)
target_link_libraries(cond PRIVATE project_warnings fastoml_header)

add_executable(qemu-launch src/bin/qemu-launch.c)
target_link_libraries(qemu-launch PRIVATE project_warnings fastoml_header)

add_executable(c-backend-tests
  tests/c-backend-tests.c
  src/bin/blkd-axi.c
  src/bin/blkd-virtio.c
  src/bin/blkd-block.c
  src/bin/cond-axi.c
  src/bin/cond-virtio.c
  src/bin/cond-console.c
)
target_include_directories(c-backend-tests PRIVATE src/bin)
target_link_libraries(c-backend-tests PRIVATE project_warnings ctest_header)
add_test(NAME c-backend-tests COMMAND c-backend-tests)

install(TARGETS blkd cond qemu-launch c-backend-tests RUNTIME DESTINATION bin)

if(CHIPLETS_FETCH_QEMU)
  FetchContent_Declare(
    qemu
    GIT_REPOSITORY https://gitlab.com/qemu-project/qemu.git
    GIT_TAG v10.2.2
  )
  FetchContent_MakeAvailable(qemu)

  set(QEMU_PATCHED_SRC ${CMAKE_SOURCE_DIR}/build/qemu-src-x64-minimal)
  set(QEMU_BUILD_DIR ${CMAKE_SOURCE_DIR}/build/qemu-x64-minimal)
  set(QEMU_INSTALL_DIR ${CMAKE_SOURCE_DIR}/out/qemu-x64-minimal)
  set(QEMU_STAMP ${QEMU_BUILD_DIR}/qemu-system-x86_64.stamp)

  file(GLOB QEMU_PATCHES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/patches/qemu/*.patch)

  add_custom_command(
    OUTPUT ${QEMU_STAMP}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${QEMU_PATCHED_SRC}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${QEMU_PATCHED_SRC}
    COMMAND rsync -a --delete --exclude=.git/ --exclude=build/ ${qemu_SOURCE_DIR}/ ${QEMU_PATCHED_SRC}/
    COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_SOURCE_DIR}/build/qemu-pc-bios
    COMMAND ${CMAKE_COMMAND} -E create_symlink ${qemu_SOURCE_DIR}/pc-bios ${CMAKE_SOURCE_DIR}/build/qemu-pc-bios
    COMMAND git -C ${QEMU_PATCHED_SRC} init -q
    COMMAND git -C ${QEMU_PATCHED_SRC} apply ${QEMU_PATCHES}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${QEMU_BUILD_DIR} ${QEMU_INSTALL_DIR}
    COMMAND ${CMAKE_COMMAND} -E env
            CCACHE_DIR=${CMAKE_SOURCE_DIR}/.cache/ccache
            CC=ccache\ gcc
            CXX=ccache\ g++
            ${QEMU_PATCHED_SRC}/configure
            --prefix=${QEMU_INSTALL_DIR}
            --target-list=x86_64-softmmu
            --with-devices-x86_64=microvm-minimal
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
            --enable-kvm
            --enable-tcg
            --enable-vhost-user
            --enable-strip
            --enable-trace-backends=nop
            --audio-drv-list=
    COMMAND ninja qemu-system-x86_64
    COMMAND ${CMAKE_COMMAND} -E make_directory ${QEMU_INSTALL_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E copy ${QEMU_BUILD_DIR}/qemu-system-x86_64 ${QEMU_INSTALL_DIR}/bin/qemu-system-x86_64
    COMMAND strip -s ${QEMU_INSTALL_DIR}/bin/qemu-system-x86_64
    COMMAND ${CMAKE_COMMAND} -E touch ${QEMU_STAMP}
    DEPENDS ${QEMU_PATCHES}
    WORKING_DIRECTORY ${QEMU_BUILD_DIR}
    VERBATIM
  )

  add_custom_target(qemu-x64-minimal DEPENDS ${QEMU_STAMP})
endif()
