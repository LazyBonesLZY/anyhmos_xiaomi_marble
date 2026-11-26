/*
 * Clean-room OpenMAX IL 1.1.2 LP64 (AArch64) & OHOS Codec HDI 4.0 ABI definitions.
 * Verified against donor libcodec_component_manager_service_4.0.z.so and libhcodec.z.so.
 */
#ifndef MARBLE_OMX_IL_MINIMAL_H
#define MARBLE_OMX_IL_MINIMAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  OMX_U8;
typedef int8_t   OMX_S8;
typedef uint16_t OMX_U16;
typedef int16_t  OMX_S16;
typedef uint32_t OMX_U32;
typedef int32_t  OMX_S32;
typedef uint32_t OMX_BOOL;
typedef int64_t  OMX_TICKS;
typedef void *   OMX_PTR;
typedef void *   OMX_HANDLETYPE;
typedef char *   OMX_STRING;
typedef void *   OMX_NATIVE_DEVICETYPE;
typedef void *   OMX_NATIVE_WINDOWTYPE;

#define OMX_FALSE 0u
#define OMX_TRUE  1u
#define OMX_MAX_STRINGNAME_SIZE 128

typedef uint8_t OMX_UUIDTYPE[128];

typedef union OMX_VERSIONTYPE {
    struct {
        OMX_U8 nVersionMajor;
        OMX_U8 nVersionMinor;
        OMX_U8 nRevision;
        OMX_U8 nStep;
    } s;
    OMX_U32 nVersion;
} OMX_VERSIONTYPE;

typedef enum OMX_ERRORTYPE {
    OMX_ErrorNone                       = 0,
    OMX_ErrorInsufficientResources      = (OMX_S32)0x80001000,
    OMX_ErrorUndefined                  = (OMX_S32)0x80001001,
    OMX_ErrorInvalidComponentName       = (OMX_S32)0x80001002,
    OMX_ErrorComponentNotFound          = (OMX_S32)0x80001003,
    OMX_ErrorInvalidComponent           = (OMX_S32)0x80001004,
    OMX_ErrorBadParameter               = (OMX_S32)0x80001005,
    OMX_ErrorNotImplemented             = (OMX_S32)0x80001006,
    OMX_ErrorUnderflow                  = (OMX_S32)0x80001007,
    OMX_ErrorOverflow                   = (OMX_S32)0x80001008,
    OMX_ErrorHardware                   = (OMX_S32)0x80001009,
    OMX_ErrorInvalidState               = (OMX_S32)0x8000100A,
    OMX_ErrorStreamCorrupt              = (OMX_S32)0x8000100B,
    OMX_ErrorPortsNotCompatible         = (OMX_S32)0x8000100C,
    OMX_ErrorResourcesLost              = (OMX_S32)0x8000100D,
    OMX_ErrorNoMore                     = (OMX_S32)0x8000100E,
    OMX_ErrorVersionMismatch            = (OMX_S32)0x8000100F,
    OMX_ErrorNotReady                   = (OMX_S32)0x80001010,
    OMX_ErrorTimeout                    = (OMX_S32)0x80001011,
    OMX_ErrorSameState                  = (OMX_S32)0x80001012,
    OMX_ErrorResourcesPreempted         = (OMX_S32)0x80001013,
    OMX_ErrorUnsupportedSetting         = (OMX_S32)0x80001019,
    OMX_ErrorUnsupportedIndex           = (OMX_S32)0x8000101A,
    OMX_ErrorIncorrectStateTransition   = (OMX_S32)0x80001017,
    OMX_ErrorIncorrectStateOperation    = (OMX_S32)0x80001018,
    OMX_ErrorBadPortIndex               = (OMX_S32)0x8000101B,
} OMX_ERRORTYPE;

typedef enum OMX_STATETYPE {
    OMX_StateInvalid = 0,
    OMX_StateLoaded,
    OMX_StateIdle,
    OMX_StateExecuting,
    OMX_StatePause,
    OMX_StateWaitForResources,
    OMX_StateMax = 0x7FFFFFFF
} OMX_STATETYPE;

typedef enum OMX_COMMANDTYPE {
    OMX_CommandStateSet = 0,
    OMX_CommandFlush,
    OMX_CommandPortDisable,
    OMX_CommandPortEnable,
    OMX_CommandMarkBuffer,
    OMX_CommandMax = 0x7FFFFFFF
} OMX_COMMANDTYPE;

typedef enum OMX_EVENTTYPE {
    OMX_EventCmdComplete = 0,
    OMX_EventError,
    OMX_EventMark,
    OMX_EventPortSettingsChanged,
    OMX_EventBufferFlag,
    OMX_EventResourcesAcquired,
    OMX_EventComponentResumed,
    OMX_EventDynamicResourcesAvailable,
    OMX_EventPortFormatDetected,
    OMX_EventMax = 0x7FFFFFFF
} OMX_EVENTTYPE;

typedef enum OMX_DIRTYPE {
    OMX_DirInput = 0,
    OMX_DirOutput,
    OMX_DirMax = 0x7FFFFFFF
} OMX_DIRTYPE;

typedef enum OMX_PORTDOMAINTYPE {
    OMX_PortDomainAudio = 0,
    OMX_PortDomainVideo,
    OMX_PortDomainImage,
    OMX_PortDomainOther,
    OMX_PortDomainMax = 0x7FFFFFFF
} OMX_PORTDOMAINTYPE;

typedef enum OMX_BUFFERSUPPLIERTYPE {
    OMX_BufferSupplyUnspecified = 0,
    OMX_BufferSupplyInput,
    OMX_BufferSupplyOutput,
    OMX_BufferSupplyMax = 0x7FFFFFFF
} OMX_BUFFERSUPPLIERTYPE;

typedef enum OMX_VIDEO_CODINGTYPE {
    OMX_VIDEO_CodingUnused = 0,
    OMX_VIDEO_CodingAutoDetect,
    OMX_VIDEO_CodingMPEG2,
    OMX_VIDEO_CodingH263,
    OMX_VIDEO_CodingMPEG4,
    OMX_VIDEO_CodingWMV,
    OMX_VIDEO_CodingRV,
    OMX_VIDEO_CodingAVC = 7,
    OMX_VIDEO_CodingMJPEG = 8,
    OMX_VIDEO_CodingVP8 = 9,
    OMX_VIDEO_CodingVP9 = 10,   /* matches CodecExtTypes.idl CODEC_VIDEO_CodingVP9 */
    OMX_VIDEO_CodingHEVC = 11,  /* matches CodecExtTypes.idl CODEC_VIDEO_CodingHEVC */
    OMX_VIDEO_CodingMax = 0x7FFFFFFF
} OMX_VIDEO_CODINGTYPE;

typedef enum OMX_COLOR_FORMATTYPE {
    OMX_COLOR_FormatUnused = 0,
    OMX_COLOR_FormatYUV420Planar = 19,
    OMX_COLOR_FormatYUV420PackedPlanar = 20,
    OMX_COLOR_FormatYUV420SemiPlanar = 21,
    OMX_COLOR_FormatYUV420PackedSemiPlanar = 39,
    OMX_COLOR_Format32bitRGBA8888 = 0x7F00A000,
    OMX_COLOR_FormatAndroidOpaque = 0x7F000789,
    OMX_COLOR_FormatMax = 0x7FFFFFFF
} OMX_COLOR_FORMATTYPE;

typedef enum OMX_INDEXTYPE {
    OMX_IndexComponentStartUnused               = 0x01000000,
    OMX_IndexParamPriorityMgmt                  = 0x01000001,
    OMX_IndexParamAudioInit                     = 0x01000002,
    OMX_IndexParamImageInit                     = 0x01000003,
    OMX_IndexParamVideoInit                     = 0x01000004,
    OMX_IndexParamOtherInit                     = 0x01000005,
    OMX_IndexParamNumAvailableStreams           = 0x01000006,
    OMX_IndexParamActiveStream                  = 0x01000007,
    OMX_IndexParamSuspensionPolicy              = 0x01000008,
    OMX_IndexParamComponentSuspended            = 0x01000009,
    OMX_IndexConfigCapturing                    = 0x0100000A,
    OMX_IndexConfigCaptureMode                  = 0x0100000B,
    OMX_IndexAutoPauseAfterCapture              = 0x0100000C,
    OMX_IndexParamContentURI                    = 0x0100000D,
    OMX_IndexParamCustomContentPipe             = 0x0100000E,
    OMX_IndexParamDisableResourceConcealment    = 0x0100000F,
    OMX_IndexConfigMetadataItemCount            = 0x01000010,
    OMX_IndexConfigContainerNodeCount           = 0x01000011,
    OMX_IndexConfigMetadataItem                 = 0x01000012,
    OMX_IndexConfigCounterNodeID                = 0x01000013,
    OMX_IndexParamMetadataFilterType            = 0x01000014,
    OMX_IndexParamMetadataKeyFilter             = 0x01000015,
    OMX_IndexConfigPriorityMgmt                 = 0x01000016,
    OMX_IndexParamStandardComponentRole         = 0x01000017,

    OMX_IndexPortStartUnused                    = 0x02000000,
    OMX_IndexParamPortDefinition                = 0x02000001,
    OMX_IndexParamCompBufferSupplier            = 0x02000002,

    OMX_IndexVideoStartUnused                   = 0x06000000,
    OMX_IndexParamVideoPortFormat               = 0x06000001,
    OMX_IndexParamVideoQuantization             = 0x06000002,
    OMX_IndexParamVideoFastUpdate               = 0x06000003,
    OMX_IndexParamVideoBitrate                  = 0x06000004,
    OMX_IndexParamVideoMotionVector             = 0x06000005,
    OMX_IndexParamVideoIntraRefresh             = 0x06000006,
    OMX_IndexParamVideoErrorCorrection          = 0x06000007,
    OMX_IndexParamVideoVBSMC                    = 0x06000008,
    OMX_IndexParamVideoMpeg2                    = 0x06000009,
    OMX_IndexParamVideoMpeg4                    = 0x0600000A,
    OMX_IndexParamVideoWmv                      = 0x0600000B,
    OMX_IndexParamVideoRv                       = 0x0600000C,
    OMX_IndexParamVideoAvc                      = 0x0600000D,
    OMX_IndexParamVideoH263                     = 0x0600000E,
    OMX_IndexParamVideoProfileLevelQuerySupported = 0x0600000F,
    OMX_IndexParamVideoProfileLevelCurrent      = 0x06000010,
    OMX_IndexConfigVideoBitrate                 = 0x06000011,
    OMX_IndexConfigVideoFramerate               = 0x06000012,
    OMX_IndexConfigVideoIntraVOPRefresh         = 0x06000013,
    OMX_IndexConfigVideoIntraMBRefresh          = 0x06000014,
    OMX_IndexConfigVideoMBErrorReporting        = 0x06000015,
    OMX_IndexParamVideoMacroblocksPerFrame      = 0x06000016,
    OMX_IndexConfigVideoMacroBlockErrorMap      = 0x06000017,
    OMX_IndexParamVideoSliceFMO                 = 0x06000018,
    OMX_IndexConfigVideoAVCIntraPeriod          = 0x06000019,
    OMX_IndexConfigVideoNalSize                 = 0x0600001A,

    OMX_IndexCommonStartUnused                  = 0x07000000,
    OMX_IndexConfigCommonInputCrop              = 0x0700000E,
    OMX_IndexConfigCommonOutputCrop             = 0x0700000F,

    /* OHOS CodecExtTypes.idl CodecIndexExType (0x6F000000 + 0x00A00000) */
    Codec_IndexParamSupportBufferType           = 0x6FA00001,
    Codec_IndexParamUseBufferType               = 0x6FA00002,
    Codec_IndexParamGetBufferHandleUsage        = 0x6FA00003,
    Codec_IndexCodecVideoPortFormat             = 0x6FA00004,
    Codec_IndexParamControlRateConstantQuality  = 0x6FA00005,
    Codec_IndexParamVideoHevc                   = 0x6FA00007,
    Codec_IndexColorAspects                     = 0x6FA00008,
    Codec_IndexParamWorkingFrequency            = 0x6FA00009,
    Codec_IndexParamProcessName                 = 0x6FA0000A,
    Codec_IndexParamLowLatency                  = 0x6FA00010,
    Codec_IndexParamVrrOrMvUpload               = 0x6FA0001F,
    Codec_IndexParamFreezeBuffers               = 0x6FA00022,
    Codec_IndexParamFreqControl                 = 0x6FA00023,
    Codec_IndexParamLowPowerMode                = 0x6FA0002B,
    Codec_IndexParamLppTargetPts                = 0x6FA0002D,
    Codec_IndexParamCodecScenario               = 0x6FA00030,
    Codec_IndexParamMasteringDisplay            = 0x6FA00031,
    Codec_IndexParamContentLightLevel           = 0x6FA00032,
    Codec_IndexParamSetBufferHandleUsage        = 0x6FA00035,
    Codec_IndexParamIdrAppendXps                = 0x6FA00036,
    OMX_IndexMax                                = 0x7FFFFFFF
} OMX_INDEXTYPE;

#define OMX_BUFFERFLAG_EOS         0x00000001u
#define OMX_BUFFERFLAG_STARTTIME   0x00000002u
#define OMX_BUFFERFLAG_DECODEONLY  0x00000004u
#define OMX_BUFFERFLAG_DATACORRUPT 0x00000008u
#define OMX_BUFFERFLAG_ENDOFFRAME  0x00000010u
#define OMX_BUFFERFLAG_SYNCFRAME   0x00000020u
#define OMX_BUFFERFLAG_EXTRADATA   0x00000040u
#define OMX_BUFFERFLAG_CODECCONFIG 0x00000080u

/* CodecBufferType from CodecExtTypes.idl */
#define CODEC_BUFFER_TYPE_INVALID        0x00u
#define CODEC_BUFFER_TYPE_VIRTUAL_ADDR   0x01u
#define CODEC_BUFFER_TYPE_AVSHARE_MEM_FD 0x02u
#define CODEC_BUFFER_TYPE_HANDLE         0x04u
#define CODEC_BUFFER_TYPE_DYNAMIC_HANDLE 0x08u
#define CODEC_BUFFER_TYPE_DMA_MEM_FD     0x10u

/* OHOS PixelFormat values (DisplayComposerType.idl) */
#define OHOS_PIXEL_FMT_RGBA_8888     12
#define OHOS_PIXEL_FMT_YCBCR_420_SP  24
#define OHOS_PIXEL_FMT_YCRCB_420_SP  25
#define OHOS_PIXEL_FMT_YCBCR_420_P   28

typedef struct OMX_BUFFERHEADERTYPE {
    OMX_U32         nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U8 *        pBuffer;
    OMX_U32         nAllocLen;
    OMX_U32         nFilledLen;
    OMX_U32         nOffset;
    OMX_PTR         pAppPrivate;
    OMX_PTR         pPlatformPrivate;
    OMX_PTR         pInputPortPrivate;
    OMX_PTR         pOutputPortPrivate;
    OMX_HANDLETYPE  hMarkTargetComponent;
    OMX_PTR         pMarkData;
    OMX_U32         nTickCount;
    OMX_TICKS       nTimeStamp;
    OMX_U32         nFlags;
    OMX_U32         nOutputPortIndex;
    OMX_U32         nInputPortIndex;
} OMX_BUFFERHEADERTYPE;

typedef struct OMX_PORT_PARAM_TYPE {
    OMX_U32         nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U32         nPorts;
    OMX_U32         nStartPortNumber;
} OMX_PORT_PARAM_TYPE;

typedef struct OMX_PARAM_COMPONENTROLETYPE {
    OMX_U32         nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U8          cRole[OMX_MAX_STRINGNAME_SIZE];
} OMX_PARAM_COMPONENTROLETYPE;

typedef struct OMX_PARAM_BUFFERSUPPLIERTYPE {
    OMX_U32                nSize;
    OMX_VERSIONTYPE        nVersion;
    OMX_U32                nPortIndex;
    OMX_BUFFERSUPPLIERTYPE eBufferSupplier;
} OMX_PARAM_BUFFERSUPPLIERTYPE;

typedef struct OMX_VIDEO_PORTDEFINITIONTYPE {
    OMX_STRING            cMIMEType;
    OMX_NATIVE_DEVICETYPE pNativeRender;
    OMX_U32               nFrameWidth;
    OMX_U32               nFrameHeight;
    OMX_S32               nStride;
    OMX_U32               nSliceHeight;
    OMX_U32               nBitrate;
    OMX_U32               xFramerate;
    OMX_BOOL              bFlagErrorConcealment;
    OMX_VIDEO_CODINGTYPE  eCompressionFormat;
    OMX_COLOR_FORMATTYPE  eColorFormat;
    OMX_NATIVE_WINDOWTYPE pNativeWindow;
} OMX_VIDEO_PORTDEFINITIONTYPE;

typedef struct OMX_AUDIO_PORTDEFINITIONTYPE {
    OMX_STRING            cMIMEType;
    OMX_NATIVE_DEVICETYPE pNativeRender;
    OMX_BOOL              bFlagErrorConcealment;
    OMX_U32               eEncoding;
} OMX_AUDIO_PORTDEFINITIONTYPE;

typedef struct OMX_IMAGE_PORTDEFINITIONTYPE {
    OMX_STRING            cMIMEType;
    OMX_NATIVE_DEVICETYPE pNativeRender;
    OMX_U32               nFrameWidth;
    OMX_U32               nFrameHeight;
    OMX_S32               nStride;
    OMX_U32               nSliceHeight;
    OMX_BOOL              bFlagErrorConcealment;
    OMX_U32               eCompressionFormat;
    OMX_COLOR_FORMATTYPE  eColorFormat;
    OMX_NATIVE_WINDOWTYPE pNativeWindow;
} OMX_IMAGE_PORTDEFINITIONTYPE;

typedef struct OMX_OTHER_PORTDEFINITIONTYPE {
    OMX_U32 eFormat;
} OMX_OTHER_PORTDEFINITIONTYPE;

typedef struct OMX_PARAM_PORTDEFINITIONTYPE {
    OMX_U32            nSize;
    OMX_VERSIONTYPE    nVersion;
    OMX_U32            nPortIndex;
    OMX_DIRTYPE        eDir;
    OMX_U32            nBufferCountActual;
    OMX_U32            nBufferCountMin;
    OMX_U32            nBufferSize;
    OMX_BOOL           bEnabled;
    OMX_BOOL           bPopulated;
    OMX_PORTDOMAINTYPE eDomain;
    union {
        OMX_AUDIO_PORTDEFINITIONTYPE audio;
        OMX_VIDEO_PORTDEFINITIONTYPE video;
        OMX_IMAGE_PORTDEFINITIONTYPE image;
        OMX_OTHER_PORTDEFINITIONTYPE other;
    } format;
    OMX_BOOL           bBuffersContiguous;
    OMX_U32            nBufferAlignment;
} OMX_PARAM_PORTDEFINITIONTYPE;

typedef struct OMX_VIDEO_PARAM_PORTFORMATTYPE {
    OMX_U32              nSize;
    OMX_VERSIONTYPE      nVersion;
    OMX_U32              nPortIndex;
    OMX_U32              nIndex;
    OMX_VIDEO_CODINGTYPE eCompressionFormat;
    OMX_COLOR_FORMATTYPE eColorFormat;
    OMX_U32              xFramerate;
} OMX_VIDEO_PARAM_PORTFORMATTYPE;

typedef enum OMX_VIDEO_CONTROLRATETYPE {
    OMX_Video_ControlRateDisable = 0,
    OMX_Video_ControlRateVariable,
    OMX_Video_ControlRateConstant,
    OMX_Video_ControlRateVariableSkipFrames,
    OMX_Video_ControlRateConstantSkipFrames,
    OMX_Video_ControlRateMax = 0x7FFFFFFF
} OMX_VIDEO_CONTROLRATETYPE;

typedef struct OMX_VIDEO_PARAM_BITRATETYPE {
    OMX_U32                   nSize;
    OMX_VERSIONTYPE           nVersion;
    OMX_U32                   nPortIndex;
    OMX_VIDEO_CONTROLRATETYPE eControlRate;
    OMX_U32                   nTargetBitrate;
} OMX_VIDEO_PARAM_BITRATETYPE;

typedef struct OMX_VIDEO_CONFIG_BITRATETYPE {
    OMX_U32         nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U32         nPortIndex;
    OMX_U32         nEncodeBitrate;
} OMX_VIDEO_CONFIG_BITRATETYPE;

typedef struct OMX_CONFIG_FRAMERATETYPE {
    OMX_U32         nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U32         nPortIndex;
    OMX_U32         xEncodeFramerate;
} OMX_CONFIG_FRAMERATETYPE;

typedef struct OMX_CONFIG_INTRAREFRESHVOPTYPE {
    OMX_U32         nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U32         nPortIndex;
    OMX_BOOL        IntraRefreshVOP;
} OMX_CONFIG_INTRAREFRESHVOPTYPE;

typedef struct OMX_VIDEO_PARAM_AVCTYPE {
    OMX_U32         nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U32         nPortIndex;
    OMX_U32         nSliceHeaderSpacing;
    OMX_U32         nPFrames;
    OMX_U32         nBFrames;
    OMX_BOOL        bUseHadamard;
    OMX_U32         nRefFrames;
    OMX_U32         nRefIdx10ActiveMinus1;
    OMX_U32         nRefIdx11ActiveMinus1;
    OMX_BOOL        bEnableUEP;
    OMX_BOOL        bEnableFMO;
    OMX_BOOL        bEnableASO;
    OMX_BOOL        bEnableRS;
    OMX_U32         eProfile;
    OMX_U32         eLevel;
    OMX_U32         nAllowedPictureTypes;
    OMX_BOOL        bFrameMBsOnly;
    OMX_BOOL        bMBAFF;
    OMX_BOOL        bEntropyCodingCABAC;
    OMX_BOOL        bWeightedPPrediction;
    OMX_U32         nWeightedBipredicitonMode;
    OMX_BOOL        bconstIpred;
    OMX_BOOL        bDirect8x8Inference;
    OMX_BOOL        bDirectSpatialTemporal;
    OMX_U32         nCabacInitIdc;
    OMX_U32         eLoopFilterMode;
} OMX_VIDEO_PARAM_AVCTYPE;

typedef struct OMX_VIDEO_PARAM_PROFILELEVELTYPE {
    OMX_U32         nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U32         nPortIndex;
    OMX_U32         eProfile;
    OMX_U32         eLevel;
    OMX_U32         nProfileIndex;
} OMX_VIDEO_PARAM_PROFILELEVELTYPE;

typedef struct OMX_VIDEO_PARAM_QUANTIZATIONTYPE {
    OMX_U32         nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U32         nPortIndex;
    OMX_U32         nQpI;
    OMX_U32         nQpP;
    OMX_U32         nQpB;
} OMX_VIDEO_PARAM_QUANTIZATIONTYPE;

typedef struct OMX_CONFIG_RECTTYPE {
    OMX_U32         nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_U32         nPortIndex;
    OMX_S32         nLeft;
    OMX_S32         nTop;
    OMX_U32         nWidth;
    OMX_U32         nHeight;
} OMX_CONFIG_RECTTYPE;

/* OHOS CodecExtTypes.idl C equivalents */
typedef struct OhosSupportBufferType {
    uint32_t        size;
    OMX_VERSIONTYPE version;
    uint32_t        portIndex;
    uint32_t        bufferTypes;
} OhosSupportBufferType;

typedef struct OhosUseBufferType {
    uint32_t        size;
    OMX_VERSIONTYPE version;
    uint32_t        portIndex;
    uint32_t        bufferType;
} OhosUseBufferType;

typedef struct OhosGetBufferHandleUsageParams {
    uint32_t        size;
    OMX_VERSIONTYPE version;
    uint32_t        portIndex;
    uint64_t        usage;
} OhosGetBufferHandleUsageParams;

typedef struct OhosCodecVideoPortFormatParam {
    uint32_t        size;
    OMX_VERSIONTYPE version;
    uint32_t        portIndex;
    uint32_t        codecColorIndex;
    uint32_t        codecColorFormat;
    uint32_t        codecCompressFormat;
    uint32_t        framerate;
} OhosCodecVideoPortFormatParam;

typedef struct OhosControlRateConstantQuality {
    uint32_t        size;
    OMX_VERSIONTYPE version;
    uint32_t        portIndex;
    uint32_t        qualityValue;
} OhosControlRateConstantQuality;

typedef struct OhosCodecVideoParamHevc {
    uint32_t        size;
    OMX_VERSIONTYPE version;
    uint32_t        portIndex;
    uint32_t        profile;
    uint32_t        level;
    uint32_t        keyFrameInterval;
} OhosCodecVideoParamHevc;

typedef struct OhosColorAspects {
    uint32_t range;
    uint32_t primaries;
    uint32_t transfer;
    uint32_t matrixCoeffs;
} OhosColorAspects;

typedef struct OhosCodecVideoColorspace {
    uint32_t         size;
    OMX_VERSIONTYPE  version;
    uint32_t         portIndex;
    uint32_t         requestingDataSpace;
    uint32_t         dataSpaceChanged;
    uint32_t         pixeFormat;
    uint32_t         dataSpace;
    OhosColorAspects aspects;
} OhosCodecVideoColorspace;

typedef struct OhosWorkingFrequencyParam {
    uint32_t        size;
    OMX_VERSIONTYPE version;
    uint32_t        level;
} OhosWorkingFrequencyParam;

typedef struct MarbleBufferHandle {
    int32_t  fd;
    int32_t  width;
    int32_t  stride;
    int32_t  height;
    int32_t  size;
    int32_t  format;
    uint64_t usage;
    void *   virAddr;
    uint64_t phyAddr;
    uint32_t reserveFds;
    uint32_t reserveInts;
    int32_t  reserve[0];
} MarbleBufferHandle;

typedef struct MarbleDynamicBuffer {
    int32_t                    type;
    struct MarbleBufferHandle *bufferHandle;
} MarbleDynamicBuffer;

typedef struct MarbleOmxAppPriv {
    int32_t  fd;
    uint32_t sizeOfParam;
    void *   param;
} MarbleOmxAppPriv;

typedef struct OMX_CALLBACKTYPE {
    OMX_ERRORTYPE (*EventHandler)(OMX_HANDLETYPE hComponent, OMX_PTR pAppData,
                                  OMX_EVENTTYPE eEvent, OMX_U32 nData1,
                                  OMX_U32 nData2, OMX_PTR pEventData);
    OMX_ERRORTYPE (*EmptyBufferDone)(OMX_HANDLETYPE hComponent, OMX_PTR pAppData,
                                     OMX_BUFFERHEADERTYPE *pBuffer);
    OMX_ERRORTYPE (*FillBufferDone)(OMX_HANDLETYPE hComponent, OMX_PTR pAppData,
                                    OMX_BUFFERHEADERTYPE *pBuffer);
} OMX_CALLBACKTYPE;

typedef struct OMX_COMPONENTTYPE {
    OMX_U32         nSize;
    OMX_VERSIONTYPE nVersion;
    OMX_PTR         pComponentPrivate;
    OMX_PTR         pApplicationPrivate;
    OMX_ERRORTYPE (*GetComponentVersion)(OMX_HANDLETYPE hComponent, OMX_STRING pComponentName,
                                         OMX_VERSIONTYPE *pComponentVersion,
                                         OMX_VERSIONTYPE *pSpecVersion,
                                         OMX_UUIDTYPE *pComponentUUID);
    OMX_ERRORTYPE (*SendCommand)(OMX_HANDLETYPE hComponent, OMX_COMMANDTYPE Cmd,
                                 OMX_U32 nParam1, OMX_PTR pCmdData);
    OMX_ERRORTYPE (*GetParameter)(OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nParamIndex,
                                  OMX_PTR pComponentParameterStructure);
    OMX_ERRORTYPE (*SetParameter)(OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nIndex,
                                  OMX_PTR pComponentParameterStructure);
    OMX_ERRORTYPE (*GetConfig)(OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nIndex,
                               OMX_PTR pComponentConfigStructure);
    OMX_ERRORTYPE (*SetConfig)(OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nIndex,
                               OMX_PTR pComponentConfigStructure);
    OMX_ERRORTYPE (*GetExtensionIndex)(OMX_HANDLETYPE hComponent, OMX_STRING cParameterName,
                                       OMX_INDEXTYPE *pIndexType);
    OMX_ERRORTYPE (*GetState)(OMX_HANDLETYPE hComponent, OMX_STATETYPE *pState);
    OMX_ERRORTYPE (*ComponentTunnelRequest)(OMX_HANDLETYPE hComponent, OMX_U32 nPort,
                                            OMX_HANDLETYPE hTunneledComp, OMX_U32 nTunneledPort,
                                            OMX_PTR pTunnelSetup);
    OMX_ERRORTYPE (*UseBuffer)(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE **ppBufferHdr,
                               OMX_U32 nPortIndex, OMX_PTR pAppPrivate,
                               OMX_U32 nSizeBytes, OMX_U8 *pBuffer);
    OMX_ERRORTYPE (*AllocateBuffer)(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE **ppBuffer,
                                    OMX_U32 nPortIndex, OMX_PTR pAppPrivate, OMX_U32 nSizeBytes);
    OMX_ERRORTYPE (*FreeBuffer)(OMX_HANDLETYPE hComponent, OMX_U32 nPortIndex,
                                OMX_BUFFERHEADERTYPE *pBuffer);
    OMX_ERRORTYPE (*EmptyThisBuffer)(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE *pBuffer);
    OMX_ERRORTYPE (*FillThisBuffer)(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE *pBuffer);
    OMX_ERRORTYPE (*SetCallbacks)(OMX_HANDLETYPE hComponent, OMX_CALLBACKTYPE *pCallbacks,
                                  OMX_PTR pAppData);
    OMX_ERRORTYPE (*ComponentDeInit)(OMX_HANDLETYPE hComponent);
    OMX_ERRORTYPE (*UseEGLImage)(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE **ppBufferHdr,
                                 OMX_U32 nPortIndex, OMX_PTR pAppPrivate, void *eglImage);
    OMX_ERRORTYPE (*ComponentRoleEnum)(OMX_HANDLETYPE hComponent, OMX_U8 *cRole, OMX_U32 nIndex);
} OMX_COMPONENTTYPE;

/* LP64 AArch64 ABI verification assertions matching donor CCM & libhcodec.z.so */
_Static_assert(sizeof(OMX_BUFFERHEADERTYPE) == 112, "OMX_BUFFERHEADERTYPE LP64 size");
_Static_assert(offsetof(OMX_BUFFERHEADERTYPE, nFilledLen) == 20, "nFilledLen offset");
_Static_assert(offsetof(OMX_BUFFERHEADERTYPE, nTimeStamp) == 88, "nTimeStamp offset");
_Static_assert(offsetof(OMX_BUFFERHEADERTYPE, nFlags) == 96, "nFlags offset");
_Static_assert(sizeof(OMX_PARAM_PORTDEFINITIONTYPE) == 112, "OMX_PARAM_PORTDEFINITIONTYPE LP64 size");
_Static_assert(offsetof(OMX_PARAM_PORTDEFINITIONTYPE, format) +
               offsetof(OMX_VIDEO_PORTDEFINITIONTYPE, nFrameWidth) == 56, "nFrameWidth offset");
_Static_assert(offsetof(OMX_PARAM_PORTDEFINITIONTYPE, format) +
               offsetof(OMX_VIDEO_PORTDEFINITIONTYPE, xFramerate) == 76, "xFramerate offset");
_Static_assert(offsetof(OMX_PARAM_PORTDEFINITIONTYPE, format) +
               offsetof(OMX_VIDEO_PORTDEFINITIONTYPE, eCompressionFormat) == 84, "eCompressionFormat offset");
_Static_assert(sizeof(OMX_VIDEO_PARAM_BITRATETYPE) == 20, "OMX_VIDEO_PARAM_BITRATETYPE size");
_Static_assert(sizeof(OhosSupportBufferType) == 16, "SupportBufferType size");
_Static_assert(sizeof(OhosUseBufferType) == 16, "UseBufferType size");
_Static_assert(sizeof(OhosGetBufferHandleUsageParams) == 24, "GetBufferHandleUsageParams size");
_Static_assert(sizeof(OhosCodecVideoPortFormatParam) == 28, "CodecVideoPortFormatParam size");
_Static_assert(sizeof(OhosCodecVideoParamHevc) == 24, "CodecVideoParamHevc size");
_Static_assert(sizeof(struct MarbleOmxAppPriv) == 16, "OMXBufferAppPrivateData size");
_Static_assert(offsetof(OMX_COMPONENTTYPE, GetParameter) == 0x28, "GetParameter vtable offset");
_Static_assert(offsetof(OMX_COMPONENTTYPE, ComponentRoleEnum) == 0xA0, "ComponentRoleEnum offset");
_Static_assert(sizeof(OMX_COMPONENTTYPE) == 0xA8, "OMX_COMPONENTTYPE LP64 size");

/* Standard OpenMAX IL 1.1.2 Core entry points */
OMX_ERRORTYPE OMX_Init(void);
OMX_ERRORTYPE OMX_Deinit(void);
OMX_ERRORTYPE OMX_ComponentNameEnum(OMX_STRING cComponentName, OMX_U32 nNameLength, OMX_U32 nIndex);
OMX_ERRORTYPE OMX_GetHandle(OMX_HANDLETYPE *pHandle, OMX_STRING cComponentName,
                            OMX_PTR pAppData, OMX_CALLBACKTYPE *pCallBacks);
OMX_ERRORTYPE OMX_FreeHandle(OMX_HANDLETYPE hComponent);
OMX_ERRORTYPE OMX_GetRolesOfComponent(OMX_STRING compName, OMX_U32 *pNumRoles, OMX_U8 **roles);
OMX_ERRORTYPE OMX_GetComponentsOfRole(OMX_STRING role, OMX_U32 *pNumComps, OMX_U8 **compNames);
OMX_ERRORTYPE OMX_SetupTunnel(OMX_HANDLETYPE hOutput, OMX_U32 nPortOutput,
                              OMX_HANDLETYPE hInput, OMX_U32 nPortInput);
OMX_ERRORTYPE OMX_GetContentPipe(OMX_HANDLETYPE *hPipe, OMX_STRING szURI);

#ifdef __cplusplus
}
#endif

#endif /* MARBLE_OMX_IL_MINIMAL_H */
