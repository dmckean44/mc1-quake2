# -*- mode: CMake; tab-width: 2; indent-tabs-mode: nil; -*-

# This file is a fairly generic CMake toolchain file for the MC1 computer.
# To use it, run CMake with the argument:
#   -DCMAKE_TOOLCHAIN_FILE=mc1-toolchain.cmake

# Define the system.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR mrisc32)
set(MC1 TRUE)

# Get the MC1 SDK installation folder.
if(NOT DEFINED MC1_SDK)
  if(CMAKE_VERSION VERSION_LESS "3.21")
    if(WIN32)
      set(_home "$ENV{homedrive}$ENV{homepath}")
    else()
      set(_home "$ENV{HOME}")
    endif()
    set(MC1_SDK "${_home}/mc1-sdk")
    unset(_home)
  else()
    file(REAL_PATH "~/mc1-sdk" MC1_SDK EXPAND_TILDE)
  endif()
  if(IS_DIRECTORY ${MC1_SDK})
    message(STATUS "Using the MC1 SDK at ${MC1_SDK}")
  else()
    message(WARNING "The MC1 SDK was not found (expected ${MC1_SDK})")
    unset(MC1_SDK)
  endif()
endif()

# Define the compiler.
set(CMAKE_C_COMPILER mrisc32-elf-gcc)
set(CMAKE_CXX_COMPILER mrisc32-elf-g++)
set(CMAKE_ASM_COMPILER mrisc32-elf-gcc)

# Set compiler flags.
set(_c_flags "")
set(_asm_flags "-x assembler-with-cpp")
set(_linker_flags "")
if(DEFINED MC1_SDK)
  set(_c_flags "${_c_flags} -I${MC1_SDK}/include")
  set(_asm_flags "${_asm_flags} -I${MC1_SDK}/include")
  set(_linker_flags "${_linker_flags} -L${MC1_SDK}/lib")
endif()
set(CMAKE_C_FLAGS          "${_c_flags}"      CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS        "${_c_flags}"      CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS        "${_asm_flags}"    CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "${_linker_flags}" CACHE STRING "" FORCE)
unset(_c_flags)
unset(_asm_flags)
unset(_linker_flags)

# We don't have any rich sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
