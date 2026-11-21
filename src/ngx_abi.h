// The part of NVIDIA NGX's ABI DittoSR's nvngx.dll speaks (ngx_dll.cpp): the result codes, the feature id, the DLSS
// create flags, the parameter names and the parameter object's interface - written here for interoperability, in the
// order and with the values the NGX runtime and its callers use (the interface's virtual functions in declaration
// order: the vtable a caller built with MSVC expects).
#pragma once
#include <d3d11.h>

struct ID3D12Resource;

typedef enum NVSDK_NGX_Result {
    NVSDK_NGX_Result_Success = 0x1,
    NVSDK_NGX_Result_Fail = (int)0xBAD00000,
    NVSDK_NGX_Result_FAIL_FeatureNotSupported = NVSDK_NGX_Result_Fail | 1,
    NVSDK_NGX_Result_FAIL_PlatformError = NVSDK_NGX_Result_Fail | 2,
    NVSDK_NGX_Result_FAIL_FeatureNotFound = NVSDK_NGX_Result_Fail | 4,
    NVSDK_NGX_Result_FAIL_InvalidParameter = NVSDK_NGX_Result_Fail | 5,
    NVSDK_NGX_Result_FAIL_NotInitialized = NVSDK_NGX_Result_Fail | 7,
    NVSDK_NGX_Result_FAIL_UnsupportedInputFormat = NVSDK_NGX_Result_Fail | 8,
    NVSDK_NGX_Result_FAIL_MissingInput = NVSDK_NGX_Result_Fail | 10,
    NVSDK_NGX_Result_FAIL_UnableToInitializeFeature = NVSDK_NGX_Result_Fail | 11,
} NVSDK_NGX_Result;

typedef enum NVSDK_NGX_Feature { NVSDK_NGX_Feature_SuperSampling = 1 } NVSDK_NGX_Feature;

typedef struct NVSDK_NGX_Handle { unsigned int Id; } NVSDK_NGX_Handle;

enum {
    NVSDK_NGX_DLSS_Feature_Flags_IsHDR = 1 << 0,
    NVSDK_NGX_DLSS_Feature_Flags_MVLowRes = 1 << 1,
    NVSDK_NGX_DLSS_Feature_Flags_MVJittered = 1 << 2,
    NVSDK_NGX_DLSS_Feature_Flags_DepthInverted = 1 << 3,
    NVSDK_NGX_DLSS_Feature_Flags_DoSharpening = 1 << 5,
    NVSDK_NGX_DLSS_Feature_Flags_AutoExposure = 1 << 6,
};

enum { NVSDK_NGX_FeatureSupportResult_Supported = 0 };

// (only the fields DittoSR fills)
typedef struct NVSDK_NGX_FeatureDiscoveryInfo {
    unsigned int SDKVersion;
    NVSDK_NGX_Feature FeatureID;
} NVSDK_NGX_FeatureDiscoveryInfo;
typedef struct NVSDK_NGX_FeatureRequirement {
    int FeatureSupported;
    unsigned int MinHWArchitecture;
    char MinOSVersion[255];
} NVSDK_NGX_FeatureRequirement;

typedef void(__cdecl *PFN_NVSDK_NGX_ProgressCallback)(float InCurrentProgress, bool &OutShouldCancel);

struct NVSDK_NGX_Parameter {
    virtual void Set(const char *InName, unsigned long long InValue) = 0;
    virtual void Set(const char *InName, float InValue) = 0;
    virtual void Set(const char *InName, double InValue) = 0;
    virtual void Set(const char *InName, unsigned int InValue) = 0;
    virtual void Set(const char *InName, int InValue) = 0;
    virtual void Set(const char *InName, ID3D11Resource *InValue) = 0;
    virtual void Set(const char *InName, ID3D12Resource *InValue) = 0;
    virtual void Set(const char *InName, void *InValue) = 0;

    virtual NVSDK_NGX_Result Get(const char *InName, unsigned long long *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, float *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, double *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, unsigned int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D11Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D12Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, void **OutValue) const = 0;

    virtual void Reset() = 0;
};

// The parameters a DLSS (super sampling) feature is created and evaluated with
#define NGX_Width "Width"
#define NGX_Height "Height"
#define NGX_OutWidth "OutWidth"
#define NGX_OutHeight "OutHeight"
#define NGX_Sharpness "Sharpness"
#define NGX_Color "Color"
#define NGX_Output "Output"
#define NGX_Depth "Depth"
#define NGX_MotionVectors "MotionVectors"
#define NGX_ExposureTexture "ExposureTexture"
#define NGX_Reset "Reset"
#define NGX_MV_Scale_X "MV.Scale.X"
#define NGX_MV_Scale_Y "MV.Scale.Y"
#define NGX_Jitter_Offset_X "Jitter.Offset.X"
#define NGX_Jitter_Offset_Y "Jitter.Offset.Y"
#define NGX_DLSS_Feature_Create_Flags "DLSS.Feature.Create.Flags"
#define NGX_DLSS_Pre_Exposure "DLSS.Pre.Exposure"
#define NGX_DLSS_Exposure_Scale "DLSS.Exposure.Scale"
#define NGX_DLSS_Render_Subrect_Width "DLSS.Render.Subrect.Dimensions.Width"
#define NGX_DLSS_Render_Subrect_Height "DLSS.Render.Subrect.Dimensions.Height"
#define NGX_DLSS_Input_Color_Subrect_Base_X "DLSS.Input.Color.Subrect.Base.X"
#define NGX_DLSS_Input_Color_Subrect_Base_Y "DLSS.Input.Color.Subrect.Base.Y"
#define NGX_DLSS_Output_Subrect_Base_X "DLSS.Output.Subrect.Base.X"
#define NGX_DLSS_Output_Subrect_Base_Y "DLSS.Output.Subrect.Base.Y"
// (OptiScaler's: the game's FSR reactive mask, when its inputs came from FSR)
#define NGX_FSR_Reactive "FSR.reactive"
