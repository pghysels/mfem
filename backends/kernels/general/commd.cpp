// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#include "../../../config/config.hpp"
#if defined(MFEM_USE_BACKENDS) && defined(MFEM_USE_KERNELS)

#include "../kernels.hpp"

namespace mfem
{

namespace kernels
{

#ifdef MFEM_USE_MPI

// ***************************************************************************
// * kCommD
// ***************************************************************************
kCommD::kCommD(ParFiniteElementSpace &pfes):
   GroupCommunicator(pfes.GroupComm()),
   d_group_ldof(group_ldof),
   d_group_ltdof(group_ltdof),
   d_group_buf(NULL) {nvtx_push(); comm_lock=0; nvtx_pop();}


// ***************************************************************************
// * ~kCommD
// ***************************************************************************
kCommD::~kCommD() { }


#ifdef __NVCC__
// ***************************************************************************
// * kCopyFromTable
// ***************************************************************************
template <class T> static 
 __global__
void k_CopyGroupToBuffer(T *buf,const T *data,const int *dofs)
{
   const int j = blockDim.x * blockIdx.x + threadIdx.x;
   const int idx = dofs[j];
   buf[j]=data[idx];
}

// ***************************************************************************
// ***************************************************************************
template <class T> static
T *d_CopyGroupToBuffer_k(const T *d_ldata,T *d_buf,
                         const ktable &d_dofs,
                         const int group)
{
   nvtx_push(PapayaWhip);
   const int ndofs = d_dofs.RowSize(group);
   const int *dofs = d_dofs.GetRow(group);
   k_CopyGroupToBuffer<<<ndofs,1>>>(d_buf,d_ldata,dofs);
   nvtx_pop();
   return d_buf + ndofs;
}

// ***************************************************************************
// * d_CopyGroupToBuffer
// ***************************************************************************
template <class T>
T *kCommD::d_CopyGroupToBuffer(const T *d_ldata, T *d_buf,
                               int group, int layout) const
{
   if (layout==2) // master
   {
      return d_CopyGroupToBuffer_k(d_ldata,d_buf,d_group_ltdof,group);
   }
   if (layout==0) // slave
   {
      return d_CopyGroupToBuffer_k(d_ldata,d_buf,d_group_ldof,group);
   }
   assert(false);
   return 0;
}

// ***************************************************************************
// * k_CopyGroupFromBuffer
// ***************************************************************************
template <class T> static __global__
void k_CopyGroupFromBuffer(const T *buf,T *data,const int *dofs)
{
   const int j = blockDim.x * blockIdx.x + threadIdx.x;
   const int idx = dofs[j];
   data[idx]=buf[j];
}

// ***************************************************************************
// * d_CopyGroupFromBuffer
// ***************************************************************************
template <class T>
const T *kCommD::d_CopyGroupFromBuffer(const T *d_buf, T *d_ldata,
                                       int group, int layout) const
{
   nvtx_push(Gold);
   assert(layout==0);
   const int ndofs = d_group_ldof.RowSize(group);
   const int *dofs = d_group_ldof.GetRow(group);
   k_CopyGroupFromBuffer<<<ndofs,1>>>(d_buf,d_ldata,dofs);
   nvtx_pop();
   return d_buf + ndofs;
}

// ***************************************************************************
// * kAtomicAdd
// ***************************************************************************
template <class T>
static __global__ void kAtomicAdd(T* adrs, const int* dofs,T *value)
{
   const int i = blockDim.x * blockIdx.x + threadIdx.x;
   const int idx = dofs[i];
   adrs[idx] += value[i];
}
template __global__ void kAtomicAdd<int>(int*, const int*, int*);
template __global__ void kAtomicAdd<double>(double*, const int*, double*);

// ***************************************************************************
// * ReduceGroupFromBuffer
// ***************************************************************************
template <class T>
const T *kCommD::d_ReduceGroupFromBuffer(const T *d_buf, T *d_ldata,
                                         int group, int layout,
                                         void (*Op)(OpData<T>)) const
{
   nvtx_push(PaleGoldenrod);
   dbg("\t[d_ReduceGroupFromBuffer]");
   OpData<T> opd;
   opd.ldata = d_ldata;
   opd.nldofs = group_ldof.RowSize(group);
   opd.nb = 1;
   opd.buf = const_cast<T*>(d_buf);
   dbg("\t\t[d_ReduceGroupFromBuffer] layout 2");
   opd.ldofs = const_cast<int*>(d_group_ltdof.GetRow(group));
   assert(opd.nb == 1);
   // this is the operation to perform: opd.ldata[opd.ldofs[i]] += opd.buf[i];
   // mfem/general/communication.cpp, line 1008
   kAtomicAdd<<<opd.nldofs,1>>>(opd.ldata,opd.ldofs,opd.buf);
   dbg("\t\t[d_ReduceGroupFromBuffer] done");
   nvtx_pop();
   return d_buf + opd.nldofs;
}


// ***************************************************************************
// * d_BcastBegin
// ***************************************************************************
template <class T>
void kCommD::d_BcastBegin(T *d_ldata, int layout)
{
   MFEM_VERIFY(comm_lock == 0, "object is already in use");
   if (group_buf_size == 0) { return; }

   nvtx_push(Moccasin);
   assert(layout==2);
   const int rnk = mfem::kernels::config::Get().Rank();
   dbg("[%d-d_BcastBegin]",rnk);
   int request_counter = 0;
   nvtx_push(alloc,Moccasin);
   group_buf.SetSize(group_buf_size*sizeof(T));
   T *buf = (T *)group_buf.GetData();
   if (!d_group_buf)
   {
      nvtx_push(alloc,Purple);
      d_group_buf = mfem::kernels::kmalloc<T>::operator new (group_buf_size);
      dbg("[%d-d_ReduceBegin] d_buf cuMemAlloc\033[m",rnk);
      nvtx_pop();
   }
   T *d_buf = (T*)d_group_buf;
   nvtx_pop();
   for (int nbr = 1; nbr < nbr_send_groups.Size(); nbr++)
   {
      const int num_send_groups = nbr_send_groups.RowSize(nbr);
      if (num_send_groups > 0)
      {
         T *buf_start = buf;
         T *d_buf_start = d_buf;
         const int *grp_list = nbr_send_groups.GetRow(nbr);
         for (int i = 0; i < num_send_groups; i++)
         {
            T *d_buf_ini = d_buf;
            assert(layout==2);
            d_buf = d_CopyGroupToBuffer(d_ldata, d_buf, grp_list[i], 2);
            buf += d_buf - d_buf_ini;
         }
         if (!mfem::kernels::config::Get().Aware())
         {
            nvtx_push(BcastBegin:DtoH,Red);
            mfem::kernels::kmemcpy::rDtoH(buf_start,d_buf_start,(buf-buf_start)*sizeof(T));
            nvtx_pop();
         }

         // make sure the device has finished
         if (mfem::kernels::config::Get().Aware())
         {
            nvtx_push(sync,Lime);
            cudaStreamSynchronize(0);//*rconfig::Get().Stream());
            nvtx_pop();
         }

         nvtx_push(MPI_Isend,Orange);
         if (mfem::kernels::config::Get().Aware())
            MPI_Isend(d_buf_start,
                      buf - buf_start,
                      MPITypeMap<T>::mpi_type,
                      gtopo.GetNeighborRank(nbr),
                      40822,
                      gtopo.GetComm(),
                      &requests[request_counter]);
         else
            MPI_Isend(buf_start,
                      buf - buf_start,
                      MPITypeMap<T>::mpi_type,
                      gtopo.GetNeighborRank(nbr),
                      40822,
                      gtopo.GetComm(),
                      &requests[request_counter]);
         nvtx_pop();
         request_marker[request_counter] = -1; // mark as send request
         request_counter++;
      }

      const int num_recv_groups = nbr_recv_groups.RowSize(nbr);
      if (num_recv_groups > 0)
      {
         const int *grp_list = nbr_recv_groups.GetRow(nbr);
         int recv_size = 0;
         for (int i = 0; i < num_recv_groups; i++)
         {
            recv_size += group_ldof.RowSize(grp_list[i]);
         }
         nvtx_push(MPI_Irecv,Orange);
         if (mfem::kernels::config::Get().Aware())
            MPI_Irecv(d_buf,
                      recv_size,
                      MPITypeMap<T>::mpi_type,
                      gtopo.GetNeighborRank(nbr),
                      40822,
                      gtopo.GetComm(),
                      &requests[request_counter]);
         else
            MPI_Irecv(buf,
                      recv_size,
                      MPITypeMap<T>::mpi_type,
                      gtopo.GetNeighborRank(nbr),
                      40822,
                      gtopo.GetComm(),
                      &requests[request_counter]);
         nvtx_pop();
         request_marker[request_counter] = nbr;
         request_counter++;
         buf_offsets[nbr] = buf - (T*)group_buf.GetData();
         buf += recv_size;
         d_buf += recv_size;
      }
   }
   assert(buf - (T*)group_buf.GetData() == group_buf_size);
   comm_lock = 1; // 1 - locked for Bcast
   num_requests = request_counter;
   dbg("[%d-d_BcastBegin] done",rnk);
   nvtx_pop();
}

// ***************************************************************************
// * d_BcastEnd
// ***************************************************************************
template <class T>
void kCommD::d_BcastEnd(T *d_ldata, int layout)
{
   if (comm_lock == 0) { return; }
   nvtx_push(PeachPuff);
   const int rnk = mfem::kernels::config::Get().Rank();
   dbg("[%d-d_BcastEnd]",rnk);
   // The above also handles the case (group_buf_size == 0).
   assert(comm_lock == 1);
   // copy the received data from the buffer to d_ldata, as it arrives
   int idx;
   nvtx_push(MPI_Waitany,Orange);
   while (MPI_Waitany(num_requests, requests, &idx, MPI_STATUS_IGNORE),
          idx != MPI_UNDEFINED)
   {
      nvtx_pop();
      int nbr = request_marker[idx];
      if (nbr == -1) { continue; } // skip send requests

      const int num_recv_groups = nbr_recv_groups.RowSize(nbr);
      if (num_recv_groups > 0)
      {
         const int *grp_list = nbr_recv_groups.GetRow(nbr);
         int recv_size = 0;
         for (int i = 0; i < num_recv_groups; i++)
         {
            recv_size += group_ldof.RowSize(grp_list[i]);
         }
         const T *buf = (T*)group_buf.GetData() + buf_offsets[nbr];
         const T *d_buf = (T*)d_group_buf + buf_offsets[nbr];
         if (!mfem::kernels::config::Get().Aware())
         {
            nvtx_push(BcastEnd:HtoD,Red);
            mfem::kernels::kmemcpy::rHtoD((void*)d_buf,buf,recv_size*sizeof(T));
            nvtx_pop();
         }
         for (int i = 0; i < num_recv_groups; i++)
         {
            d_buf = d_CopyGroupFromBuffer(d_buf, d_ldata, grp_list[i], layout);
         }
      }
   }
   comm_lock = 0; // 0 - no lock
   num_requests = 0;
   dbg("[%d-d_BcastEnd] done",rnk);
   nvtx_pop();
}

// ***************************************************************************
// * d_ReduceBegin
// ***************************************************************************
template <class T>
void kCommD::d_ReduceBegin(const T *d_ldata)
{
   MFEM_VERIFY(comm_lock == 0, "object is already in use");
   if (group_buf_size == 0) { return; }
   nvtx_push(PapayaWhip);
   const int rnk = mfem::kernels::config::Get().Rank();
   dbg("[%d-d_ReduceBegin]",rnk);

   int request_counter = 0;
   group_buf.SetSize(group_buf_size*sizeof(T));
   T *buf = (T *)group_buf.GetData();
   if (!d_group_buf)
   {
      d_group_buf = mfem::kernels::kmalloc<T>::operator new (group_buf_size);
   }
   T *d_buf = (T*)d_group_buf;
   for (int nbr = 1; nbr < nbr_send_groups.Size(); nbr++)
   {
      const int num_send_groups = nbr_recv_groups.RowSize(nbr);
      if (num_send_groups > 0)
      {
         T *buf_start = buf;
         T *d_buf_start = d_buf;
         const int *grp_list = nbr_recv_groups.GetRow(nbr);
         for (int i = 0; i < num_send_groups; i++)
         {
            T *d_buf_ini = d_buf;
            d_buf = d_CopyGroupToBuffer(d_ldata, d_buf, grp_list[i], 0);
            buf += d_buf - d_buf_ini;
         }
         dbg("[%d-d_ReduceBegin] MPI_Isend",rnk);
         if (!mfem::kernels::config::Get().Aware())
         {
            nvtx_push(ReduceBegin:DtoH,Red);
            mfem::kernels::kmemcpy::rDtoH(buf_start,d_buf_start,(buf-buf_start)*sizeof(T));
            nvtx_pop();
         }
         // make sure the device has finished
         if (mfem::kernels::config::Get().Aware())
         {
            nvtx_push(sync,Lime);
            cudaStreamSynchronize(0);//*rconfig::Get().Stream());
            nvtx_pop();
         }
         nvtx_push(MPI_Isend,Orange);
         if (mfem::kernels::config::Get().Aware())
            MPI_Isend(d_buf_start,
                      buf - buf_start,
                      MPITypeMap<T>::mpi_type,
                      gtopo.GetNeighborRank(nbr),
                      43822,
                      gtopo.GetComm(),
                      &requests[request_counter]);
         else
            MPI_Isend(buf_start,
                      buf - buf_start,
                      MPITypeMap<T>::mpi_type,
                      gtopo.GetNeighborRank(nbr),
                      43822,
                      gtopo.GetComm(),
                      &requests[request_counter]);
         nvtx_pop();
         request_marker[request_counter] = -1; // mark as send request
         request_counter++;
      }

      // In Reduce operation: send_groups <--> recv_groups
      const int num_recv_groups = nbr_send_groups.RowSize(nbr);
      if (num_recv_groups > 0)
      {
         const int *grp_list = nbr_send_groups.GetRow(nbr);
         int recv_size = 0;
         for (int i = 0; i < num_recv_groups; i++)
         {
            recv_size += group_ldof.RowSize(grp_list[i]);
         }
         dbg("[%d-d_ReduceBegin] MPI_Irecv",rnk);
         nvtx_push(MPI_Irecv,Orange);
         if (mfem::kernels::config::Get().Aware())
            MPI_Irecv(d_buf,
                      recv_size,
                      MPITypeMap<T>::mpi_type,
                      gtopo.GetNeighborRank(nbr),
                      43822,
                      gtopo.GetComm(),
                      &requests[request_counter]);
         else
            MPI_Irecv(buf,
                      recv_size,
                      MPITypeMap<T>::mpi_type,
                      gtopo.GetNeighborRank(nbr),
                      43822,
                      gtopo.GetComm(),
                      &requests[request_counter]);
         nvtx_pop();
         request_marker[request_counter] = nbr;
         request_counter++;
         buf_offsets[nbr] = buf - (T*)group_buf.GetData();
         buf += recv_size;
         d_buf += recv_size;
      }
   }
   assert(buf - (T*)group_buf.GetData() == group_buf_size);
   comm_lock = 2;
   num_requests = request_counter;
   dbg("[%d-d_ReduceBegin] done",rnk);
   nvtx_pop();
}

// ***************************************************************************
// * d_ReduceEnd
// ***************************************************************************
template <class T>
void kCommD::d_ReduceEnd(T *d_ldata, int layout,
                         void (*Op)(OpData<T>))
{
   if (comm_lock == 0) { return; }
   nvtx_push(LavenderBlush);
   const int rnk = mfem::kernels::config::Get().Rank();
   dbg("[%d-d_ReduceEnd]",rnk);
   // The above also handles the case (group_buf_size == 0).
   assert(comm_lock == 2);

   nvtx_push(MPI_Waitall,Orange);
   MPI_Waitall(num_requests, requests, MPI_STATUSES_IGNORE);
   nvtx_pop();
   for (int nbr = 1; nbr < nbr_send_groups.Size(); nbr++)
   {
      // In Reduce operation: send_groups <--> recv_groups
      const int num_recv_groups = nbr_send_groups.RowSize(nbr);
      if (num_recv_groups > 0)
      {
         const int *grp_list = nbr_send_groups.GetRow(nbr);
         int recv_size = 0;
         for (int i = 0; i < num_recv_groups; i++)
         {
            recv_size += group_ldof.RowSize(grp_list[i]);
         }
         const T *buf = (T*)group_buf.GetData() + buf_offsets[nbr];
         assert(d_group_buf);
         const T *d_buf = (T*)d_group_buf + buf_offsets[nbr];
         if (!mfem::kernels::config::Get().Aware())
         {
            nvtx_push(ReduceEnd:HtoD,Red);
            mfem::kernels::kmemcpy::rHtoD((void*)d_buf,buf,recv_size*sizeof(T));
            nvtx_pop();
         }
         for (int i = 0; i < num_recv_groups; i++)
         {
            d_buf = d_ReduceGroupFromBuffer(d_buf, d_ldata, grp_list[i], layout, Op);
         }
      }
   }
   comm_lock = 0; // 0 - no lock
   num_requests = 0;
   dbg("[%d-d_ReduceEnd] end",rnk);
   nvtx_pop();
}

// ***************************************************************************
// * instantiate kCommD::Bcast and Reduce for doubles
// ***************************************************************************
template void kCommD::d_BcastBegin<double>(double*, int);
template void kCommD::d_BcastEnd<double>(double*, int);
template void kCommD::d_ReduceBegin<double>(const double *);
template void kCommD::d_ReduceEnd<double>(double*,int,void (*)(OpData<double>));
#else // __NVCC__
template <class T> void kCommD::d_ReduceBegin(const T*) {}
template <class T> void kCommD::d_ReduceEnd(T*,int,void (*Op)(OpData<T>)) {}
template <class T> void kCommD::d_BcastBegin(T*, int) {}
template <class T> void kCommD::d_BcastEnd(T*, int) {}
template void kCommD::d_BcastBegin<double>(double*, int);
template void kCommD::d_BcastEnd<double>(double*, int);
template void kCommD::d_ReduceBegin<double>(const double *);
template void kCommD::d_ReduceEnd<double>(double*,int,void (*)(OpData<double>));
#endif // __NVCC__

#endif // MFEM_USE_MPI

} // namespace mfem::kernels

} // namespace mfem

#endif // defined(MFEM_USE_BACKENDS) && defined(MFEM_USE_KERNELS)