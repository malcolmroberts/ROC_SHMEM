/******************************************************************************
 * Copyright (c) 2022 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *****************************************************************************/

#ifndef ROCSHMEM_LIBRARY_SRC_HEAP_TYPE_HPP
#define ROCSHMEM_LIBRARY_SRC_HEAP_TYPE_HPP

#include "hip_allocator.hpp"

#include "config.h"

/**
 * @file heap_type.hpp
 *
 * @brief Contains the type of heap memory used to allocate the symmetric heap
 *
 * The heap type in this file is used by other classes to select the allocation
 * policy for the symmetric heap. The heap type choices depend on whether
 * the heap is cacheable in hardware and if the heap is a managed memory type.
 */

namespace rocshmem {

#if defined USE_MANAGED_HEAP
    using HEAP_T = HeapMemory<HIPAllocatorManaged>;
#elif defined USE_COHERENT_HEAP || defined USE_CACHED_HEAP
    using HEAP_T = HeapMemory<HIPAllocator>;
#elif defined USE_HOST_HEAP
    using HEAP_T = HeapMemory<HostAllocator>;
#elif defined USE_HIP_HOST_HEAP
    using HEAP_T = HeapMemory<HIPHostAllocator>;
#else
    using HEAP_T = HeapMemory<HIPAllocatorFinegrained>;
#endif

} // namespace rocshmem

#endif  // ROCSHMEM_LIBRARY_SRC_HEAP_TYPE_HPP
