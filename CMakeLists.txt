cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(ntfsfs C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

file(GLOB NTFS3G_SOURCES third_party/ntfs-3g/libntfs-3g/*.c)

add_executable(ntfsfs
  src/main.c
  src/vfs.c
  src/devio.c
  src/acl.c
  src/worker.c
  src/port/libc.c
  ${NTFS3G_SOURCES}
)

target_include_directories(ntfsfs PRIVATE
  src
  src/port
  third_party/ntfs-3g/include/ntfs-3g
)

target_compile_definitions(ntfsfs PRIVATE HAVE_CONFIG_H)
target_compile_options(ntfsfs PRIVATE
  -O2 -std=gnu11 -Wall
  # libntfs-3g uses enum types in on-disk structures; it needs 4-byte enums
  -fno-short-enums
  -fno-builtin-printf -fno-builtin-fprintf -fno-builtin-vfprintf
  -Wno-unused-parameter -Wno-address-of-packed-member -Wno-zero-length-bounds
)
set_source_files_properties(${NTFS3G_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-unused;-Wno-maybe-uninitialized;-Wno-use-after-free")

target_link_libraries(ntfsfs
  taihenForKernel_stub
  taihenModuleUtils_stub
  SceIofilemgrForDriver_stub
  SceKernelSuspendForDriver_stub
  SceSysclibForDriver_stub
  SceSysmemForDriver_stub
  SceThreadmgrForDriver_stub
  SceDebugForDriver_stub
  SceRtcForDriver_stub
  gcc
)

set_target_properties(ntfsfs PROPERTIES LINK_FLAGS "-nostdlib")

vita_create_self(ntfsfs.skprx ntfsfs CONFIG exports.yml UNSAFE)
