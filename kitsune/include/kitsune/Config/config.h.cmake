/*
 * Copyright (c) 2020 Triad National Security, LLC
 *                         All rights reserved.
 *
 * This file is part of the kitsune/llvm project.  It is released under
 * the LLVM license.
 */

/* This generated file is for internal use. Do not include it from headers. */

#ifdef KITSUNE_CONFIG_H
#error kitsune/Config/config.h should only be included once.
#else
#define KITSUNE_CONFIG_H

// Kokkos configuration
#cmakedefine01 KITSUNE_KOKKOS_ENABLE

#define KITSUNE_KOKKOS_EXTRA_PREPROCESSOR_FLAGS "${KITSUNE_KOKKOS_EXTRA_PREPROCESSOR_FLAGS}"
#define KITSUNE_KOKKOS_EXTRA_COMPILER_FLAGS "${KITSUNE_KOKKOS_EXTRA_COMPILER_FLAGS}"
#define KITSUNE_KOKKOS_EXTRA_LINKER_FLAGS "${KITSUNE_KOKKOS_EXTRA_LINKER_FLAGS}"

// Cuda configuration
#cmakedefine01 KITSUNE_CUDA_ENABLE

#define KITSUNE_CUDA_EXTRA_PREPROCESSOR_FLAGS "${KITSUNE_CUDA_EXTRA_PREPROCESSOR_FLAGS}"
#define KITSUNE_CUDA_EXTRA_COMPILER_FLAGS "${KITSUNE_CUDA_EXTRA_COMPILER_FLAGS}"
#define KITSUNE_CUDA_EXTRA_LINKER_FLAGS "${KITSUNE_CUDA_EXTRA_LINKER_FLAGS}"

#define KITSUNE_CUDA_PREFIX        "${KITSUNE_CUDA_PREFIX}"
#define KITSUNE_CUDA_BINARY_DIR    "${KITSUNE_CUDA_BINARY_DIR}"
#define KITSUNE_CUDA_LIBRARY_DIR   "${KITSUNE_CUDA_LIBRARY_DIR}"
#define KITSUNE_CUDA_STUBS_DIR     "${KITSUNE_CUDA_STUBS_DIR}"
#define KITSUNE_CUDA_LIBDEVICE_DIR "${KITSUNE_CUDA_LIBDEVICE_DIR}"
#define KITSUNE_CUDA_LIBDEVICE_BC  "${KITSUNE_CUDA_LIBDEVICE_BC}"
#define KITSUNE_CUDA_PTXAS         "${KITSUNE_CUDA_PTXAS}"
#define KITSUNE_CUDA_FATBINARY     "${KITSUNE_CUDA_FATBINARY}"

// Hip configuration
#cmakedefine01 KITSUNE_HIP_ENABLE

#define KITSUNE_HIP_PREFIX                   "${KITSUNE_HIP_PREFIX}"
#define KITSUNE_HIP_LIBRARY_DIR              "${KITSUNE_HIP_LIBRARY_DIR}"
#define KITSUNE_HIP_ALT_LIBRARY_DIR          "${KITSUNE_HIP_ALT_LIBRARY_DIR}"
#define KITSUNE_HIP_EXTRA_PREPROCESSOR_FLAGS "${KITSUNE_HIP_EXTRA_PREPROCESSOR_FLAGS}"
#define KITSUNE_HIP_EXTRA_COMPILER_FLAGS     "${KITSUNE_HIP_EXTRA_COMPILER_FLAGS}"
#define KITSUNE_HIP_EXTRA_LINKER_FLAGS       "${KITSUNE_HIP_EXTRA_LINKER_FLAGS}"
#define KITSUNE_HIP_BITCODE_DIR              "${KITSUNE_HIP_BITCODE_DIR}"

// OpenCilk configuration
#cmakedefine01 KITSUNE_OPENCILK_ENABLE

#define KITSUNE_OPENCILK_EXTRA_PREPROCESSOR_FLAGS "${KITSUNE_OPENCILK_EXTRA_PREPROCESSOR_FLAGS}"
#define KITSUNE_OPENCILK_EXTRA_COMPILER_FLAGS "${KITSUNE_OPENCILK_EXTRA_COMPILER_FLAGS}"
#define KITSUNE_OPENCILK_EXTRA_LINKER_FLAGS "${KITSUNE_OPENCILK_EXTRA_LINKER_FLAGS}"

// OpenMP configuration
#cmakedefine01 KITSUNE_OPENMP_ENABLE

#define KITSUNE_OPENMP_EXTRA_PREPROCESSOR_FLAGS "${KITSUNE_OPENMP_EXTRA_PREPROCESSOR_FLAGS}"
#define KITSUNE_OPENMP_EXTRA_COMPILER_FLAGS "${KITSUNE_OPENMP_EXTRA_COMPILER_FLAGS}"
#define KITSUNE_OPENMP_EXTRA_LINKER_FLAGS "${KITSUNE_OPENMP_EXTRA_LINKER_FLAGS}"

// Qthreads configuration
#cmakedefine01 KITSUNE_QTHREADS_ENABLE

#define KITSUNE_QTHREADS_EXTRA_PREPROCESSOR_FLAGS "${KITSUNE_QTHREADS_EXTRA_PREPROCESSOR_FLAGS}"
#define KITSUNE_QTHREADS_EXTRA_COMPILER_FLAGS  "${KITSUNE_QTHREADS_EXTRA_COMPILER_FLAGS}"
#define KITSUNE_QTHREADS_EXTRA_LINKER_FLAGS "${KITSUNE_QTHREADS_EXTRA_LINKER_FLAGS}"

// Realm configuration
#cmakedefine01 KITSUNE_REALM_ENABLE

#define KITSUNE_REALM_EXTRA_PREPROCESSOR_FLAGS "${KITSUNE_REALM_EXTRA_PREPROCESSOR_FLAGS}"
#define KITSUNE_REALM_EXTRA_COMPILER_FLAGS "${KITSUNE_REALM_EXTRA_COMPILER_FLAGS}"
#define KITSUNE_REALM_EXTRA_LINKER_FLAGS "${KITSUNE_REALM_EXTRA_LINKER_FLAGS}"

#endif
