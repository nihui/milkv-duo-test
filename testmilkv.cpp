#include <stdio.h>

#include <string>
#include <vector>

#include <sys/time.h> //gettimeofday()
#include <unistd.h>   // sleep()

#if __riscv_vector
#include <riscv_vector.h>
#endif

// #define STB_IMAGE_IMPLEMENTATION
// #define STBI_RVV
// // #define STBI_NEON
// #include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// #include <sstream>
// #include "exif.hpp"

// cvi header
#include <cvi_buffer.h>
#include <cvi_gdc.h>
#include <cvi_sys.h>
#include <cvi_vb.h>
#include <cvi_vdec.h>
#include <cvi_vpss.h>

double get_current_time()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

class CVI_VDEC_JpegDecoder
{
public:
    CVI_VDEC_JpegDecoder(const unsigned char* jpgdata, size_t jpgsize, int _orientation);

    int ping();

    int decode(unsigned char* outrgb, int approx_yuv444_rotate = 0);

protected:
    const unsigned char* jpgdata;
    size_t jpgsize;

public:
    int corrupted; // 0=fine
    int width;
    int height;
    int components; // 1=gray 3=yuv
    int sampling_factor; // 0=444 1=422h 2=422v 3=420 4=400
    int progressive;

//
//     1        2       3      4         5            6           7          8
//
//   888888  888888      88  88      8888888888  88                  88  8888888888
//   88          88      88  88      88  88      88  88          88  88      88  88
//   8888      8888    8888  8888    88          8888888888  8888888888          88
//   88          88      88  88
//   88          88  888888  888888
//
// ref http://sylvana.net/jpegcrop/exif_orientation.html

    int orientation; // exif

    // for CVI_VDEC
    PIXEL_FORMAT_E yuv_pixel_format;
    CVI_U32 yuv_buffer_size;
    PIXEL_FORMAT_E yuv_rotated_pixel_format;
    CVI_U32 yuv_rotated_buffer_size;
    PIXEL_FORMAT_E rgb_pixel_format;
    CVI_U32 rgb_buffer_size;
};

CVI_VDEC_JpegDecoder::CVI_VDEC_JpegDecoder(const unsigned char* _jpgdata, size_t _jpgsize, int _orientation)
{
    jpgdata = _jpgdata;
    jpgsize = _jpgsize;

    corrupted = 1;
    width = 0;
    height = 0;
    components = 0;
    sampling_factor = -1;
    progressive = 0;
    orientation = _orientation;

    yuv_pixel_format = PIXEL_FORMAT_YUV_PLANAR_444;
    yuv_buffer_size = 0;
    yuv_rotated_pixel_format = PIXEL_FORMAT_NV12;
    yuv_rotated_buffer_size = 0;
    rgb_pixel_format = PIXEL_FORMAT_RGB_888;
    rgb_buffer_size = 0;
}

int CVI_VDEC_JpegDecoder::ping()
{
    if (!jpgdata || jpgsize < 4)
        return -1;

    // jpg magic
    if (jpgdata[0] != 0xFF || jpgdata[1] != 0xD8)
        return -1;

    // parse jpg for width height components sampling-factor progressive
    const unsigned char* pbuf = jpgdata;
    const unsigned char* pend = pbuf + jpgsize;
    while (pbuf + 1 < pend)
    {
        unsigned char marker0 = pbuf[0];
        unsigned char marker1 = pbuf[1];
        pbuf += 2;

        if (marker0 != 0xFF)
            break;

        // SOI EOI
        if (marker1 == 0xD8 || marker1 == 0xD9)
            continue;

        if (marker1 != 0xC0 && marker1 != 0xC2)
        {
            unsigned int skipsize = (pbuf[0] << 8) + pbuf[1];
            pbuf += skipsize;
            continue;
        }

        // SOF0 SOF2
        unsigned int skipsize = (pbuf[0] << 8) + pbuf[1];
        if (pbuf + skipsize > pend)
            break;

        // only 8bit supported
        if (pbuf[2] != 8)
            break;

        height = (pbuf[3] << 8) + pbuf[4];
        width = (pbuf[5] << 8) + pbuf[6];
        if (height == 0 || width == 0)
            break;

        components = pbuf[7];
        if (components != 1 && components != 3)
            break;

        pbuf += 8;

        unsigned char phv[3][2];
        for (int c = 0; c < components; c++)
        {
            unsigned char q = pbuf[1];
            phv[c][0] = (q >> 4); // 2 1 1   2 1 1   1 1 1   1 1 1
            phv[c][1] = (q & 15); // 2 1 1   1 1 1   2 1 1   1 1 1
            pbuf += 3;
        }

        if (components == 3 && phv[1][0] == 1 && phv[1][1] == 1 && phv[2][0] == 1 && phv[2][1] == 1)
        {
            if (phv[0][0] == 1 && phv[0][1] == 1) sampling_factor = 0;
            if (phv[0][0] == 2 && phv[0][1] == 1) sampling_factor = 1;
            if (phv[0][0] == 1 && phv[0][1] == 2) sampling_factor = 2;
            if (phv[0][0] == 2 && phv[0][1] == 2) sampling_factor = 3;
        }
        if (components == 1 && phv[0][0] == 1 && phv[0][1] == 1)
        {
            sampling_factor = 4;
        }

        // unsupported sampling factor
        if (sampling_factor == -1)
            break;

        // jpg is fine
        corrupted = 0;

        if (marker1 == 0xC2)
            progressive = 1;

        break;
    }

    // resolve exif orientation
    // {
    //     std::string s((const char*)jpgdata, jpgsize);
    //     std::istringstream iss(s);
    //
    //     cv::ExifReader exif_reader(iss);
    //     if (exif_reader.parse())
    //     {
    //         cv::ExifEntry_t e = exif_reader.getTag(cv::ORIENTATION);
    //         orientation = e.field_u16;
    //         if (orientation < 1 && orientation > 8)
    //             orientation = 1;
    //     }
    // }

    if (orientation > 4)
    {
        // swap width height
        int tmp = height;
        height = width;
        width = tmp;
    }

    if (corrupted)
        return -1;

    switch (sampling_factor)
    {
    case 0:
        yuv_pixel_format = PIXEL_FORMAT_YUV_PLANAR_444;
        break;
    case 1:
        yuv_pixel_format = PIXEL_FORMAT_YUV_PLANAR_422;
        break;
    case 2:
        yuv_pixel_format = PIXEL_FORMAT_YUV_PLANAR_422;
        break;
    case 3:
        yuv_pixel_format = PIXEL_FORMAT_YUV_PLANAR_420;
        break;
    case 4:
        yuv_pixel_format = PIXEL_FORMAT_YUV_400;
        yuv_rotated_pixel_format = PIXEL_FORMAT_YUV_400; // fast path for grayscale
        break;
    default:
        // should never reach here
        break;
    }

    if (orientation > 4)
    {
        yuv_buffer_size = VDEC_GetPicBufferSize(PT_JPEG, height, width, yuv_pixel_format, DATA_BITWIDTH_8, COMPRESS_MODE_NONE);
        yuv_rotated_buffer_size = COMMON_GetPicBufferSize(ALIGN(width, 64), ALIGN(height, 64), yuv_rotated_pixel_format, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
        rgb_buffer_size = COMMON_GetPicBufferSize(ALIGN(width, 64), ALIGN(height, 64), rgb_pixel_format, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    }
    else
    {
        yuv_buffer_size = VDEC_GetPicBufferSize(PT_JPEG, width, height, yuv_pixel_format, DATA_BITWIDTH_8, COMPRESS_MODE_NONE);
        rgb_buffer_size = COMMON_GetPicBufferSize(width, height, rgb_pixel_format, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    }

    // fprintf(stderr, "%d %d %d\n", yuv_buffer_size, yuv_rotated_buffer_size, rgb_buffer_size);

    return 0;
}

int CVI_VDEC_JpegDecoder::decode(unsigned char* outrgb, int approx_yuv444_rotate)
{
    if (!outrgb)
        return -1;

    // corrupted file
    if (corrupted)
        return -1;

    // progressive not supported
    if (progressive)
        return -1;

    // yuv422v not supported
    if (sampling_factor == 2)
        return -1;

    // yuv444 rotate 90/270 not supported as nv12 color loss
    if (sampling_factor == 0 && orientation > 4 && approx_yuv444_rotate == 0)
        return -1;

    // flag
    int ret_val = 0;
    int b_vb_inited = 0;
    int b_sys_inited = 0;

    // vb pool
    int b_vb_pool0_created = 0;
    int b_vb_pool1_created = 0;
    int b_vb_pool2_created = 0;
    VB_POOL VbPool0 = VB_INVALID_POOLID;
    VB_POOL VbPool1 = VB_INVALID_POOLID;
    VB_POOL VbPool2 = VB_INVALID_POOLID;

    // vdec
    int b_vdec_chn_created = 0;
    int b_vdec_vbpool_attached = 0;
    int b_vdec_recvstream_started = 0;
    int b_vdec_frame_got = 0;

    VDEC_CHN VdChn = 0;
    VIDEO_FRAME_INFO_S stFrameInfo;

    // vpss rotate
    int b_vpss_rotate_grp_created = 0;
    int b_vpss_rotate_chn_enabled = 0;
    int b_vpss_rotate_vbpool_attached = 0;
    int b_vpss_rotate_grp_started = 0;
    int b_vpss_rotate_frame_got = 0;

    VPSS_GRP VpssRotateGrp = 1;
    // VPSS_GRP VpssGrp = CVI_VPSS_GetAvailableGrp();
    VPSS_CHN VpssRotateChn = VPSS_CHN0;
    VIDEO_FRAME_INFO_S stFrameInfo_rotated;

    // vpss rgb
    int b_vpss_rgb_grp_created = 0;
    int b_vpss_rgb_chn_enabled = 0;
    int b_vpss_rgb_vbpool_attached = 0;
    int b_vpss_rgb_grp_started = 0;
    int b_vpss_rgb_frame_got = 0;

    VPSS_GRP VpssRgbGrp = 0;
    // VPSS_GRP VpssGrp = CVI_VPSS_GetAvailableGrp();
    VPSS_CHN VpssRgbChn = VPSS_CHN0;
    VIDEO_FRAME_INFO_S stFrameInfo_rgb;


    // init vb pool
    {
        VB_CONFIG_S stVbConfig;
        stVbConfig.u32MaxPoolCnt = 1;
        stVbConfig.astCommPool[0].u32BlkSize = orientation > 4 ? yuv_rotated_buffer_size : 4096;
        stVbConfig.astCommPool[0].u32BlkCnt = 1;
        stVbConfig.astCommPool[0].enRemapMode = VB_REMAP_MODE_NONE;
        snprintf(stVbConfig.astCommPool[0].acName, MAX_VB_POOL_NAME_LEN, "cv-imread-jpg-comm0");

        CVI_S32 ret = CVI_VB_SetConfig(&stVbConfig);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VB_SetConfig failed %x\n", ret);
            ret_val = -1;
            goto OUT;
        }
    }

    {
        CVI_S32 ret = CVI_VB_Init();
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VB_Init failed %x\n", ret);
            ret_val = -1;
            goto OUT;
        }

        b_vb_inited = 1;
    }

    {
        CVI_S32 ret = CVI_SYS_Init();
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_SYS_Init failed %x\n", ret);
            ret_val = -1;
            goto OUT;
        }

        b_sys_inited = 1;
    }

    // prepare vb pool
    {
        // create vb pool0
        {
            VB_POOL_CONFIG_S stVbPoolCfg;
            stVbPoolCfg.u32BlkSize = yuv_buffer_size;
            stVbPoolCfg.u32BlkCnt = 1;
            stVbPoolCfg.enRemapMode = VB_REMAP_MODE_NONE;
            snprintf(stVbPoolCfg.acName, MAX_VB_POOL_NAME_LEN, "cv-imread-jpg-%d-0", orientation);

            VbPool0 = CVI_VB_CreatePool(&stVbPoolCfg);
            if (VbPool0 == VB_INVALID_POOLID)
            {
                fprintf(stderr, "CVI_VB_CreatePool VbPool0 failed %d\n", VbPool0);
                ret_val = -1;
                goto OUT;
            }

            b_vb_pool0_created = 1;
        }

        // create vb pool1
        {
            VB_POOL_CONFIG_S stVbPoolCfg;
            stVbPoolCfg.u32BlkSize = orientation > 4 ? yuv_rotated_buffer_size : rgb_buffer_size;
            stVbPoolCfg.u32BlkCnt = 1;
            stVbPoolCfg.enRemapMode = VB_REMAP_MODE_NONE;
            snprintf(stVbPoolCfg.acName, MAX_VB_POOL_NAME_LEN, "cv-imread-jpg-%d-1", orientation);

            VbPool1 = CVI_VB_CreatePool(&stVbPoolCfg);
            if (VbPool1 == VB_INVALID_POOLID)
            {
                fprintf(stderr, "CVI_VB_CreatePool VbPool1 failed %d\n", VbPool1);
                ret_val = -1;
                goto OUT;
            }

            b_vb_pool1_created = 1;
        }

        // create vb pool2
        if (orientation > 4)
        {
            VB_POOL_CONFIG_S stVbPoolCfg;
            stVbPoolCfg.u32BlkSize = rgb_buffer_size;
            stVbPoolCfg.u32BlkCnt = 1;
            stVbPoolCfg.enRemapMode = VB_REMAP_MODE_NONE;
            snprintf(stVbPoolCfg.acName, MAX_VB_POOL_NAME_LEN, "cv-imread-jpg-%d-1", orientation);

            VbPool2 = CVI_VB_CreatePool(&stVbPoolCfg);
            if (VbPool2 == VB_INVALID_POOLID)
            {
                fprintf(stderr, "CVI_VB_CreatePool VbPool2 failed %d\n", VbPool2);
                ret_val = -1;
                goto OUT;
            }

            b_vb_pool2_created = 1;
        }
    }

    // prepare vdec
    {
        // vdec create chn
        {
            VDEC_CHN_ATTR_S stAttr;
            stAttr.enType = PT_JPEG;
            stAttr.enMode = VIDEO_MODE_FRAME;
            stAttr.u32PicWidth = orientation > 4 ? height : width;
            stAttr.u32PicHeight = orientation > 4 ? width : height;
            stAttr.u32StreamBufSize = ALIGN(width * height, 0x4000);
            stAttr.u32FrameBufSize = yuv_buffer_size;
            stAttr.u32FrameBufCnt = 1;
            stAttr.stVdecVideoAttr.u32RefFrameNum = 0;
            stAttr.stVdecVideoAttr.bTemporalMvpEnable = CVI_FALSE;
            stAttr.stVdecVideoAttr.u32TmvBufSize = 0;

            CVI_S32 ret = CVI_VDEC_CreateChn(VdChn, &stAttr);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VDEC_CreateChn failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }

            b_vdec_chn_created = 1;
        }

        // vdec set mod param
        {
            VDEC_MOD_PARAM_S stModParam;
            CVI_S32 ret = CVI_VDEC_GetModParam(&stModParam);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VDEC_GetModParam failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }

            stModParam.enVdecVBSource = VB_SOURCE_USER;

            ret = CVI_VDEC_SetModParam(&stModParam);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VDEC_SetModParam failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }
        }

        // vdec set chn param
        {
            VDEC_CHN_PARAM_S stParam;
            CVI_S32 ret = CVI_VDEC_GetChnParam(VdChn, &stParam);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VDEC_GetChnParam failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }

            stParam.stVdecPictureParam.u32Alpha = 255;
            stParam.enPixelFormat = yuv_pixel_format;
            stParam.u32DisplayFrameNum = 0;

            ret = CVI_VDEC_SetChnParam(VdChn, &stParam);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VDEC_SetChnParam failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }
        }

        // vdec attach vbpool
        {
            VDEC_CHN_POOL_S stPool;
            stPool.hPicVbPool = VbPool0;
            stPool.hTmvVbPool = VB_INVALID_POOLID;

            CVI_S32 ret = CVI_VDEC_AttachVbPool(VdChn, &stPool);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VDEC_AttachVbPool failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }

            b_vdec_vbpool_attached = 1;
        }
    }

    // prepare vpss rotate
    if (orientation > 4)
    {
        // vpss create grp
        {
            VPSS_GRP_ATTR_S stGrpAttr;
            stGrpAttr.u32MaxW = height;
            stGrpAttr.u32MaxH = width;
            stGrpAttr.enPixelFormat = yuv_pixel_format;
            stGrpAttr.stFrameRate.s32SrcFrameRate = -1;
            stGrpAttr.stFrameRate.s32DstFrameRate = -1;
            stGrpAttr.u8VpssDev = 0;

            CVI_S32 ret = CVI_VPSS_CreateGrp(VpssRotateGrp, &stGrpAttr);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_CreateGrp failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }

            b_vpss_rotate_grp_created = 1;
        }

        // vpss set chn attr
        {
            CVI_BOOL need_mirror = CVI_FALSE;
            CVI_BOOL need_flip = CVI_FALSE;
            if (orientation == 5 || orientation == 7)
            {
                need_mirror = CVI_TRUE;
            }

            VPSS_CHN_ATTR_S stChnAttr;
            stChnAttr.u32Width = ALIGN(height, 64);
            stChnAttr.u32Height = ALIGN(width, 64);
            stChnAttr.enVideoFormat = VIDEO_FORMAT_LINEAR;
            stChnAttr.enPixelFormat = yuv_rotated_pixel_format;
            stChnAttr.stFrameRate.s32SrcFrameRate = -1;
            stChnAttr.stFrameRate.s32DstFrameRate = -1;
            stChnAttr.bMirror = need_mirror;
            stChnAttr.bFlip = need_flip;
            stChnAttr.u32Depth = 0;
            stChnAttr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
            stChnAttr.stAspectRatio.bEnableBgColor = CVI_FALSE;
            stChnAttr.stAspectRatio.u32BgColor = 0;
            stChnAttr.stAspectRatio.stVideoRect.s32X = 0;
            stChnAttr.stAspectRatio.stVideoRect.s32Y = 0;
            stChnAttr.stAspectRatio.stVideoRect.u32Width = ALIGN(height, 64);
            stChnAttr.stAspectRatio.stVideoRect.u32Height = ALIGN(width, 64);
            stChnAttr.stNormalize.bEnable = CVI_FALSE;
            stChnAttr.stNormalize.factor[0] = 0.f;
            stChnAttr.stNormalize.factor[1] = 0.f;
            stChnAttr.stNormalize.factor[2] = 0.f;
            stChnAttr.stNormalize.mean[0] = 0.f;
            stChnAttr.stNormalize.mean[1] = 0.f;
            stChnAttr.stNormalize.mean[2] = 0.f;
            stChnAttr.stNormalize.rounding = VPSS_ROUNDING_TO_EVEN;

            CVI_S32 ret = CVI_VPSS_SetChnAttr(VpssRotateGrp, VpssRotateChn, &stChnAttr);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_SetChnAttr failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }
        }

        // vpss rotate
        {
            ROTATION_E rotation = ROTATION_0;
            if (orientation == 5 || orientation == 6)
            {
                rotation = ROTATION_270;
            }
            if (orientation == 7 || orientation == 8)
            {
                rotation = ROTATION_90;
            }

            CVI_S32 ret = CVI_VPSS_SetChnRotation(VpssRotateGrp, VpssRotateChn, rotation);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_SetChnRotation %d failed %x\n", rotation, ret);
                ret_val = -1;
                goto OUT;
            }
        }

        // vpss enable chn
        {
            CVI_S32 ret = CVI_VPSS_EnableChn(VpssRotateGrp, VpssRotateChn);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_EnableChn failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }

            b_vpss_rotate_chn_enabled = 1;
        }

        {
            CVI_S32 ret = CVI_VPSS_DetachVbPool(VpssRotateGrp, VpssRotateChn);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_DetachVbPool failed %x\n", ret);
                ret_val = -1;
            }
        }

        // vpss attach vb pool
        {
            CVI_S32 ret = CVI_VPSS_AttachVbPool(VpssRotateGrp, VpssRotateChn, VbPool1);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_AttachVbPool failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }

            b_vpss_rotate_vbpool_attached = 1;
        }
    }

    // prepare vpss rgb
    {
        // vpss create grp
        {
            VPSS_GRP_ATTR_S stGrpAttr;
            stGrpAttr.u32MaxW = orientation > 4 ? ALIGN(width, 64) : width;
            stGrpAttr.u32MaxH = orientation > 4 ? ALIGN(height, 64) : height;
            stGrpAttr.enPixelFormat = orientation > 4 ? yuv_rotated_pixel_format : yuv_pixel_format;
            stGrpAttr.stFrameRate.s32SrcFrameRate = -1;
            stGrpAttr.stFrameRate.s32DstFrameRate = -1;
            stGrpAttr.u8VpssDev = 0;

            CVI_S32 ret = CVI_VPSS_CreateGrp(VpssRgbGrp, &stGrpAttr);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_CreateGrp failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }

            b_vpss_rgb_grp_created = 1;
        }

        // vpss set chn attr
        {
            CVI_BOOL need_mirror = CVI_FALSE;
            CVI_BOOL need_flip = CVI_FALSE;
            if (orientation == 2 || orientation == 3)
            {
                need_mirror = CVI_TRUE;
            }
            if (orientation == 3 || orientation == 4)
            {
                need_flip = CVI_TRUE;
            }

            VPSS_CHN_ATTR_S stChnAttr;
            stChnAttr.u32Width = width;
            stChnAttr.u32Height = height;
            stChnAttr.enVideoFormat = VIDEO_FORMAT_LINEAR;
            stChnAttr.enPixelFormat = rgb_pixel_format;
            stChnAttr.stFrameRate.s32SrcFrameRate = -1;
            stChnAttr.stFrameRate.s32DstFrameRate = -1;
            stChnAttr.bMirror = need_mirror;
            stChnAttr.bFlip = need_flip;
            stChnAttr.u32Depth = 0;
            stChnAttr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
            stChnAttr.stAspectRatio.bEnableBgColor = CVI_FALSE;
            stChnAttr.stAspectRatio.u32BgColor = 0;
            stChnAttr.stAspectRatio.stVideoRect.s32X = 0;
            stChnAttr.stAspectRatio.stVideoRect.s32Y = 0;
            stChnAttr.stAspectRatio.stVideoRect.u32Width = width;
            stChnAttr.stAspectRatio.stVideoRect.u32Height = height;
            stChnAttr.stNormalize.bEnable = CVI_FALSE;
            stChnAttr.stNormalize.factor[0] = 0.f;
            stChnAttr.stNormalize.factor[1] = 0.f;
            stChnAttr.stNormalize.factor[2] = 0.f;
            stChnAttr.stNormalize.mean[0] = 0.f;
            stChnAttr.stNormalize.mean[1] = 0.f;
            stChnAttr.stNormalize.mean[2] = 0.f;
            stChnAttr.stNormalize.rounding = VPSS_ROUNDING_TO_EVEN;

            CVI_S32 ret = CVI_VPSS_SetChnAttr(VpssRgbGrp, VpssRgbChn, &stChnAttr);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_SetChnAttr failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }
        }

        // vpss enable chn
        {
            CVI_S32 ret = CVI_VPSS_EnableChn(VpssRgbGrp, VpssRgbChn);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_EnableChn failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }

            b_vpss_rgb_chn_enabled = 1;
        }

        // vpss attach vb pool
        {
            CVI_S32 ret = CVI_VPSS_AttachVbPool(VpssRgbGrp, VpssRgbChn, orientation > 4 ? VbPool2 : VbPool1);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_AttachVbPool failed %x\n", ret);
                ret_val = -1;
                goto OUT;
            }

            b_vpss_rgb_vbpool_attached = 1;
        }
    }

    // vdec start recv stream
    {
        CVI_S32 ret = CVI_VDEC_StartRecvStream(VdChn);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VDEC_StartRecvStream failed %x\n", ret);
            ret_val = -1;
            goto OUT;
        }

        b_vdec_recvstream_started = 1;
    }

    // vpss rotate start grp
    if (orientation > 4)
    {
        CVI_S32 ret = CVI_VPSS_StartGrp(VpssRotateGrp);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VPSS_StartGrp failed %x\n", ret);
            ret_val = -1;
            goto OUT;
        }

        b_vpss_rotate_grp_started = 1;
    }

    // vpss rgb start grp
    {
        CVI_S32 ret = CVI_VPSS_StartGrp(VpssRgbGrp);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VPSS_StartGrp failed %x\n", ret);
            ret_val = -1;
            goto OUT;
        }

        b_vpss_rgb_grp_started = 1;
    }

    // vdec send stream
    {
        VDEC_STREAM_S stStream;
        stStream.u32Len = jpgsize;
        stStream.u64PTS = 0;
        stStream.bEndOfFrame = CVI_TRUE;
        stStream.bEndOfStream = CVI_TRUE;
        stStream.bDisplay = 1;
        stStream.pu8Addr = (unsigned char*)jpgdata;

        CVI_S32 ret = CVI_VDEC_SendStream(VdChn, &stStream, -1);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VDEC_SendStream failed %x\n", ret);
            ret_val = -1;
            goto OUT;
        }
    }

    // vdec get frame
    {
        CVI_S32 ret = CVI_VDEC_GetFrame(VdChn, &stFrameInfo, -1);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VDEC_GetFrame failed %x\n", ret);
            ret_val = -1;
            goto OUT;
        }

        b_vdec_frame_got = 1;

        fprintf(stderr, "stFrameInfo %d   %d x %d  %d\n", stFrameInfo.u32PoolId, stFrameInfo.stVFrame.u32Width, stFrameInfo.stVFrame.u32Height, stFrameInfo.stVFrame.enPixelFormat);
    }

    // vpss rotate send frame
    if (orientation > 4)
    {
        CVI_S32 ret = CVI_VPSS_SendFrame(VpssRotateGrp, &stFrameInfo, -1);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VPSS_SendFrame rotate failed %x\n", ret);
            ret_val = -1;
            goto OUT;
        }
    }

    // vpss rotate get frame
    if (orientation > 4)
    {
        CVI_S32 ret = CVI_VPSS_GetChnFrame(VpssRotateGrp, VpssRotateChn, &stFrameInfo_rotated, 5000);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VPSS_GetChnFrame rotate failed %x\n", ret);
            ret_val = -1;
            goto OUT;
        }

        b_vpss_rotate_frame_got = 1;

        fprintf(stderr, "stFrameInfo_rotated %d   %d x %d  %d\n", stFrameInfo_rotated.u32PoolId, stFrameInfo_rotated.stVFrame.u32Width, stFrameInfo_rotated.stVFrame.u32Height, stFrameInfo_rotated.stVFrame.enPixelFormat);
    }

    // vpss rgb send frame
    {
        CVI_S32 ret = CVI_VPSS_SendFrame(VpssRgbGrp, orientation > 4 ? &stFrameInfo_rotated : &stFrameInfo, -1);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VPSS_SendFrame rgb failed %x\n", ret);
            ret_val = -1;
            goto OUT;
        }
    }

    // vpss rgb get frame
    {
        CVI_S32 ret = CVI_VPSS_GetChnFrame(VpssRgbGrp, VpssRgbChn, &stFrameInfo_rgb, 5000);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VPSS_GetChnFrame rgb failed %x\n", ret);
            ret_val = -1;
            goto OUT;
        }

        b_vpss_rgb_frame_got = 1;

        fprintf(stderr, "stFrameInfo_rgb %d   %d x %d  %d\n", stFrameInfo_rgb.u32PoolId, stFrameInfo_rgb.stVFrame.u32Width, stFrameInfo_rgb.stVFrame.u32Height, stFrameInfo_rgb.stVFrame.enPixelFormat);
    }

    if (stFrameInfo_rgb.stVFrame.enPixelFormat != rgb_pixel_format)
    {
        fprintf(stderr, "unsupported pixel format %d\n", stFrameInfo_rgb.stVFrame.enPixelFormat);
        ret_val = -1;
        goto OUT;
    }

    // memcpy RGB
    {
        CVI_U64 phyaddr = stFrameInfo_rgb.stVFrame.u64PhyAddr[0];
        // const unsigned char* rgb_ptr = stFrameInfo_rgb.stVFrame.pu8VirAddr[0];
        const int rgb_stride = stFrameInfo_rgb.stVFrame.u32Stride[0];
        const int rgb_length = stFrameInfo_rgb.stVFrame.u32Length[0];

        const int border_top = stFrameInfo_rgb.stVFrame.s16OffsetTop;
        // const int border_bottom = stFrameInfo_rgb.stVFrame.s16OffsetBottom;
        const int border_left = stFrameInfo_rgb.stVFrame.s16OffsetLeft;
        // const int border_right = stFrameInfo_rgb.stVFrame.s16OffsetRight;

        // fprintf(stderr, "border %d %d %d %d\n", border_top, border_bottom, border_left, border_right);

        void* mapped_ptr = CVI_SYS_MmapCache(phyaddr, rgb_length);
        CVI_SYS_IonInvalidateCache(phyaddr, mapped_ptr, rgb_length);

        const unsigned char* rgb_ptr = (const unsigned char*)mapped_ptr + border_top * rgb_stride + border_left;

        int h2 = height;
        int w2 = width * 3;
        if (rgb_stride == width * 3)
        {
            h2 = 1;
            w2 = height * width * 3;
        }

        for (int i = 0; i < h2; i++)
        {
#if __riscv_vector
            int j = 0;
            int n = w2;
            while (n > 0) {
                size_t vl = vsetvl_e8m8(n);
                vuint8m8_t rgb = vle8_v_u8m8(rgb_ptr + j, vl);
                vse8_v_u8m8(outrgb, rgb, vl);
                outrgb += vl;
                j += vl;
                n -= vl;
            }
#else
            memcpy(outrgb, rgb_ptr, w2);
            outrgb += w2;
#endif

            rgb_ptr += rgb_stride;
        }

        CVI_SYS_Munmap(mapped_ptr, rgb_length);
    }

OUT:

    // fprintf(stderr, "...\n");
    // getchar();
    // getchar();

    if (b_vpss_rgb_frame_got)
    {
        CVI_S32 ret = CVI_VPSS_ReleaseChnFrame(VpssRgbGrp, VpssRgbChn, &stFrameInfo_rgb);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VPSS_ReleaseChnFrame failed %x\n", ret);
            ret_val = -1;
        }
    }

    if (b_vpss_rotate_frame_got)
    {
        CVI_S32 ret = CVI_VPSS_ReleaseChnFrame(VpssRotateGrp, VpssRotateChn, &stFrameInfo_rotated);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VPSS_ReleaseChnFrame failed %x\n", ret);
            ret_val = -1;
        }
    }

    if (b_vdec_frame_got)
    {
        CVI_S32 ret = CVI_VDEC_ReleaseFrame(VdChn, &stFrameInfo);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VDEC_ReleaseFrame failed %x\n", ret);
            ret_val = -1;
        }
    }

    if (b_vpss_rgb_grp_started)
    {
        CVI_S32 ret = CVI_VPSS_StopGrp(VpssRgbGrp);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VPSS_StopGrp failed %x\n", ret);
            ret_val = -1;
        }
    }

    if (b_vpss_rotate_grp_started)
    {
        CVI_S32 ret = CVI_VPSS_StopGrp(VpssRotateGrp);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VPSS_StopGrp failed %x\n", ret);
            ret_val = -1;
        }
    }

    if (b_vdec_recvstream_started)
    {
        CVI_S32 ret = CVI_VDEC_StopRecvStream(VdChn);
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VDEC_StopRecvStream failed %x\n", ret);
            ret_val = -1;
        }
    }

    // vpss rgb exit
    {
        if (b_vpss_rgb_vbpool_attached)
        {
            CVI_S32 ret = CVI_VPSS_DetachVbPool(VpssRgbGrp, VpssRgbChn);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_DetachVbPool failed %x\n", ret);
                ret_val = -1;
            }
        }

        if (b_vpss_rgb_chn_enabled)
        {
            CVI_S32 ret = CVI_VPSS_DisableChn(VpssRgbGrp, VpssRgbChn);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_DisableChn failed %x\n", ret);
                ret_val = -1;
            }
        }

        if (b_vpss_rgb_grp_created)
        {
            CVI_S32 ret = CVI_VPSS_DestroyGrp(VpssRgbGrp);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_DestroyGrp failed %x\n", ret);
                ret_val = -1;
            }
        }
    }

    // vpss rotate exit
    {
        if (b_vpss_rotate_vbpool_attached)
        {
            CVI_S32 ret = CVI_VPSS_DetachVbPool(VpssRotateGrp, VpssRotateChn);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_DetachVbPool failed %x\n", ret);
                ret_val = -1;
            }
        }

        if (b_vpss_rotate_chn_enabled)
        {
            CVI_S32 ret = CVI_VPSS_DisableChn(VpssRotateGrp, VpssRotateChn);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_DisableChn failed %x\n", ret);
                ret_val = -1;
            }
        }

        if (b_vpss_rotate_grp_created)
        {
            CVI_S32 ret = CVI_VPSS_DestroyGrp(VpssRotateGrp);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VPSS_DestroyGrp failed %x\n", ret);
                ret_val = -1;
            }
        }
    }

    // vdec exit
    {
        if (b_vdec_vbpool_attached)
        {
            CVI_S32 ret = CVI_VDEC_DetachVbPool(VdChn);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VDEC_DetachVbPool failed %x\n", ret);
                ret_val = -1;
            }
        }

        if (b_vdec_chn_created)
        {
            CVI_S32 ret = CVI_VDEC_ResetChn(VdChn);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VDEC_DestroyChn failed %x\n", ret);
                ret_val = -1;
            }
        }

        if (b_vdec_chn_created)
        {
            CVI_S32 ret = CVI_VDEC_DestroyChn(VdChn);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VDEC_DestroyChn failed %x\n", ret);
                ret_val = -1;
            }
        }
    }

    // vb pool exit
    {
        if (b_vb_pool0_created)
        {
            CVI_S32 ret = CVI_VB_DestroyPool(VbPool0);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VB_DestroyPool failed %x\n", ret);
                ret_val = -1;
            }
        }

        if (b_vb_pool1_created)
        {
            CVI_S32 ret = CVI_VB_DestroyPool(VbPool1);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VB_DestroyPool failed %x\n", ret);
                ret_val = -1;
            }
        }

        if (b_vb_pool2_created)
        {
            CVI_S32 ret = CVI_VB_DestroyPool(VbPool2);
            if (ret != CVI_SUCCESS)
            {
                fprintf(stderr, "CVI_VB_DestroyPool failed %x\n", ret);
                ret_val = -1;
            }
        }
    }

    if (b_sys_inited)
    {
        CVI_S32 ret = CVI_SYS_Exit();
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_SYS_Exit failed %x\n", ret);
            ret_val = -1;
        }
    }

    if (b_vb_inited)
    {
        CVI_S32 ret = CVI_VB_Exit();
        if (ret != CVI_SUCCESS)
        {
            fprintf(stderr, "CVI_VB_Exit failed %x\n", ret);
            ret_val = -1;
        }
    }

    return ret_val;
}

int main(int argc, char** argv)
{
    // const char* path = argc >= 2 ? argv[1] : "in.jpg";

    // int orientation = argc == 3 ? atoi(argv[2]) : 1;

    const char* images[] =
    {
        "120x90.jpg",
        "160x120.jpg",
        "320x240.jpg",
        "400x300.jpg",
        "480x270.jpg",
        "640x480.jpg",
        "800x600.jpg",
        "800x800.jpg",
        "960x540.jpg",
        "1080x1080.jpg",
        "1280x1080.jpg",
        "1280x720.jpg",
        "1920x1080.jpg"
    };

    const int image_count = sizeof(images) / sizeof(const char*);

    for (int i = 0; i < image_count; i++)
    {
        const char* path = images[i];

        // read file
        std::vector<unsigned char> buf;
        {
            FILE* fp = fopen(path, "rb");
            if (!fp)
            {
                fprintf(stderr, "fopen failed %s\n", path);
                continue;
            }

            fseek(fp, 0, SEEK_END);
            int size = ftell(fp);
            rewind(fp);
            buf.resize(size);
            fread(buf.data(), 1, size, fp);
            fclose(fp);
            // fprintf(stderr, "size = %d\n", size);
        }

        for (int j = 1; j <= 8; j++)
        {
            const int orientation = j;

            CVI_VDEC_JpegDecoder jpg_decoder(buf.data(), buf.size(), orientation);

            int r0 = jpg_decoder.ping();
            if (r0 != 0)
                break;

            const int width = jpg_decoder.width;
            const int height = jpg_decoder.height;

            // fprintf(stderr, "%d x %d\n", width, height);
            // fprintf(stderr, "sampling_factor = %d\n", jpg_decoder.sampling_factor);

            std::vector<unsigned char> outrgb;
            outrgb.resize(width * height * 3);

            // double t0 = get_current_time();

            int ret = jpg_decoder.decode(outrgb.data(), 1);

            // double t1 = get_current_time();

            if (ret != 0)
            {
                fprintf(stderr, "decode failed %s %d\n", path, orientation);
                break;
            }

            // fprintf(stderr, "%.3f\n", t1-t0);

            // save to out image
            char outpath[256];
            snprintf(outpath, 256, "out/%s.%d.jpg", path, orientation);
            stbi_write_jpg(outpath, width, height, 3, outrgb.data(), 90);
        }
    }

    return 0;
}
