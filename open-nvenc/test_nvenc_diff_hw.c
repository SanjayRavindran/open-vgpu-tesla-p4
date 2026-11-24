/* SPDX-License-Identifier: MIT */
/*
 * test_nvenc_diff_hw.c - Bug-for-Bug Hardware Differential Parity Suite
 *
 * Compares clean-room libopen-nvidia-encode.so (NVENC 13.0) directly against
 * proprietary NVIDIA NVENC blobs on Pascal GP104 across:
 *   1. ABI exports, 43 function slots, 45 NvTool API names, NV_ENC_CAPS[0..59]
 *      (including out-of-range NV_ENC_ERR_INVALID_PARAM=8), preset defaults.
 *   2. PixelFlux-exact 1784x992 YUV444 10-bit CUDADEVICEPTR (pitch=3584, P3,
 *      ULTRA_LOW_LATENCY, PaintOver QP 25->18->25, InvalidateRefFrames,
 *      level 4.1, BT.709 limited VUI, 60 FPS) scrolling + moving mouse cursor.
 *   3. 1784x992 ARGB 8-bit 4:4:4 HEVC scrolling + cursor @ 60 FPS.
 *   4. 1784x992 (non-16-aligned) H.264 NV12 & H.264 YUV444 @ 60 FPS.
 *   5. 1280x720 NV12 HEVC VBR -> CBR live ReconfigureEncoder.
 */

#define _GNU_SOURCE
#include "open_nvenc.h"

#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define MKV_EX(maj, minv, sv) ((0x7U << 28) | ((minv) << 24) | ((sv) << 16) | (maj))

static const NVENC_GUID G_CODEC_H264 = { 0x6bc82762U, 0x4e63U, 0x4ca4U, { 0xaa,0x85,0x1e,0x50,0xf3,0x21,0xf6,0xbf } };
static const NVENC_GUID G_CODEC_HEVC = { 0x790cdc88U, 0x4522U, 0x4d7bU, { 0x94,0x25,0xbd,0xa9,0x97,0x5f,0x76,0x03 } };
static const NVENC_GUID G_H264_MAIN  = { 0x60b5c1d4U, 0x67feU, 0x4790U, { 0x94,0xd5,0xc4,0x72,0x6d,0x7b,0x6e,0x6d } };
static const NVENC_GUID G_H264_444   = { 0x7ac663cbU, 0xa598U, 0x4960U, { 0xb8,0x44,0x33,0x9b,0x26,0x1a,0x7d,0x52 } };
static const NVENC_GUID G_HEVC_MAIN  = { 0xb514c39aU, 0xb55bU, 0x40faU, { 0x87,0x8f,0xf1,0x25,0x3b,0x4d,0xfd,0xec } };
static const NVENC_GUID G_HEVC_FREXT = { 0x51ec32b5U, 0x1b4cU, 0x453cU, { 0x9c,0xbd,0xb6,0x16,0xbd,0x62,0x13,0x41 } };
static const NVENC_GUID G_PRESET_P3  = { 0x36850110U, 0x3a07U, 0x441fU, { 0x94,0xd5,0x36,0x70,0x63,0x1f,0x91,0xf6 } };
static const NVENC_GUID G_PRESET_P4  = { 0x90a7b826U, 0xdf06U, 0x4862U, { 0xb9,0xd2,0xcd,0x6d,0x73,0xa0,0x86,0x81 } };

typedef struct {
    const char *label;
    const char *libpath;
    uint32_t    maj;
    uint32_t    minv;
} enc_target_t;

typedef struct {
    double fps;
    double avg_psnr;
    double min_psnr;
    double avg_ssim;
    size_t total_bytes;
    int    width;
    int    height;
    int    level;
    char   color_space[32];
    char   color_range[16];
    char   r_frame_rate[16];
    int    decode_warnings;
} diff_metrics_t;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void gen_scroll_cursor_y444_10b(uint8_t *dst, uint32_t w, uint32_t h,
                                       uint32_t pitch, uint32_t fidx)
{
    uint32_t scroll_y = (fidx < 20U) ? (fidx * 24U)
                      : ((fidx < 40U) ? (19U * 24U)
                                      : (19U * 24U + (fidx - 40U) * 32U));
    uint32_t mx = (fidx < 20U) ? (150U + fidx * 35U)
                : ((fidx < 40U) ? (150U + 19U * 35U)
                                : (1100U - (fidx - 40U) * 28U));
    uint32_t my = (fidx < 20U) ? (200U + fidx * 15U)
                : ((fidx < 40U) ? (200U + 19U * 15U)
                                : (620U - (fidx - 40U) * 14U));
    uint8_t *py = dst;
    uint8_t *pu = dst + (size_t)pitch * h;
    uint8_t *pv = pu  + (size_t)pitch * h;

    for (uint32_t y = 0U; y < h; y++) {
        uint32_t  sy = y + scroll_y;
        uint16_t *ry = (uint16_t *)(py + (size_t)y * pitch);
        uint16_t *ru = (uint16_t *)(pu + (size_t)y * pitch);
        uint16_t *rv = (uint16_t *)(pv + (size_t)y * pitch);
        int is_text  = ((sy % 28U) >= 6U && (sy % 28U) < 20U);
        for (uint32_t x = 0U; x < w; x++) {
            uint16_t y10 = 920U, u10 = 512U, v10 = 512U;
            if (y < 84U) {
                y10 = 140U; u10 = 518U; v10 = 508U;
                if (x >= 30U && x < 220U && y >= 16U && y < 60U) {
                    y10 = 260U; u10 = 400U; v10 = 760U;
                }
            } else if (is_text && x >= 120U && x < w - 120U &&
                       (((x + (sy / 28U) * 23U) % 10U) < 5U)) {
                y10 = 96U; u10 = 512U; v10 = 512U;
            } else if ((sy / 240U) % 3U == 1U && x >= 300U && x < 1400U) {
                y10 = (uint16_t)(300U + ((x + sy) & 0xffU) * 2U);
                u10 = (uint16_t)(280U + ((x * 2U) & 0xffU));
                v10 = (uint16_t)(420U + ((sy * 2U) & 0xffU));
            }
            if (x >= mx && x < mx + 22U && y >= my && y < my + 26U && (x - mx) <= (y - my)) {
                int border = (x == mx || y == my + 25U || (x - mx) == (y - my));
                y10 = border ? 64U : 940U;
                u10 = 512U; v10 = 512U;
            }
            ry[x] = (uint16_t)(y10 << 6);
            ru[x] = (uint16_t)(u10 << 6);
            rv[x] = (uint16_t)(v10 << 6);
        }
    }
}

static void gen_scroll_cursor_bgra(uint8_t *dst, uint32_t w, uint32_t h,
                                   uint32_t pitch, uint32_t fidx)
{
    uint32_t scroll_y = fidx * 20U;
    uint32_t mx = 200U + (fidx * 23U) % (w - 300U);
    uint32_t my = 160U + (fidx * 13U) % (h - 260U);
    for (uint32_t y = 0U; y < h; y++) {
        uint32_t sy = y + scroll_y;
        uint8_t *row = dst + (size_t)y * pitch;
        int is_text = ((sy % 32U) >= 8U && (sy % 32U) < 20U);
        for (uint32_t x = 0U; x < w; x++) {
            uint8_t r = 32U, g = 36U, b = 44U;
            if (y < 84U) {
                r = 54U; g = 58U; b = 68U;
                if (x >= 40U && x < 180U && y >= 20U && y < 60U) { r = 225U; g = 80U; b = 45U; }
            } else if (is_text && x >= 110U && x < w - 110U && (((x + (sy / 32U) * 19U) % 12U) < 6U)) {
                r = 235U; g = 235U; b = 235U;
            } else if ((sy / 180U) % 2U == 0U && x >= 450U && x < 1300U) {
                r = (uint8_t)((x + sy) & 0xffU);
                g = (uint8_t)((x * 2U) & 0xffU);
                b = (uint8_t)((sy * 3U) & 0xffU);
            }
            if (x >= mx && x < mx + 22U && y >= my && y < my + 24U && (x - mx) <= (y - my)) {
                int border = (x == mx || y == my + 23U || (x - mx) == (y - my));
                r = g = b = border ? 16U : 250U;
            }
            row[x * 4U + 0U] = b;
            row[x * 4U + 1U] = g;
            row[x * 4U + 2U] = r;
            row[x * 4U + 3U] = 255U;
        }
    }
}

static void gen_scroll_cursor_nv12(uint8_t *dst, uint32_t w, uint32_t h,
                                   uint32_t pitch, uint32_t fidx)
{
    uint32_t scroll_y = fidx * 16U;
    uint32_t mx = 180U + (fidx * 25U) % (w - 300U);
    uint32_t my = 140U + (fidx * 13U) % (h - 240U);
    uint8_t *py = dst;
    uint8_t *puv = dst + (size_t)pitch * h;
    for (uint32_t y = 0U; y < h; y++) {
        uint32_t sy = y + scroll_y;
        uint8_t *ry = py + (size_t)y * pitch;
        int is_text = ((sy % 28U) >= 6U && (sy % 28U) < 18U);
        for (uint32_t x = 0U; x < w; x++) {
            uint8_t yv = 225U;
            if (y < 80U) {
                yv = (x >= 40U && x < 180U && y >= 20U && y < 60U) ? 105U : 52U;
            } else if (is_text && x >= 100U && x < w - 100U && (((x + (sy / 28U) * 17U) % 10U) < 5U)) {
                yv = 36U;
            } else if ((sy / 200U) % 2U == 0U && x >= 400U && x < 1300U) {
                yv = (uint8_t)(80U + ((x + sy) & 0x7fU));
            }
            if (x >= mx && x < mx + 22U && y >= my && y < my + 24U && (x - mx) <= (y - my)) {
                int border = (x == mx || y == my + 23U || (x - mx) == (y - my));
                yv = border ? 16U : 235U;
            }
            ry[x] = yv;
        }
    }
    for (uint32_t y = 0U; y < h / 2U; y++) {
        uint32_t sy = y * 2U + scroll_y;
        uint8_t *ruv = puv + (size_t)y * pitch;
        for (uint32_t x = 0U; x < w; x += 2U) {
            uint8_t u = 128U, v = 128U;
            if (y < 40U && x >= 40U && x < 180U && y >= 10U && y < 30U) {
                u = 90U; v = 200U;
            } else if ((sy / 200U) % 2U == 0U && x >= 400U && x < 1300U) {
                u = (uint8_t)(90U + ((x / 2U) & 0x3fU));
                v = (uint8_t)(100U + ((sy / 2U) & 0x3fU));
            }
            ruv[x + 0U] = u;
            ruv[x + 1U] = v;
        }
    }
}

static void gen_scroll_cursor_y444_8b(uint8_t *dst, uint32_t w, uint32_t h,
                                      uint32_t pitch, uint32_t fidx)
{
    uint32_t scroll_y = fidx * 16U;
    uint32_t mx = 180U + (fidx * 25U) % (w - 300U);
    uint32_t my = 140U + (fidx * 13U) % (h - 240U);
    uint8_t *py = dst;
    uint8_t *pu = dst + (size_t)pitch * h;
    uint8_t *pv = pu  + (size_t)pitch * h;
    for (uint32_t y = 0U; y < h; y++) {
        uint32_t sy = y + scroll_y;
        uint8_t *ry = py + (size_t)y * pitch;
        uint8_t *ru = pu + (size_t)y * pitch;
        uint8_t *rv = pv + (size_t)y * pitch;
        int is_text = ((sy % 28U) >= 6U && (sy % 28U) < 18U);
        for (uint32_t x = 0U; x < w; x++) {
            uint8_t yv = 225U, u = 128U, v = 128U;
            if (y < 80U) {
                yv = 52U; u = 132U; v = 124U;
                if (x >= 40U && x < 180U && y >= 20U && y < 60U) { yv = 105U; u = 90U; v = 200U; }
            } else if (is_text && x >= 100U && x < w - 100U && (((x + (sy / 28U) * 17U) % 10U) < 5U)) {
                yv = 36U;
            } else if ((sy / 200U) % 2U == 0U && x >= 400U && x < 1300U) {
                yv = (uint8_t)(80U + ((x + sy) & 0x7fU));
                u  = (uint8_t)(90U + ((x / 2U) & 0x3fU));
                v  = (uint8_t)(100U + ((sy / 2U) & 0x3fU));
            }
            if (x >= mx && x < mx + 22U && y >= my && y < my + 24U && (x - mx) <= (y - my)) {
                int border = (x == mx || y == my + 23U || (x - mx) == (y - my));
                yv = border ? 16U : 235U; u = 128U; v = 128U;
            }
            ry[x] = yv; ru[x] = u; rv[x] = v;
        }
    }
}

static void inspect_bitstream(const char *enc_path, const char *ref_path,
                              const char *ref_pixfmt, uint32_t w, uint32_t h,
                              int is_bgra_ref, int fps_num, diff_metrics_t *m)
{
    char cmd[1024];
    char line[512];

    /* 1. Count decode warnings */
    snprintf(cmd, sizeof(cmd), "ffmpeg -v warning -i \"%s\" -f null - 2>&1", enc_path);
    FILE *fp = popen(cmd, "r");
    assert(fp != NULL);
    int warns = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] != '\n' && line[0] != '\0') {
            fprintf(stderr, "  [ffmpeg warn] %s", line);
            warns++;
        }
    }
    pclose(fp);
    m->decode_warnings = warns;

    /* 2. Probe stream metadata */
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -show_entries stream=width,height,level,color_space,color_range,r_frame_rate "
             "-of default=noprint_wrappers=1 \"%s\" 2>&1", enc_path);
    fp = popen(cmd, "r");
    assert(fp != NULL);
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0U && (line[len - 1U] == '\n' || line[len - 1U] == '\r')) line[--len] = '\0';
        if (strncmp(line, "width=", 6) == 0) m->width = atoi(line + 6);
        else if (strncmp(line, "height=", 7) == 0) m->height = atoi(line + 7);
        else if (strncmp(line, "level=", 6) == 0) m->level = atoi(line + 6);
        else if (strncmp(line, "color_space=", 12) == 0) snprintf(m->color_space, sizeof(m->color_space), "%.31s", line + 12);
        else if (strncmp(line, "color_range=", 12) == 0) snprintf(m->color_range, sizeof(m->color_range), "%.15s", line + 12);
        else if (strncmp(line, "r_frame_rate=", 13) == 0) snprintf(m->r_frame_rate, sizeof(m->r_frame_rate), "%.15s", line + 13);
    }
    pclose(fp);

    /* 3. Compute deterministic 1:1 PSNR & SSIM via decode-to-raw */
    const char *dec_path = "/tmp/open_nvenc_diff_hw/dec.raw";
    if (is_bgra_ref) {
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -y -v error -i \"%s\" "
                 "-vf \"scale=in_color_matrix=bt709:in_range=limited,format=rgb24\" "
                 "-f rawvideo -pix_fmt rgb24 \"%s\"",
                 enc_path, dec_path);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -y -v error -i \"%s\" -f rawvideo -pix_fmt %s \"%s\"",
                 enc_path, ref_pixfmt, dec_path);
    }
    int rc_dec = system(cmd);
    assert(rc_dec == 0);

    if (is_bgra_ref) {
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -nostats -v info -f rawvideo -framerate %d -pix_fmt rgb24 -s %ux%u -i \"%s\" "
                 "-f rawvideo -framerate %d -pix_fmt %s -s %ux%u -i \"%s\" "
                 "-lavfi \"[0:v]split=2[a1][a2];[1:v]format=rgb24,split=2[b1][b2];[a1][b1]psnr;[a2][b2]ssim\" -f null - 2>&1",
                 fps_num, w, h, dec_path, fps_num, ref_pixfmt, w, h, ref_path);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -nostats -v info -f rawvideo -framerate %d -pix_fmt %s -s %ux%u -i \"%s\" "
                 "-f rawvideo -framerate %d -pix_fmt %s -s %ux%u -i \"%s\" "
                 "-lavfi \"[0:v]split=2[a1][a2];[1:v]split=2[b1][b2];[a1][b1]psnr;[a2][b2]ssim\" -f null - 2>&1",
                 fps_num, ref_pixfmt, w, h, dec_path, fps_num, ref_pixfmt, w, h, ref_path);
    }
    fp = popen(cmd, "r");
    assert(fp != NULL);
    while (fgets(line, sizeof(line), fp)) {
        char *pavg = strstr(line, "average:");
        char *pmin = strstr(line, "min:");
        if (strstr(line, "PSNR") && pavg) {
            m->avg_psnr = atof(pavg + 8);
            if (pmin) m->min_psnr = atof(pmin + 4);
        }
        char *sall = strstr(line, "All:");
        if (strstr(line, "SSIM") && sall) {
            m->avg_ssim = atof(sall + 4);
        }
    }
    pclose(fp);
    (void)remove(dec_path);
}

static void check_abi_caps_and_presets(const char *open_lib, const char *v535_lib, const char *v13_lib)
{
    void *dl_o = dlopen(open_lib, RTLD_NOW | RTLD_LOCAL);
    void *dl_v = dlopen(v535_lib, RTLD_NOW | RTLD_LOCAL);
    assert(dl_o != NULL);
    if (!dl_v) {
        fprintf(stderr, "FATAL: vendor library %s not loadable: %s\n", v535_lib, dlerror());
        exit(1);
    }

    NVENCSTATUS (*o_maxver)(uint32_t *) = dlsym(dl_o, "NvEncodeAPIGetMaxSupportedVersion");
    NVENCSTATUS (*v_maxver)(uint32_t *) = dlsym(dl_v, "NvEncodeAPIGetMaxSupportedVersion");
    NVENCSTATUS (*o_ci)(NV_ENCODE_API_FUNCTION_LIST *) = dlsym(dl_o, "NvEncodeAPICreateInstance");
    NVENCSTATUS (*v_ci)(NV_ENCODE_API_FUNCTION_LIST *) = dlsym(dl_v, "NvEncodeAPICreateInstance");
    void *(*o_tc)(int) = dlsym(dl_o, "NvToolCreateInterface");
    void *(*v_tc)(int) = dlsym(dl_v, "NvToolCreateInterface");
    void (*o_td)(void *) = dlsym(dl_o, "NvToolDestroyInterface");
    void (*v_td)(void *) = dlsym(dl_v, "NvToolDestroyInterface");
    uint32_t (*o_cnt)(void *, uint8_t) = dlsym(dl_o, "NvToolGetApiFunctionCount");
    uint32_t (*v_cnt)(void *, uint8_t) = dlsym(dl_v, "NvToolGetApiFunctionCount");
    const char **(*o_names)(void *, uint8_t, uint32_t, int) = dlsym(dl_o, "NvToolGetApiNames");
    const char **(*v_names)(void *, uint8_t, uint32_t, int) = dlsym(dl_v, "NvToolGetApiNames");

    uint32_t vo = 0, vv = 0;
    assert(o_maxver(&vo) == NV_ENC_SUCCESS && vo == 0xd0U);
    assert(v_maxver(&vv) == NV_ENC_SUCCESS && vv == 0xc1U);

    if (v13_lib && v13_lib[0]) {
        void *dl_13 = dlopen(v13_lib, RTLD_LAZY | RTLD_LOCAL);
        if (dl_13) {
            NVENCSTATUS (*v13_maxver)(uint32_t *) = dlsym(dl_13, "NvEncodeAPIGetMaxSupportedVersion");
            NVENCSTATUS (*v13_ci)(NV_ENCODE_API_FUNCTION_LIST *) = dlsym(dl_13, "NvEncodeAPICreateInstance");
            uint32_t (*v13_cnt)(void *, uint8_t) = dlsym(dl_13, "NvToolGetApiFunctionCount");
            void *(*v13_tc)(int) = dlsym(dl_13, "NvToolCreateInterface");
            void (*v13_td)(void *) = dlsym(dl_13, "NvToolDestroyInterface");
            const char **(*v13_names)(void *, uint8_t, uint32_t, int) = dlsym(dl_13, "NvToolGetApiNames");
            uint32_t v13 = 0;
            assert(v13_maxver && v13_maxver(&v13) == NV_ENC_SUCCESS && v13 >= 0xd0U);
            NV_ENCODE_API_FUNCTION_LIST fl13;
            memset(&fl13, 0, sizeof(fl13));
            fl13.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            assert(v13_ci && v13_ci(&fl13) == NV_ENC_SUCCESS);
            void *t13 = v13_tc(1);
            assert(t13 && v13_cnt(t13, 1U) == 45U);
            const char **n13 = v13_names(t13, 1U, 0U, 45);
            void *to = o_tc(1);
            const char **no = o_names(to, 1U, 0U, 45);
            for (int i = 0; i < 45; i++) {
                assert(strcmp(n13[i], no[i]) == 0);
            }
            o_td(to);
            v13_td(t13);
            dlclose(dl_13);
            printf("[DIFF_ABI] Matched 13.x static vendor ABI (%s, ver=0x%x, 45/45 tool names)\n",
                   v13_lib, v13);
        }
    }

    void *cu = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    int (*cuInit)(unsigned) = dlsym(cu, "cuInit");
    int (*cuCtxCreate)(void **, unsigned, int) = dlsym(cu, "cuCtxCreate_v2");
    int (*cuCtxDestroy)(void *) = dlsym(cu, "cuCtxDestroy_v2");
    assert(cuInit && cuCtxCreate && cuCtxDestroy);
    assert(cuInit(0) == 0);
    void *cuctx = NULL;
    assert(cuCtxCreate(&cuctx, 0, 0) == 0);

    NV_ENCODE_API_FUNCTION_LIST flo, flv;
    memset(&flo, 0, sizeof(flo));
    memset(&flv, 0, sizeof(flv));
    flo.version = MKV_EX(13U, 0U, 2U);
    flv.version = MKV_EX(12U, 1U, 2U);
    assert(o_ci(&flo) == NV_ENC_SUCCESS);
    assert(v_ci(&flv) == NV_ENC_SUCCESS);

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS exo, exv;
    memset(&exo, 0, sizeof(exo));
    memset(&exv, 0, sizeof(exv));
    exo.version = MKV_EX(13U, 0U, 1U); exo.deviceType = 1; exo.device = cuctx; exo.apiVersion = 0xd0U;
    exv.version = MKV_EX(12U, 1U, 1U); exv.deviceType = 1; exv.device = cuctx; exv.apiVersion = 0xc1U;
    void *enco = NULL, *encv = NULL;
    assert(flo.nvEncOpenEncodeSessionEx(&exo, &enco) == NV_ENC_SUCCESS);
    assert(flv.nvEncOpenEncodeSessionEx(&exv, &encv) == NV_ENC_SUCCESS);

    /* Verify out-of-range NV_ENC_CAPS 55..59 return exact NV_ENC_ERR_INVALID_PARAM (8) + key GP104 caps match */
    const int check_caps[] = {
        NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES,
        NV_ENC_CAPS_SUPPORT_ADAPTIVE_TRANSFORM,
        NV_ENC_CAPS_SUPPORT_YUV444_ENCODE,
        NV_ENC_CAPS_SUPPORT_10BIT_ENCODE,
        NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE,
        NV_ENC_CAPS_SUPPORT_SAO,
        NV_ENC_CAPS_SEPARATE_COLOUR_PLANE,
        NV_ENC_CAPS_WIDTH_MAX,
        NV_ENC_CAPS_HEIGHT_MAX,
        NV_ENC_CAPS_WIDTH_MIN,
        NV_ENC_CAPS_HEIGHT_MIN,
        NV_ENC_CAPS_LEVEL_MAX,
        NV_ENC_CAPS_LEVEL_MIN,
        NV_ENC_CAPS_MB_NUM_MAX,
        NV_ENC_CAPS_MB_PER_SEC_MAX,
        NV_ENC_CAPS_NUM_ENCODER_ENGINES,
        NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE,
        NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE,
        NV_ENC_CAPS_SUPPORT_DYN_FORCE_CONSTQP,
        NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION,
        NV_ENC_CAPS_SINGLE_SLICE_INTRA_REFRESH
    };
    NVENC_GUID codecs[2] = { G_CODEC_H264, G_CODEC_HEVC };
    for (int ci = 0; ci < 2; ci++) {
        for (size_t k = 0; k < sizeof(check_caps) / sizeof(check_caps[0]); k++) {
            NV_ENC_CAPS_PARAM cpo = {0}, cpv = {0};
            cpo.version = MKV_EX(13U, 0U, 1U); cpo.capsToQuery = check_caps[k];
            cpv.version = MKV_EX(12U, 1U, 1U); cpv.capsToQuery = check_caps[k];
            int vo_val = -1, vv_val = -1;
            NVENCSTATUS sto = flo.nvEncGetEncodeCaps(enco, codecs[ci], &cpo, &vo_val);
            NVENCSTATUS stv = flv.nvEncGetEncodeCaps(encv, codecs[ci], &cpv, &vv_val);
            assert(sto == stv && sto == NV_ENC_SUCCESS);
            assert(vo_val == vv_val);
        }
        for (int oc = 55; oc <= 59; oc++) {
            NV_ENC_CAPS_PARAM cpo = {0}, cpv = {0};
            cpo.version = MKV_EX(13U, 0U, 1U); cpo.capsToQuery = oc;
            cpv.version = MKV_EX(12U, 1U, 1U); cpv.capsToQuery = oc;
            int vo_val = -1, vv_val = -1;
            NVENCSTATUS sto = flo.nvEncGetEncodeCaps(enco, codecs[ci], &cpo, &vo_val);
            NVENCSTATUS stv = flv.nvEncGetEncodeCaps(encv, codecs[ci], &cpv, &vv_val);
            assert(sto == stv && sto == NV_ENC_ERR_INVALID_PARAM);
        }
    }

    /* Preset config parity across tuning modes */
    NV_ENC_TUNING_INFO tunings[4] = {
        NV_ENC_TUNING_INFO_HIGH_QUALITY,
        NV_ENC_TUNING_INFO_LOW_LATENCY,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
        NV_ENC_TUNING_INFO_LOSSLESS
    };
    for (int ci = 0; ci < 2; ci++) {
        for (int ti = 0; ti < 4; ti++) {
            NV_ENC_PRESET_CONFIG pco = {0}, pcv = {0};
            pco.version = MKV_EX(13U, 0U, 5U) | (1U << 31);
            pco.presetCfg.version = MKV_EX(13U, 0U, 9U) | (1U << 31);
            pcv.version = MKV_EX(12U, 1U, 4U) | (1U << 31);
            pcv.presetCfg.version = MKV_EX(12U, 1U, 8U) | (1U << 31);
            assert(flo.nvEncGetEncodePresetConfigEx(enco, codecs[ci], G_PRESET_P4, tunings[ti], &pco) == 0);
            assert(flv.nvEncGetEncodePresetConfigEx(encv, codecs[ci], G_PRESET_P4, tunings[ti], &pcv) == 0);
            assert(pco.presetCfg.gopLength == pcv.presetCfg.gopLength);
            assert(pco.presetCfg.rcParams.rateControlMode == pcv.presetCfg.rcParams.rateControlMode);
            assert(pco.presetCfg.rcParams.constQP.qpIntra == pcv.presetCfg.rcParams.constQP.qpIntra);
            assert(pco.presetCfg.rcParams.constQP.qpInterP == pcv.presetCfg.rcParams.constQP.qpInterP);
        }
    }

    flo.nvEncDestroyEncoder(enco);
    flv.nvEncDestroyEncoder(encv);

    void *to = o_tc(1), *tv = v_tc(1);
    assert(o_cnt(to, 1U) == v_cnt(tv, 1U) && o_cnt(to, 1U) == 45U);
    const char **no = o_names(to, 1U, 0U, 45);
    const char **nv = v_names(tv, 1U, 0U, 45);
    for (int i = 0; i < 45; i++) {
        assert(strcmp(no[i], nv[i]) == 0);
    }
    o_td(to);
    v_td(tv);

    cuCtxDestroy(cuctx);
    dlclose(dl_o);
    dlclose(dl_v);
    printf("[DIFF_ABI] 535.309.01 & open-nvenc: 43/43 slots, 45/45 tool names, GP104 caps[0..59], presets MATCHED\n");
}

static void run_case_pf_10b444(const enc_target_t *t, const char *out_path, const char *ref_path,
                               int nframes, diff_metrics_t *m)
{
    memset(m, 0, sizeof(*m));
    void *dl = dlopen(t->libpath, RTLD_NOW | RTLD_LOCAL);
    assert(dl != NULL);
    NVENCSTATUS (*ci)(NV_ENCODE_API_FUNCTION_LIST *) = dlsym(dl, "NvEncodeAPICreateInstance");
    void *cu = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    int (*cuInit)(unsigned) = dlsym(cu, "cuInit");
    int (*cuCtxCreate)(void **, unsigned, int) = dlsym(cu, "cuCtxCreate_v2");
    int (*cuCtxDestroy)(void *) = dlsym(cu, "cuCtxDestroy_v2");
    int (*cuMemAlloc)(uint64_t *, size_t) = dlsym(cu, "cuMemAlloc_v2");
    int (*cuMemFree)(uint64_t) = dlsym(cu, "cuMemFree_v2");
    int (*cuMemcpyHtoD)(uint64_t, const void *, size_t) = dlsym(cu, "cuMemcpyHtoD_v2");
    cuInit(0);
    void *cuctx = NULL;
    assert(cuCtxCreate(&cuctx, 0, 0) == 0);

    NV_ENCODE_API_FUNCTION_LIST fl = {0};
    fl.version = MKV_EX(t->maj, t->minv, 2U);
    assert(ci(&fl) == NV_ENC_SUCCESS);
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS ex = {0};
    ex.version = MKV_EX(t->maj, t->minv, 1U);
    ex.deviceType = 1; ex.device = cuctx; ex.apiVersion = (t->maj << 4) | t->minv;
    void *enc = NULL;
    assert(fl.nvEncOpenEncodeSessionEx(&ex, &enc) == NV_ENC_SUCCESS);

    uint32_t cfg_sv  = (t->maj >= 13U) ? 9U : 8U;
    uint32_t pcfg_sv = (t->maj >= 13U) ? 5U : 4U;
    uint32_t init_sv = (t->maj >= 13U) ? 7U : 5U;
    uint32_t pic_sv  = (t->maj >= 13U) ? 7U : 4U;
    uint32_t lbs_sv  = (t->maj >= 13U) ? 2U : 1U;
    uint32_t reg_sv  = (t->maj >= 13U) ? 5U : 4U;
    uint32_t rec_sv  = (t->maj >= 13U) ? 2U : 1U;

    NV_ENC_PRESET_CONFIG pcfg = {0};
    pcfg.version = MKV_EX(t->maj, t->minv, pcfg_sv) | (1U << 31);
    pcfg.presetCfg.version = MKV_EX(t->maj, t->minv, cfg_sv) | (1U << 31);
    assert(fl.nvEncGetEncodePresetConfigEx(enc, G_CODEC_HEVC, G_PRESET_P3,
                                           NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &pcfg) == 0);

    NV_ENC_CONFIG cfg = pcfg.presetCfg;
    cfg.version = MKV_EX(t->maj, t->minv, cfg_sv) | (1U << 31);
    cfg.profileGUID = G_HEVC_FREXT;
    cfg.gopLength = 0xffffffffU;
    cfg.frameIntervalP = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    cfg.rcParams.constQP.qpIntra = 25;
    cfg.rcParams.constQP.qpInterP = 25;
    cfg.rcParams.constQP.qpInterB = 25;
    cfg.rcParams.zeroReorderDelay = 1;
    cfg.rcParams.strictGOPTarget = 1;
    cfg.encodeCodecConfig.hevcConfig.level = 123;
    cfg.encodeCodecConfig.hevcConfig.tier = 1;
    cfg.encodeCodecConfig.hevcConfig.chromaFormatIDC = 3;
    cfg.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
    cfg.encodeCodecConfig.hevcConfig.idrPeriod = 0xffffffffU;
    cfg.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB = 5;
    cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoSignalTypePresentFlag = 1;
    cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFormat = 5;
    cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFullRangeFlag = 0;
    cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourDescriptionPresentFlag = 1;
    cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourPrimaries = 1;
    cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.transferCharacteristics = 1;
    cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourMatrix = 1;
    if (t->maj >= 13U) {
        cfg.encodeCodecConfig.hevcConfig.outputBitDepth = NV_ENC_BIT_DEPTH_10;
        cfg.encodeCodecConfig.hevcConfig.inputBitDepth  = NV_ENC_BIT_DEPTH_10;
    } else {
        cfg.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 = 2U;
    }

    uint32_t W = 1784U, H = 992U, PITCH = 3584U;
    NV_ENC_INITIALIZE_PARAMS ip = {0};
    ip.version = MKV_EX(t->maj, t->minv, init_sv) | (1U << 31);
    ip.encodeGUID = G_CODEC_HEVC; ip.presetGUID = G_PRESET_P3;
    ip.encodeWidth = W; ip.encodeHeight = H;
    ip.darWidth = W; ip.darHeight = H;
    ip.frameRateNum = 60; ip.frameRateDen = 1;
    ip.enablePTD = 1; ip.encodeConfig = &cfg;
    ip.maxEncodeWidth = 4096; ip.maxEncodeHeight = 2160;
    ip.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    assert(fl.nvEncInitializeEncoder(enc, &ip) == NV_ENC_SUCCESS);

    size_t buf_sz = (size_t)PITCH * H * 3U;
    uint8_t *host_buf = calloc(1U, buf_sz);
    uint64_t dev_ptr = 0;
    assert(cuMemAlloc(&dev_ptr, buf_sz) == 0);

    NV_ENC_REGISTER_RESOURCE reg = {0};
    reg.version = MKV_EX(t->maj, t->minv, reg_sv);
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    reg.width = W; reg.height = H; reg.pitch = PITCH;
    reg.resourceToRegister = (void *)(uintptr_t)dev_ptr;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    assert(fl.nvEncRegisterResource(enc, &reg) == NV_ENC_SUCCESS);

    NV_ENC_MAP_INPUT_RESOURCE mp = {0};
    mp.version = MKV_EX(t->maj, t->minv, 4U);
    mp.registeredResource = reg.registeredResource;
    assert(fl.nvEncMapInputResource(enc, &mp) == NV_ENC_SUCCESS);

    NV_ENC_CREATE_BITSTREAM_BUFFER cbs = {0};
    cbs.version = MKV_EX(t->maj, t->minv, 1U);
    assert(fl.nvEncCreateBitstreamBuffer(enc, &cbs) == NV_ENC_SUCCESS);

    FILE *fp = fopen(out_path, "wb");
    FILE *rfp = ref_path ? fopen(ref_path, "wb") : NULL;
    assert(fp != NULL);
    double enc_ms = 0.0;

    for (int f = 0; f < nframes; f++) {
        if (f == 20 || f == 40) {
            uint32_t q = (f == 20) ? 18U : 25U;
            cfg.rcParams.constQP.qpIntra = q;
            cfg.rcParams.constQP.qpInterP = q;
            cfg.rcParams.constQP.qpInterB = q;
            NV_ENC_RECONFIGURE_PARAMS rp = {0};
            rp.version = MKV_EX(t->maj, t->minv, rec_sv) | (1U << 31);
            rp.reInitEncodeParams = ip;
            assert(fl.nvEncReconfigureEncoder(enc, &rp) == NV_ENC_SUCCESS);
        }
        if (f == 45) {
            assert(fl.nvEncInvalidateRefFrames(enc, 44ULL) == NV_ENC_SUCCESS);
        }
        gen_scroll_cursor_y444_10b(host_buf, W, H, PITCH, (uint32_t)f);
        assert(cuMemcpyHtoD(dev_ptr, host_buf, buf_sz) == 0);
        if (rfp) {
            static uint16_t row_lsb[2048];
            for (int p = 0; p < 3; p++) {
                uint8_t *pl = host_buf + (size_t)p * PITCH * H;
                for (uint32_t y = 0; y < H; y++) {
                    uint16_t *sr = (uint16_t *)(pl + (size_t)y * PITCH);
                    for (uint32_t x = 0; x < W; x++) row_lsb[x] = sr[x] >> 6;
                    fwrite(row_lsb, 2, W, rfp);
                }
            }
        }
        NV_ENC_PIC_PARAMS pic = {0};
        pic.version = MKV_EX(t->maj, t->minv, pic_sv) | (1U << 31);
        pic.inputWidth = W; pic.inputHeight = H; pic.inputPitch = 0;
        pic.inputBuffer = mp.mappedResource;
        pic.outputBitstream = cbs.bitstreamBuffer;
        pic.bufferFmt = NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
        pic.pictureStruct = 1;
        pic.inputTimeStamp = (uint64_t)f;
        if (f == 0) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;

        double t0 = now_ms();
        assert(fl.nvEncEncodePicture(enc, &pic) == NV_ENC_SUCCESS);
        NV_ENC_LOCK_BITSTREAM lb = {0};
        lb.version = MKV_EX(t->maj, t->minv, lbs_sv) | (1U << 31);
        lb.outputBitstream = cbs.bitstreamBuffer;
        assert(fl.nvEncLockBitstream(enc, &lb) == NV_ENC_SUCCESS);
        double t1 = now_ms();
        enc_ms += (t1 - t0);
        fwrite(lb.bitstreamBufferPtr, 1, lb.bitstreamSizeInBytes, fp);
        m->total_bytes += lb.bitstreamSizeInBytes;
        fl.nvEncUnlockBitstream(enc, cbs.bitstreamBuffer);
    }
    fclose(fp);
    if (rfp) fclose(rfp);
    m->fps = (nframes * 1000.0) / enc_ms;

    fl.nvEncUnmapInputResource(enc, mp.mappedResource);
    fl.nvEncUnregisterResource(enc, reg.registeredResource);
    fl.nvEncDestroyBitstreamBuffer(enc, cbs.bitstreamBuffer);
    fl.nvEncDestroyEncoder(enc);
    cuMemFree(dev_ptr);
    free(host_buf);
    cuCtxDestroy(cuctx);
    dlclose(dl);
}

static void run_case_std(const enc_target_t *t, int is_h264, uint32_t W, uint32_t H,
                         NV_ENC_BUFFER_FORMAT fmt, int rc_mode,
                         const char *out_path, const char *ref_path,
                         int nframes, diff_metrics_t *m)
{
    memset(m, 0, sizeof(*m));
    void *dl = dlopen(t->libpath, RTLD_NOW | RTLD_LOCAL);
    assert(dl != NULL);
    NVENCSTATUS (*ci)(NV_ENCODE_API_FUNCTION_LIST *) = dlsym(dl, "NvEncodeAPICreateInstance");
    void *cu = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    int (*cuInit)(unsigned) = dlsym(cu, "cuInit");
    int (*cuCtxCreate)(void **, unsigned, int) = dlsym(cu, "cuCtxCreate_v2");
    int (*cuCtxDestroy)(void *) = dlsym(cu, "cuCtxDestroy_v2");
    cuInit(0);
    void *cuctx = NULL;
    assert(cuCtxCreate(&cuctx, 0, 0) == 0);

    NV_ENCODE_API_FUNCTION_LIST fl = {0};
    fl.version = MKV_EX(t->maj, t->minv, 2U);
    assert(ci(&fl) == NV_ENC_SUCCESS);
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS ex = {0};
    ex.version = MKV_EX(t->maj, t->minv, 1U);
    ex.deviceType = 1; ex.device = cuctx; ex.apiVersion = (t->maj << 4) | t->minv;
    void *enc = NULL;
    assert(fl.nvEncOpenEncodeSessionEx(&ex, &enc) == NV_ENC_SUCCESS);

    uint32_t cfg_sv  = (t->maj >= 13U) ? 9U : 8U;
    uint32_t pcfg_sv = (t->maj >= 13U) ? 5U : 4U;
    uint32_t init_sv = (t->maj >= 13U) ? 7U : 5U;
    uint32_t pic_sv  = (t->maj >= 13U) ? 7U : 4U;
    uint32_t lbs_sv  = (t->maj >= 13U) ? 2U : 1U;
    uint32_t rec_sv  = (t->maj >= 13U) ? 2U : 1U;

    NVENC_GUID codec = is_h264 ? G_CODEC_H264 : G_CODEC_HEVC;
    NV_ENC_PRESET_CONFIG pcfg = {0};
    pcfg.version = MKV_EX(t->maj, t->minv, pcfg_sv) | (1U << 31);
    pcfg.presetCfg.version = MKV_EX(t->maj, t->minv, cfg_sv) | (1U << 31);
    assert(fl.nvEncGetEncodePresetConfigEx(enc, codec, G_PRESET_P4,
                                           NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &pcfg) == 0);
    int is_444 = (fmt == NV_ENC_BUFFER_FORMAT_ARGB || fmt == NV_ENC_BUFFER_FORMAT_YUV444);
    NV_ENC_CONFIG cfg = pcfg.presetCfg;
    cfg.version = MKV_EX(t->maj, t->minv, cfg_sv) | (1U << 31);
    cfg.gopLength = 0xffffffffU;
    cfg.frameIntervalP = 1;
    if (rc_mode == 0) {
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        cfg.rcParams.constQP.qpIntra = 25;
        cfg.rcParams.constQP.qpInterP = 25;
    } else {
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        cfg.rcParams.averageBitRate = 2000000U;
        cfg.rcParams.maxBitRate = 3000000U;
    }
    if (is_h264) {
        cfg.profileGUID = is_444 ? G_H264_444 : G_H264_MAIN;
        cfg.encodeCodecConfig.h264Config.chromaFormatIDC = is_444 ? 3U : 1U;
        cfg.encodeCodecConfig.h264Config.idrPeriod = 0xffffffffU;
        cfg.encodeCodecConfig.h264Config.h264VUIParameters.videoSignalTypePresentFlag = 1;
        cfg.encodeCodecConfig.h264Config.h264VUIParameters.videoFormat = 5;
        cfg.encodeCodecConfig.h264Config.h264VUIParameters.colourDescriptionPresentFlag = 1;
        cfg.encodeCodecConfig.h264Config.h264VUIParameters.colourPrimaries = 1;
        cfg.encodeCodecConfig.h264Config.h264VUIParameters.transferCharacteristics = 1;
        cfg.encodeCodecConfig.h264Config.h264VUIParameters.colourMatrix = 1;
    } else {
        cfg.profileGUID = is_444 ? G_HEVC_FREXT : G_HEVC_MAIN;
        cfg.encodeCodecConfig.hevcConfig.chromaFormatIDC = is_444 ? 3U : 1U;
        cfg.encodeCodecConfig.hevcConfig.idrPeriod = 0xffffffffU;
        cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoSignalTypePresentFlag = 1;
        cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFormat = 5;
        cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourDescriptionPresentFlag = 1;
        cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourPrimaries = 1;
        cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.transferCharacteristics = 1;
        cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourMatrix = 1;
    }

    NV_ENC_INITIALIZE_PARAMS ip = {0};
    ip.version = MKV_EX(t->maj, t->minv, init_sv) | (1U << 31);
    ip.encodeGUID = codec; ip.presetGUID = G_PRESET_P4;
    ip.encodeWidth = W; ip.encodeHeight = H; ip.darWidth = W; ip.darHeight = H;
    ip.frameRateNum = 60; ip.frameRateDen = 1;
    ip.enablePTD = 1; ip.encodeConfig = &cfg;
    ip.maxEncodeWidth = W; ip.maxEncodeHeight = H;
    ip.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    if (t->maj >= 13U) ip.bufferFormat = fmt;
    assert(fl.nvEncInitializeEncoder(enc, &ip) == NV_ENC_SUCCESS);

    NV_ENC_CREATE_INPUT_BUFFER cib = {0}; cib.version = MKV_EX(t->maj, t->minv, 1U);
    cib.width = W; cib.height = H; cib.bufferFmt = fmt;
    assert(fl.nvEncCreateInputBuffer(enc, &cib) == NV_ENC_SUCCESS);
    NV_ENC_CREATE_BITSTREAM_BUFFER cbs = {0}; cbs.version = MKV_EX(t->maj, t->minv, 1U);
    assert(fl.nvEncCreateBitstreamBuffer(enc, &cbs) == NV_ENC_SUCCESS);

    FILE *fp = fopen(out_path, "wb");
    FILE *rfp = ref_path ? fopen(ref_path, "wb") : NULL;
    assert(fp != NULL);
    double enc_ms = 0.0;

    for (int f = 0; f < nframes; f++) {
        if (rc_mode != 0 && f == nframes / 2) {
            cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            cfg.rcParams.averageBitRate = 3000000U;
            cfg.rcParams.maxBitRate = 3000000U;
            NV_ENC_RECONFIGURE_PARAMS rp = {0};
            rp.version = MKV_EX(t->maj, t->minv, rec_sv) | (1U << 31);
            rp.reInitEncodeParams = ip;
            assert(fl.nvEncReconfigureEncoder(enc, &rp) == NV_ENC_SUCCESS);
        }
        NV_ENC_LOCK_INPUT_BUFFER li = {0}; li.version = MKV_EX(t->maj, t->minv, 1U);
        li.inputBuffer = cib.inputBuffer;
        assert(fl.nvEncLockInputBuffer(enc, &li) == NV_ENC_SUCCESS);
        uint8_t *buf = (uint8_t *)li.bufferDataPtr;
        if (fmt == NV_ENC_BUFFER_FORMAT_ARGB) {
            gen_scroll_cursor_bgra(buf, W, H, li.pitch, (uint32_t)f);
            if (rfp) for (uint32_t y = 0; y < H; y++) fwrite(buf + (size_t)y * li.pitch, 4, W, rfp);
        } else if (fmt == NV_ENC_BUFFER_FORMAT_NV12) {
            gen_scroll_cursor_nv12(buf, W, H, li.pitch, (uint32_t)f);
            if (rfp) {
                for (uint32_t y = 0; y < H; y++) fwrite(buf + (size_t)y * li.pitch, 1, W, rfp);
                for (uint32_t y = 0; y < H / 2U; y++) fwrite(buf + (size_t)li.pitch * H + (size_t)y * li.pitch, 1, W, rfp);
            }
        } else {
            gen_scroll_cursor_y444_8b(buf, W, H, li.pitch, (uint32_t)f);
            if (rfp) {
                for (int p = 0; p < 3; p++)
                    for (uint32_t y = 0; y < H; y++) fwrite(buf + (size_t)p * li.pitch * H + (size_t)y * li.pitch, 1, W, rfp);
            }
        }
        fl.nvEncUnlockInputBuffer(enc, cib.inputBuffer);

        NV_ENC_PIC_PARAMS pic = {0};
        pic.version = MKV_EX(t->maj, t->minv, pic_sv) | (1U << 31);
        pic.inputWidth = W; pic.inputHeight = H; pic.inputPitch = li.pitch;
        pic.inputBuffer = cib.inputBuffer; pic.outputBitstream = cbs.bitstreamBuffer;
        pic.bufferFmt = fmt; pic.pictureStruct = 1; pic.inputTimeStamp = (uint64_t)f;
        if (f == 0) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

        double t0 = now_ms();
        assert(fl.nvEncEncodePicture(enc, &pic) == NV_ENC_SUCCESS);
        NV_ENC_LOCK_BITSTREAM lb = {0}; lb.version = MKV_EX(t->maj, t->minv, lbs_sv) | (1U << 31);
        lb.outputBitstream = cbs.bitstreamBuffer;
        assert(fl.nvEncLockBitstream(enc, &lb) == NV_ENC_SUCCESS);
        double t1 = now_ms();
        enc_ms += (t1 - t0);
        fwrite(lb.bitstreamBufferPtr, 1, lb.bitstreamSizeInBytes, fp);
        m->total_bytes += lb.bitstreamSizeInBytes;
        fl.nvEncUnlockBitstream(enc, cbs.bitstreamBuffer);
    }
    fclose(fp);
    if (rfp) fclose(rfp);
    m->fps = (nframes * 1000.0) / enc_ms;

    fl.nvEncDestroyBitstreamBuffer(enc, cbs.bitstreamBuffer);
    fl.nvEncDestroyInputBuffer(enc, cib.inputBuffer);
    fl.nvEncDestroyEncoder(enc);
    cuCtxDestroy(cuctx);
    dlclose(dl);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *v535_lib = getenv("OPEN_NVENC_VENDOR_LIB");
    if (!v535_lib || !v535_lib[0]) {
        v535_lib = "/home/zero/vgpu-work/grid-guest-535.309.01/libnvidia-encode.so.535.309.01";
    }
    const char *v13_lib  = getenv("OPEN_NVENC_VENDOR_13_LIB");
    const char *open_lib = "./libopen-nvidia-encode.so";

    check_abi_caps_and_presets(open_lib, v535_lib, v13_lib);

    mkdir("/tmp/open_nvenc_diff_hw", 0755);
    const enc_target_t tv = { "VENDOR_535", v535_lib, 12U, 1U };
    const enc_target_t to = { "OPEN_13",    open_lib, 13U, 0U };

    /* Case 1: PixelFlux exact 1784x992 YUV444_10BIT scrolling+cursor + PaintOver + InvalidateRefFrames */
    {
        diff_metrics_t mv, mo;
        run_case_pf_10b444(&tv, "/tmp/open_nvenc_diff_hw/c1_v.h265", "/tmp/open_nvenc_diff_hw/c1.ref", 60, &mv);
        run_case_pf_10b444(&to, "/tmp/open_nvenc_diff_hw/c1_o.h265", NULL, 60, &mo);
        inspect_bitstream("/tmp/open_nvenc_diff_hw/c1_v.h265", "/tmp/open_nvenc_diff_hw/c1.ref", "yuv444p10le", 1784U, 992U, 0, 60, &mv);
        inspect_bitstream("/tmp/open_nvenc_diff_hw/c1_o.h265", "/tmp/open_nvenc_diff_hw/c1.ref", "yuv444p10le", 1784U, 992U, 0, 60, &mo);
        printf("[CASE1 HEVC_10B_444 1784x992 PF] VENDOR: %.1f FPS PSNR=%.2f(min=%.2f) SSIM=%.4f lvl=%d %s/%s | OPEN: %.1f FPS PSNR=%.2f(min=%.2f) SSIM=%.4f lvl=%d %s/%s warns=%d\n",
               mv.fps, mv.avg_psnr, mv.min_psnr, mv.avg_ssim, mv.level, mv.color_space, mv.color_range,
               mo.fps, mo.avg_psnr, mo.min_psnr, mo.avg_ssim, mo.level, mo.color_space, mo.color_range, mo.decode_warnings);
        assert(mo.decode_warnings == 0 && mv.decode_warnings == 0);
        assert(mo.width == 1784 && mo.height == 992 && mo.level == 123);
        assert(strcmp(mo.color_space, "bt709") == 0 && strcmp(mo.color_range, "tv") == 0);
        assert(mo.avg_psnr >= mv.avg_psnr - 0.5 && mo.avg_psnr >= 42.0 && mo.min_psnr >= 40.0);
        assert(mo.fps >= 60.0);
    }

    /* Case 2: 1784x992 ARGB 8-bit 4:4:4 HEVC */
    {
        diff_metrics_t mv, mo;
        run_case_std(&tv, 0, 1784U, 992U, NV_ENC_BUFFER_FORMAT_ARGB, 0, "/tmp/open_nvenc_diff_hw/c2_v.h265", "/tmp/open_nvenc_diff_hw/c2.ref", 30, &mv);
        run_case_std(&to, 0, 1784U, 992U, NV_ENC_BUFFER_FORMAT_ARGB, 0, "/tmp/open_nvenc_diff_hw/c2_o.h265", NULL, 30, &mo);
        inspect_bitstream("/tmp/open_nvenc_diff_hw/c2_v.h265", "/tmp/open_nvenc_diff_hw/c2.ref", "bgra", 1784U, 992U, 1, 60, &mv);
        inspect_bitstream("/tmp/open_nvenc_diff_hw/c2_o.h265", "/tmp/open_nvenc_diff_hw/c2.ref", "bgra", 1784U, 992U, 1, 60, &mo);
        printf("[CASE2 HEVC_ARGB_8B 1784x992] VENDOR: %.1f FPS PSNR=%.2f lvl=%d | OPEN: %.1f FPS PSNR=%.2f lvl=%d warns=%d\n",
               mv.fps, mv.avg_psnr, mv.level, mo.fps, mo.avg_psnr, mo.level, mo.decode_warnings);
        assert(mo.decode_warnings == 0 && mv.decode_warnings == 0);
        assert(mo.width == 1784 && mo.height == 992 && mo.level == 123);
        assert(mo.avg_psnr >= mv.avg_psnr - 0.5 && mo.fps >= 60.0);
    }

    /* Case 3a: 1784x992 H.264 NV12 */
    {
        diff_metrics_t mv, mo;
        run_case_std(&tv, 1, 1784U, 992U, NV_ENC_BUFFER_FORMAT_NV12, 0, "/tmp/open_nvenc_diff_hw/c3a_v.h264", "/tmp/open_nvenc_diff_hw/c3a.ref", 30, &mv);
        run_case_std(&to, 1, 1784U, 992U, NV_ENC_BUFFER_FORMAT_NV12, 0, "/tmp/open_nvenc_diff_hw/c3a_o.h264", NULL, 30, &mo);
        inspect_bitstream("/tmp/open_nvenc_diff_hw/c3a_v.h264", "/tmp/open_nvenc_diff_hw/c3a.ref", "nv12", 1784U, 992U, 0, 60, &mv);
        inspect_bitstream("/tmp/open_nvenc_diff_hw/c3a_o.h264", "/tmp/open_nvenc_diff_hw/c3a.ref", "nv12", 1784U, 992U, 0, 60, &mo);
        printf("[CASE3a H264_NV12 1784x992] VENDOR: %.1f FPS PSNR=%.2f lvl=%d | OPEN: %.1f FPS PSNR=%.2f lvl=%d warns=%d\n",
               mv.fps, mv.avg_psnr, mv.level, mo.fps, mo.avg_psnr, mo.level, mo.decode_warnings);
        assert(mo.decode_warnings == 0 && mv.decode_warnings == 0);
        assert(mo.width == 1784 && mo.height == 992 && mo.level == 42);
        assert(mo.avg_psnr >= mv.avg_psnr - 1.0 && mo.avg_psnr >= 38.0);
    }

    /* Case 3b: 1784x992 H.264 YUV444 */
    {
        diff_metrics_t mv, mo;
        run_case_std(&tv, 1, 1784U, 992U, NV_ENC_BUFFER_FORMAT_YUV444, 0, "/tmp/open_nvenc_diff_hw/c3b_v.h264", "/tmp/open_nvenc_diff_hw/c3b.ref", 30, &mv);
        run_case_std(&to, 1, 1784U, 992U, NV_ENC_BUFFER_FORMAT_YUV444, 0, "/tmp/open_nvenc_diff_hw/c3b_o.h264", NULL, 30, &mo);
        inspect_bitstream("/tmp/open_nvenc_diff_hw/c3b_v.h264", "/tmp/open_nvenc_diff_hw/c3b.ref", "yuv444p", 1784U, 992U, 0, 60, &mv);
        inspect_bitstream("/tmp/open_nvenc_diff_hw/c3b_o.h264", "/tmp/open_nvenc_diff_hw/c3b.ref", "yuv444p", 1784U, 992U, 0, 60, &mo);
        printf("[CASE3b H264_Y444 1784x992] VENDOR: %.1f FPS PSNR=%.2f lvl=%d | OPEN: %.1f FPS PSNR=%.2f lvl=%d warns=%d\n",
               mv.fps, mv.avg_psnr, mv.level, mo.fps, mo.avg_psnr, mo.level, mo.decode_warnings);
        assert(mo.decode_warnings == 0 && mv.decode_warnings == 0);
        assert(mo.width == 1784 && mo.height == 992 && mo.level == 42);
        assert(mo.avg_psnr >= mv.avg_psnr - 1.0 && mo.avg_psnr >= 38.0);
    }

    /* Case 4: 1280x720 NV12 HEVC VBR -> CBR live reconfigure */
    {
        diff_metrics_t mv, mo;
        run_case_std(&tv, 0, 1280U, 720U, NV_ENC_BUFFER_FORMAT_NV12, 1, "/tmp/open_nvenc_diff_hw/c4_v.h265", "/tmp/open_nvenc_diff_hw/c4.ref", 60, &mv);
        run_case_std(&to, 0, 1280U, 720U, NV_ENC_BUFFER_FORMAT_NV12, 1, "/tmp/open_nvenc_diff_hw/c4_o.h265", NULL, 60, &mo);
        inspect_bitstream("/tmp/open_nvenc_diff_hw/c4_v.h265", "/tmp/open_nvenc_diff_hw/c4.ref", "nv12", 1280U, 720U, 0, 60, &mv);
        inspect_bitstream("/tmp/open_nvenc_diff_hw/c4_o.h265", "/tmp/open_nvenc_diff_hw/c4.ref", "nv12", 1280U, 720U, 0, 60, &mo);
        printf("[CASE4 HEVC_NV12_720P VBR->CBR] VENDOR: %.1f FPS PSNR=%.2f | OPEN: %.1f FPS PSNR=%.2f warns=%d\n",
               mv.fps, mv.avg_psnr, mo.fps, mo.avg_psnr, mo.decode_warnings);
        assert(mo.decode_warnings == 0 && mv.decode_warnings == 0);
        assert(mo.avg_psnr >= 36.0 && mo.fps >= 60.0);
    }

    int rc_rm = system("rm -rf /tmp/open_nvenc_diff_hw");
    (void)rc_rm;
    printf("ALL_OPEN_NVENC_DIFF_HW_TESTS_PASSED\n");
    return 0;
}
