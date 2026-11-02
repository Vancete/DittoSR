// The part of Intel XeSS 2.0's API (xess.h, xess_d3d12.h) the libxess.dll proxy intercepts, declared from the SDK's
// public layout. Everything else is forwarded by name (xess_forward.h).
#pragma once
#include <d3d12.h>
#include <stdint.h>

typedef struct _xess_context_handle_t *xess_context_handle_t;

typedef enum _xess_result_t {
    XESS_RESULT_WARNING_NONEXISTING_FOLDER = 1,
    XESS_RESULT_WARNING_OLD_DRIVER = 2,
    XESS_RESULT_SUCCESS = 0,
    XESS_RESULT_ERROR_UNSUPPORTED_DEVICE = -1,
    XESS_RESULT_ERROR_UNSUPPORTED_DRIVER = -2,
    XESS_RESULT_ERROR_UNINITIALIZED = -3,
    XESS_RESULT_ERROR_INVALID_ARGUMENT = -4,
    XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY = -5,
    XESS_RESULT_ERROR_DEVICE = -6,
    XESS_RESULT_ERROR_NOT_IMPLEMENTED = -7,
    XESS_RESULT_ERROR_INVALID_CONTEXT = -8,
    XESS_RESULT_ERROR_OPERATION_IN_PROGRESS = -9,
    XESS_RESULT_ERROR_UNSUPPORTED = -10,
    XESS_RESULT_ERROR_CANT_LOAD_LIBRARY = -11,
    XESS_RESULT_ERROR_WRONG_CALL_ORDER = -12,
    XESS_RESULT_ERROR_UNKNOWN = -1000,
} xess_result_t;

typedef struct _xess_2d_t { uint32_t x, y; } xess_2d_t;
typedef xess_2d_t xess_coord_t;

typedef enum _xess_quality_settings_t {
    XESS_QUALITY_SETTING_ULTRA_PERFORMANCE = 100,
    XESS_QUALITY_SETTING_PERFORMANCE = 101,
    XESS_QUALITY_SETTING_BALANCED = 102,
    XESS_QUALITY_SETTING_QUALITY = 103,
    XESS_QUALITY_SETTING_ULTRA_QUALITY = 104,
    XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS = 105,
    XESS_QUALITY_SETTING_AA = 106,
} xess_quality_settings_t;

typedef enum _xess_init_flags_t {
    XESS_INIT_FLAG_NONE = 0,
    XESS_INIT_FLAG_HIGH_RES_MV = 1 << 0,               // motion vectors at the output resolution
    XESS_INIT_FLAG_INVERTED_DEPTH = 1 << 1,            // 1 = near
    XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE = 1 << 2,    // pExposureScaleTexture given
    XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK = 1 << 3,     // pResponsivePixelMaskTexture given
    XESS_INIT_FLAG_USE_NDC_VELOCITY = 1 << 4,          // motion vectors in NDC instead of pixels
    XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP = 1 << 5,
    XESS_INIT_FLAG_LDR_INPUT_COLOR = 1 << 6,           // the colour is LDR (else HDR)
    XESS_INIT_FLAG_JITTERED_MV = 1 << 7,               // the motion vectors include the jitter
    XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE = 1 << 8,
} xess_init_flags_t;

typedef struct _xess_d3d12_init_params_t {
    xess_2d_t outputResolution;
    xess_quality_settings_t qualitySetting;
    uint32_t initFlags;
    uint32_t creationNodeMask;
    uint32_t visibleNodeMask;
    ID3D12Heap *pTempBufferHeap;
    uint64_t bufferHeapOffset;
    ID3D12Heap *pTempTextureHeap;
    uint64_t textureHeapOffset;
    ID3D12PipelineLibrary *pPipelineLibrary;
} xess_d3d12_init_params_t;

// All inputs in NON_PIXEL_SHADER_RESOURCE, the output in UNORDERED_ACCESS (XeSS's rule)
typedef struct _xess_d3d12_execute_params_t {
    ID3D12Resource *pColorTexture;
    ID3D12Resource *pVelocityTexture;
    ID3D12Resource *pDepthTexture;
    ID3D12Resource *pExposureScaleTexture;
    ID3D12Resource *pResponsivePixelMaskTexture;
    ID3D12Resource *pOutputTexture;
    float jitterOffsetX;  // [-0.5, 0.5]
    float jitterOffsetY;
    float exposureScale;
    uint32_t resetHistory;
    uint32_t inputWidth;
    uint32_t inputHeight;
    xess_coord_t inputColorBase;
    xess_coord_t inputMotionVectorBase;
    xess_coord_t inputDepthBase;
    xess_coord_t inputResponsiveMaskBase;
    xess_coord_t reserved0;
    xess_coord_t outputColorBase;
    ID3D12DescriptorHeap *pDescriptorHeap;
    uint32_t descriptorHeapOffset;
} xess_d3d12_execute_params_t;

typedef xess_result_t (*PfnXessD3D12CreateContext)(ID3D12Device *, xess_context_handle_t *);
typedef xess_result_t (*PfnXessD3D12Init)(xess_context_handle_t, const xess_d3d12_init_params_t *);
typedef xess_result_t (*PfnXessD3D12Execute)(xess_context_handle_t, ID3D12GraphicsCommandList *, const xess_d3d12_execute_params_t *);
typedef xess_result_t (*PfnXessDestroyContext)(xess_context_handle_t);
typedef xess_result_t (*PfnXessSetScale)(xess_context_handle_t, float, float);
typedef xess_result_t (*PfnXessSetFloat)(xess_context_handle_t, float);
typedef xess_result_t (*PfnXessGetInputResolution)(xess_context_handle_t, const xess_2d_t *, xess_quality_settings_t, xess_2d_t *);
