cmake_minimum_required(VERSION 3.22)
project(NetherSX2LauncherDependencies LANGUAGES C CXX)

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBKRB5 OFF CACHE BOOL "" FORCE)
set(ENABLE_GSSAPI OFF CACHE BOOL "" FORCE)

FetchContent_Declare(libsmb2
  GIT_REPOSITORY https://github.com/ITotalJustice/libsmb2.git
  GIT_TAG 867beea093f2863dfddea01945204f724afd6c45)
FetchContent_MakeAvailable(libsmb2)
target_compile_options(smb2 PRIVATE -UNEED_READV -UNEED_WRITEV)
target_compile_definitions(smb2 PRIVATE AES128_ECB_encrypt=smb2_AES128_ECB_encrypt)

set(USBHSFS_GPL OFF CACHE BOOL "" FORCE)
set(USBHSFS_NTFS OFF CACHE BOOL "" FORCE)
set(USBHSFS_EXT4 OFF CACHE BOOL "" FORCE)
set(USBHSFS_SXOS_DISABLE ON CACHE BOOL "" FORCE)
set(USBHSFS_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(libusbhsfs
  GIT_REPOSITORY https://github.com/ITotalJustice/libusbhsfs.git
  GIT_TAG 625269b7725a6e2a3f2724e8d45b602c1b20ead5)
FetchContent_MakeAvailable(libusbhsfs)
