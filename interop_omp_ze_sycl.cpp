#include <CL/sycl.hpp>
#include <omp.h>
#include <algorithm>
#include <iostream>
#include <level_zero/ze_api.h>
#include <CL/sycl/backend/level_zero.hpp>
namespace sycl = cl::sycl;

struct syclDeviceInfo {
    sycl::context sycl_context;
    ze_context_handle_t ze_context;
    int local_device_id;
};

// General case where each Context can have target multiple Device
std::vector<struct syclDeviceInfo> get_ompDeviceInfos() {

    std::vector<struct syclDeviceInfo> ompDeviceId2Context(omp_get_num_devices());

    //1. Map each level zero context to a vector a sycl::device.
    //   This is requied to create SYCL::context spaming multiple devices.
    std::unordered_map<ze_context_handle_t,std::vector<sycl::device>> hContext2device;
    for (int D=0; D< omp_get_num_devices(); D++) {
        omp_interop_t o = 0;
        #pragma omp interop init(targetsync: o) device(D)
        int err = -1;
        ze_driver_handle_t hPlatform = static_cast<ze_driver_handle_t>(omp_get_interop_ptr(o, omp_ipr_platform, &err));
        assert (err >= 0 && "omp_get_interop_ptr(omp_ipr_platform)");
        ze_context_handle_t hContext = static_cast<ze_context_handle_t>(omp_get_interop_ptr(o, omp_ipr_device_context, &err));
        assert (err >= 0 && "omp_get_interop_ptr(omp_ipr_device_context)");
        // equivalent to:
        //ze_context_handle_t hContext = static_cast<ze_context_handle_t>(omp_target_get_context(0));
        ze_device_handle_t hDevice =  static_cast<ze_device_handle_t>(omp_get_interop_ptr(o, omp_ipr_device, &err));
        assert (err >= 0 && "omp_get_interop_ptr(omp_ipr_device)");
        #pragma omp interop destroy(o)

        sycl::platform sycl_platform = sycl::level_zero::make<sycl::platform>(hPlatform);
        hContext2device[hContext].push_back(sycl::level_zero::make<sycl::device>(sycl_platform, hDevice));

        // Store the Level_zero context. This will be required to create the SYCL context latter
        ompDeviceId2Context[D].ze_context = hContext;
        //  Mapping between OpenMP device ID -> Sycl device ID in the data-structure
        ompDeviceId2Context[D].local_device_id = hContext2device[hContext].size() - 1;
    }

    // Construct sycl::contexts who stawn multiple openmp device, if possible.
    // This is N2, but trivial to make it log(N)
    for ( const auto& [hContext, sycl_devices]: hContext2device ) {
        
        
        // This only work because the backend poiter is saved as a shared_pointer in SYCL context with Intel Implementation
        // https://github.com/intel/llvm/blob/ef33c57e48237c7d918f5dab7893554cecc001dd/sycl/source/backend/level_zero.cpp#L59
        // As far as I know this is not required by the SYCL2020 Spec
        sycl::context sycl_context = sycl::level_zero::make<sycl::context>(sycl_devices, hContext,  sycl::level_zero::ownership::keep);

        for (int D=0; D< omp_get_num_devices(); D++)
           if (ompDeviceId2Context[D].ze_context == hContext)
              ompDeviceId2Context[D].sycl_context = sycl_context;
    }

    // This datascructure is a vector used to map a OpenMP device ID to a sycl context
    return ompDeviceId2Context;
}

int main() {

    std::vector<struct syclDeviceInfo> ompDeviceId2Context = get_ompDeviceInfos();

    int D = omp_get_num_devices() -1;
    omp_set_default_device(D);

    const syclDeviceInfo mapping = ompDeviceId2Context[D];
    sycl::context sycl_context = mapping.sycl_context;

    sycl::queue Q { sycl_context, sycl_context.get_devices()[mapping.local_device_id] };

    const int N = 100;
    int *cpuMem = (int*) malloc(N*sizeof(int));

    // OMP -> SYCL
    int *ompMem = (int*)omp_target_alloc_device(N*sizeof(int), D);
    #pragma omp target is_device_ptr(ompMem)
    for (int i=0 ; i < N; i++)
        ompMem[i] = N;

    //Sycl memcopy is using OpenMP pointer
    Q.memcpy(cpuMem, ompMem, N*sizeof(int) ).wait();

    for (int i=0 ; i < N; i++)
        assert(cpuMem[i] == N);

    // SYCL -> SYCL
    int *syclMem = sycl::malloc_device<int>(N,Q);
    // Omp memcopy is using SYCL pointer
    #pragma omp target is_device_ptr(syclMem) map( from:cpuMem[0:N])
    for (int i=0 ; i < N; i++)
        cpuMem[i] = syclMem[i];

    for (int i=0 ; i < N; i++)
        assert(cpuMem[i] == 0);

    std::cout << "Computation Done" << std::endl;
}
