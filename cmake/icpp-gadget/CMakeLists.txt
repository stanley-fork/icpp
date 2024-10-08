#
# This file is used to build the icpp-gadget/icpp-server for Android and iOS.
#

# cmake
cmake_minimum_required(VERSION 3.20)
project(icpp CXX C)

# c++
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED)

set(ICPP_ROOT ${CMAKE_SOURCE_DIR}/../..)
set(ICPP_SRCROOT ${ICPP_ROOT}/src)

set(PYTHON python3)

# build all dependent libraries as static
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# build the device architecture target only
if (DEFINED ANDROID_ABI AND ANDROID_ABI STREQUAL "x86_64")
  set(LLVM_TARGETS_TO_BUILD "X86" CACHE STRING "" FORCE)
  set(UNICORN_ARCH "x86" CACHE STRING "" FORCE)
else()
  set(LLVM_TARGETS_TO_BUILD "AArch64" CACHE STRING "" FORCE)
  set(UNICORN_ARCH "aarch64" CACHE STRING "" FORCE)
  # patch to use the right tb_ret_addr
  execute_process(COMMAND ${PYTHON}
    ${ICPP_ROOT}/tool/qemu_tbret_patcher.py
    ${ICPP_ROOT}/third/unicorn/qemu/tcg/aarch64/tcg-target.inc.c)
endif()

# enable libcxx for C/C++ runtime library
message(STATUS "Configuring llvm...")
set(LLVM_INCLUDE_TOOLS OFF CACHE BOOL "" FORCE)
set(LLVM_INCLUDE_UTILS OFF CACHE BOOL "" FORCE)
set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LLVM_ENABLE_ZLIB "OFF" CACHE STRING "" FORCE)
set(LLVM_ENABLE_ZSTD "OFF" CACHE STRING "" FORCE)
add_subdirectory(${ICPP_ROOT}/third/llvm-project/llvm
                 ${CMAKE_BINARY_DIR}/llvm)

#
# third-party projects
#

# boost
message(STATUS "Configuring boost...")
if (WIN32)
  set(BOOST_CONTEXT_IMPLEMENTATION winfib)
endif()
add_subdirectory(${ICPP_ROOT}/third/boost
                 ${CMAKE_BINARY_DIR}/boost)

# protobuf
message(STATUS "Configuring protobuf...")
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
add_subdirectory(${ICPP_ROOT}/third/protobuf
                 ${CMAKE_BINARY_DIR}/protobuf)

# brotli
message(STATUS "Configuring brotli...")
add_subdirectory(${ICPP_ROOT}/third/brotli
                 ${CMAKE_BINARY_DIR}/brotli)

# unicorn
message(STATUS "Configuring unicorn...")
# patch qemu mmu to enable the emulated code to access memory directly
execute_process(COMMAND ${PYTHON}
  ${ICPP_ROOT}/tool/qemu_mmu_patcher.py
  ${ICPP_ROOT}/third/unicorn/qemu/accel/tcg/cputlb.c)
# patch qemu tbgen buffer size to 128MB
execute_process(COMMAND ${PYTHON}
  ${ICPP_ROOT}/tool/qemu_tbgen_patcher.py
  ${ICPP_ROOT}/third/unicorn/qemu/softmmu/vl.c 128)
add_subdirectory(${ICPP_ROOT}/third/unicorn
                 ${CMAKE_BINARY_DIR}/unicorn)

#
# icpp projects
#

message(STATUS "Configuring icpp...")

if(APPLE)
  add_link_options(-Wl,-rpath,@executable_path 
                   -Wl,-rpath,@executable_path/../lib
                   -Wl,-rpath,@executable_path/../lib/boost)
else()
  add_link_options(@${ICPP_SRCROOT}/ld.txt)
endif()

set(THIRD_ROOT ${ICPP_ROOT}/third)
set(LLVM_OBJDUMP_ROOT ${THIRD_ROOT}/llvm-project/llvm/tools/llvm-objdump)

# generate the protocol sources with protoc
add_custom_command(
  OUTPUT 
    icppdbg.pb.cc icppiobj.pb.cc isymhash.pb.cc
    icppmod.pb.cc
    icpppad.pb.cc
  COMMAND ${CMAKE_BINARY_DIR}/third/protobuf/protoc 
    -I=${ICPP_ROOT}/proto --cpp_out=${CMAKE_BINARY_DIR} 
       ${ICPP_ROOT}/proto/icppdbg.proto
  COMMAND ${CMAKE_BINARY_DIR}/third/protobuf/protoc 
    -I=${ICPP_ROOT}/proto --cpp_out=${CMAKE_BINARY_DIR} 
       ${ICPP_ROOT}/proto/icppiobj.proto
  COMMAND ${CMAKE_BINARY_DIR}/third/protobuf/protoc 
    -I=${ICPP_ROOT}/proto --cpp_out=${CMAKE_BINARY_DIR} 
       ${ICPP_ROOT}/proto/isymhash.proto
  COMMAND ${CMAKE_BINARY_DIR}/third/protobuf/protoc 
    -I=${ICPP_ROOT}/proto --cpp_out=${CMAKE_CURRENT_BINARY_DIR} 
       ${ICPP_ROOT}/proto/icppmod.proto
  COMMAND ${CMAKE_BINARY_DIR}/third/protobuf/protoc 
    -I=${ICPP_ROOT}/proto --cpp_out=${CMAKE_CURRENT_BINARY_DIR} 
       ${ICPP_ROOT}/proto/icpppad.proto
  DEPENDS ${ICPP_ROOT}/proto/icppdbg.proto
          ${ICPP_ROOT}/proto/icppiobj.proto
          ${ICPP_ROOT}/proto/isymhash.proto
          ${ICPP_ROOT}/proto/icpppad.proto
  VERBATIM)

file(GLOB ICPP_CORE_SOURCES
  ${ICPP_SRCROOT}/arch.cpp
  ${ICPP_SRCROOT}/debugger.cpp
  ${ICPP_SRCROOT}/exec.cpp
  ${ICPP_SRCROOT}/icpp.cpp
  ${ICPP_SRCROOT}/loader.cpp
  ${ICPP_SRCROOT}/log.cpp
  ${ICPP_SRCROOT}/platform.cpp
  ${ICPP_SRCROOT}/profile.cpp
  ${ICPP_SRCROOT}/runcfg.cpp
  ${ICPP_SRCROOT}/runtime.cpp
  ${ICPP_SRCROOT}/trace.cpp
  ${ICPP_SRCROOT}/utils.cpp
)

set(GADGET_LLVM_FLAGS "-fno-rtti -DICPP_CROSS_GADGET=1")

# icpp gadget, running as a daemon in somewhere to receive the
# interpretable object to execute
message(STATUS "Configuring memory resident interpreter icpp-gadget...")
set(LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  Object
  Option
  Support
)
file(GLOB IGADGET_LLVM_SOURCES
  ${ICPP_SRCROOT}/icpp-gadget.cpp
  ${ICPP_SRCROOT}/object.cpp
  ${ICPP_SRCROOT}/object-coff.cpp
  ${ICPP_SRCROOT}/object-llvm.cpp
  ${LLVM_OBJDUMP_ROOT}/SourcePrinter.cpp
)
add_llvm_library(icpp-gadget SHARED 
  ${ICPP_CORE_SOURCES}

  # llvm relational files
  ${IGADGET_LLVM_SOURCES}

  # auto generated by protoc
  icppdbg.pb.cc
  icppiobj.pb.cc
  isymhash.pb.cc
  icpppad.pb.cc

  PARTIAL_SOURCES_INTENDED
)
set_target_properties(icpp-gadget PROPERTIES PREFIX "")
set_source_files_properties(${IGADGET_LLVM_SOURCES} PROPERTIES 
  COMPILE_FLAGS ${GADGET_LLVM_FLAGS}
)
target_include_directories(icpp-gadget PRIVATE 
  ${THIRD_ROOT}/llvm-project/llvm/include
  ${CMAKE_BINARY_DIR}/llvm/include
  ${THIRD_ROOT}/unicorn/include
  ${CMAKE_BINARY_DIR}
  ${LLVM_OBJDUMP_ROOT}
  boost_asio
  boost_beast
  boost_json
  libprotobuf
)
target_link_libraries(icpp-gadget PRIVATE 
  boost_asio
  boost_beast
  boost_json
  libprotobuf
  unicorn
)
if(APPLE)
  target_link_options(icpp-gadget PRIVATE -Wl,-exported_symbol -Wl,_icpp_gadget)
else()
  target_link_options(icpp-gadget PRIVATE -Wl,--version-script=${ICPP_SRCROOT}/ld-gadget.version)
endif()

# icpp module manager command line
message(STATUS "Configuring icpp module manager imod...")

set(LLVM_LINK_COMPONENTS
  Object
  Option
  Support
)
file(GLOB IMOD_LLVM_SOURCES
  ${ICPP_SRCROOT}/icpp-mod.cpp
  ${ICPP_SRCROOT}/object.cpp
  ${ICPP_SRCROOT}/object-coff.cpp
)
add_llvm_tool(imod 
  ${IMOD_LLVM_SOURCES}
  ${ICPP_SRCROOT}/arch.cpp
  ${ICPP_SRCROOT}/icpp.cpp
  ${ICPP_SRCROOT}/loader.cpp
  ${ICPP_SRCROOT}/platform.cpp
  ${ICPP_SRCROOT}/runcfg.cpp
  ${ICPP_SRCROOT}/runtime.cpp
  ${ICPP_SRCROOT}/utils.cpp
  ${ICPP_SRCROOT}/imod/createcfg.cpp

  isymhash.pb.cc
  icppmod.pb.cc
  icppiobj.pb.cc

  PARTIAL_SOURCES_INTENDED
)
set_source_files_properties(${IMOD_LLVM_SOURCES} PROPERTIES 
  COMPILE_FLAGS ${GADGET_LLVM_FLAGS})
target_include_directories(imod PRIVATE
  ${THIRD_ROOT}/llvm-project/llvm/include
  ${CMAKE_BINARY_DIR}/llvm/include
  ${THIRD_ROOT}/brotli/c/include
  ${CMAKE_BINARY_DIR}
  boost_asio
  boost_beast
  boost_json
  boost_process
  libprotobuf
)
target_link_libraries(imod PRIVATE
  boost_asio
  boost_beast
  boost_json
  boost_process
  brotlicommon
  brotlienc
  brotlidec
  libprotobuf
)

# icpp-server command line
message(STATUS "Configuring icpp-server remote daemon...")
add_executable(icpp-server ${ICPP_SRCROOT}/icpp-server.cpp)
target_include_directories(icpp-server PRIVATE 
  ${CMAKE_BINARY_DIR}/llvm/include
  boost_dll
)
target_link_libraries(icpp-server PRIVATE boost_dll)

message(STATUS "Configuring icpp done")
