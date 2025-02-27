#
# Copyright (c) 2021 Triad National Security, LLC
#                         All rights reserved.
#
# This file is part of the kitsune+tapir project.  It is released under
# the LLVM license.
#
cmake_minimum_required(VERSION 3.20)
project(kitsune C CXX)

include(ExternalProject)
include(FetchContent)
include(GetClangResourceDir)

if (WIN32)
  message(FATAL_ERROR "Kitsune is not supported on Windows")
endif ()

set(KITSUNE_C_FRONTEND kitcc)
set(KITSUNE_CXX_FRONTEND kit++)
set(KITSUNE_Fortran_FRONTEND kitfort)

option(KITSUNE_INCLUDE_TESTS
  "Generate build targets for the Kitsune tests"
  ${LLVM_INCLUDE_TESTS})

option(KITSUNE_KOKKOS_ENABLE
  "Enable custom recognition and compilation of Kokkos"
  ON)

set(KITSUNE_CUDA_ENABLE OFF CACHE INTERNAL "Enable the cuda tapir target" FORCE)
set(KITSUNE_HIP_ENABLE OFF CACHE INTERNAL "Enable the hip tapir target" FORCE)
set(KITSUNE_OPENCILK_ENABLE OFF CACHE INTERNAL "Enable the opencilk tapir target" FORCE)
set(KITSUNE_OPENMP_ENABLE OFF CACHE INTERNAL "Enable the openmp tapir target" FORCE)
set(KITSUNE_QTHREADS_ENABLE OFF CACHE INTERNAL "Enable the qthreads tapir target" FORCE)
set(KITSUNE_REALM_ENABLE OFF CACHE INTERNAL "Enable the realm tapir target" FORCE)

# TODO: The _MAX variables are not currently used but it would be good to
# require a range instead of just a minimum. In practice, we may want to
# restrict it to just a single version because maintaining even a small range
# is likely to be a pain.
set(KITSUNE_CUDA_VERSION_MIN 12.3 CACHE INTERNAL "Minimum Cuda version required")
set(KITSUNE_CUDA_VERSION_MAX 12.3 CACHE INTERNAL "Maxmimum Cuda version supported")
set(KITSUNE_ROCM_VERSION_MIN 7.0 CACHE INTERNAL "Minimum ROCM version required")
set(KITSUNE_ROCM_VERSION_MAX 7.0 CACHE INTERNAL "Maximum ROCM version supported")

set(KITSUNE_SOURCE_DIR  ${CMAKE_CURRENT_SOURCE_DIR})
set(KITSUNE_INCLUDE_DIR ${KITSUNE_SOURCE_DIR}/include)
set(KITSUNE_BINARY_DIR  ${PROJECT_BINARY_DIR})
set(KITSUNE_TARGETS_BINARY_DIR ${KITSUNE_BINARY_DIR}/targets)

# TODO: Need to update the openmp, qthreads and realm targets and add them
# back into this list. These targets have not been tested in a while and have
# probably bit-rotted. Enabling them will raise a configure-time error.
set(KITSUNE_ALL_TAPIR_TARGETS
  "cuda;hip;opencilk"
  CACHE STRING
  "All known Tapir targets.")

set(KITSUNE_DEFAULT_TAPIR_TARGETS
  "cuda;hip;opencilk"
  CACHE STRING
  "The Tapir targets that are built by default if targets are not explicitly specified.")

set(KITSUNE_ENABLE_TAPIR_TARGETS
  "${KITSUNE_DEFAULT_TAPIR_TARGETS}"
  CACHE STRING
  "Semicolon-separated list of runtime targets to build (or \"all\".)")

if (NOT KITSUNE_ENABLE_TAPIR_TARGETS)
  message(FATAL_ERROR "At least one runtime target must be built")
endif ()

foreach(target IN LISTS KITSUNE_ENABLE_TAPIR_TARGETS)
  if (target IN_LIST KITSUNE_ALL_TAPIR_TARGETS)
    message(STATUS "${target} tapir target enabled")
    string(TOUPPER "${target}" upper_target)
    set(KITSUNE_${upper_target}_ENABLE ON CACHE INTERNAL "Enable the ${target} tapir target" FORCE)
  else ()
    message(FATAL_ERROR "Unknown Tapir target: ${target}")
  endif()
endforeach()

get_clang_resource_dir(CLANG_RESOURCE_INTDIR
  PREFIX ${CMAKE_BINARY_DIR})

get_clang_resource_dir(CLANG_RESOURCE_INSTALL_DIR
  PREFIX ${CMAKE_INSTALL_PREFIX})

if (KITSUNE_KOKKOS_ENABLE)
  # We currently don't support using a pre-built Kokkos - even if it has been
  # patched the way we need it to be. This could possibly be moved to
  # kitsune/runtime, just so everything that needs to be fetched and built is
  # there, even if this is not technically a runtime.

  set(KITSUNE_KOKKOS_BUILD_CONFIGURE_FLAGS ""
    CACHE STRING
  "Additional options to be passed to be CMake when configuring Kokkos. These are only used when building Kitsune")

  set(KITSUNE_KOKKOS_BUILD_COMPILE_FLAGS ""
    CACHE STRING
    "Additional C++ compiler flags needed to build Kokkos. These are only used when building Kitsune")

  set(KITSUNE_KOKKOS_SRC_DIR  ${KITSUNE_TARGETS_BINARY_DIR}/kokkos/kokkos)
  set(KITSUNE_KOKKOS_BUILD_DIR ${KITSUNE_TARGETS_BINARY_DIR}/kokkos/build)
  set(KITSUNE_KOKKOS_STAMP_DIR ${KITSUNE_TARGETS_BINARY_DIR}/kokkos/stamp)
  set(KITSUNE_KOKKOS_BUILD_CMAKE_FLAGS
    -DCMAKE_CXX_COMPILER=${LLVM_RUNTIME_OUTPUT_INTDIR}/clang++
    -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_INSTALL_PREFIX=${CLANG_RESOURCE_INSTALL_DIR}
    -DCMAKE_CXX_FLAGS=${KITSUNE_KOKKOS_BUILD_COMPILE_FLAGS}
    -DKokkos_ENABLE_SERIAL=ON
    -DBUILD_TESTING=OFF
    -DBUILD_SHARED_LIBS=ON
    ${KITSUNE_KOKKOS_BUILD_CONFIGURE_FLAGS})

  # We have to patch kokkos to disable some sections of code that are using
  # exceptions.
  set(KITSUNE_KOKKOS_PATCH_EXCS ${KITSUNE_SOURCE_DIR}/patches/Kokkos_Exceptions.patch)

  #
  # WARNING   This absolutely sucks!
  #
  # As I understand it, ExternalProject_Add will always check for updates and
  # download any updates. Obviously, since we have a fixed git tag, there will
  # never be any. However, the PATCH_COMMAND is always executed, which means
  # that it will attempt to patch code that has already been patched. Naturally,
  # this fails. The workaround here is to disable checking for updates by
  # setting UPDATE_COMMAND to "".
  #
  # HORRORS! It is possible that this means any update to GIT_TAG below will
  # also not have any effect since the updates will never be retrieved. In this
  # case, the entire build directory may need to be deleted for everything to
  # work correctly. I cannot think of a way to automatically warn when this
  # happens (never mind making sure that this doesn't happen).
  #
  # TODO: Look for newer Kokkos updates.
  #
  ExternalProject_Add(kokkos
    DEPENDS             clang lld llvm-config llvm-link
    GIT_REPOSITORY      https://github.com/kokkos/kokkos.git
    GIT_TAG             606866d # 4.0.00
    SOURCE_DIR          ${KITSUNE_KOKKOS_SRC_DIR}
    BINARY_DIR          ${KITSUNE_KOKKOS_BUILD_DIR}
    STAMP_DIR           ${KITSUNE_KOKKOS_STAMP_DIR}
    CMAKE_ARGS          ${KITSUNE_KOKKOS_BUILD_CMAKE_FLAGS}
    PATCH_COMMAND       patch -p0 --input=${KITSUNE_KOKKOS_PATCH_EXCS}
    UPDATE_COMMAND      ""
    UPDATE_DISCONNECTED FALSE
    STEP_TARGETS        build)

  # Copy the Kokkos files into a "staging" area. This is only needed to run
  # the Kitsune + Kokkos tests.
  add_custom_target(kokkos-copy-headers-to-build-dir ALL
    # Copy all the headers directly from the source. These are installed as is
    # anyway.
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${KITSUNE_KOKKOS_SRC_DIR}/core/src
    ${KITSUNE_KOKKOS_SRC_DIR}/tpls/desul/include
    ${CLANG_RESOURCE_INTDIR}/include

    # We don't copy the entire build directory because we only want specific
    # headers.
    COMMAND ${CMAKE_COMMAND} -E copy
    ${KITSUNE_KOKKOS_BUILD_DIR}/KokkosCore_config.h
    ${KITSUNE_KOKKOS_BUILD_DIR}/KokkosCore_Config_DeclareBackend.hpp
    ${KITSUNE_KOKKOS_BUILD_DIR}/KokkosCore_Config_FwdBackend.hpp
    ${KITSUNE_KOKKOS_BUILD_DIR}/KokkosCore_Config_PostInclude.hpp
    ${KITSUNE_KOKKOS_BUILD_DIR}/KokkosCore_Config_SetupBackend.hpp
    ${CLANG_RESOURCE_INTDIR}/include

    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${KITSUNE_KOKKOS_BUILD_DIR}/core/src/desul
    ${CLANG_RESOURCE_INTDIR}/include/desul

    COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CLANG_RESOURCE_INTDIR}/lib64

    # Copy the core libraries that are built. There are others that are also
    # built, but these are the only ones that we care about.
    #
    # NOTE: These should be updated when the GIT_TAG is changed because the
    # trailing version suffix may also change. There might be an automatic way
    # to do this that does not involve copying the entire directory, and we
    # probably should implement that at some point.
    COMMAND ${CMAKE_COMMAND} -E copy
    ${KITSUNE_KOKKOS_BUILD_DIR}/core/src/libkokkoscore.so
    ${KITSUNE_KOKKOS_BUILD_DIR}/core/src/libkokkoscore.so.4.0
    ${KITSUNE_KOKKOS_BUILD_DIR}/core/src/libkokkoscore.so.4.0.0
    ${CLANG_RESOURCE_INTDIR}/lib64

    COMMENT "Copying Kokkos files to the 'top-level' build directory"
    VERBATIM USES_TERMINAL
  )
  add_dependencies(kokkos-copy-headers-to-build-dir kokkos)

  set(KITSUNE_KOKKOS_EXTRA_PREPROCESSOR_FLAGS
    ""
    CACHE STRING
    "Additional preprocessor flags needed for Kokkos support")

  set(KITSUNE_KOKKOS_EXTRA_COMPILER_FLAGS
    ""
    CACHE STRING
    "Additional compiler flags needed for Kokkos support")

  set(KITSUNE_KOKKOS_EXTRA_LINKER_FLAGS
    ""
    CACHE STRING
    "Additional linker flags needed for Kokkos support")
endif()

if (KITSUNE_CUDA_ENABLE)
  message(STATUS "kitsune: CUDA enabled.")
  find_package(CUDAToolkit ${KITSUNE_CUDA_VERSION_MIN} REQUIRED)

  set(KITSUNE_CUDA_PREFIX $ENV{CUDA_HOME})
  set(KITSUNE_CUDA_INCLUDE_DIR ${CUDAToolkit_INCLUDE_DIRS})
  set(KITSUNE_CUDA_BINARY_DIR ${CUDAToolkit_BIN_DIR})
  set(KITSUNE_CUDA_LIBRARY_DIR ${CUDAToolkit_LIBRARY_DIR})
  set(KITSUNE_CUDA_STUBS_DIR "${KITSUNE_CUDA_LIBRARY_DIR}/stubs")
  set(KITSUNE_CUDA_LIBDEVICE_DIR "${KITSUNE_CUDA_PREFIX}/nvvm/libdevice")

  message(STATUS "CUDA prefix: ${KITSUNE_CUDA_PREFIX}")

  find_library(KITSUNE_CUDA_LIB_CUDART cudart REQUIRED
    PATHS ${KITSUNE_CUDA_LIBRARY_DIR}
    NO_DEFAULT_PATH)

  # This should almost always be the libcuda that is installed on the system,
  # so look for it in the "usual" places. In some cases, both /usr/lib and
  # /usr/lib64 are present with libcuda in the latter. But cmake does not seem
  # to look there, so force it to.
  find_library(KITSUNE_CUDA_LIB_CUDA cuda REQUIRED
    HINTS /usr/lib64 /usr/local/lib64)

  find_library(KITSUNE_CUDA_LIB_NVPTX_STATIC nvptxcompiler_static REQUIRED
    PATHS ${KITSUNE_CUDA_LIBRARY_DIR}
    NO_DEFAULT_PATH)

  find_file(KITSUNE_CUDA_LIBDEVICE_BC libdevice.10.bc REQUIRED
    PATHS ${KITSUNE_CUDA_LIBDEVICE_DIR}
    NO_DEFAULT_PATH)

  find_program(KITSUNE_CUDA_PTXAS ptxas REQUIRED
    PATHS ${KITSUNE_CUDA_BINARY_DIR}
    NO_DEFAULT_PATH)

  find_program(KITSUNE_CUDA_FATBINARY fatbinary REQUIRED
    PATHS ${KITSUNE_CUDA_BINARY_DIR}
    NO_DEFAULT_PATH)

  set(KITSUNE_CUDA_EXTRA_PREPROCESSOR_FLAGS
    ""
    CACHE STRING
    "Additional preprocessor flags needed for the Cuda target")

  set(KITSUNE_CUDA_EXTRA_COMPILER_FLAGS
    ""
    CACHE STRING
    "Additional compiler flags needed for the Cuda target")

  set(KITSUNE_CUDA_EXTRA_LINKER_FLAGS
    ""
    CACHE STRING
    "Additional linker flags needed for the Cuda target")
endif()

if (KITSUNE_HIP_ENABLE)
  message(STATUS "kitsune: HIP enabled.")
  if (NOT DEFINED ROCM_PATH)
    message(STATUS "Kitsune HIP support enabled but ROCM_PATH not set.")
    if (DEFINED $ENV{ROCM_PATH})
      message(STATUS "  using ROCM_PATH environment variable.")
      set(ROCM_PATH $ENV{ROCM_PATH} CACHE PATH "AMD ROCM/HIP installation directory.")
    else() 
      message(STATUS "  selecting a common default install location.")
      set(ROCM_PATH "/opt/rocm" CACHE PATH "AMD ROCM/HIP installation directory.")
    endif()
  endif()
  message(STATUS "ROCm/HIP path: ${ROCM_PATH}")
  list(APPEND CMAKE_MODULE_PATH "${ROCM_PATH}/lib/cmake/hip")
  message(STATUS "   updated cmake module search path: ${CMAKE_MODULE_PATH}")
  find_package(HIP MODULE REQUIRED)
  include(${ROCM_PATH}/lib/cmake/hip/hip-config.cmake)

  find_library(LIB_AMDHIP64 amdhip64 REQUIRED
    PATHS ${HIP_LIBRARY_INSTALL_DIR}
    ${ROCM_PATH}/lib)

  message(STATUS "ROCm/HIP path: ${ROCM_PATH}")
  list(APPEND CMAKE_MODULE_PATH "${ROCM_PATH}/lib/cmake/hip")
  message(STATUS "   updated cmake module search path: ${CMAKE_MODULE_PATH}")
  find_package(HIP MODULE REQUIRED)
  include(${ROCM_PATH}/lib/cmake/hip/hip-config.cmake)

  find_library(LIB_AMDHIP64 amdhip64 REQUIRED
    PATHS ${HIP_LIB_INSTALL_DIR}
    ${ROCM_PATH}/lib)

  set(KITSUNE_HIP_PREFIX ${HIP_LIBRARY_INSTALL_DIR})
  set(KITSUNE_HIP_INCLUDE_DIR ${HIP_INCLUDE_DIR})
  set(KITSUNE_HIP_LIBRARY_DIR ${HIP_LIB_INSTALL_DIR})

  message(STATUS "hip include dir: ${HIP_INCLUDE_DIR}")
  message(STATUS "hip library dir: ${HIP_LIB_INSTALL_DIR}")

  set(KITSUNE_HIP_DEFAULT_PREPROCESSOR_FLAGS
    "-D_tapir_hip_target")

  set(KITSUNE_HIP_DEFAULT_COMPILER_FLAGS
    "")

  set(KITSUNE_HIP_DEFAULT_LINKER_FLAGS
    "-L${ROCM_PATH}/lib -L${ROCM_PATH}/hip/lib -lamdhip64 -lm")

  set(KITSUNE_HIP_EXTRA_PREPROCESSOR_FLAGS
    ""
    CACHE STRING
    "Additional preprocessor flags needed for the HIP target")

  set(KITSUNE_HIP_EXTRA_COMPILER_FLAGS
    ""
    CACHE STRING
    "Additional compiler flags needed for the HIP target")

  set(KITSUNE_HIP_EXTRA_LINKER_FLAGS
    ""
    CACHE STRING
    "Additional linker flags needed for the HIP target")

  set(KITSUNE_HIP_BITCODE_DIR "${ROCM_PATH}/amdgcn/bitcode/")
  find_file(KITSUNE_HIP_BITCODE_LOC hip.bc REQUIRED
    PATHS ${KITSUNE_HIP_BITCODE_DIR}
    NO_DEFAULT_PATH)
  message(STATUS "hip bitcode file location: ${KITSUNE_HIP_BITCODE_DIR}")
endif()

if (KITSUNE_OPENCILK_ENABLE)
  # We currently don't allow using a pre-built Cheetah. It would have to be
  # built with a "compatible" version of clang which is possible by using a
  # vanilla clang build of the same version, but it's probably not worth all the
  # error checking hassles that would be needed to detect if the versions were
  # mismatched.
  set(KITSUNE_OPENCILK_BUILD_CONFIGURE_FLAGS ""
    CACHE STRING
  "Additional options to be passed to be CMake when configuring Kokkos. These are only used when building Kitsune")

  set(KITSUNE_OPENCILK_BUILD_COMPILE_FLAGS ""
    CACHE STRING
    "Additional C++ compiler flags needed to build Kokkos. These are only used when building Kitsune")


  # We pass some LLVM_* variables to make Cheetah behave as if it were an
  # in-tree build.
  set(KITSUNE_CHEETAH_SOURCE_DIR ${KITSUNE_TARGETS_BINARY_DIR}/cheetah/cheetah)
  set(KITSUNE_CHEETAH_BINARY_DIR ${KITSUNE_TARGETS_BINARY_DIR}/cheetah/build)
  set(KITSUNE_CHEETAH_BUILD_CMAKE_FLAGS
    -DCMAKE_C_COMPILER=${LLVM_RUNTIME_OUTPUT_INTDIR}/clang
    -DCMAKE_CXX_COMPILER=${LLVM_RUNTIME_OUTPUT_INTDIR}/clang++
    -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
    -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
    -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
    -DCMAKE_CXX_FLAGS=${KITSUNE_OPENCILK_BUILD_COMPILE_FLAGS}
    -DCHEETAH_DEFAULT_TARGET_TRIPLE=${LLVM_DEFAULT_TARGET_TRIPLE}
    -DCHEETAH_DIRECTORY_PER_TARGET=ON
    -DLLVM_CMAKE_DIR=${CMAKE_BINARY_DIR}
    -DLLVM_COMMON_CMAKE_UTILS=${CMAKE_SOURCE_DIR}/llvm/cmake
    -DLLVM_LIBRARY_OUTPUT_INTDIR=${LLVM_LIBRARY_OUTPUT_INTDIR}
    -DLLVM_RUNTIME_OUTPUT_INTDIR=${LLVM_RUNTIME_OUTPUT_INTDIR}
    -DPACKAGE_VERSION=${PACKAGE_VERSION}
    ${KITSUNE_OPENCILK_BUILD_CONFIGURE_FLAGS}
  )

  message(STATUS "opencilk install prefix: ${CLANG_RESOURCE_INSTALL_DIR}")

  ExternalProject_Add(cheetah
    DEPENDS        clang lld llvm-config llvm-link
    GIT_REPOSITORY https://github.com/OpenCilk/cheetah.git
    GIT_TAG        opencilk/v2.1
    SOURCE_DIR     ${KITSUNE_CHEETAH_SOURCE_DIR}
    BINARY_DIR     ${KITSUNE_CHEETAH_BINARY_DIR}
    CMAKE_ARGS     ${KITSUNE_CHEETAH_BUILD_CMAKE_FLAGS})

  # The values here are populated given that we know where Cheetah will be
  # installed. The code to actual fetch, configure, build etc. is in
  # kitsune/runtime.
  set(KITSUNE_OPENCILK_DEFAULT_PREPROCESSOR_FLAGS
    "")

  set(KITSUNE_OPENCILK_DEFAULT_COMPILER_FLAGS
    "")

  # All the defaults are handled in the source code because finding the
  # OpenCilk runtime library is not straightforward on all platforms.
  set(KITSUNE_OPENCILK_DEFAULT_LINKER_FLAGS
    "")

  set(KITSUNE_OPENCILK_EXTRA_PREPROCESSOR_FLAGS
    ""
    CACHE STRING
    "Additional preprocessor flags needed for the OpenCilk target")

  set(KITSUNE_OPENCILK_EXTRA_COMPILER_FLAGS
    ""
    CACHE STRING
    "Additional compiler flags needed for the OpenCilk target")

  set(KITSUNE_OPENCILK_EXTRA_LINKER_FLAGS
    ""
    CACHE STRING
    "Additional linker flags needed for the OpenCilk target")

endif()

if (KITSUNE_OPENMP_ENABLE)
  # Do we need to check for libomp?

  set(KITSUNE_OPENMP_DEFAULT_PREPROCESSOR_FLAGS
    "")

  set(KITSUNE_OPENMP_DEFAULT_COMPILER_FLAGS
    "")

  set(KITSUNE_OPENMP_DEFAULT_LINKER_FLAGS
    "-fopenmp -lomp")

  set(KITSUNE_OPENMP_EXTRA_PREPROCESSOR_FLAGS
    ""
    CACHE STRING
    "Additional preprocessor flags needed for the OpenMP target")

  set(KITSUNE_OPENMP_EXTRA_COMPILER_FLAGS
    ""
    CACHE STRING
    "Additional compiler flags needed for the OpenMP target")

  set(KITSUNE_OPENMP_EXTRA_LINKER_FLAGS
    ""
    CACHE STRING
    "Additional linker flags needed for the OpenMP target")
endif()

if (KITSUNE_QTHREADS_ENABLE)
  # FIXME: Do find_package or something here for qthreads. That will set
  # QTHREADS_INCLUDE_DIR and QTHREADS_LIBRARY_DIR below.
  message(FATAL_ERROR "Finding package for QThreads has not been implemented")

  # TODO: Do we really need the qthreads include directory to be in the search
  # path?
  set(KITSUNE_QTHREADS_DEFAULT_PREPROCESSOR_FLAGS
    "-I${QTHREADS_INCLUDE_DIR}")

  set(KITSUNE_QTHREADS_DEFAULT_COMPILER_FLAGS
    "")

  set(KITSUNE_QTHREADS_DEFAULT_LINKER_FLAGS
    "-L${QTHREADS_LIBRARY_DIR} -lqthread -lhwloc")

  set(KITSUNE_QTHREADS_EXTRA_PREPROCESSOR_FLAGS
    ""
    CACHE STRING
    "Additional preprocessor flags needed for the Qthreads target")

  set(KITSUNE_QTHREADS_EXTRA_COMPILER_FLAGS
    ""
    CACHE STRING
    "Additional compiler flags needed for the Qthreads target")

  set(KITSUNE_QTHREADS_EXTRA_LINKER_FLAGS
    ""
    CACHE STRING
    "Additional linker flags needed for the Qthreads target")
endif()

if (KITSUNE_REALM_ENABLE)
  # FIXME: Do find_package or something here for realm. That will set
  # Realm_LIBRARY_DIR and other such variables which are used below.
  message(FATAL_ERROR "Finding package for Realm has not been implemented")

  set(KITSUNE_REALM_DEFAULT_PREPROCESSOR_FLAGS
    "")

  set(KITSUNE_REALM_DEFAULT_COMPILER_FLAGS
    "")

  set(KITSUNE_REALM_DEFAULT_LINKER_FLAGS
    "-L${Realm_LIBRARY_DIR} -L${Realm_WRAPPER_LIBRARY_DIR} -lrealm-abi -lrealm")

  set(KITSUNE_REALM_EXTRA_PREPROCESSOR_FLAGS
    ""
    CACHE STRING
    "Additional preprocessor flags needed for the Realm target")

  set(KITSUNE_REALM_EXTRA_COMPILER_FLAGS
    ""
    CACHE STRING
    "Additional compiler flags needed for the Realm target")

  set(KITSUNE_REALM_EXTRA_LINKER_FLAGS
    ""
    CACHE STRING
    "Additional linker flags needed for the Realm target")
endif()

# FIXME KITSUNE: We probably don't need this. The -ftapir flag requires a
# backend and not providing it disables Tapir lowering altogether. This may be
# useful if we ever change that and stop requiring -ftapir. There is a
# reasonable argument to be made for why we should always lower using Tapir, but
# then this will have to be set to some runtime that is actually built.
set(KITSUNE_DEFAULT_TAPIR_RUNTIME "opencilk" CACHE STRING
  "Default Tapir runtime used by -ftapir.")

# This is ugly because it creates a dangling symlink because this is executed
# before clang is built. However, we force clang to be built, so we are sure
# of it not dangling by the time everything is built.
add_custom_target(${KITSUNE_C_FRONTEND} ALL
  ${CMAKE_COMMAND} -E create_symlink
  clang
  ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${KITSUNE_C_FRONTEND})

add_custom_target(${KITSUNE_CXX_FRONTEND} ALL
  ${CMAKE_COMMAND} -E create_symlink
  clang++
  ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${KITSUNE_CXX_FRONTEND})

foreach (link ${KITSUNE_C_FRONTEND} ${KITSUNE_CXX_FRONTEND})
  install(FILES
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${link}
    DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()

add_subdirectory(configs)
add_subdirectory(include/kitsune)
add_subdirectory(experiments)

if(KITSUNE_INCLUDE_TESTS)
  add_subdirectory(test)
endif()

# We don't descend into the runtime subdirectory because that should only be
# configured when the runtimes are built. All the cmake environment variables
# that the runtime might need have to be passed through here.
#
# FIXME: Instead of passing all the Kitsune variables here by hand, look for
# for every variable starting with KITSUNE_, KITRT_ and LLVM_ and pass those
# in. Perhaps something like what is below.
#
#     get_cmake_property(vars VARIABLES)
#     list(FILTER vars INCLUDE REGEX "^KIT")
#     list(SORT vars)
#     foreach (var ${vars})
#       message(STATUS "${var}=${${var}}")
#     endforeach()
#
ExternalProject_Add(kitsune-runtimes
  DEPENDS llvm-config clang LLVM
  SOURCE_DIR ${KITSUNE_SOURCE_DIR}/runtime
  STAMP_DIR ${KITSUNE_BINARY_DIR}/stamp
  BINARY_DIR ${KITSUNE_BINARY_DIR}/runtime
  CMAKE_ARGS -DCMAKE_C_COMPILER=${LLVM_RUNTIME_OUTPUT_INTDIR}/clang
             -DCMAKE_CXX_COMPILER=${LLVM_RUNTIME_OUTPUT_INTDIR}/clang++
             -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
             -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
             -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
             -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
             -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
             -DCMAKE_COLOR_DIAGNOSTICS=${CMAKE_COLOR_DIAGNOSTICS}
             -DKITSUNE_CUDA_ENABLE=${KITSUNE_CUDA_ENABLE}
             -DKITSUNE_CUDA_INCLUDE_DIR=${KITSUNE_CUDA_INCLUDE_DIR}
             -DKITSUNE_CUDA_BINARY_DIR=${KITSUNE_CUDA_BINARY_DIR}
             -DKITSUNE_CUDA_LIBRARY_DIR=${KITSUNE_CUDA_LIBRARY_DIR}
             -DKITSUNE_CUDA_STUBS_DIR=${KITSUNE_CUDA_STUBS_DIR}
             -DKITSUNE_CUDA_LIB_CUDA=${KITSUNE_CUDA_LIB_CUDA}
             -DKITSUNE_CUDA_LIB_CUDART=${KITSUNE_CUDA_LIB_CUDART}
             -DKITSUNE_CUDA_LIB_NVPTX_STATIC=${KITSUNE_CUDA_LIB_NVPTX_STATIC}
             -DKITSUNE_HIP_ENABLE=${KITSUNE_HIP_ENABLE}
             -DKITSUNE_OPENCILK_ENABLE=${KITSUNE_OPENCILK_ENABLE}
             -DKITSUNE_OPENMP_ENABLE=${KITSUNE_OPENMP_ENABLE}
             -DKITSUNE_QTHREADS_ENABLE=${KITSUNE_QTHREADS_ENABLE}
             -DKITSUNE_REALM_ENABLE=${KITSUNE_REALM_ENABLE}
             -DKITSUNE_CUDA_VERSION_MIN=${KITSUNE_CUDA_VERSION_MIN}
             -DKITSUNE_ROCM_VERSION_MIN=${KITSUNE_ROCM_VERSION_MIN}
             -DKITRT_ENABLE_DEBUG=${KITRT_ENABLE_DEBUG}
             -DKITRT_ENABLE_VERBOSE=${KITRT_ENABLE_VERBOSE}
             -DKITCUDA_ENABLE_NVTX=${KITCUDA_ENABLE_NVTX}
             -DCLANG_RESOURCE_INTDIR=${CLANG_RESOURCE_INTDIR}
             -DCLANG_RESOURCE_DIR=${CLANG_RESOURCE_INSTALL_DIR}
             -DLLVM_CONFIG_PATH=${LLVM_RUNTIME_OUTPUT_INTDIR}/llvm-config
             -DLLVM_VERSION_MAJOR=${LLVM_VERSION_MAJOR}
             -DLLVM_VERSION_MINOR=${LLVM_VERSION_MINOR}
             -DLLVM_VERSION_PATCH=${LLVM_VERSION_PATCH}
             -DLLVM_BINARY_DIR=${LLVM_BINARY_DIR}
             -DLLVM_INCLUDE_DIR=${LLVM_INCLUDE_DIR}
             -DLLVM_LIBRARY_DIR=${LLVM_LIBRARY_DIR}
             -DLLVM_LIBRARY_OUTPUT_INTDIR=${LLVM_LIBRARY_OUTPUT_INTDIR}
             -DLLVM_RUNTIME_OUTPUT_INTDIR=${LLVM_RUNTIME_OUTPUT_INTDIR}
             -DLLVM_LIT_ARGS=${LLVM_LIT_ARGS}
             -DLLVM_LIBDIR_SUFFIX=${LLVM_LIBDIR_SUFFIX}
             -DLLVM_MAIN_INCLUDE_DIR=${LLVM_MAIN_INCLUDE_DIR}
             -DLLVM_RUNTIME_OUTPUT_INTDIR=${LLVM_RUNTIME_OUTPUT_INTDIR}
             -DCUDAToolkit_INCLUDE_DIRS=${CUDAToolkit_INCLUDE_DIRS}
             -DCUDAToolkit_LIBRARY_DIR=${CUDAToolkit_LIBRARY_DIR}
  USES_TERMINAL_CONFIGURE TRUE
  USES_TERMINAL_BUILD TRUE
  USES_TERMINAL_INSTALL TRUE
  BUILD_ALWAYS TRUE
  CONFIGURE_HANDLED_BY_BUILD TRUE)

