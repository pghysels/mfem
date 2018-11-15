// Copyright (c) 2017, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-734707. All Rights
// reserved. See files LICENSE and NOTICE for details.
//
// This file is part of CEED, a collection of benchmarks, miniapps, software
// libraries and APIs for efficient high-order finite element and spectral
// element discretizations for exascale applications. For more information and
// source code availability see http://github.com/ceed.
//
// The CEED research is supported by the Exascale Computing Project 17-SC-20-SC,
// a collaborative effort of two U.S. Department of Energy organizations (Office
// of Science and the National Nuclear Security Administration) responsible for
// the planning and preparation of a capable exascale ecosystem, including
// software, applications, hardware, advanced system engineering and early
// testbed platforms, in support of the nation's exascale computing imperative.

#ifndef MFEM_BACKENDS_KERNELS_MALLOC_HPP
#define MFEM_BACKENDS_KERNELS_MALLOC_HPP

#include "../../../config/config.hpp"
#if defined(MFEM_USE_BACKENDS) && defined(MFEM_USE_KERNELS)

namespace mfem
{

namespace kernels
{

// ***************************************************************************
template<class T> struct kmalloc: public kmemcpy
{

   // *************************************************************************
   inline void* operator new (size_t n, bool lock_page = false)
   {
      dbp("+]\033[m");
      if (!config::Get().Cuda())
      {
         return ::new T[n];
      }
#ifdef __NVCC__
      void *ptr = NULL;
      nvtx_push(new,Purple);
      if (!config::Get().Uvm())
      {
         //dbg("\033[31;1m>cuMemAlloc");
         if (lock_page) { checkCudaErrors(cuMemHostAlloc(&ptr, n*sizeof(T),CU_MEMHOSTALLOC_PORTABLE)); }
         else
         {
            //assert(n>0); // DevExtension<>::SetEngine does a 'InitLayout(*e.MakeLayout(0));'
            if (n==0) { n=1; }
            checkCudaErrors(cuMemAlloc((CUdeviceptr*)&ptr, n*sizeof(T)));
         }
      }
      else
      {
         //dbg("\033[31;1m>cuMemAllocManaged");
         checkCudaErrors(cuMemAllocManaged((CUdeviceptr*)&ptr, n*sizeof(T),
                                           CU_MEM_ATTACH_GLOBAL));
      }
      nvtx_pop();
      return ptr;
#else
      // We come here when the user requests a manager,
      // but has compiled the code without NVCC
      assert(false);
      return ::new T[n];
#endif // __NVCC__
   }

   // ***************************************************************************
   inline void operator delete (void *ptr)
   {
      dbp("-]\033[m");
      if (!config::Get().Cuda())
      {
         if (ptr)
         {
            ::delete[] static_cast<T*>(ptr);
         }
      }
#ifdef __NVCC__
      else
      {
         nvtx_push(delete,Fuchsia);
         cuMemFree((CUdeviceptr)ptr); // or cuMemFreeHost if page_locked was used
         nvtx_pop();
      }
#endif // __NVCC__
      ptr = nullptr;
   }
};

} // kernels

} // mfem

#endif // defined(MFEM_USE_BACKENDS) && defined(MFEM_USE_KERNELS)

#endif // MFEM_BACKENDS_KERNELS_MALLOC_HPP