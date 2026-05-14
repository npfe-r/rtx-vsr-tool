// Minimal NGX init test — standalone console app
#include <windows.h>
#include <cstdio>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_helpers_vsr.h>
#include <cuda.h>

#include "utils.h"

int main() {
    // Init CUDA driver API
    CUdevice  cuDevice;
    CUcontext cuContext;

    printf("Step 1: cuInit...\n");
    CUresult res = cuInit(0);
    if (res != CUDA_SUCCESS) { printf("cuInit failed: %d\n", res); return 1; }
    printf("OK\n");

    printf("Step 2: cuDeviceGet(0)...\n");
    res = cuDeviceGet(&cuDevice, 0);
    if (res != CUDA_SUCCESS) { printf("cuDeviceGet failed: %d\n", res); return 1; }
    printf("OK\n");

    printf("Step 3: cuDevicePrimaryCtxRetain...\n");
    res = cuDevicePrimaryCtxRetain(&cuContext, cuDevice);
    if (res != CUDA_SUCCESS) { printf("cuDevicePrimaryCtxRetain failed: %d\n", res); return 1; }
    printf("OK\n");

    printf("Step 4: cuCtxPushCurrent...\n");
    cuCtxPushCurrent(cuContext);
    printf("OK\n");

    // NGX Init
    printf("Step 5: NVSDK_NGX_CUDA_Init(APP_ID=%d, APP_PATH=L\".\")...\n", APP_ID);
    fflush(stdout);
    NVSDK_NGX_Result ngxResult = NVSDK_NGX_CUDA_Init(APP_ID, APP_PATH);
    printf("NVSDK_NGX_CUDA_Init returned: %d (Success=%d)\n", (int)ngxResult, (int)NVSDK_NGX_Result_Success);
    if (ngxResult != NVSDK_NGX_Result_Success) {
        printf("NGX Init FAILED!\n");
        cuCtxPopCurrent(&cuContext);
        cuDevicePrimaryCtxRelease(cuDevice);
        return 1;
    }

    printf("Step 6: NVSDK_NGX_CUDA_GetCapabilityParameters...\n");
    NVSDK_NGX_Parameter* params = nullptr;
    ngxResult = NVSDK_NGX_CUDA_GetCapabilityParameters(&params);
    printf("GetCapabilityParameters returned: %d\n", (int)ngxResult);
    if (ngxResult != NVSDK_NGX_Result_Success || !params) {
        printf("GetCapabilityParameters FAILED!\n");
        NVSDK_NGX_CUDA_Shutdown();
        cuCtxPopCurrent(&cuContext);
        cuDevicePrimaryCtxRelease(cuDevice);
        return 1;
    }
    printf("OK, params=%p\n", (void*)params);

    printf("Step 7: Check VSR availability...\n");
    int vsrAvail = 0;
    ngxResult = params->Get(NVSDK_NGX_Parameter_VSR_Available, &vsrAvail);
    printf("VSR_Available GET returned %d, value=%d\n", (int)ngxResult, vsrAvail);

    if (vsrAvail) {
        printf("VSR is AVAILABLE on this system!\n");

        printf("Step 8: Creating VSR feature...\n");
        NVSDK_NGX_Handle* vsrHandle = nullptr;
        NVSDK_NGX_CUDA_VSR_Create_Params vsrParams = {};
        vsrParams.InCUContext = cuContext;
        vsrParams.InCUStream = nullptr;

        ngxResult = NGX_CUDA_CREATE_VSR(&vsrHandle, params, &vsrParams);
        printf("NGX_CUDA_CREATE_VSR returned: %d, handle=%p\n", (int)ngxResult, (void*)vsrHandle);
        if (ngxResult != NVSDK_NGX_Result_Success) {
            printf("VSR creation FAILED!\n");
        } else {
            printf("VSR creation SUCCESS!\n");
            // Cleanup VSR
            NVSDK_NGX_CUDA_ReleaseFeature(vsrHandle);
        }
    } else {
        printf("VSR NOT available on this system. Check GPU/driver.\n");
    }

    printf("\nStep 9: Shutdown...\n");
    NVSDK_NGX_CUDA_Shutdown();
    cuCtxPopCurrent(&cuContext);
    cuDevicePrimaryCtxRelease(cuDevice);
    printf("Done!\n");
    return 0;
}
