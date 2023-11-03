#pragma once

#include <math_constants.h>
#include <cuda_runtime.h>
#include <cutlass/numeric_types.h>
#include <cutlass/array.h>
#include <cutlass/fast_math.h>

using cutlass::half_t;
using cutlass::Array;

#define hexp cutlass::fast_exp
#define hlog cutlass::fast_log

#define uint unsigned int
#define uchar unsigned char
#define ushort unsigned short

// Pack two half_t values.
inline __device__ unsigned
__pack_half2(const half_t x, const half_t y) {
  unsigned v0 = *((unsigned short *)&x);
  unsigned v1 = *((unsigned short *)&y);
  return (v1 << 16) | v0;
}