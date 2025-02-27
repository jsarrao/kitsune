// Try the valid and invalid value two different ways because the command line
// argument is passed in a different place in each case and we need to check
// that both valid and invalid values are handled correctly in both places.

// Providing a valid value to ftapir should not produce any output and return
// a success code
// RUN: %kitxx -fsyntax-only -ftapir=openmp %s
// RUN: %kitxx -### -ftapir=openmp %s

// The option value is case sensitive.
// RUN: not %kitxx -fsyntax-only -ftapir=OpenMP %s 2>&1 | FileCheck %s -check-prefix=CHECK-BAD-TARGET
// RUN: %kitxx -### -ftapir=OpenMP %s 2>&1 | FileCheck %s -check-prefix=CHECK-BAD-TARGET

// CHECK-BAD-TARGET: invalid value '{{.+}}' in '-ftapir={{.+}}'
