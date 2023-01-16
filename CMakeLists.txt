cmake_minimum_required(VERSION 3.14)

project(quibble VERSION 20210111)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)

if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /ENTRY:efi_main")
    add_compile_options("/GS-")
    string(REGEX REPLACE "/RTC(su|[1su])" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REGEX REPLACE "/RTC(su|[1su])" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

    # work around bug in Visual Studio
    if (${MSVC_CXX_ARCHITECTURE_ID} STREQUAL "X86")
        set(CMAKE_SYSTEM_PROCESSOR "X86")
    endif()
else()
    add_compile_options(-fno-stack-check -fno-stack-protector -mno-stack-arg-probe)
endif()

set(SRC_FILES src/apiset.cpp
    src/boot.cpp
    src/debug.cpp
    src/hw.cpp
    src/mem.cpp
    src/menu.cpp
    src/misc.cpp
    src/peload.cpp
    src/reg.cpp
    src/tinymt32.cpp
    src/print.cpp)

if(MSVC)
    enable_language(ASM_MASM)
    set(SRC_FILES ${SRC_FILES} src/quibble.asm)
endif()

add_executable(quibble ${SRC_FILES})

set(CMAKE_DISABLE_FIND_PACKAGE_BZip2 TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_HARFBUZZ TRUE)
add_definitions(-DFT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT)
add_subdirectory(freetype)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(freetype/include)

set_target_properties(quibble PROPERTIES SUFFIX ".efi")

include_directories(gnu-efi)
if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "AMD64" OR ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
    include_directories(gnu-efi/x86_64)
elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "X86")
    include_directories(gnu-efi/ia32)
endif()

target_compile_options(quibble PRIVATE
     $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
          -Wall -Wextra -Wno-address-of-packed-member -Werror=pointer-arith -fno-exceptions>
     $<$<CXX_COMPILER_ID:MSVC>:
          /W4 /Oi->)

if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "AMD64" OR ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
    add_definitions(-D__x86_64__)
elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "X86")
    add_definitions(-D_X86_)
endif()

if(${CMAKE_C_COMPILER_ID} STREQUAL "GNU" OR ${CMAKE_C_COMPILER_ID} STREQUAL "Clang")
target_compile_options(quibble PRIVATE "-ffreestanding")
target_compile_options(quibble PRIVATE "-fno-stack-protector")
target_compile_options(quibble PRIVATE "-fno-stack-check")
target_compile_options(quibble PRIVATE "-mno-stack-arg-probe")

target_link_options(quibble PRIVATE "-nostartfiles")
target_link_options(quibble PRIVATE "-shared")

if(${CMAKE_C_COMPILER_ID} STREQUAL "Clang")
    target_link_options(quibble PRIVATE "-Wl,--subsystem,efi_application")
else()
    target_link_options(quibble PRIVATE "-Wl,--subsystem,10")
endif()

if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "X86")
target_link_options(quibble PRIVATE "-e_efi_main")
else()
target_link_options(quibble PRIVATE "-eefi_main")
endif()

elseif(MSVC)
target_link_options(quibble PRIVATE "/SUBSYSTEM:EFI_APPLICATION")
endif()

target_link_libraries(quibble freetype)

# -----------------------------------

set(SRC_FILES src/btrfs/btrfs.c
    src/btrfs/crc32c.c
    src/misc.cpp
    src/btrfs/zlib/adler32.c
    src/btrfs/zlib/deflate.c
    src/btrfs/zlib/inffast.c
    src/btrfs/zlib/inflate.c
    src/btrfs/zlib/inftrees.c
    src/btrfs/zlib/trees.c
    src/btrfs/zlib/zutil.c
    src/btrfs/lzo.c
    src/btrfs/xxhash.c
    src/btrfs/zstd/entropy_common.c
    src/btrfs/zstd/error_private.c
    src/btrfs/zstd/fse_decompress.c
    src/btrfs/zstd/huf_decompress.c
    src/btrfs/zstd/zstd_common.c
    src/btrfs/zstd/zstd_ddict.c
    src/btrfs/zstd/zstd_decompress.c
    src/btrfs/zstd/zstd_decompress_block.c)

add_executable(btrfs ${SRC_FILES})

set_target_properties(btrfs PROPERTIES SUFFIX ".efi")

if(${CMAKE_C_COMPILER_ID} STREQUAL "GNU" OR ${CMAKE_C_COMPILER_ID} STREQUAL "Clang")
target_compile_options(btrfs PRIVATE "-ffreestanding")
target_compile_options(btrfs PRIVATE "-fno-stack-protector")
target_compile_options(btrfs PRIVATE "-fno-stack-check")
target_compile_options(btrfs PRIVATE "-mno-stack-arg-probe")
target_compile_options(btrfs PRIVATE "-Werror-implicit-function-declaration")

target_link_options(btrfs PRIVATE "-nostartfiles")
target_link_options(btrfs PRIVATE "-shared")

if(${CMAKE_C_COMPILER_ID} STREQUAL "Clang")
    target_link_options(btrfs PRIVATE "-Wl,--subsystem,efi_boot_service_driver")
else()
    target_link_options(btrfs PRIVATE "-Wl,--subsystem,11")
endif()

if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "X86")
target_link_options(btrfs PRIVATE "-e_efi_main")
else()
target_link_options(btrfs PRIVATE "-eefi_main")
endif()

elseif(MSVC)
target_link_options(btrfs PRIVATE "/SUBSYSTEM:EFI_BOOT_SERVICE_DRIVER")
target_compile_options(btrfs PRIVATE "/Oi-")
endif()
