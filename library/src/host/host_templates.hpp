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
#ifndef ROCSHMEM_LIBRARY_SRC_HOST_HOST_TEMPLATES_HPP
#define ROCSHMEM_LIBRARY_SRC_HOST_HOST_TEMPLATES_HPP

#include "config.h"

#include "host_helpers.hpp"
#include "window_info.hpp"

namespace rocshmem {

template <typename T>
__host__ void
HostInterface::p(T* dest,
                 T value,
                 int pe,
                 WindowInfo* window_info) {
    DPRINTF("Function: host_p\n");
    putmem(dest,
           &value,
           sizeof(T),
           pe,
           window_info);
}

template <typename T>
__host__ void
HostInterface::put(T* dest,
                   const T* source,
                   size_t nelems,
                   int pe,
                   WindowInfo* window_info) {
    DPRINTF("Function: host_put\n");
    putmem(dest,
           source,
           sizeof(T) * nelems,
           pe,
           window_info);
}

template <typename T>
__host__ void
HostInterface::put_nbi(T* dest,
                       const T* source,
                       size_t nelems,
                       int pe,
                       WindowInfo* window_info) {
    DPRINTF("Function: host_put_nbi\n");
    putmem_nbi(dest,
               source,
               sizeof(T) * nelems,
               pe,
               window_info);
}

template <typename T>
__host__ T
HostInterface::g(const T* source,
                 int pe,
                 WindowInfo* window_info) {
    DPRINTF("Function: host_g\n");

    T ret {};

    /*
     * We don't call getmem directly here
     * since it flushes the local HDP. We
     * don't need the flush because the
     * destination buffer is on the CPU.
     */
    getmem_nbi(&ret,
               source,
               sizeof(T),
               pe,
               window_info);

    MPI_Win_flush_local(pe, window_info->get_win());

    return ret;
}

template <typename T>
__host__ void
HostInterface::get(T* dest,
                   const T* source,
                   size_t nelems,
                   int pe,
                   WindowInfo* window_info) {
    DPRINTF("Function: host_get\n");
    getmem(dest,
           source,
           sizeof(T) * nelems,
           pe,
           window_info);
}

template <typename T>
__host__ void
HostInterface::get_nbi(T* dest,
                       const T* source,
                       size_t nelems,
                       int pe,
                       WindowInfo* window_info) {
    DPRINTF("Function: host_get_nbi\n");
    getmem_nbi(dest,
               source,
               sizeof(T) * nelems,
               pe,
               window_info);
}

__host__ MPI_Comm
HostInterface::get_mpi_comm(int pe_start,
                            int log_pe_stride,
                            int pe_size) {
    MPI_Comm active_set_comm {};

    /*
     * First, check to see if the active set is the same as COMM_WORLD
     */
    int comm_world_size {-1};
    MPI_Comm_size(host_comm_world_, &comm_world_size);

    if (pe_start == 0 &&
        log_pe_stride == 0 &&
        pe_size == comm_world_size) {
        /*
         * Use the host interface's copy of MPI_COMM_WORLD
         * TODO: replace with a per-context copy of MPI_COMM_WORLD when we
         * have multiple contexts
         */
        active_set_comm = host_comm_world_;
        return active_set_comm;
    }

    /*
     * Then, check to see if we had already created a communicator for
     * this active set
     */
    ActiveSetKey key(pe_start, log_pe_stride, pe_size);

    auto it {comm_map.find(key)};
    if (it != comm_map.end()) {
        DPRINTF("Using cached communicator\n");
        return it->second;
    }

    /*
     * If there is not one cached, create a new one (expensive)
     */
    int active_set_ranks[pe_size];
    int stride {1 << log_pe_stride};
    active_set_ranks[0] = pe_start;

    for (int i {1}; i < pe_size; i++) {
        active_set_ranks[i] = active_set_ranks[i - 1] + stride;
    }

    MPI_Group comm_world_group {};
    MPI_Group active_set_group {};

    MPI_Comm_group(host_comm_world_, &comm_world_group);

    MPI_Group_incl(comm_world_group,
                   pe_size,
                   active_set_ranks,
                   &active_set_group);

    MPI_Comm_create_group(host_comm_world_,
                          active_set_group,
                          0,
                          &active_set_comm);

    /*
     * Cache the new communicator
     */
    DPRINTF("Created a new communicator. Now caching it\n");
    comm_map.insert(std::pair<ActiveSetKey, MPI_Comm>(key, active_set_comm));

    return active_set_comm;
}

template <typename T>
__host__ void
HostInterface::broadcast_internal(MPI_Comm mpi_comm,
                                  T* dest,
                                  const T* source,
                                  int nelems,
                                  int pe_root) {
    DPRINTF("Function: host_broadcast_internal\n");

    /*
     * Choose the right pointer for my buffer depending
     * on whether or not I am the root.
     */
    int active_set_rank {-1};
    void* buffer {nullptr};
    MPI_Comm_rank(mpi_comm, &active_set_rank);
    if (pe_root == active_set_rank) {
        buffer = const_cast<T*>(source);
    } else {
        buffer = const_cast<T*>(dest);
    }

    /*
     * Flush my HDP so that the NIC does not read stale values
     */
    hdp_policy_->hdp_flush();

    /*
     * Offload the broadcast to MPI
     */
    MPI_Bcast(buffer,
              nelems * sizeof(T),
              MPI_CHAR,
              pe_root,
              mpi_comm);

    return;
}

template <typename T>
__host__ void
HostInterface::broadcast(T* dest,
                         const T* source,
                         int nelems,
                         int pe_root,
                         int pe_start,
                         int log_pe_stride,
                         int pe_size,
                         long* p_sync) {
    DPRINTF("Function: host_broadcast\n");

    /*
     * Get an MPI communicator for active set of PEs
     * Note: pe_root is w.r.t the active set, hence
     * the MPI communicator contains the root as well.
     */
    MPI_Comm mpi_comm {get_mpi_comm(pe_start,
                                    log_pe_stride,
                                    pe_size)};

    broadcast_internal<T>(mpi_comm,
                          dest,
                          source,
                          nelems,
                          pe_root);

    return;
}

template <typename T>
__host__ void
HostInterface::broadcast(roc_shmem_team_t team,
                         T* dest,
                         const T* source,
                         int nelems,
                         int pe_root) {
    DPRINTF("Function: Team-based host_broadcast\n");

    /*
     * Get the MPI communicator of this team
     */
    Team* team_obj {get_internal_team(team)};
    MPI_Comm mpi_comm {team_obj->mpi_comm};

    broadcast_internal<T>(mpi_comm,
                          dest,
                          source,
                          nelems,
                          pe_root);

    return;
}

__host__ inline MPI_Op
HostInterface::get_mpi_op(ROC_SHMEM_OP Op) {
    switch(Op) {
        case ROC_SHMEM_SUM:
            return MPI_SUM;
        case ROC_SHMEM_MAX:
            return MPI_MAX;
        case ROC_SHMEM_MIN:
            return MPI_MIN;
        case ROC_SHMEM_PROD:
            return MPI_PROD;
        case ROC_SHMEM_AND:
            return MPI_BAND;
        case ROC_SHMEM_OR:
            return MPI_BOR;
        case ROC_SHMEM_XOR:
            return MPI_BXOR;
        default:
            fprintf(stderr, "Unknown ROC_SHMEM op MPI conversion %d\n", Op);
            exit(-1);
            return 0;
    }
}

template <typename T>
__host__ inline MPI_Datatype
HostInterface::get_mpi_type() {
    fprintf(stderr, "Unknown or unimplemented datatype \n");
}

#define GET_MPI_TYPE(T, MPI_T) \
    template <> \
    __host__ inline MPI_Datatype \
    HostInterface::get_mpi_type<T>() \
    { \
      return MPI_T; \
    }

GET_MPI_TYPE(int, MPI_INT)
GET_MPI_TYPE(unsigned int, MPI_UNSIGNED)
GET_MPI_TYPE(short, MPI_SHORT)
GET_MPI_TYPE(unsigned short, MPI_UNSIGNED_SHORT)
GET_MPI_TYPE(long, MPI_LONG)
GET_MPI_TYPE(unsigned long, MPI_UNSIGNED_LONG)
GET_MPI_TYPE(long long, MPI_LONG_LONG)
GET_MPI_TYPE(unsigned long long, MPI_UNSIGNED_LONG_LONG)
GET_MPI_TYPE(float, MPI_FLOAT)
GET_MPI_TYPE(double, MPI_DOUBLE)
GET_MPI_TYPE(char, MPI_CHAR)
GET_MPI_TYPE(signed char, MPI_SIGNED_CHAR)
GET_MPI_TYPE(unsigned char, MPI_UNSIGNED_CHAR)

template <typename T, ROC_SHMEM_OP Op>
__host__ void
HostInterface::to_all_internal(MPI_Comm mpi_comm,
                               T* dest,
                               const T* source,
                               int nreduce) {
    DPRINTF("Function: host_to_all_internal\n");

    MPI_Op mpi_op {get_mpi_op(Op)};

    MPI_Datatype mpi_type {get_mpi_type<T>()};

    void* send_buf {const_cast<T*>(source)};
    void* recv_buf {const_cast<T*>(dest)};

    /*
     * Flush my HDP so that the NIC does not read stale values
     */
    hdp_policy_->hdp_flush();

    /*
     * Offload the allreduce to MPI
     */
    MPI_Allreduce((dest == source) ? MPI_IN_PLACE : send_buf,
                  recv_buf,
                  nreduce,
                  mpi_type,
                  mpi_op,
                  mpi_comm);

    return;
}

template <typename T, ROC_SHMEM_OP Op>
__host__ void
HostInterface::to_all(T* dest,
                      const T* source,
                      int nreduce,
                      int pe_start,
                      int log_pe_stride,
                      int pe_size,
                      T* p_wrk,
                      long* p_sync) {
    DPRINTF("Function: host_to_all\n");

    /*
     * Get an MPI communicator for active set of PEs
     * Note: pe_root is w.r.t. the active set, hence
     * the MPI communicator contains the root as well.
     */
    MPI_Comm mpi_comm {get_mpi_comm(pe_start,
                                    log_pe_stride,
                                    pe_size)};

    to_all_internal<T, Op>(mpi_comm,
                           dest,
                           source,
                           nreduce);

    return;
}

template <typename T, ROC_SHMEM_OP Op>
__host__ void
HostInterface::to_all(roc_shmem_team_t team,
                      T* dest,
                      const T* source,
                      int nreduce) {
    DPRINTF("Function: Team-based host_to_all\n");

    /*
     * Get the MPI communicator of this team
     */
    Team* team_obj {get_internal_team(team)};
    MPI_Comm mpi_comm {team_obj->mpi_comm};

    to_all_internal<T, Op>(mpi_comm,
                           dest,
                           source,
                           nreduce);

    return;
}

template <typename T>
__host__ inline int
HostInterface::compare(roc_shmem_cmps cmp,
                       T input_val,
                       T target_val) {
    int cond_satisfied {0};

    switch (cmp) {
        case ROC_SHMEM_CMP_EQ:
            cond_satisfied = (input_val == target_val) ? 1 : 0;
            break;
        case ROC_SHMEM_CMP_NE:
            cond_satisfied = (input_val != target_val) ? 1 : 0;
            break;
        case ROC_SHMEM_CMP_GT:
            cond_satisfied = (input_val > target_val) ? 1 : 0;
            break;
        case ROC_SHMEM_CMP_GE:
            cond_satisfied = (input_val >= target_val) ? 1 : 0;
            break;
        case ROC_SHMEM_CMP_LT:
            cond_satisfied = (input_val < target_val) ? 1 : 0;
            break;
        case ROC_SHMEM_CMP_LE:
            cond_satisfied = (input_val <= target_val) ? 1 : 0;
            break;
        default:
            assert(cmp >= ROC_SHMEM_CMP_EQ && cmp <= ROC_SHMEM_CMP_LE);
            break;
        }

    return cond_satisfied;
}

template <typename T>
__host__ inline int
HostInterface::test_and_compare(MPI_Aint offset,
                                MPI_Datatype mpi_type,
                                roc_shmem_cmps cmp,
                                T val,
                                MPI_Win win) {
    T fetched_val {};

    /*
     * Flush the HDP so that the CPU doesn't read stale values
     */
    hdp_policy_->hdp_flush();

    MPI_Fetch_and_op(nullptr,  // because no operation happening here
                     &fetched_val,
                     mpi_type,
                     my_pe_,
                     offset,
                     MPI_NO_OP,
                     win);
    MPI_Win_flush_local(my_pe_, win);

    /*
     * Compare based on the operation
     */
    return compare(cmp, fetched_val, val);
}

template <typename T>
__host__ void
HostInterface::wait_until(T* ptr,
                          roc_shmem_cmps cmp,
                          T val,
                          WindowInfo* window_info) {
    DPRINTF("Function: host_wait_until\n");

    /*
     * Find the offset of this memory in the window
     */
    MPI_Aint offset {compute_offset(ptr,
                                    window_info->get_start(),
                                    window_info->get_end())};

    MPI_Datatype mpi_type {get_mpi_type<T>()};
    MPI_Win win {window_info->get_win()};

    /*
     * Continuously read the ptr atomically until it satisfies the condition
     */
    while (1) {
        int cond_satisfied {test_and_compare(offset,
                                             mpi_type,
                                             cmp,
                                             val,
                                             win)};

        if (cond_satisfied) {
            break;
        }
    }
}

template <typename T> __host__ int
HostInterface::test(T* ptr,
                    roc_shmem_cmps cmp,
                    T val,
                    WindowInfo* window_info) {
    DPRINTF("Function: host_test\n");

    /*
     * Find the offset of this memory in the window
     */
    MPI_Aint offset {compute_offset(ptr,
                                    window_info->get_start(),
                                    window_info->get_end())};

    MPI_Datatype mpi_type {get_mpi_type<T>()};

    return test_and_compare(offset,
                            mpi_type,
                            cmp,
                            val,
                            window_info->get_win());
}

}  // namespace rocshmem

#endif // ROCSHMEM_LIBRARY_SRC_HOST_HOST_TEMPLATES_HPP
