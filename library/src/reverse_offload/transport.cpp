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

#include "transport.hpp"
#include "ro_net_internal.hpp"
#include "util.hpp"
#include "backend_ro.hpp"

using namespace std;

namespace rocshmem {

#define NET_CHECK(cmd) \
{\
    if (cmd != MPI_SUCCESS) {\
        fprintf(stderr, "Unrecoverable error: MPI Failure\n");\
        exit(-1);\
    }\
}

MPI_Op
MPITransport::get_mpi_op(ROC_SHMEM_OP op)
{
    switch(op){
        case ROC_SHMEM_SUM : return MPI_SUM;
            break;
        case ROC_SHMEM_MAX : return MPI_MAX;
            break;
        case ROC_SHMEM_MIN : return MPI_MIN;
            break;
        case ROC_SHMEM_PROD: return MPI_PROD;
            break;
        case ROC_SHMEM_AND : return MPI_BAND;
            break;
        case ROC_SHMEM_OR  : return MPI_BOR;
            break;
        case ROC_SHMEM_XOR : return MPI_BXOR;
            break;
        default:
            break;
    }
}

MPITransport::MPITransport()
    : Transport(), hostBarrierDone(0), transport_up(false), handle(nullptr)
{
    int provided;
    indices = new int[INDICES_SIZE];

    int init_done = 0;
    NET_CHECK(MPI_Initialized(&init_done));

    if (!init_done) {
        NET_CHECK(MPI_Init_thread(0, 0, MPI_THREAD_MULTIPLE, &provided));
        if (provided != MPI_THREAD_MULTIPLE) {
            fprintf(stderr, "Warning requested multi-thread level is not "
                        "supported \n");
        }
    }
    NET_CHECK(MPI_Comm_dup(MPI_COMM_WORLD, &ro_net_comm_world));

    NET_CHECK(MPI_Comm_size(ro_net_comm_world, &num_pes));
    NET_CHECK(MPI_Comm_rank(ro_net_comm_world, &my_pe));
}

MPITransport::~MPITransport()
{
    delete [] indices;
}

void
MPITransport::threadProgressEngine()
{
    transport_up = true;
    while (!handle->done_flag) {
        submitRequestsToMPI();
        progress();
    }
    transport_up = false;
}

void
MPITransport::insertRequest(const queue_element_t * element, int queue_id)
{
    std::unique_lock<std::mutex> mlock(queue_mutex);
    q.push(element);
    q_wgid.push(queue_id);
}

void
MPITransport::submitRequestsToMPI()
{
    if (q.empty())
        return;

    std::unique_lock<std::mutex> mlock(queue_mutex);

    const queue_element_t *next_element = q.front();
    int queue_idx = q_wgid.front();
    q.pop();
    q_wgid.pop();
    mlock.unlock();

    switch (next_element->type) {
        case RO_NET_PUT: //put
            putMem(next_element->dst, next_element->src,
                                next_element->size, next_element->PE,
                                queue_idx, next_element->threadId, true);

            DPRINTF(("Received PUT dst %p src %p size %d "
                    "pe %d\n",
                    next_element->dst, next_element->src,
                    next_element->size, next_element->PE));

            break;
        case RO_NET_P: //put
            {
            // No equivalent inline OP for MPI, so allocate a temp buffer
            // for value.
            void * source_buffer = malloc(next_element->size);

            ::memcpy(source_buffer,
                     &next_element->src,
                     next_element->size);

            putMem(next_element->dst, source_buffer,
                   next_element->size, next_element->PE,
                   queue_idx, next_element->threadId, true, true);

            DPRINTF(("Received P dst %p value %p pe %d\n",
                    next_element->dst, next_element->src,
                    next_element->PE));

            break;
            }
        case RO_NET_GET: //get
            getMem(next_element->dst, next_element->src,
                                next_element->size, next_element->PE,
                                queue_idx, next_element->threadId, true);

            DPRINTF(("Received GET dst %p src %p size %d pe %d\n",
                    next_element->dst, next_element->src,
                    next_element->size, next_element->PE));

            break;
        case RO_NET_PUT_NBI: //put_nbi
            putMem(next_element->dst, next_element->src,
                                next_element->size, next_element->PE,
                                queue_idx, next_element->threadId, false);
            DPRINTF(("Received PUT NBI dst %p src %p size %d "
                    "pe %d\n",
                    next_element->dst, next_element->src,
                    next_element->size, next_element->PE));

            break;
        case RO_NET_GET_NBI: //get_nbi;
            getMem(next_element->dst, next_element->src,
                                next_element->size, next_element->PE,
                                queue_idx, next_element->threadId, false);

            DPRINTF(("Received GET NBI dst %p src %p size %d "
                        "pe %d\n", next_element->dst, next_element->src,
                        next_element->size, next_element->PE));

            break;
        case RO_NET_AMO_FOP: //AMD Fetch Op
            amoFOP(next_element->dst, next_element->src,
                                next_element->size, next_element->PE,
                                queue_idx, next_element->threadId, true,
                                (ROC_SHMEM_OP) next_element->op);

            DPRINTF(("Received AMO dst %p src %p Val %d "
                    "pe %d\n",
                    next_element->dst, next_element->src,
                    next_element->size, next_element->PE));

            break;
        case RO_NET_AMO_FCAS: //AMD Fetch CSWAP
            amoFCAS(next_element->dst, next_element->src,
                                next_element->size, next_element->PE,
                                queue_idx, next_element->threadId, true,
                                (int64_t) next_element->pWrk);

            DPRINTF(("Received F_CSWAP dst %p src %p Val %ld "
                    "pe %d cond %ld\n",
                    next_element->dst, next_element->src,
                    next_element->size, next_element->PE,
                    (int64_t)next_element->pWrk ));

            break;
        case RO_NET_TEAM_TO_ALL: //float_sum_to_all;
            team_reduction(next_element->dst, next_element->src,
                                    next_element->size,
                                    queue_idx,
                                    next_element->team_comm,
                                    (ROC_SHMEM_OP) next_element->op,
                                    (ro_net_types) next_element->datatype,
                                    next_element->threadId, true);

            DPRINTF(("Received FLOAT_SUM_TEAM_TO_ALL dst %p src %p size %d "
                    "team %d\n ", next_element->dst, next_element->src,
                    next_element->size, next_element->team_comm));

            break;
        case RO_NET_TO_ALL: //float_sum_to_all;
            reduction(next_element->dst, next_element->src,
                                    next_element->size,
                                    next_element->PE, queue_idx,
                                    next_element->PE,
                                    next_element->logPE_stride,
                                    next_element->PE_size, next_element->pWrk,
                                    next_element->pSync,
                                    (ROC_SHMEM_OP) next_element->op,
                                    (ro_net_types) next_element->datatype,
                                    next_element->threadId, true);

            DPRINTF(("Received FLOAT_SUM_TO_ALL dst %p src %p size %d "
                    "PE_start %d, logPE_stride %d, PE_size %d, pWrk %p, "
                    "pSync %p\n", next_element->dst, next_element->src,
                    next_element->size, next_element->PE,
                    next_element->logPE_stride, next_element->PE_size,
                    next_element->pWrk, next_element->pSync));

            break;
        case RO_NET_TEAM_BROADCAST:
            team_broadcast(next_element->dst, next_element->src,
                                    next_element->size, queue_idx,
                                    next_element->team_comm,
                                    next_element->PE_root,
                                    (ro_net_types) next_element->datatype,
                                    next_element->threadId, true);

            DPRINTF(("Received TEAM_BROADCAST  dst %p src %p size %d "
                    "team %d, PE_root %d \n", next_element->dst,
                    next_element->src, next_element->size,
                    next_element->team_comm, next_element->PE_root));

            break;

        case RO_NET_BROADCAST:
            broadcast(next_element->dst, next_element->src,
                                    next_element->size,
                                    next_element->PE, queue_idx,
                                    next_element->PE,
                                    next_element->logPE_stride,
                                    next_element->PE_size,
                                    next_element->PE_root,
                                    next_element->pSync,
                                    (ro_net_types) next_element->datatype,
                                    next_element->threadId, true);

            DPRINTF(("Received BROADCAST  dst %p src %p size %d "
                    "PE_start %d, logPE_stride %d, PE_size %d, PE_root %d, "
                    "pSync %p\n", next_element->dst, next_element->src,
                    next_element->size, next_element->PE,
                    next_element->logPE_stride, next_element->PE_size,
                    next_element->PE_root, next_element->pSync));

            break;

        case RO_NET_BARRIER_ALL: //Barrier_all;
            barrier(queue_idx, next_element->threadId, true);

            DPRINTF(("Received Barrier_all\n"));

            break;

        case RO_NET_FENCE: //fence
        case RO_NET_QUIET: //quiet
            quiet(queue_idx, next_element->threadId);
            DPRINTF(("Received FENCE/QUIET\n"));

            break;
        case RO_NET_FINALIZE: //finalize
            quiet(queue_idx, next_element->threadId);
            DPRINTF(("Received Finalize\n"));

            break;
        default:
            fprintf(stderr,
                    "Invalid GPU Packet received, exiting....\n");
            exit(-1);
            break;
    }

    delete next_element;
}

Status
MPITransport::initTransport(int num_queues,
                            ro_net_handle *ro_net_gpu_handle)
{

    waiting_quiet.resize(num_queues, std::vector<int>());
    outstanding.resize(num_queues, 0);
    transport_up = false;
    handle = ro_net_gpu_handle;
    host_interface = new HostInterface(handle->hdp_policy, ro_net_comm_world);
    progress_thread =
        new std::thread(&MPITransport::threadProgressEngine, this);
    while (!transport_up);
    return Status::ROC_SHMEM_SUCCESS;
}

Status
MPITransport::finalizeTransport()
{
    progress_thread->join();
    delete progress_thread;
    delete host_interface;

    return Status::ROC_SHMEM_SUCCESS;
}

roc_shmem_team_t
get_external_team(ROTeam *team)
{
    return reinterpret_cast<roc_shmem_team_t>(team);
}

Status
MPITransport::createNewTeam(ROBackend *backend_handle,
                            Team *parent_team,
                            TeamInfo *team_info_wrt_parent,
                            TeamInfo *team_info_wrt_world,
                            int num_pes,
                            int my_pe_in_new_team,
                            MPI_Comm team_comm,
                            roc_shmem_team_t *new_team) {
    ROTeam *new_team_obj;

    /**
     * Allocate device-side memory for team_world and
     * construct a RO team in it
     */
    CHECK_HIP(hipMalloc(&new_team_obj, sizeof(ROTeam)));
    new (new_team_obj) ROTeam(*backend_handle,
                              team_info_wrt_parent,
                              team_info_wrt_world,
                              num_pes,
                              my_pe_in_new_team,
                              team_comm);

    *new_team = get_external_team(new_team_obj);

    return Status::ROC_SHMEM_SUCCESS;
}

MPI_Comm
MPITransport::createComm(int start, int logPstride, int size)
{
    // Check if communicator is cached
    CommKey key(start, logPstride, size);
    MPI_Comm comm;
    auto it = comm_map.find(key);
    if (it != comm_map.end()) {
        DPRINTF(("Using cached communicator\n"));
        return it->second;
    }

    int world_size;
    NET_CHECK(MPI_Comm_size(ro_net_comm_world, &world_size));

    if (start == 0 && logPstride == 0 && size == world_size) {
        NET_CHECK(MPI_Comm_dup(ro_net_comm_world, &comm));
    } else {

        MPI_Group world_group;
        NET_CHECK(MPI_Comm_group(ro_net_comm_world, &world_group));

        int group_ranks[size];
        int stride = 2^(logPstride);
        group_ranks[0] =start;
        for (int i = 1 ; i < size; i++)
            group_ranks[i]= group_ranks[i-1]+stride;

        MPI_Group new_group;
        NET_CHECK(MPI_Group_incl(world_group, size, group_ranks, &new_group));
        NET_CHECK(MPI_Comm_create_group(ro_net_comm_world, new_group, 0, &comm));
    }

    comm_map.insert(std::pair<CommKey, MPI_Comm>(key, comm));
    DPRINTF(("Creating new communicator\n"));

    return comm;
}

void
MPITransport::global_exit(int status)
{
    MPI_Abort(ro_net_comm_world, status);
}

Status
MPITransport::barrier(int wg_id, int threadId, bool blocking)
{
    MPI_Request request;
    NET_CHECK(MPI_Ibarrier(ro_net_comm_world, &request));

    req_prop_vec.emplace_back(threadId, wg_id, blocking);
    req_vec.push_back(request);
    outstanding[wg_id]++;
    return Status::ROC_SHMEM_SUCCESS;
}

static MPI_Op
convertOp(ROC_SHMEM_OP op)
{
    switch(op) {
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
            fprintf(stderr, "Unknown ROC_SHMEM op MPI conversion %d\n", op);
            exit(-1);
            return 0;
    }
}

static MPI_Op
convertType(ro_net_types type)
{
    switch(type) {
        case RO_NET_FLOAT:
            return MPI_FLOAT;
        case RO_NET_DOUBLE:
            return MPI_DOUBLE;
        case RO_NET_INT:
            return MPI_INT;
        case RO_NET_LONG:
            return MPI_LONG;
        case RO_NET_LONG_LONG:
            return MPI_LONG_LONG;
        case RO_NET_SHORT:
            return MPI_SHORT;
        case RO_NET_LONG_DOUBLE:
            return MPI_LONG_DOUBLE;
        default:
            fprintf(stderr, "Unknown ROC_SHMEM type MPI conversion %d\n",
                    type);
            exit(-1);
            return 0;
    }
}

Status
MPITransport::reduction(void *dst, void *src, int size, int pe, int wg_id,
                        int start, int logPstride, int sizePE,
                        void *pWrk, long *pSync, ROC_SHMEM_OP op,
                        ro_net_types type, int threadId, bool blocking)
{
    MPI_Request request;

    MPI_Op mpi_op = convertOp(op);
    MPI_Datatype mpi_type = convertType(type);
    MPI_Comm comm = createComm(start, logPstride, sizePE);

    if (dst == src) {
        NET_CHECK(MPI_Iallreduce(MPI_IN_PLACE, dst, size, mpi_type, mpi_op,
                                 comm, &request));
    } else {
        NET_CHECK(MPI_Iallreduce(src, dst, size, mpi_type, mpi_op, comm,
                                 &request));
    }

    req_prop_vec.emplace_back(threadId, wg_id, blocking);
    req_vec.push_back(request);
    outstanding[wg_id]++;
    return Status::ROC_SHMEM_SUCCESS;
}
Status
MPITransport::broadcast(void *dst, void *src, int size, int pe, int wg_id,
                        int start, int logPstride, int sizePE,
                        int root, long *pSync, ro_net_types type,
                        int threadId, bool blocking)
{
    MPI_Request request;
    int new_rank;
    void *data;

    MPI_Datatype mpi_type = convertType(type);
    MPI_Comm comm = createComm(start, logPstride, sizePE);
    MPI_Comm_rank(comm, &new_rank);
    if(new_rank == root)
        data = src;
    else
        data = dst;

    NET_CHECK(MPI_Ibcast(data, size, mpi_type, root, comm,
                                 &request));

    req_prop_vec.emplace_back(threadId, wg_id, blocking);
    req_vec.push_back(request);
    outstanding[wg_id]++;
    return Status::ROC_SHMEM_SUCCESS;
}

Status
MPITransport::team_reduction(void *dst, void *src, int size, int wg_id,
                             MPI_Comm team, ROC_SHMEM_OP op, ro_net_types type,
                             int threadId, bool blocking)
{
    MPI_Request request;

    MPI_Op mpi_op = convertOp(op);
    MPI_Datatype mpi_type = convertType(type);
    MPI_Comm comm = team;

    if (dst == src) {
        NET_CHECK(MPI_Iallreduce(MPI_IN_PLACE, dst, size, mpi_type, mpi_op,
                                 comm, &request));
    } else {
        NET_CHECK(MPI_Iallreduce(src, dst, size, mpi_type, mpi_op, comm,
                                 &request));
    }

    req_prop_vec.emplace_back(threadId, wg_id, blocking);
    req_vec.push_back(request);
    outstanding[wg_id]++;
    return Status::ROC_SHMEM_SUCCESS;
}

Status
MPITransport::team_broadcast(void *dst, void *src, int size, int wg_id,
                             MPI_Comm team, int root, ro_net_types type,
                             int threadId, bool blocking)
{
    MPI_Request request;
    int new_rank;
    void *data;

    MPI_Datatype mpi_type = convertType(type);
    MPI_Comm comm = team;
    MPI_Comm_rank(comm, &new_rank);
    if(new_rank == root)
        data = src;
    else
        data = dst;

    NET_CHECK(MPI_Ibcast(data, size, mpi_type, root, comm,
                                 &request));

    req_prop_vec.emplace_back(threadId, wg_id, blocking);
    req_vec.push_back(request);
    outstanding[wg_id]++;
    return Status::ROC_SHMEM_SUCCESS;
}

Status
MPITransport::putMem(void *dst, void *src, int size, int pe, int wg_id,
                     int threadId, bool blocking, bool inline_data)
{
    MPI_Request request;

    if (!handle->gpu_queue) {
        // Need to flush HDP read cache so that the NIC can see data to push
        // out to the network.  If we have the network buffers allocated
        // on the host or we've already flushed for the command queue on the
        // GPU then we can ignore this step.
        handle->hdp_policy->hdp_flush();
    }

    NET_CHECK(MPI_Rput(src, size, MPI_CHAR, pe,
                       handle->heap_window_info->get_offset(dst),
                       size, MPI_CHAR, handle->heap_window_info->get_win(),
                       &request));

    // Since MPI makes puts as complete as soon as the local buffer is free,
    // we need a flush to satisfy quiet.  Put it here as a hack for now even
    // though it should be in the progress loop.
    NET_CHECK(MPI_Win_flush_all(handle->heap_window_info->get_win()));

    req_prop_vec.emplace_back(threadId, wg_id, blocking, src, inline_data);
    req_vec.push_back(request);
    outstanding[wg_id]++;
    return Status::ROC_SHMEM_SUCCESS;
}

Status
MPITransport::amoFOP(void *dst, void *src, int64_t val, int pe, int wg_id,
                     int threadId, bool blocking, ROC_SHMEM_OP op)
{
    //MPI_Request request;

    if (!handle->gpu_queue) {
        // Need to flush HDP read cache so that the NIC can see data to push
        // out to the network.  If we have the network buffers allocated
        // on the host or we've already flushed for the command queue on the
        // GPU then we can ignore this step.
        handle->hdp_policy->hdp_flush();
    }

    NET_CHECK(MPI_Fetch_and_op((void*)&val, src, MPI_INT64_T, pe,
                                handle->heap_window_info->get_offset(dst),
                                get_mpi_op(op),
                                handle->heap_window_info->get_win()));

    // Since MPI makes puts as complete as soon as the local buffer is free,
    // we need a flush to satisfy quiet.  Put it here as a hack for now even
    // though it should be in the progress loop.
    NET_CHECK(MPI_Win_flush_local(pe, handle->heap_window_info->get_win()));

    handle->queue_descs[wg_id].status[threadId] = 1;
    if (handle->gpu_queue) {
        SFENCE();
        handle->hdp_policy->hdp_flush();
    }

    //req_prop_vec.emplace_back(threadId, wg_id, blocking, src, inline_data);
    //req_vec.push_back(request);
    //outstanding[wg_id]++;
    return Status::ROC_SHMEM_SUCCESS;
}

Status
MPITransport::amoFCAS(void *dst, void *src, int64_t val, int pe, int wg_id,
                     int threadId, bool blocking, int64_t cond)
{
    //MPI_Request request;

    if (!handle->gpu_queue) {
        // Need to flush HDP read cache so that the NIC can see data to push
        // out to the network.  If we have the network buffers allocated
        // on the host or we've already flushed for the command queue on the
        // GPU then we can ignore this step.
        handle->hdp_policy->hdp_flush();
    }

    NET_CHECK(MPI_Compare_and_swap((const void*)&val, (const void*) &cond,
                                    src, MPI_INT64_T, pe,
                                    handle->heap_window_info->get_offset(dst),
                                    handle->heap_window_info->get_win()));

    // Since MPI makes puts as complete as soon as the local buffer is free,
    // we need a flush to satisfy quiet.  Put it here as a hack for now even
    // though it should be in the progress loop.
    NET_CHECK(MPI_Win_flush_local(pe, handle->heap_window_info->get_win()));

    handle->queue_descs[wg_id].status[threadId] = 1;
    if (handle->gpu_queue) {
        SFENCE();
        handle->hdp_policy->hdp_flush();
    }

    return Status::ROC_SHMEM_SUCCESS;
}


Status
MPITransport::getMem(void *dst, void *src, int size, int pe, int wg_id,
                     int threadId, bool blocking)
{

    MPI_Request request;

    outstanding[wg_id]++;

    NET_CHECK(MPI_Rget(dst, size, MPI_CHAR, pe, handle->heap_window_info->get_offset(src),
                    size, MPI_CHAR, handle->heap_window_info->get_win(), &request));

   req_prop_vec.emplace_back(threadId, wg_id, blocking);
   req_vec.push_back(request);

    return Status::ROC_SHMEM_SUCCESS;
}

Status
MPITransport::progress()
{
    MPI_Status status;
    int flag = 0;

    DPRINTF(("Entering progress engine\n"));

    // Enter the progress engine if there aren't any pending requests
    if (req_vec.size() == 0) {
        DPRINTF(("Probing MPI\n"));

        NET_CHECK(MPI_Iprobe(num_pes - 1, 1000,
                            ro_net_comm_world, &flag, &status));

    } else {
        DPRINTF(("Testing all outstanding requests (%zu)\n",
                req_vec.size()));

        // Check completion of any oustanding requests.  We check on either
        // the first 64 requests or the size of the request vector
        int incount = (req_vec.size() < INDICES_SIZE) ?
            req_vec.size() : INDICES_SIZE;
        int outcount;
        NET_CHECK(MPI_Testsome(incount, req_vec.data(), &outcount,
                               indices, MPI_STATUSES_IGNORE));
        // If any request has completed remove it from the outstanding request
        // vector
        for (int i = 0; i < outcount; i++) {
            int indx = indices[i];
            int wg_id = req_prop_vec[indx].wgId;
            int threadId = req_prop_vec[indx].threadId;

            if (wg_id != -1) {
                outstanding[wg_id]--;
                DPRINTF(("Finished op for wg_id %d at threadId %d "
                        "(%d requests outstanding)\n",
                        wg_id, threadId, outstanding[wg_id]));
            } else {
                DPRINTF(("Finished host barrier\n"));
                hostBarrierDone = 1;
            }

            if (req_prop_vec[indx].blocking) {
                if (wg_id != -1)
                    handle->queue_descs[wg_id].status[threadId] = 1;
                if (handle->gpu_queue) {
                    SFENCE();
                    handle->hdp_policy->hdp_flush();
                }
            }

            if (req_prop_vec[indx].inline_data)
                free(req_prop_vec[indx].src);

            // If the GPU has requested a quiet, notify it of completion when
            // all outstanding requests are complete.
            if (!outstanding[wg_id] && !waiting_quiet[wg_id].empty()) {
                for (const auto threadId : waiting_quiet[wg_id]) {
                    DPRINTF(("Finished Quiet for wg_id %d at threadId %d\n",
                            wg_id, threadId));
                    handle->queue_descs[wg_id].status[threadId] = 1;
                }

                waiting_quiet[wg_id].clear();

                if (handle->gpu_queue) {
                    SFENCE();
                    handle->hdp_policy->hdp_flush();
                }
            }
        }

        // Remove the MPI Request and the RequestProperty tracking entry
        sort(indices, indices + outcount, std::greater<int>());
        for (int i = 0; i < outcount; i++) {
            int indx = indices[i];
            req_vec.erase(req_vec.begin() + indx);
            req_prop_vec.erase(req_prop_vec.begin() + indx);
        }
    }

    return Status::ROC_SHMEM_SUCCESS;
}

Status
MPITransport::quiet(int wg_id, int threadId)
{
    if (!outstanding[wg_id]) {
        DPRINTF(("Finished Quiet immediately for wg_id %d at threadId %d\n",
                wg_id, threadId));
        handle->queue_descs[wg_id].status[threadId] = 1;
    } else {
        waiting_quiet[wg_id].emplace_back(threadId);
    }
    return Status::ROC_SHMEM_SUCCESS;
}

int
MPITransport::numOutstandingRequests()
{
    return req_vec.size() + q.size();
}

#ifdef OPENSHMEM_TRANSPORT
#define NET_CHECK(cmd) \
{\
    if (cmd != 0) {\
        fprintf(stderr, "Unrecoverable error: SHMEM Failure\n");\
        exit(-1);\
    }\
}

OpenSHMEMTransport::OpenSHMEMTransport()
    : Transport()
{
    // TODO: Provide context support
    int provided;
    shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided);
    if (provided != SHMEM_THREAD_MULTIPLE) {
        fprintf(stderr, "Warning requested multi-thread level is not "
                        "supported \n");
    }
    num_pes = shmem_n_pes();
    my_pe = shmem_my_pe();
}

Status
OpenSHMEMTransport::initTransport(int num_queues)
{
    // setup a context per queue
    ctx_vec.resize(num_queues);
    for (int i = 0; i < ctx_vec.size(); i++) {
        NET_CHECK(shmem_ctx_create(SHMEM_CTX_SERIALIZED,
                                   ctx_vec.data() + i));
    }

    return Status::ROC_SHMEM_SUCCESS;
}

Status
OpenSHMEMTransport::finalizeTransport()
{
    shmem_finalize();
    return Status::ROC_SHMEM_SUCCESS;
}

Status
OpenSHMEMTransport::allocateMemory(void **ptr, size_t size)
{
    // TODO: only works for host memory
    if ((*ptr = shmem_malloc(size)) == nullptr)
        return ROC_SHMEM_OOM_ERROR;
    CHECK_HIP(hipHostRegister(*ptr, size, 0));
    return Status::ROC_SHMEM_SUCCESS;
}

Status
OpenSHMEMTransport::deallocateMemory(void *ptr)
{
    CHECK_HIP(hipHostUnregister(ptr));
    shmem_free(ptr);
    return Status::ROC_SHMEM_SUCCESS;
}

Status
OpenSHMEMTransport::barrier(int wg_id)
{
    shmem_barrier_all();
    return Status::ROC_SHMEM_SUCCESS;
}

Status
OpenSHMEMTransport::reduction(void *dst, void *src, int size, int pe,
                              int wg_id, int start, int logPstride, int sizePE,
                              void *pWrk, long *pSync, RO_NET_Op op)
{
    assert(op == RO_NET_SUM);
    shmem_float_sum_to_all((float *) dst, (float *) src, size, pe, logPstride,
                           sizePE, (float *) pWrk, pSync);
    return Status::ROC_SHMEM_SUCCESS;
}

Status
OpenSHMEMTransport::broadcast(void *dst, void *src, int size, int pe,
                              int wg_id, int start, int logPstride, int sizePE,
                              int root, long *pSync)
{
    shmem_broadcast((float *) dst, (float *) src, size, root, pe, logPstride,
                           sizePE, pSync);
    return Status::ROC_SHMEM_SUCCESS;
}

Status
OpenSHMEMTransport::putMem(void *dst, void *src, int size, int pe, int wg_id)
{
    assert(wg_id < ctx_vec.size());
    shmem_ctx_putmem_nbi(ctx_vec[wg_id], dst, src, size, pe);
    return Status::ROC_SHMEM_SUCCESS;
}

Status
OpenSHMEMTransport::getMem(void *dst, void *src, int size, int pe, int wg_id)
{
    assert(wg_id < ctx_vec.size());
    shmem_ctx_getmem_nbi(ctx_vec[wg_id], dst, src, size, pe);
    return Status::ROC_SHMEM_SUCCESS;
}

Status
OpenSHMEMTransport::amoFOP(void *dst, void *src, int64_t val, int pe,
                                      int wg_id, int threadId, bool blocking,
                                      ROC_SHMEM_OP op)
{
    return Status::ROC_SHMEM_SUCCESS;
}

Status
OpenSHMEMTransport::progress(int wg_id,
                             struct ro_net_handle *ronet_gpu_handle)
{
    // TODO: Might want to delay a quiet for a while to make sure we get
    // messages from other contexts injected before we block the service
    // thread.
    if (ronet_gpu_handle->needs_quiet[wg_id] ||
        ronet_gpu_handle->needs_blocking[wg_id]) {
        assert(wg_id < ctx_vec.size());
        shmem_ctx_quiet(ctx_vec[wg_id]);
        ronet_gpu_handle->needs_quiet[wg_id] = false;
        ronet_gpu_handle->needs_blocking[wg_id] = false;
        ronet_gpu_handle->queue_descs[wg_id].status = 1;

        if (handle->gpu_queue) {
            SFENCE();
            ro_net_gpu_handle->hdp_policy->hdp_flush();
        }
    }

    return Status::ROC_SHMEM_SUCCESS;
}

Status
OpenSHMEMTransport::quiet(int wg_id)
{
    return Status::ROC_SHMEM_SUCCESS;
}

int
OpenSHMEMTransport::numOutstandingRequests()
{
    for (auto ctx : ctx_vec)
        shmem_ctx_quiet(ctx);
    return 0;
}

#endif

}  // namespace rocshmem
