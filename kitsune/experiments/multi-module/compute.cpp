

// Copyright(c) 2020 Triad National Security, LLC
// All rights reserved.
//
// This file is part of the kitsune / llvm project.  It is released under
// the LLVM license.
//
// Simple example of an element-wise vector sum.
// To enable kitsune+tapir compilation add the flags to a standard
// clang compilation:
//
//    * -ftapir=rt-target : the runtime ABI to target.
//
#include <stdlib.h>
#include <kitsune.h>

void vecadd(const float *A,
	    const float *B,
	    float *C,
	    size_t N) {
  forall(size_t i = 0; i < N; ++i) {
    C[i] = A[i] + B[i];
  }
}
