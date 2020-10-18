cmake_minimum_required(VERSION 3.14)

project(quibble VERSION 20200405)

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
endif()

set(SRC_FILES src/apiset.c
    src/boot.c
    src/debug.c
    src/hw.c
    src/mem.c
    src/menu.c
    src/misc.c
    src/peload.c
    src/reg.c
    src/tinymt32.c)

if(MSVC)
    enable_language(ASM_MASM)
    set(SRC_FILES ${SRC_FILES} src/quibble.asm)
endif()

add_executable(quibble ${SRC_FILES})

include_directories(${CMAKE_CURRENT_BINARY_DIR})

if(${CMAKE_C_COMPILER_ID} STREQUAL "GNU")
    target_link_libraries(quibble libgcc.a)
endif()

set_target_properties(quibble PROPERTIES SUFFIX ".efi")

include_directories(gnu-efi)
if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "AMD64" OR ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
    include_directories(gnu-efi/x86_64)
elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "X86")
    include_directories(gnu-efi/ia32)
endif()

target_compile_options(quibble PRIVATE
     $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
          -Wall -Wextra -Werror-implicit-function-declaration -Wno-address-of-packed-member -Wno-packed-not-aligned -Werror=pointer-arith>
     $<$<CXX_COMPILER_ID:MSVC>:
          /W4>)

if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "AMD64" OR ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
    add_definitions(-D__x86_64__)
elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "X86")
    add_definitions(-D_X86_)
endif()

if(${CMAKE_C_COMPILER_ID} STREQUAL "GNU")
target_compile_options(quibble PRIVATE "-ffreestanding")
target_compile_options(quibble PRIVATE "-fno-stack-protector")
target_compile_options(quibble PRIVATE "-fno-stack-check")
target_compile_options(quibble PRIVATE "-mno-stack-arg-probe")
target_compile_options(quibble PRIVATE "-masm=intel")

target_link_options(quibble PRIVATE "-nostdlib")
target_link_options(quibble PRIVATE "-Wl,-dll")
target_link_options(quibble PRIVATE "-shared")
target_link_options(quibble PRIVATE "-Wl,--subsystem,10")

if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "X86")
target_link_options(quibble PRIVATE "-e_efi_main")
else()
target_link_options(quibble PRIVATE "-eefi_main")
endif()

elseif(MSVC)
target_link_options(quibble PRIVATE "/SUBSYSTEM:EFI_APPLICATION")
endif()

set(SRC_FILES src/btrfs/btrfs.c
    src/btrfs/crc32c.c
    src/misc.c)

add_executable(btrfs ${SRC_FILES})

if(${CMAKE_C_COMPILER_ID} STREQUAL "GNU")
    target_link_libraries(btrfs libgcc.a)
endif()

set_target_properties(btrfs PROPERTIES SUFFIX ".efi")

if(${CMAKE_C_COMPILER_ID} STREQUAL "GNU")
target_compile_options(btrfs PRIVATE "-ffreestanding")
target_compile_options(btrfs PRIVATE "-fno-stack-protector")
target_compile_options(btrfs PRIVATE "-fno-stack-check")
target_compile_options(btrfs PRIVATE "-mno-stack-arg-probe")
target_compile_options(btrfs PRIVATE "-masm=intel")

target_link_options(btrfs PRIVATE "-nostdlib")
target_link_options(btrfs PRIVATE "-Wl,-dll")
target_link_options(btrfs PRIVATE "-shared")
target_link_options(btrfs PRIVATE "-Wl,--subsystem,11")

if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "X86")
target_link_options(btrfs PRIVATE "-e_efi_main")
else()
target_link_options(btrfs PRIVATE "-eefi_main")
endif()

elseif(MSVC)
target_link_options(btrfs PRIVATE "/SUBSYSTEM:EFI_BOOT_SERVICE_DRIVER")
endif()
