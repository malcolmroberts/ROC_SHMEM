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

#ifndef ROCSHMEM_LIBRARY_SRC_REVERSE_OFFLOAD_RO_TEAM_PROXY_HPP
#define ROCSHMEM_LIBRARY_SRC_REVERSE_OFFLOAD_RO_TEAM_PROXY_HPP

#include "mpi.h"

#include "device_proxy.hpp"
#include "ro_net_team.hpp"
#include "team_info_proxy.hpp"

namespace rocshmem {

template <typename ALLOCATOR>
class ROTeamProxy {
    using ProxyT = DeviceProxy<ALLOCATOR, ROTeam>;

  public:
    /*
     * Placement new the memory which is allocated by proxy_
     */
    ROTeamProxy(Backend *backend) {
        MPI_Comm_dup(transport_.get_world_comm(), &team_world_comm_);

        new (proxy_.get()) ROTeam(backend,
                                  wrt_parent_.get(),
                                  wrt_world_.get(),
                                  team_size_,
                                  my_pe_,
                                  team_world_comm_);
    }

    /*
     * Since placement new is called in the constructor, then
     * delete must be called manually.
     */
    ~ROTeamProxy() {
        proxy_.get()->~ROTeam();

        MPI_Comm_free(&team_world_comm_);
    }

    /*
     * @brief Provide access to the memory referenced by the proxy
     */
    __host__ __device__
    ROTeam*
    get() {
        return proxy_.get();
    }

  private:
    /**
     * @brief Holds duplicated mpi world communicator.
     */
    MPI_Comm team_world_comm_ {MPI_COMM_NULL};

    /**
     * @brief Used by TeamInfo members and the constructor to build ROTeam.
     */
    MPITransport transport_ {};

    /**
     * @brief Used for team information.
     */
    int my_pe_ {transport_.getMyPe()};

    /**
     * @brief Used for team information.
     */
    int team_size_ {transport_.getNumPes()};

    /**
     * @brief Input for TeamInfo proxies.
     */
    int pe_start_ {0};

    /**
     * @brief Input for TeamInfo proxies.
     */
    int stride_ {1};

    /**
     * @brief Used by the constructor to build out the ROTeam.
     *
     * @note This embedded proxy object manages its own memory.
     */
    TeamInfoProxyT wrt_parent_ {nullptr, pe_start_, stride_, team_size_};

    /**
     * @brief Used by the constructor to build out the ROTeam.
     *
     * @note This embedded proxy object manages its own memory.
     */
    TeamInfoProxyT wrt_world_ {nullptr, pe_start_, stride_, team_size_};

    /*
     * @brief Memory managed by the lifetime of this object
     */
    ProxyT proxy_ {};
};

using ROTeamProxyT = ROTeamProxy<HIPAllocator>;

}  // namespace rocshmem

#endif  // ROCSHMEM_LIBRARY_SRC_REVERSE_OFFLOAD_RO_TEAM_PROXY_HPP
