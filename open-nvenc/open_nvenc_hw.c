/* SPDX-License-Identifier: MIT */
/*
 * open_nvenc_hw.c - Standalone Clean-Room GP104 NVC2B7 (0xc2b7) Hardware HEVC Encoder
 *
 * Drives /dev/nvidiactl + /dev/nvidia0 directly via RM ioctls, NVC06F GPFIFO
 * pushbuffers, and NVC2B7_VIDEO_ENCODER Falcon methods without delegating to
 * proprietary libnvidia-encode.so, libnvcuvid.so, or libcuda.so.
 */

#define _GNU_SOURCE
#include "open_nvenc_hw.h"
#include "open_nvenc_hevc_hdr.h"
#include "open_nvenc_h264_hdr.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Linux NVIDIA RM / escape ioctl ABI */
#define NV_IOCTL_MAGIC               'F'
#define NV_ESC_REGISTER_FD           201U /* 0xc9 */
#define NV_ESC_CHECK_VERSION_STR     210U /* 0xd2 */

#define NV_ESC_RM_ALLOC_MEMORY       0x27U
#define NV_ESC_RM_FREE               0x29U
#define NV_ESC_RM_CONTROL            0x2aU
#define NV_ESC_RM_ALLOC              0x2bU
#define NV_ESC_RM_MAP_MEMORY         0x4eU
#define NV_ESC_RM_UNMAP_MEMORY       0x4fU
#define NV_ESC_RM_MAP_MEMORY_DMA     0x57U
#define NV_ESC_RM_UNMAP_MEMORY_DMA   0x58U

#define NVA06F_CTRL_CMD_GPFIFO_SCHEDULE 0xa06f0103U
#define NVA06F_CTRL_CMD_BIND            0xa06f0104U
#define NV906F_CTRL_CMD_GET_CLASS_ENGINEID 0x906f0101U
#define NVC0B5_ENGINE_TYPE_COPY0        0x12U
#define NVC2B7_ENGINE_TYPE_NVENC0       0x1bU

typedef struct {
    uint32_t cmd;
    uint32_t reply;
    char     versionString[64];
} nv_rm_api_version_t;

typedef struct {
    int ctl_fd;
} nv_register_fd_t;

typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectNew;
    uint32_t hClass;
    uint64_t pAllocParms;
    uint32_t paramsSize;
    uint32_t status;
} nv_nvos21_t;

typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectNew;
    uint32_t hClass;
    uint64_t pAllocParms;
    uint64_t pRightsRequested;
    uint32_t paramsSize;
    uint32_t flags;
    uint32_t status;
    uint32_t pad;
} nv_nvos64_t;

typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectNew;
    uint32_t hClass;
    uint32_t flags;
    uint32_t pad0;
    uint64_t pMemory;
    uint64_t limit;
    uint32_t status;
    uint32_t pad1;
    int      fd;
    uint32_t pad2;
} nv_nvos02_t;

typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectOld;
    uint32_t status;
} nv_nvos00_t;

typedef struct {
    uint32_t hClient;
    uint32_t hObject;
    uint32_t cmd;
    uint32_t flags;
    uint64_t params;
    uint32_t paramsSize;
    uint32_t status;
} nv_nvos54_t;

typedef struct {
    uint32_t hClient;
    uint32_t hDevice;
    uint32_t hMemory;
    uint32_t pad0;
    uint64_t offset;
    uint64_t length;
    uint64_t pLinearAddress;
    uint32_t status;
    uint32_t flags;
    int      fd;
    uint32_t pad1;
} nv_nvos33_t;

typedef struct {
    uint32_t hClient;
    uint32_t hDevice;
    uint32_t hDma;
    uint32_t hMemory;
    uint64_t offset;
    uint64_t length;
    uint32_t flags;
    uint32_t pad0;
    uint64_t dmaOffset;
    uint32_t status;
    uint32_t pad1;
} nv_nvos46_t;

typedef struct {
    uint32_t hClient;
    uint32_t hDevice;
    uint32_t hDma;
    uint32_t hMemory;
    uint32_t flags;
    uint32_t pad0;
    uint64_t dmaOffset;
    uint32_t status;
    uint32_t pad1;
} nv_nvos47_t;

/* NV_MEMORY_ALLOCATION_PARAMS (padded to 512 B for RM paramsSize=0 copy_from_user) */
typedef struct {
    uint32_t owner;
    uint32_t pad0;
    uint32_t type;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t attr;
    uint32_t attr2;
    uint32_t format;
    uint32_t comprCovg;
    uint32_t zcullCovg;
    uint32_t pageSize;
    uint32_t alignment_pad0;
    uint32_t pitch;
    uint32_t ctagOffset;
    uint32_t pad1;
    uint64_t size;
    uint64_t alignment;
    uint64_t offset;
    uint64_t limit;
    uint32_t address_lo;
    uint32_t address_hi;
    uint32_t internalflags;
    uint32_t tag;
    uint64_t hPhysHandle;
    uint64_t reserved_end;
    uint8_t  rm_tail_pad[512 - 128];
} nv_mem_alloc_params_t;

typedef struct {
    uint32_t hSubDevice;
    uint32_t flags;
    uint32_t hMemory;
    uint32_t pad0;
    uint64_t offset;
    uint64_t limit;
    uint8_t  rm_tail_pad[512 - 32];
} nv_ctx_dma_alloc_params_t;

typedef struct {
    uint32_t hObjectError;
    uint32_t hObjectBuffer;
    uint64_t gpFifoOffset;
    uint32_t gpFifoEntries;
    uint32_t flags;
    uint32_t hContextShare;
    uint32_t hVASpace;
    uint32_t userdOffset[8];
    uint32_t userdSubDevice[8];
    uint32_t pad[8];
    uint8_t  rm_tail_pad[512 - 128];
} nv_channel_alloc_params_t;

typedef struct {
    uint32_t h_mem;
    uint64_t gpu_va;
    uint64_t size;
    void    *cpu;
    void    *mmap_base;
    size_t   mmap_len;
    uint64_t rm_linear_addr;
    int      map_fd;
    int      is_vidmem;
    int      dma_mapped;
} hw_buf_t;

struct _open_nvenc_hw_ctx {
    int      ctl_fd;
    int      dev_fd;
    char     ctl_path[256];
    char     dev_path[256];

    uint32_t h_client;
    uint32_t h_device;
    uint32_t h_subdevice;
    uint32_t h_vaspace;
    uint32_t h_sw_sess;
    uint32_t h_ctx_dma;
    uint32_t h_mem_virt;
    uint32_t h_channel;
    uint32_t h_nvenc;
    uint32_t h_next;

    void    *userd_cpu;
    void    *userd_mmap_base;
    size_t   userd_mmap_len;
    uint64_t userd_rm_addr;
    int      userd_fd;
    int      channel_mapped;

    hw_buf_t pb_buf;       /* 0x29000 B: pushbuffer (0..0x20000) + GPFIFO (0x20000..) */
    hw_buf_t err_buf;      /* 0x1000 B */
    hw_buf_t sema_buf;     /* 0x3000 B */
    hw_buf_t pic_setup_buf;/* 0xe000 B */
    hw_buf_t scratch_buf;  /* 0x10000 B vidmem */
    hw_buf_t qm_rc_buf;    /* 0x1000 B */
    hw_buf_t status_buf;   /* 0x1000 B */
    hw_buf_t bs_buf;       /* >= 0x65000 B */
    hw_buf_t io_aux_buf;     /* scaled with CTUs (>= 0x9000 B) */
    hw_buf_t input_buf;      /* block-linear input surface */
    hw_buf_t recon_buf[2];   /* double-buffered vidmem recon surfaces */
    hw_buf_t coloc_mv_buf[2];/* double-buffered vidmem colocated MV buffers */
    hw_buf_t h264_mv_buf;    /* 0x20000 B vidmem for H.264 (0x0728/0x072c) */

    uint32_t h_ce_channel;
    uint32_t h_ce_obj;
    int      ce_mapped;
    int      ce_userd_fd;
    void    *ce_userd_cpu;
    void    *ce_userd_mmap_base;
    size_t   ce_userd_mmap_len;
    uint64_t ce_userd_rm_addr;
    uint32_t ce_gp_put;
    uint32_t ce_pb_dw_cur;
    int      has_ce;

    uint32_t pb_dw_cur;    /* current dword offset in pb_buf */
    uint32_t gp_put;
    uint32_t sema_seq;

    open_nvenc_hw_init_cfg_t cfg;
    uint32_t aligned_w;
    uint32_t aligned_h;
    uint32_t ctu_w;
    uint32_t ctu_h;
    uint32_t n_ctus;
    uint32_t mb_w;
    uint32_t mb_h;
    uint32_t n_mbs;
    uint32_t uv_aligned_h;
    uint32_t surface_pitch;
    size_t   surface_size;

    uint32_t frame_num;
    uint32_t gop_poc;
    uint32_t last_recon_slot;
    int      has_ref;
    int      force_next_idr;

    double   rc_vbv_level;
    double   rc_bits_per_frame;
    double   rc_vbv_size;
    uint32_t rc_cur_qp;

    uint8_t *stage_buf;
};

static size_t hw_page_size(void)
{
    long pg = sysconf(_SC_PAGESIZE);
    return (pg > 0L) ? (size_t)pg : 4096U;
}

static uint32_t next_handle(open_nvenc_hw_ctx_t *ctx)
{
    return ctx->h_next++;
}

static void set_err(char *err_buf, size_t err_cap, const char *msg)
{
    if (err_buf && err_cap > 0U) {
        snprintf(err_buf, err_cap, "%s", msg ? msg : "unknown");
    }
}

static int rm_alloc21(int ctl_fd, uint32_t h_root, uint32_t h_parent,
                      uint32_t *h_new_io, uint32_t h_class,
                      void *params, uint32_t params_sz, uint32_t *out_status)
{
    nv_nvos21_t req;
    memset(&req, 0, sizeof(req));
    req.hRoot         = h_root;
    req.hObjectParent = h_parent;
    req.hObjectNew    = h_new_io ? *h_new_io : 0U;
    req.hClass        = h_class;
    req.pAllocParms   = (uint64_t)(uintptr_t)params;
    req.paramsSize    = params_sz;
    req.status        = 0xffffffffU;

    if (ioctl(ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, nv_nvos21_t), &req) < 0) {
        if (out_status) {
            *out_status = (uint32_t)errno;
        }
        return -errno;
    }
    if (out_status) {
        *out_status = req.status;
    }
    if (req.status == 0U && h_new_io) {
        *h_new_io = req.hObjectNew;
    }
    return (req.status == 0U) ? 0 : -EIO;
}

static int rm_alloc64(int ctl_fd, uint32_t h_root, uint32_t h_parent,
                      uint32_t h_new, uint32_t h_class,
                      void *params, uint32_t params_sz,
                      open_nvenc_rm_stats_t *stats, uint32_t *out_status)
{
    nv_nvos64_t req;
    memset(&req, 0, sizeof(req));
    req.hRoot         = h_root;
    req.hObjectParent = h_parent;
    req.hObjectNew    = h_new;
    req.hClass        = h_class;
    req.pAllocParms   = (uint64_t)(uintptr_t)params;
    req.paramsSize    = params_sz;
    req.status        = 0xffffffffU;

    if (ioctl(ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC, nv_nvos64_t), &req) < 0) {
        if (out_status) {
            *out_status = (uint32_t)errno;
        }
        return -errno;
    }
    if (out_status) {
        *out_status = req.status;
    }
    if (req.status != 0U) {
        return -EIO;
    }
    if (stats) {
        stats->rm_alloc_count++;
    }
    return 0;
}

static int rm_ctrl(int ctl_fd, uint32_t h_client, uint32_t h_obj, uint32_t cmd,
                   void *params, uint32_t params_sz,
                   open_nvenc_rm_stats_t *stats, uint32_t *out_status)
{
    nv_nvos54_t req;
    memset(&req, 0, sizeof(req));
    req.hClient    = h_client;
    req.hObject    = h_obj;
    req.cmd        = cmd;
    req.params     = (uint64_t)(uintptr_t)params;
    req.paramsSize = params_sz;
    req.status     = 0xffffffffU;

    if (ioctl(ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL, nv_nvos54_t), &req) < 0) {
        if (out_status) {
            *out_status = (uint32_t)errno;
        }
        return -errno;
    }
    if (out_status) {
        *out_status = req.status;
    }
    if (req.status != 0U) {
        return -EIO;
    }
    if (stats) {
        stats->rm_ctrl_count++;
    }
    return 0;
}

static int rm_free(int ctl_fd, uint32_t h_root, uint32_t h_parent, uint32_t h_old,
                   open_nvenc_rm_stats_t *stats)
{
    if (ctl_fd < 0 || h_old == 0U) {
        return 0;
    }
    nv_nvos00_t req;
    memset(&req, 0, sizeof(req));
    req.hRoot         = h_root;
    req.hObjectParent = h_parent;
    req.hObjectOld    = h_old;
    req.status        = 0xffffffffU;
    if (ioctl(ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_FREE, nv_nvos00_t), &req) < 0) {
        return -errno;
    }
    if (req.status == 0U && stats) {
        stats->rm_free_count++;
    }
    return (req.status == 0U) ? 0 : -EIO;
}

static int rm_map_dma(open_nvenc_hw_ctx_t *ctx, uint32_t h_dma, uint32_t h_mem,
                      uint64_t len, uint32_t flags, uint64_t *out_va)
{
    nv_nvos46_t req;
    memset(&req, 0, sizeof(req));
    req.hClient = ctx->h_client;
    req.hDevice = ctx->h_device;
    req.hDma    = h_dma;
    req.hMemory = h_mem;
    req.offset  = 0ULL;
    req.length  = len;
    req.flags   = flags;
    req.status  = 0xffffffffU;

    if (ioctl(ctx->ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_MAP_MEMORY_DMA, nv_nvos46_t), &req) < 0) {
        return -errno;
    }
    if (req.status != 0U) {
        return -EIO;
    }
    *out_va = req.dmaOffset;
    return 0;
}

static void rm_unmap_dma(open_nvenc_hw_ctx_t *ctx, uint32_t h_dma, const hw_buf_t *buf)
{
    if (!buf || !buf->dma_mapped || ctx->ctl_fd < 0) {
        return;
    }
    nv_nvos47_t req;
    memset(&req, 0, sizeof(req));
    req.hClient   = ctx->h_client;
    req.hDevice   = ctx->h_device;
    req.hDma      = h_dma;
    req.hMemory   = buf->h_mem;
    req.dmaOffset = buf->gpu_va;
    (void)ioctl(ctx->ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_UNMAP_MEMORY_DMA, nv_nvos47_t), &req);
}

static int alloc_buf_sysmem(open_nvenc_hw_ctx_t *ctx, hw_buf_t *buf,
                            uint64_t size, int need_cpu_map,
                            open_nvenc_rm_stats_t *stats,
                            char *err_buf, size_t err_cap)
{
    memset(buf, 0, sizeof(*buf));
    buf->map_fd = -1;
    buf->size   = size;
    buf->h_mem  = next_handle(ctx);

    nv_mem_alloc_params_t mp;
    memset(&mp, 0, sizeof(mp));
    mp.owner     = ctx->h_vaspace;
    mp.type      = 0x0001c101U;
    mp.attr      = 0x2a800000U;
    mp.attr2     = 0x00000009U;
    mp.size      = size;
    mp.alignment = 0x1000ULL;
    mp.limit     = size - 1ULL;

    uint32_t st = 0U;
    if (rm_alloc64(ctx->ctl_fd, ctx->h_client, ctx->h_device, buf->h_mem,
                   OPEN_NVENC_RM_CLASS_SYSMEM, &mp, 0U, stats, &st) < 0) {
        snprintf(err_buf, err_cap, "RM sysmem alloc (0x%llx B) failed status=0x%x",
                 (unsigned long long)size, st);
        buf->h_mem = 0U;
        return -1;
    }

    if (rm_map_dma(ctx, ctx->h_vaspace, buf->h_mem, size, 0x110U, &buf->gpu_va) < 0) {
        set_err(err_buf, err_cap, "RM_MAP_MEMORY_DMA sysmem failed");
        return -1;
    }
    if ((buf->gpu_va >> 40) != 0ULL || (buf->gpu_va & 0xffULL) != 0ULL) {
        snprintf(err_buf, err_cap, "Invalid sysmem GPU VA 0x%llx", (unsigned long long)buf->gpu_va);
        return -1;
    }
    buf->dma_mapped = 1;

    if (need_cpu_map) {
        int mfd = open(ctx->ctl_path, O_RDWR | O_CLOEXEC);
        if (mfd < 0) {
            set_err(err_buf, err_cap, "open nvidiactl for mmap failed");
            return -1;
        }
        nv_nvos33_t mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.hClient = ctx->h_client;
        mreq.hDevice = ctx->h_subdevice;
        mreq.hMemory = buf->h_mem;
        mreq.offset  = 0ULL;
        mreq.length  = size;
        mreq.flags   = 0x03008000U;
        mreq.fd      = mfd;
        mreq.status  = 0xffffffffU;
        if (ioctl(ctx->ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_MAP_MEMORY, nv_nvos33_t), &mreq) < 0 ||
            mreq.status != 0U) {
            snprintf(err_buf, err_cap, "RM_MAP_MEMORY sysmem failed st=0x%x errno=%d", mreq.status, errno);
            close(mfd);
            return -1;
        }
        buf->rm_linear_addr = mreq.pLinearAddress;
        buf->map_fd         = mfd;

        size_t pg       = hw_page_size();
        size_t pg_mask  = pg - 1U;
        size_t page_off = (size_t)(mreq.pLinearAddress & (uint64_t)pg_mask);
        size_t map_len  = (page_off + (size_t)size + pg_mask) & ~pg_mask;
        void *map_base  = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        if (map_base == MAP_FAILED) {
            set_err(err_buf, err_cap, "mmap sysmem failed");
            return -1;
        }
        buf->mmap_base = map_base;
        buf->mmap_len  = map_len;
        buf->cpu       = (uint8_t *)map_base + page_off;
        memset(buf->cpu, 0, (size_t)size);
    }
    return 0;
}

static int alloc_buf_vidmem(open_nvenc_hw_ctx_t *ctx, hw_buf_t *buf,
                            uint64_t size, open_nvenc_rm_stats_t *stats,
                            char *err_buf, size_t err_cap)
{
    memset(buf, 0, sizeof(*buf));
    buf->map_fd    = -1;
    buf->is_vidmem = 1;
    buf->size      = size;
    buf->h_mem     = next_handle(ctx);

    nv_mem_alloc_params_t mp;
    memset(&mp, 0, sizeof(mp));
    mp.owner     = ctx->h_vaspace;
    mp.type      = 0x0001c101U;
    mp.attr      = 0x11000000U;
    mp.attr2     = 0x00000005U;
    mp.size      = size;
    mp.alignment = 0x10000ULL;
    mp.limit     = size - 1ULL;

    uint32_t st = 0U;
    if (rm_alloc64(ctx->ctl_fd, ctx->h_client, ctx->h_device, buf->h_mem,
                   OPEN_NVENC_RM_CLASS_VIDMEM, &mp, 0U, stats, &st) < 0) {
        snprintf(err_buf, err_cap, "RM vidmem alloc (0x%llx B) failed status=0x%x",
                 (unsigned long long)size, st);
        buf->h_mem = 0U;
        return -1;
    }
    if (rm_map_dma(ctx, ctx->h_vaspace, buf->h_mem, size, 0U, &buf->gpu_va) < 0) {
        set_err(err_buf, err_cap, "RM_MAP_MEMORY_DMA vidmem failed");
        return -1;
    }
    if ((buf->gpu_va >> 40) != 0ULL || (buf->gpu_va & 0xffULL) != 0ULL) {
        snprintf(err_buf, err_cap, "Invalid vidmem GPU VA 0x%llx", (unsigned long long)buf->gpu_va);
        return -1;
    }
    buf->dma_mapped = 1;
    return 0;
}

static void free_hw_buf(open_nvenc_hw_ctx_t *ctx, hw_buf_t *buf, uint32_t h_dma,
                        open_nvenc_rm_stats_t *stats)
{
    if (!ctx || !buf) {
        return;
    }
    if (buf->mmap_base && buf->mmap_base != MAP_FAILED) {
        munmap(buf->mmap_base, buf->mmap_len ? buf->mmap_len : (size_t)buf->size);
    } else if (buf->cpu && buf->cpu != MAP_FAILED) {
        munmap(buf->cpu, (size_t)buf->size);
    }
    buf->mmap_base = NULL;
    buf->mmap_len  = 0U;
    buf->cpu       = NULL;
    if (buf->map_fd >= 0) {
        nv_nvos33_t ureq;
        memset(&ureq, 0, sizeof(ureq));
        ureq.hClient        = ctx->h_client;
        ureq.hDevice        = ctx->h_subdevice;
        ureq.hMemory        = buf->h_mem;
        ureq.pLinearAddress = buf->rm_linear_addr;
        ureq.flags          = 0x03008000U;
        (void)ioctl(ctx->ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_UNMAP_MEMORY, nv_nvos33_t), &ureq);
        close(buf->map_fd);
        buf->map_fd         = -1;
        buf->rm_linear_addr = 0ULL;
    }
    rm_unmap_dma(ctx, h_dma, buf);
    buf->dma_mapped = 0;
    if (buf->h_mem) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, ctx->h_device, buf->h_mem, stats);
        buf->h_mem = 0U;
    }
}

static int alloc_buf_nvos02(open_nvenc_hw_ctx_t *ctx, hw_buf_t *buf,
                            uint32_t alloc_flags, uint64_t limit, size_t mmap_len,
                            uint32_t h_dma, uint64_t dma_len, uint32_t dma_flags,
                            open_nvenc_rm_stats_t *stats,
                            char *err_buf, size_t err_cap)
{
    memset(buf, 0, sizeof(*buf));
    buf->map_fd = -1;
    buf->size   = mmap_len;
    buf->h_mem  = next_handle(ctx);

    int mfd = open(ctx->ctl_path, O_RDWR | O_CLOEXEC);
    if (mfd < 0) {
        set_err(err_buf, err_cap, "open nvidiactl for nvos02 failed");
        buf->h_mem = 0U;
        return -1;
    }

    uint64_t pmem = 0ULL;
    nv_nvos02_t req;
    memset(&req, 0, sizeof(req));
    req.hRoot         = ctx->h_client;
    req.hObjectParent = ctx->h_device;
    req.hObjectNew    = buf->h_mem;
    req.hClass        = OPEN_NVENC_RM_CLASS_SYSMEM;
    req.flags         = alloc_flags;
    req.pMemory       = (uint64_t)(uintptr_t)&pmem;
    req.limit         = limit;
    req.fd            = mfd;
    req.status        = 0xffffffffU;

    if (ioctl(ctx->dev_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_ALLOC_MEMORY, nv_nvos02_t), &req) < 0 ||
        req.status != 0U) {
        snprintf(err_buf, err_cap, "RM nvos02 alloc failed st=0x%x errno=%d", req.status, errno);
        close(mfd);
        buf->h_mem = 0U;
        return -1;
    }
    if (stats) {
        stats->rm_alloc_count++;
    }

    void *cpu = mmap(NULL, mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    close(mfd);
    if (cpu == MAP_FAILED) {
        set_err(err_buf, err_cap, "mmap nvos02 failed");
        return -1;
    }
    buf->cpu = cpu;
    memset(buf->cpu, 0, mmap_len);

    if (rm_map_dma(ctx, h_dma, buf->h_mem, dma_len, dma_flags, &buf->gpu_va) < 0) {
        set_err(err_buf, err_cap, "RM_MAP_MEMORY_DMA nvos02 failed");
        return -1;
    }
    if ((buf->gpu_va >> 40) != 0ULL || (buf->gpu_va & 0xffULL) != 0ULL) {
        snprintf(err_buf, err_buf ? err_cap : 0U, "Invalid nvos02 GPU VA 0x%llx",
                 (unsigned long long)buf->gpu_va);
        return -1;
    }
    buf->dma_mapped = 1;
    return 0;
}

/* NVC06F pushbuffer + GPFIFO submission helpers */
static inline uint32_t pb_hdr_inc(uint32_t subch, uint32_t method_byte, uint32_t count)
{
    return (0x1U << 29) | ((count & 0x1fffU) << 16) | ((subch & 0x7U) << 13) | ((method_byte >> 2) & 0xfffU);
}

static int submit_pb_range(open_nvenc_hw_ctx_t *ctx, uint32_t start_dw, uint32_t dw_cnt, int sync_bit)
{
    uint64_t gpu_va = ctx->pb_buf.gpu_va + ((uint64_t)start_dw * 4ULL);
    volatile uint32_t *gpfifo = (volatile uint32_t *)((uint8_t *)ctx->pb_buf.cpu + 0x20000U);
    uint32_t idx = ctx->gp_put % 0x800U;

    uint32_t e0 = (uint32_t)(gpu_va & 0xfffffffcU);
    uint32_t e1 = (uint32_t)((gpu_va >> 32) & 0xffU) |
                  ((dw_cnt & 0x1fffffU) << 10) |
                  ((uint32_t)(sync_bit & 1) << 31);

    gpfifo[idx * 2U + 0U] = e0;
    gpfifo[idx * 2U + 1U] = e1;
    __sync_synchronize();

    ctx->gp_put = (ctx->gp_put + 1U) % 0x800U;
    volatile uint32_t *userd = (volatile uint32_t *)ctx->userd_cpu;
    userd[0x8cU / 4U] = ctx->gp_put;
    __sync_synchronize();
    return 0;
}

static int wait_sema_equal(volatile const uint32_t *ptr, uint32_t expected, int timeout_ms)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        __sync_synchronize();
        if (*ptr == expected) {
            return 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        if (elapsed_ms >= timeout_ms) {
            return -ETIMEDOUT;
        }
        struct timespec sl = { 0, 100000L }; /* 100 us */
        nanosleep(&sl, NULL);
    }
}

static void init_qm_rc_buf(uint32_t *qm)
{
    qm[0x08U / 4U] = 0x00000018U;
    qm[0x0cU / 4U] = 0x0000000cU;
    qm[0x10U / 4U] = 0x00000030U;
    qm[0x14U / 4U] = 0x00000018U;
    qm[0x18U / 4U] = 0x00000018U;
    qm[0x1cU / 4U] = 0x00000018U;
    qm[0x20U / 4U] = 0x0000000cU;
    qm[0x24U / 4U] = 0x0000000cU;
    qm[0x28U / 4U] = 0x0000000cU;
    qm[0x2cU / 4U] = 0x00000030U;
    qm[0x30U / 4U] = 0x00000030U;
    qm[0x34U / 4U] = 0x00000030U;
    qm[0x38U / 4U] = 0x00000f90U;
    qm[0x50U / 4U] = 0x00000100U;
    qm[0x54U / 4U] = 0x0000001aU;
}

static void rc_init_state(open_nvenc_hw_ctx_t *ctx)
{
    uint32_t min_qp = (ctx->cfg.min_qp > 0U && ctx->cfg.min_qp <= 51U) ? ctx->cfg.min_qp : 10U;
    uint32_t max_qp = (ctx->cfg.max_qp > min_qp && ctx->cfg.max_qp <= 51U) ? ctx->cfg.max_qp : 46U;
    double fps = (ctx->cfg.fps_num > 0U && ctx->cfg.fps_den > 0U)
                     ? ((double)ctx->cfg.fps_num / (double)ctx->cfg.fps_den)
                     : 60.0;
    if (fps < 1.0) {
        fps = 60.0;
    }

    uint32_t target_bps = ctx->cfg.average_bitrate ? ctx->cfg.average_bitrate : ctx->cfg.max_bitrate;
    if (ctx->cfg.rc_mode != 0U && target_bps > 0U) {
        ctx->rc_bits_per_frame = (double)target_bps / fps;
        ctx->rc_vbv_size = (ctx->cfg.vbv_buffer_size > 0U)
                               ? (double)ctx->cfg.vbv_buffer_size
                               : (double)target_bps;
        if (ctx->rc_vbv_size < ctx->rc_bits_per_frame * 4.0) {
            ctx->rc_vbv_size = ctx->rc_bits_per_frame * 4.0;
        }
        ctx->rc_vbv_level = ctx->rc_vbv_size * 0.25;

        double pixels = (double)ctx->cfg.width * (double)ctx->cfg.height;
        double bpp = (pixels > 0.0) ? (ctx->rc_bits_per_frame / pixels) : 0.1;
        uint32_t init_qp = 22U;
        if (bpp >= 0.30) {
            init_qp = 18U;
        } else if (bpp >= 0.12) {
            init_qp = 22U;
        } else if (bpp >= 0.05) {
            init_qp = 26U;
        } else {
            init_qp = 30U;
        }
        if (init_qp < min_qp) {
            init_qp = min_qp;
        }
        if (init_qp > max_qp) {
            init_qp = max_qp;
        }
        ctx->rc_cur_qp = init_qp;
    } else if (ctx->cfg.rc_mode == NV_ENC_PARAMS_RC_VBR &&
               ctx->cfg.target_quality > 0U && ctx->cfg.target_quality <= 51U) {
        ctx->rc_bits_per_frame = 0.0;
        ctx->rc_vbv_size       = 0.0;
        ctx->rc_vbv_level      = 0.0;
        ctx->rc_cur_qp         = ctx->cfg.target_quality;
    } else {
        ctx->rc_bits_per_frame = 0.0;
        ctx->rc_vbv_size       = 0.0;
        ctx->rc_vbv_level      = 0.0;
        ctx->rc_cur_qp         = (ctx->cfg.qp <= 51U) ? ctx->cfg.qp : 24U;
    }
}

static uint32_t rc_select_frame_qp(const open_nvenc_hw_ctx_t *ctx, int is_idr)
{
    if (ctx->cfg.rc_mode != 0U && ctx->rc_bits_per_frame > 0.0) {
        uint32_t min_qp = (ctx->cfg.min_qp > 0U && ctx->cfg.min_qp <= 51U) ? ctx->cfg.min_qp : 10U;
        uint32_t qp     = ctx->rc_cur_qp;
        if (is_idr && qp > min_qp + 2U) {
            qp -= 2U;
        }
        return qp;
    }
    if (ctx->cfg.rc_mode == NV_ENC_PARAMS_RC_VBR &&
        ctx->cfg.target_quality > 0U && ctx->cfg.target_quality <= 51U) {
        return ctx->cfg.target_quality;
    }
    uint32_t qp_i = (ctx->cfg.qp <= 51U) ? ctx->cfg.qp : 24U;
    uint32_t qp_p = (ctx->cfg.qp_p > 0U && ctx->cfg.qp_p <= 51U) ? ctx->cfg.qp_p : qp_i;
    return is_idr ? qp_i : qp_p;
}

static void rc_update_after_frame(open_nvenc_hw_ctx_t *ctx, uint32_t encoded_bits, int is_idr)
{
    if (ctx->cfg.rc_mode == 0U || ctx->rc_bits_per_frame <= 0.0) {
        return;
    }
    uint32_t min_qp = (ctx->cfg.min_qp > 0U && ctx->cfg.min_qp <= 51U) ? ctx->cfg.min_qp : 10U;
    uint32_t max_qp = (ctx->cfg.max_qp > min_qp && ctx->cfg.max_qp <= 51U) ? ctx->cfg.max_qp : 46U;

    ctx->rc_vbv_level += (double)encoded_bits - ctx->rc_bits_per_frame;
    if (ctx->rc_vbv_level < 0.0) {
        ctx->rc_vbv_level = 0.0;
    }

    if (is_idr) {
        return;
    }

    double fullness = ctx->rc_vbv_level / ctx->rc_vbv_size;
    int32_t delta = 0;
    if ((double)encoded_bits > ctx->rc_bits_per_frame * 1.40 || fullness > 0.65) {
        delta = ((double)encoded_bits > ctx->rc_bits_per_frame * 2.2) ? 2 : 1;
    } else if ((double)encoded_bits < ctx->rc_bits_per_frame * 0.68 && fullness < 0.35) {
        delta = ((double)encoded_bits < ctx->rc_bits_per_frame * 0.35) ? -2 : -1;
    }

    int32_t next_qp = (int32_t)ctx->rc_cur_qp + delta;
    if (next_qp < (int32_t)min_qp) {
        next_qp = (int32_t)min_qp;
    }
    if (next_qp > (int32_t)max_qp) {
        next_qp = (int32_t)max_qp;
    }
    ctx->rc_cur_qp = (uint32_t)next_qp;
}

static void build_pic_setup(const open_nvenc_hw_ctx_t *ctx,
                            uint8_t *buf,
                            int is_idr,
                            uint32_t ref_dpb_slot,
                            uint32_t cur_dpb_slot,
                            uint32_t qp)
{
    memset(buf, 0, 0xe000U);
    uint32_t *w = (uint32_t *)buf;
    (void)cur_dpb_slot;

    uint32_t aligned_h = ctx->aligned_h;
    uint32_t aligned_w = ctx->aligned_w;
    int      is_444    = (ctx->cfg.chroma_format_idc == 3U);
    int      is_10b    = (ctx->cfg.bit_depth == 10U);
    uint32_t gop_len   = (ctx->cfg.gop_length > 0U) ? ctx->cfg.gop_length : 0xfaU;

    uint32_t y_stride_bytes = aligned_w * (is_10b ? 2U : 1U);
    uint32_t y_plane_pages  = (y_stride_bytes * aligned_h) >> 8; /* in 256B units */
    uint32_t uv_plane_pages = (y_stride_bytes * ctx->uv_aligned_h) >> 8;
    uint32_t uv_stride_hdr  = is_444 ? (aligned_w * 2U) : aligned_w;
    uint32_t dims_m1        = ((aligned_h - 1U) << 16) | (aligned_w - 1U);
    uint32_t stride_word    = (uv_stride_hdr << 16) | aligned_w;
    uint32_t fmt_slot_word  = is_444 ? 0x00000302U : 0x00000002U;

    uint32_t slot_w14 = is_444 ? 0U : (y_plane_pages >> 1);
    uint32_t slot_w18 = y_plane_pages;
    uint32_t slot_w1c = is_444 ? (y_plane_pages * 2U) : (y_plane_pages + (uv_plane_pages >> 1));

    w[0x00000U / 4U] = 0xc2b70006U;

    /* Slot 0: Current Input */
    w[0x00004U / 4U] = dims_m1;
    w[0x00008U / 4U] = stride_word;
    w[0x00014U / 4U] = slot_w14;
    w[0x00018U / 4U] = slot_w18;
    w[0x0001cU / 4U] = slot_w1c;
    w[0x00020U / 4U] = fmt_slot_word;

    /* Slot 1: Reconstructed Output */
    w[0x00024U / 4U] = dims_m1;
    w[0x00028U / 4U] = stride_word;
    w[0x00038U / 4U] = slot_w18;
    w[0x0003cU / 4U] = is_444 ? (y_plane_pages * 2U) : 0U;
    w[0x00040U / 4U] = fmt_slot_word;

    /* Slot 2: Reference / Scratch geometry */
    w[0x00044U / 4U] = dims_m1;
    w[0x00048U / 4U] = stride_word;
    w[0x00054U / 4U] = slot_w14;
    w[0x00058U / 4U] = slot_w18;
    w[0x0005cU / 4U] = slot_w1c;
    w[0x00060U / 4U] = fmt_slot_word;

    /* Sequence/Picture config flags */
    w[0x00064U / 4U] = is_444 ? 0x028364a3U : 0x028364a1U;
    w[0x00068U / 4U] = is_10b ? 0x00000240U : 0x00000200U;
    w[0x0006cU / 4U] = is_10b ? 0x04c00000U : 0x04d00000U;
    w[0x00070U / 4U] = is_444 ? 0x00003180U : 0x00000000U;
    w[0x00074U / 4U] = 0x131f0000U | ((qp & 0x3fU) << 8);
    w[0x00078U / 4U] = is_10b ? 0x33f4f4f4U : 0x33000000U;
    w[0x0007cU / 4U] = 0x00003333U;
    w[0x000a4U / 4U] = gop_len;

    if (is_idr) {
        /* DPB empty slots for IDR */
        w[0x000d4U / 4U] = 0xfefefefeU;
        w[0x000d8U / 4U] = 0xfefefefeU;
        w[0x000dcU / 4U] = 0xfefefefeU;
        w[0x000e0U / 4U] = 0xfefefefeU;
    } else {
        /* P-frame active L0 reference list & RPS */
        w[0x000d4U / 4U] = 0xfefefe00U | ((ref_dpb_slot & 0xfU) << 1);
        w[0x000d8U / 4U] = 0xfefefefeU;
        w[0x000dcU / 4U] = 0xfefefefeU;
        w[0x000e0U / 4U] = 0xfefefefeU;
        w[0x000e4U / 4U] = 0xffffff01U;
        for (uint32_t off = 0x000e8U; off <= 0x00170U; off += 4U) {
            int is_hdr = (off == 0x000f4U || off == 0x00104U || off == 0x00114U ||
                          off == 0x00124U || off == 0x00134U || off == 0x00144U ||
                          off == 0x00154U || off == 0x00164U);
            w[off / 4U] = is_hdr ? 0xffff0100U : 0xffffffffU;
        }
        w[0x00174U / 4U] = 0xfefefefeU;
        w[0x00178U / 4U] = 0xfefefefeU;
    }

    /* Internal descriptor offsets, CTU geometry, output bitstream alloc size */
    uint32_t log2_ctus = 6U;
    while (log2_ctus < 15U && (1U << log2_ctus) < ctx->n_ctus) {
        log2_ctus++;
    }

    w[0x00184U / 4U] = ((ctx->gop_poc & 0xffffU) << 16) | 2U;
    w[0x0018cU / 4U] = 0x00005900U;
    w[0x00190U / 4U] = 0x00000800U;
    w[0x00194U / 4U] = 0x00003800U;
    w[0x00198U / 4U] = 0x00005800U;
    w[0x0019cU / 4U] = ((ctx->ctu_w * ((aligned_h >> 4) + 1U) * 3U) / 2U) << 8;
    w[0x001a0U / 4U] = (uint32_t)ctx->bs_buf.size;
    w[0x001a4U / 4U] = 0x000000ffU;
    w[0x001a8U / 4U] = 0x00ffc0a0U | ((log2_ctus & 0xfU) << 8) | (is_idr ? 7U : 4U);
    w[0x001b0U / 4U] = 0x00000100U;
    w[0x001b4U / 4U] = 0x00000100U;
    w[0x001b8U / 4U] = 0x00000100U;
    w[0x001bcU / 4U] = is_idr ? 0x00901300U : 0x00900101U; /* IDR_W_RADL vs TRAIL_R */

    w[0x00288U / 4U] = is_idr ? 0x00000000U : 0x00000001U;
    w[0x002acU / 4U] = ((aligned_h >> 3) + 1U) << 8;
    w[0x002b0U / 4U] = 0x00006900U;
    w[0x002b4U / 4U] = 0x00010000U;
    w[0x002e4U / 4U] = ((aligned_h >> 4) + 1U) << 8;
    w[0x002e8U / 4U] = 0x00000100U;
    w[0x0030cU / 4U] = (ctx->n_ctus > 1900U)
                           ? 0x16698000U
                           : ((ctx->n_ctus > 1024U)
                                  ? 0x13128000U
                                  : ((ctx->n_ctus > 256U) ? 0x0a1b8000U : 0x09e34000U));

    /* Motion/search/CU cost parameters (+0x800 block) */
    w[0x00800U / 4U] = 0x00074002U;
    w[0x00804U / 4U] = 0x0c006001U;
    w[0x00808U / 4U] = 0x03ff0fffU;
    w[0x0080cU / 4U] = 0x28000280U;
    w[0x00814U / 4U] = 0xa2400000U;
    w[0x0082cU / 4U] = 0x7f1c0800U;
    w[0x00830U / 4U] = 0x0000081cU;
    w[0x00838U / 4U] = 0x7f7f1c41U;
    w[0x0083cU / 4U] = 0x0000411cU;
    w[0x00840U / 4U] = 0x00000002U;
    w[0x00890U / 4U] = 0x00007c00U;

    /* RDO / SAO / Deblock tables (+0x3800 block) */
    w[0x03804U / 4U] = 0x55555557U;
    w[0x03808U / 4U] = 0x55555557U;
    w[0x0380cU / 4U] = 0x55555557U;
    w[0x03810U / 4U] = 0x55555557U;
    w[0x03818U / 4U] = 0x00000b6dU;
    w[0x0381cU / 4U] = 0x0f00f70fU;
    w[0x03820U / 4U] = 0x000179ffU;
    w[0x03848U / 4U] = 0xffff0000U;
    w[0x0384cU / 4U] = 0x023affffU;

    /* Quant/cabac init (+0x5800 block) */
    w[0x05800U / 4U] = 0xaaff056bU;
    w[0x05804U / 4U] = 0x000fffaaU;
    w[0x05808U / 4U] = 0x000f0f0fU;

    /* Slice header template (+0x5900 block) */
    w[0x05900U / 4U] = (is_idr ? 0x00980000U : 0x00c00000U) | (ctx->n_ctus & 0xffffU);
    w[0x05904U / 4U] = is_idr ? 0x18000204U : 0x18000200U;
}

static void build_pic_setup_h264(const open_nvenc_hw_ctx_t *ctx,
                                 uint8_t *buf,
                                 int is_idr,
                                 uint32_t ref_dpb_slot,
                                 uint32_t cur_dpb_slot,
                                 uint32_t qp)
{
    memset(buf, 0, 0xe000U);
    uint32_t *w = (uint32_t *)buf;
    (void)cur_dpb_slot;

    uint32_t width     = ctx->cfg.width;
    uint32_t aligned_w = ctx->aligned_w;
    uint32_t aligned_h = ctx->aligned_h;
    uint32_t w16       = ctx->mb_w << 4;
    uint32_t h16       = ctx->mb_h << 4;
    int      is_444    = (ctx->cfg.chroma_format_idc == 3U);
    uint32_t gop_len   = (ctx->cfg.gop_length > 0U) ? ctx->cfg.gop_length : 0xfaU;
    uint8_t  level_idc = open_nvenc_h264_compute_level_idc_fps(width, ctx->cfg.height,
                                                               ctx->cfg.fps_num, ctx->cfg.fps_den);
    if (ctx->cfg.level_idc >= 10U && ctx->cfg.level_idc <= 62U && (uint8_t)ctx->cfg.level_idc > level_idc) {
        level_idc = (uint8_t)ctx->cfg.level_idc;
    }

    uint32_t y_plane_pages  = (aligned_w * aligned_h) >> 8;
    uint32_t uv_plane_pages = (aligned_w * ctx->uv_aligned_h) >> 8;
    uint32_t uv_stride_hdr  = is_444 ? (aligned_w * 2U) : aligned_w;
    uint32_t dims_m1        = ((h16 - 1U) << 16) | (w16 - 1U);
    uint32_t stride_word    = (uv_stride_hdr << 16) | aligned_w;
    uint32_t fmt_slot_word  = is_444 ? 0x00000302U : 0x00000002U;

    uint32_t slot_w14 = is_444 ? 0U : (y_plane_pages >> 1);
    uint32_t slot_w18 = y_plane_pages;
    uint32_t slot_w1c = is_444 ? (y_plane_pages * 2U) : (y_plane_pages + (uv_plane_pages >> 1));

    w[0x0000U / 4U] = 0xc2b70006U;

    /* Slot 0: Current Input, Slot 1: Recon Output, Slot 2: Ref/Scratch geometry */
    for (uint32_t base = 0x0004U; base <= 0x0044U; base += 0x0020U) {
        w[(base + 0x00U) / 4U] = dims_m1;
        w[(base + 0x04U) / 4U] = stride_word;
        w[(base + 0x10U) / 4U] = slot_w14;
        w[(base + 0x14U) / 4U] = slot_w18;
        w[(base + 0x18U) / 4U] = slot_w1c;
        w[(base + 0x1cU) / 4U] = fmt_slot_word;
    }

    /* H.264 sequence/picture flags (verified vs vendor h264_nv12, h264_y444, h264_720p traces) */
    w[0x0064U / 4U] = (is_444 ? 0x144300f4U : 0x1441004dU) | ((uint32_t)level_idc << 8);
    w[0x0068U / 4U] = is_444 ? 0x30000000U : 0x00000100U;
    w[0x006cU / 4U] = is_444 ? 0x000000c6U : 0x00000040U;
    w[0x0070U / 4U] = 0x131f0002U | ((qp & 0x3fU) << 8);
    w[0x0074U / 4U] = 0x33000000U;
    w[0x0078U / 4U] = 0x03063333U;
    w[0x007cU / 4U] = 0x00000100U;
    w[0x0080U / 4U] = 0x00000100U;
    w[0x0084U / 4U] = 0x00000100U;
    w[0x0088U / 4U] = 0x00001e00U;
    w[0x008cU / 4U] = 0x003d0900U;
    w[0x0090U / 4U] = 0x003d0900U;
    w[0x0094U / 4U] = 0x001e8480U;
    w[0x0098U / 4U] = 0x003d0900U;
    w[0x009cU / 4U] = 0x001e8480U;
    w[0x00a0U / 4U] = gop_len;
    w[0x00a4U / 4U] = 0x00000007U;
    w[0x00a8U / 4U] = (width > 512U) ? 0x0000006fU : 0x0000061aU;
    w[0x00acU / 4U] = (width > 512U) ? 0x00000012U : 0x00000104U;
    w[0x00b0U / 4U] = (width > 512U) ? 0x00000012U : 0x00000104U;
    w[0x00b8U / 4U] = 0x00010000U;
    w[0x00bcU / 4U] = 0x00010100U;

    if (is_idr) {
        w[0x00c8U / 4U] = 0xfefefefeU;
        w[0x00ccU / 4U] = 0xfefefefeU;
        w[0x00d0U / 4U] = 0xfefefefeU;
        w[0x00d4U / 4U] = 0xfefefefeU;
    } else {
        w[0x00c8U / 4U] = 0xfefefe00U | ((ref_dpb_slot & 0xfU) << 1);
        w[0x00ccU / 4U] = 0xfefefefeU;
        w[0x00d0U / 4U] = 0xfefefefeU;
        w[0x00d4U / 4U] = 0xfefefefeU;
        w[0x00d8U / 4U] = 0xffffff03U;
        for (uint32_t off = 0x00dcU; off <= 0x0164U; off += 4U) {
            int is_hdr = (((off - 0x00d8U) % 0x10U) == 0U);
            w[off / 4U] = is_hdr ? 0xffff0100U : 0xffffffffU;
        }
        w[0x0168U / 4U] = 0xfefefefeU;
        w[0x016cU / 4U] = 0xfefefefeU;
        w[0x017cU / 4U] = 0x00060000U | (ctx->gop_poc & 0xffffU);
    }

    w[0x018cU / 4U] = 0x00000500U;
    w[0x0190U / 4U] = 0x00000200U;
    w[0x0194U / 4U] = 0x00000300U;
    w[0x0198U / 4U] = 0x00000400U;
    w[0x019cU / 4U] = ((ctx->ctu_w * (ctx->mb_h + 1U) * 3U) / 2U) << 8;
    w[0x01a0U / 4U] = (uint32_t)ctx->bs_buf.size;
    w[0x01a8U / 4U] = is_idr ? 0x0000005cU : 0x00000050U;
    w[0x01b4U / 4U] = 0x00000100U;
    w[0x01b8U / 4U] = 0x00000100U;
    w[0x01bcU / 4U] = 0x00002500U;
    w[0x01d4U / 4U] = (width > 512U) ? 0x00000400U : 0x00000200U;
    w[0x01d8U / 4U] = 0x00000100U;
    w[0x01fcU / 4U] = (ctx->n_mbs > 3600U) ? 0x16698000U : ((ctx->n_mbs > 1024U) ? 0x0a1b8000U : 0x09e34000U);

    /* Search & ME tables (+0x200 block) */
    w[0x0200U / 4U] = 0x00074002U;
    w[0x0204U / 4U] = (width > 512U) ? 0x0e004001U : 0x0f004001U;
    w[0x0208U / 4U] = (width > 512U) ? 0x01ff1fffU : 0x00ff1fffU;
    w[0x020cU / 4U] = is_idr ? 0x28080000U : 0x29282689U;
    w[0x0214U / 4U] = 0xa0000000U;
    if (is_idr) {
        w[0x022cU / 4U] = 0x1c1c0000U;
        w[0x0230U / 4U] = 0x0000001cU;
    } else {
        w[0x022cU / 4U] = 0x7f1c0800U;
        w[0x0230U / 4U] = 0x0000081cU;
        w[0x0238U / 4U] = 0x1c1c0000U;
        w[0x023cU / 4U] = 0x0000001cU;
        w[0x0244U / 4U] = 0x1c1c0000U;
        w[0x0248U / 4U] = 0x0000001cU;
    }

    /* RDO / Deblock / Mode tables (+0x300 block) */
    w[0x0304U / 4U] = is_444 ? 0x03ffffffU : 0x03fc01ffU;
    w[0x0308U / 4U] = is_idr ? 0x0e0f0f0fU : 0x0800000fU;
    w[0x030cU / 4U] = 0xffff0000U;
    w[0x0310U / 4U] = is_idr ? 0xffffffffU : 0xfffeffffU;
    w[0x0314U / 4U] = 0x00020000U;
    w[0x0318U / 4U] = is_idr ? 0xfffe0000U : 0xfffeffffU;
    w[0x031cU / 4U] = is_444 ? 0xfffefffeU : 0xffffffffU;
    w[0x0324U / 4U] = 0x00030020U;
    w[0x0328U / 4U] = 0x00000002U;
    w[0x032cU / 4U] = 0x00130009U;
    w[0x0330U / 4U] = 0xffff0000U;
    w[0x0334U / 4U] = 0x3ceaffffU;
    w[0x0338U / 4U] = 0x0000fff3U;

    /* Quant/lambda tables (+0x400..+0x490) */
    static const uint32_t h264_qtbl[] = {
        0xaaff056bU, 0x000fffaaU, 0x02020302U, 0x00000000U,
        0x036e03ffU, 0x02aa0334U, 0x0334036eU, 0x020002aaU,
        0x02aa0334U, 0x019a0200U, 0x020002aaU, 0x019a019aU,
        0x024a02aaU, 0x01c80222U, 0x0222024aU, 0x015601c8U,
        0x01c80222U, 0x01240156U, 0x015601c8U, 0x01120124U,
        0x015602aaU, 0x02aa02aaU, 0x02aa02aaU, 0x02aa02aaU,
        0x02aa02aaU, 0x02aa02aaU, 0x02aa02aaU, 0x02aa02aaU,
        0x02aa02aaU, 0x01560156U, 0x01560156U, 0x01560156U,
        0x01560156U, 0x01560156U, 0x01560156U, 0x01560156U,
        0x01560156U
    };
    for (size_t i = 0U; i < sizeof(h264_qtbl) / sizeof(h264_qtbl[0]); i++) {
        w[(0x0400U / 4U) + i] = h264_qtbl[i];
    }

    /* Slice header template (+0x500 block) */
    w[0x0500U / 4U] = (is_idr ? 0x00980000U : 0x00c00000U) | (ctx->n_mbs & 0xffffU);
    w[0x0508U / 4U] = 0x00330000U;
}

static void ce_init_and_clear_vidmem(open_nvenc_hw_ctx_t *ctx,
                                     hw_buf_t *const *bufs, size_t nbufs,
                                     open_nvenc_rm_stats_t *rm_stats)
{
    const char *enable_ce = getenv("OPEN_NVENC_ENABLE_CE");
    if (!ctx || !enable_ce || !enable_ce[0]) {
        return;
    }
    ctx->h_ce_channel = next_handle(ctx);
    ctx->h_ce_obj     = next_handle(ctx);

    nv_channel_alloc_params_t chp;
    memset(&chp, 0, sizeof(chp));
    chp.hObjectError  = ctx->h_ctx_dma;
    chp.hObjectBuffer = ctx->h_vaspace;
    chp.gpFifoOffset  = ctx->pb_buf.gpu_va + 0x24000ULL;
    chp.gpFifoEntries = 0x800U;

    uint32_t st = 0U;
    if (rm_alloc64(ctx->ctl_fd, ctx->h_client, ctx->h_device, ctx->h_ce_channel,
                   OPEN_NVENC_RM_CLASS_CHANNEL_GPFIFO, &chp, 0U, rm_stats, &st) < 0) {
        ctx->h_ce_channel = 0U;
        ctx->h_ce_obj     = 0U;
        return;
    }

    uint32_t bind_eng     = NVC0B5_ENGINE_TYPE_COPY0;
    uint16_t sched_enable = 1U;
    if (rm_ctrl(ctx->ctl_fd, ctx->h_client, ctx->h_ce_channel, NVA06F_CTRL_CMD_BIND,
                &bind_eng, sizeof(bind_eng), rm_stats, &st) < 0 ||
        rm_ctrl(ctx->ctl_fd, ctx->h_client, ctx->h_ce_channel, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE,
                &sched_enable, sizeof(sched_enable), rm_stats, &st) < 0) {
        ctx->has_ce = 0;
        return;
    }

    ctx->ce_userd_fd = open(ctx->dev_path, O_RDWR | O_CLOEXEC);
    if (ctx->ce_userd_fd < 0) {
        ctx->has_ce = 0;
        return;
    }
    nv_register_fd_t urfd;
    urfd.ctl_fd = ctx->ctl_fd;
    if (ioctl(ctx->ce_userd_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_REGISTER_FD, nv_register_fd_t), &urfd) < 0) {
        ctx->has_ce = 0;
        return;
    }
    nv_nvos33_t umap;
    memset(&umap, 0, sizeof(umap));
    umap.hClient = ctx->h_client;
    umap.hDevice = ctx->h_subdevice;
    umap.hMemory = ctx->h_ce_channel;
    umap.length  = 0x200ULL;
    umap.flags   = 0x03000000U;
    umap.fd      = ctx->ce_userd_fd;
    umap.status  = 0xffffffffU;
    if (ioctl(ctx->ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_MAP_MEMORY, nv_nvos33_t), &umap) < 0 ||
        umap.status != 0U) {
        ctx->has_ce = 0;
        return;
    }
    ctx->ce_mapped        = 1;
    ctx->ce_userd_rm_addr = umap.pLinearAddress;
    {
        size_t pg       = hw_page_size();
        size_t pg_mask  = pg - 1U;
        size_t page_off = (size_t)(umap.pLinearAddress & (uint64_t)pg_mask);
        size_t map_len  = (page_off + (size_t)umap.length + pg_mask) & ~pg_mask;
        void *map_base  = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->ce_userd_fd, 0);
        if (map_base == MAP_FAILED) {
            ctx->has_ce = 0;
            return;
        }
        ctx->ce_userd_mmap_base = map_base;
        ctx->ce_userd_mmap_len  = map_len;
        ctx->ce_userd_cpu       = (uint8_t *)map_base + page_off;
    }

    if (rm_alloc64(ctx->ctl_fd, ctx->h_client, ctx->h_ce_channel, ctx->h_ce_obj,
                   OPEN_NVENC_RM_CLASS_COPY_ENGINE, NULL, 0U, rm_stats, &st) < 0) {
        ctx->h_ce_obj = 0U;
        ctx->has_ce   = 0;
        return;
    }

    ctx->ce_pb_dw_cur = 0x1e000U / 4U;
    volatile uint32_t *pb        = (volatile uint32_t *)ctx->pb_buf.cpu;
    volatile uint32_t *ce_gpfifo = (volatile uint32_t *)((uint8_t *)ctx->pb_buf.cpu + 0x24000U);
    uint32_t start_dw            = ctx->ce_pb_dw_cur;

    pb[ctx->ce_pb_dw_cur++] = pb_hdr_inc(4U, 0x0000U, 1U);
    pb[ctx->ce_pb_dw_cur++] = OPEN_NVENC_RM_CLASS_COPY_ENGINE;

    for (size_t i = 0U; i < nbufs; i++) {
        const hw_buf_t *b = bufs[i];
        if (!b || !b->h_mem || b->size == 0ULL) {
            continue;
        }
        uint32_t sz = (uint32_t)b->size;
        pb[ctx->ce_pb_dw_cur++] = pb_hdr_inc(4U, 0x0410U, 4U);
        pb[ctx->ce_pb_dw_cur++] = sz;
        pb[ctx->ce_pb_dw_cur++] = sz;
        pb[ctx->ce_pb_dw_cur++] = sz;
        pb[ctx->ce_pb_dw_cur++] = 1U;
        pb[ctx->ce_pb_dw_cur++] = pb_hdr_inc(4U, 0x0700U, 1U);
        pb[ctx->ce_pb_dw_cur++] = 0U;
        pb[ctx->ce_pb_dw_cur++] = pb_hdr_inc(4U, 0x0708U, 1U);
        pb[ctx->ce_pb_dw_cur++] = 0x00006664U;
        pb[ctx->ce_pb_dw_cur++] = pb_hdr_inc(4U, 0x0400U, 4U);
        pb[ctx->ce_pb_dw_cur++] = (uint32_t)(b->gpu_va >> 32);
        pb[ctx->ce_pb_dw_cur++] = (uint32_t)(b->gpu_va & 0xffffffffU);
        pb[ctx->ce_pb_dw_cur++] = (uint32_t)(b->gpu_va >> 32);
        pb[ctx->ce_pb_dw_cur++] = (uint32_t)(b->gpu_va & 0xffffffffU);
        pb[ctx->ce_pb_dw_cur++] = pb_hdr_inc(4U, 0x0300U, 1U);
        pb[ctx->ce_pb_dw_cur++] = 0x00000586U;
    }

    uint32_t ce_token            = 0xce000001U;
    volatile uint32_t *ce_sema   = (volatile uint32_t *)((uint8_t *)ctx->sema_buf.cpu + 0x2000U);
    *ce_sema                     = 0U;
    uint64_t ce_sema_va          = ctx->sema_buf.gpu_va + 0x2000ULL;
    pb[ctx->ce_pb_dw_cur++]      = pb_hdr_inc(0U, 0x0010U, 4U);
    pb[ctx->ce_pb_dw_cur++]      = (uint32_t)(ce_sema_va >> 32);
    pb[ctx->ce_pb_dw_cur++]      = (uint32_t)(ce_sema_va & 0xffffffffU);
    pb[ctx->ce_pb_dw_cur++]      = ce_token;
    pb[ctx->ce_pb_dw_cur++]      = 0x01100002U;

    uint32_t dw_cnt = ctx->ce_pb_dw_cur - start_dw;
    uint64_t pb_va  = ctx->pb_buf.gpu_va + ((uint64_t)start_dw * 4ULL);
    uint32_t idx    = ctx->ce_gp_put % 0x800U;
    ce_gpfifo[idx * 2U + 0U] = (uint32_t)(pb_va & 0xfffffffcU);
    ce_gpfifo[idx * 2U + 1U] = (uint32_t)((pb_va >> 32) & 0xffU) | ((dw_cnt & 0x1fffffU) << 10);
    __sync_synchronize();
    ctx->ce_gp_put = (ctx->ce_gp_put + 1U) % 0x800U;
    ((volatile uint32_t *)ctx->ce_userd_cpu)[0x8cU / 4U] = ctx->ce_gp_put;
    __sync_synchronize();

    if (wait_sema_equal(ce_sema, ce_token, 200) == 0) {
        ctx->has_ce = 1;
    }
}

int open_nvenc_hw_create(open_nvenc_hw_ctx_t **out_ctx,
                         int ctl_fd, int dev_fd,
                         const open_nvenc_hw_init_cfg_t *cfg,
                         open_nvenc_rm_stats_t *rm_stats,
                         char *err_buf, size_t err_cap)
{
    if (!out_ctx || !cfg || ctl_fd < 0 || dev_fd < 0) {
        set_err(err_buf, err_cap, "invalid hardware create arguments");
        return -EINVAL;
    }

    open_nvenc_hw_ctx_t *ctx = (open_nvenc_hw_ctx_t *)calloc(1U, sizeof(*ctx));
    if (!ctx) {
        return -ENOMEM;
    }
    ctx->ctl_fd      = ctl_fd;
    ctx->dev_fd      = dev_fd;
    ctx->userd_fd    = -1;
    ctx->ce_userd_fd = -1;
    ctx->cfg         = *cfg;
    ctx->h_next      = 0x80000000U;

    const char *cpath = getenv("OPEN_NVENC_NVIDIACTL_PATH");
    const char *dpath = getenv("OPEN_NVENC_NVIDIA0_PATH");
    snprintf(ctx->ctl_path, sizeof(ctx->ctl_path), "%s", (cpath && cpath[0]) ? cpath : "/dev/nvidiactl");
    snprintf(ctx->dev_path, sizeof(ctx->dev_path), "%s", (dpath && dpath[0]) ? dpath : "/dev/nvidia0");

    /* Register ctl_fd on dev_fd & query version (matches trace seq 680/687) */
    nv_rm_api_version_t ver;
    memset(&ver, 0, sizeof(ver));
    ver.cmd = '1'; /* NV_RM_API_VERSION_CMD_RELAXED */
    snprintf(ver.versionString, sizeof(ver.versionString), "535.309.01");
    (void)ioctl(ctx->ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_CHECK_VERSION_STR, nv_rm_api_version_t), &ver);

    nv_register_fd_t rfd;
    rfd.ctl_fd = ctx->ctl_fd;
    if (ioctl(ctx->dev_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_REGISTER_FD, nv_register_fd_t), &rfd) < 0) {
        if (errno != EINVAL && errno != EBUSY && errno != EEXIST) {
            set_err(err_buf, err_cap, "NV_ESC_REGISTER_FD failed");
            free(ctx);
            return -EIO;
        }
    }

    /* 1. Root client (0x41) -> Device (0x80) -> Subdevice (0x2080) -> VASpace (0x70) -> SW (0xa0bc) */
    uint32_t h_cli = 0U;
    uint32_t st = 0U;
    if (rm_alloc21(ctx->ctl_fd, 0U, 0U, &h_cli, OPEN_NVENC_RM_CLASS_ROOT_CLIENT, NULL, 0U, &st) < 0) {
        snprintf(err_buf, err_cap, "RM client alloc failed st=0x%x", st);
        free(ctx);
        return -EIO;
    }
    ctx->h_client    = h_cli;
    ctx->h_device    = next_handle(ctx);
    ctx->h_subdevice = next_handle(ctx);
    ctx->h_vaspace   = next_handle(ctx);
    ctx->h_sw_sess   = next_handle(ctx);

    uint32_t dp[128];
    memset(dp, 0, sizeof(dp));
    dp[1]         = ctx->h_client;
    dp[0x60 / 4]  = 0x2aU;
    dp[0x68 / 4]  = 1U;
    uint32_t zero_params[128];
    memset(zero_params, 0, sizeof(zero_params));
    uint64_t vp70[64];
    memset(vp70, 0, sizeof(vp70));
    vp70[1] = 0x0001ffffffffffffULL;

    if (rm_alloc64(ctx->ctl_fd, ctx->h_client, ctx->h_client, ctx->h_device,
                   OPEN_NVENC_RM_CLASS_DEVICE_0, dp, 0U, rm_stats, &st) < 0 ||
        rm_alloc64(ctx->ctl_fd, ctx->h_client, ctx->h_device, ctx->h_subdevice,
                   OPEN_NVENC_RM_CLASS_SUBDEVICE_0, zero_params, 0U, rm_stats, &st) < 0 ||
        rm_alloc64(ctx->ctl_fd, ctx->h_client, ctx->h_device, ctx->h_vaspace,
                   OPEN_NVENC_RM_CLASS_VASPACE, vp70, 0U, rm_stats, &st) < 0) {
        snprintf(err_buf, err_cap, "RM dev/subdev/vaspace alloc failed st=0x%x", st);
        goto fail;
    }
    (void)rm_alloc64(ctx->ctl_fd, ctx->h_client, ctx->h_subdevice, ctx->h_sw_sess,
                     OPEN_NVENC_RM_CLASS_NVENC_SW_SESSION, NULL, 0U, rm_stats, &st);

    /* 2. Semaphore & Error buffers */
    if (alloc_buf_sysmem(ctx, &ctx->sema_buf, 0x3000ULL, 1, rm_stats, err_buf, err_cap) < 0 ||
        alloc_buf_nvos02(ctx, &ctx->err_buf, 0x5010U, 0x9fULL, 0x1000U,
                         ctx->h_vaspace, 0xa0ULL, 0x10U, rm_stats, err_buf, err_cap) < 0) {
        goto fail;
    }

    /* 3. Channel GPFIFO (0xc06f) + NV50_MEMORY_VIRTUAL (0x50a0) + Pushbuffer (0x29000 B sysmem) + NVC2B7 (0xc2b7) */
    ctx->h_ctx_dma  = next_handle(ctx);
    ctx->h_mem_virt = next_handle(ctx);
    ctx->h_channel  = next_handle(ctx);
    ctx->h_nvenc    = next_handle(ctx);

    nv_ctx_dma_alloc_params_t cdp;
    memset(&cdp, 0, sizeof(cdp));
    cdp.hSubDevice = 0U;
    cdp.flags      = 0x20100000U;
    cdp.hMemory    = ctx->err_buf.h_mem;
    cdp.offset     = 0ULL;
    cdp.limit      = 0x9fULL;
    if (rm_alloc64(ctx->ctl_fd, ctx->h_client, ctx->h_device, ctx->h_ctx_dma,
                   OPEN_NVENC_RM_CLASS_CTXDMA, &cdp, 0U, rm_stats, &st) < 0) {
        snprintf(err_buf, err_cap, "RM ctxdma alloc failed st=0x%x", st);
        goto fail;
    }

    nv_mem_alloc_params_t vp;
    memset(&vp, 0, sizeof(vp));
    vp.owner  = 0x70757368U;
    vp.pad0   = 6U;
    vp.type   = 0x0008c000U;
    vp.attr   = 0x01000000U;
    vp.attr2  = 8U;
    vp.size   = 0x29000ULL;
    vp.offset = 0x100030000ULL;
    vp.limit  = 0x28fffULL;
    if (rm_alloc64(ctx->ctl_fd, ctx->h_client, ctx->h_device, ctx->h_mem_virt,
                   OPEN_NVENC_RM_CLASS_MEM_VIRTUAL, &vp, 0U, rm_stats, &st) < 0) {
        snprintf(err_buf, err_cap, "RM 0x50a0 virt alloc failed st=0x%x", st);
        ctx->h_mem_virt = 0U;
        goto fail;
    }

    /* Pushbuffer + GPFIFO sysmem (0x29000 B: PB @ +0x0..0x20000, GPFIFO @ +0x20000..0x24000) */
    if (alloc_buf_nvos02(ctx, &ctx->pb_buf, 0x2010U, 0x28fffULL, 0x29000U,
                         ctx->h_mem_virt, 0x29000ULL, 0U, rm_stats, err_buf, err_cap) < 0) {
        goto fail;
    }

    nv_channel_alloc_params_t chp;
    memset(&chp, 0, sizeof(chp));
    chp.hObjectError  = ctx->h_ctx_dma;
    chp.hObjectBuffer = ctx->h_vaspace;
    chp.gpFifoOffset  = ctx->pb_buf.gpu_va + 0x20000ULL;
    chp.gpFifoEntries = 0x800U;

    if (rm_alloc64(ctx->ctl_fd, ctx->h_client, ctx->h_device, ctx->h_channel,
                   OPEN_NVENC_RM_CLASS_CHANNEL_GPFIFO, &chp, 0U, rm_stats, &st) < 0) {
        snprintf(err_buf, err_cap, "Channel NVC06F alloc failed st=0x%x", st);
        goto fail;
    }

    uint32_t bind_eng = NVC2B7_ENGINE_TYPE_NVENC0;
    if (rm_ctrl(ctx->ctl_fd, ctx->h_client, ctx->h_channel, NVA06F_CTRL_CMD_BIND,
                &bind_eng, sizeof(bind_eng), rm_stats, &st) < 0) {
        snprintf(err_buf, err_cap, "Channel BIND NVENC0 failed st=0x%x", st);
        goto fail;
    }
    uint16_t sched_enable = 1U;
    if (rm_ctrl(ctx->ctl_fd, ctx->h_client, ctx->h_channel, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE,
                &sched_enable, sizeof(sched_enable), rm_stats, &st) < 0) {
        snprintf(err_buf, err_cap, "Channel GPFIFO_SCHEDULE failed st=0x%x", st);
        goto fail;
    }

    /* Map USERD BAR0 window on fresh /dev/nvidia0 fd */
    ctx->userd_fd = open(ctx->dev_path, O_RDWR | O_CLOEXEC);
    if (ctx->userd_fd < 0) {
        set_err(err_buf, err_cap, "open nvidia0 for USERD failed");
        goto fail;
    }
    nv_register_fd_t urfd;
    urfd.ctl_fd = ctx->ctl_fd;
    if (ioctl(ctx->userd_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_REGISTER_FD, nv_register_fd_t), &urfd) < 0) {
        set_err(err_buf, err_cap, "REGISTER_FD for USERD failed");
        goto fail;
    }
    nv_nvos33_t umap;
    memset(&umap, 0, sizeof(umap));
    umap.hClient = ctx->h_client;
    umap.hDevice = ctx->h_subdevice;
    umap.hMemory = ctx->h_channel;
    umap.offset  = 0ULL;
    umap.length  = 0x200ULL;
    umap.flags   = 0x03000000U;
    umap.fd      = ctx->userd_fd;
    umap.status  = 0xffffffffU;
    if (ioctl(ctx->ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_MAP_MEMORY, nv_nvos33_t), &umap) < 0 ||
        umap.status != 0U) {
        snprintf(err_buf, err_cap, "RM_MAP_MEMORY USERD failed st=0x%x errno=%d", umap.status, errno);
        goto fail;
    }
    ctx->channel_mapped = 1;
    ctx->userd_rm_addr  = umap.pLinearAddress;
    {
        size_t pg       = hw_page_size();
        size_t pg_mask  = pg - 1U;
        size_t page_off = (size_t)(umap.pLinearAddress & (uint64_t)pg_mask);
        size_t map_len  = (page_off + (size_t)umap.length + pg_mask) & ~pg_mask;
        void *map_base  = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->userd_fd, 0);
        if (map_base == MAP_FAILED) {
            set_err(err_buf, err_cap, "mmap USERD failed");
            goto fail;
        }
        ctx->userd_mmap_base = map_base;
        ctx->userd_mmap_len  = map_len;
        ctx->userd_cpu       = (uint8_t *)map_base + page_off;
    }

    /* Allocate NVC2B7 engine on channel */
    if (rm_alloc64(ctx->ctl_fd, ctx->h_client, ctx->h_channel, ctx->h_nvenc,
                   OPEN_NVENC_RM_CLASS_VIDEO_ENCODER, NULL, 0U, rm_stats, &st) < 0) {
        snprintf(err_buf, err_cap, "NVC2B7 engine alloc failed st=0x%x", st);
        goto fail;
    }

    uint32_t cls_eng_id[4] = { ctx->h_nvenc, 0U, 0U, 0U };
    (void)rm_ctrl(ctx->ctl_fd, ctx->h_client, ctx->h_channel, NV906F_CTRL_CMD_GET_CLASS_ENGINEID,
                  cls_eng_id, sizeof(cls_eng_id), rm_stats, &st);

    /* 4. Allocate descriptor, QM/RC, status, bitstream, io_aux, scratch & double-buffered surface/coloc buffers */
    ctx->aligned_w    = (cfg->width + 31U) & ~31U;
    ctx->aligned_h    = (cfg->height + 31U) & ~31U;
    ctx->ctu_w        = ctx->aligned_w >> 5;
    ctx->ctu_h        = ctx->aligned_h >> 5;
    ctx->n_ctus       = ctx->ctu_w * ctx->ctu_h;
    ctx->mb_w         = (cfg->width + 15U) >> 4;
    ctx->mb_h         = (cfg->height + 15U) >> 4;
    ctx->n_mbs        = ctx->mb_w * ctx->mb_h;
    int is_444_fmt    = (cfg->chroma_format_idc == 3U);
    ctx->uv_aligned_h = is_444_fmt ? ctx->aligned_h : (((ctx->aligned_h >> 1) + 31U) & ~31U);

    uint32_t bpp       = (cfg->bit_depth == 10U) ? 2U : 1U;
    ctx->surface_pitch = ctx->aligned_w * bpp;
    ctx->surface_size  = is_444_fmt
                             ? ((size_t)ctx->surface_pitch * (size_t)ctx->aligned_h * 3U)
                             : ((size_t)ctx->surface_pitch * ((size_t)ctx->aligned_h + (size_t)ctx->uv_aligned_h));
    if (ctx->surface_size < 0x18000U) {
        ctx->surface_size = 0x18000U;
    }
    ctx->surface_size = (ctx->surface_size + 0xfffU) & ~(size_t)0xfffU;

    size_t bs_size = ctx->surface_size * 2U;
    if (bs_size < 0x65000U) {
        bs_size = 0x65000U;
    }
    bs_size = (bs_size + 0xfffU) & ~(size_t)0xfffU;

    size_t scratch_size = cfg->is_h264
                              ? ((size_t)ctx->n_mbs * 384U)
                              : ((size_t)ctx->n_ctus * 1024U);
    size_t min_scratch  = cfg->is_h264 ? 0x20000U : 0x10000U;
    if (scratch_size < min_scratch) {
        scratch_size = min_scratch;
    }
    scratch_size = (scratch_size + 0xffffU) & ~(size_t)0xffffU;

    size_t io_aux_size = 0x8000U + ((size_t)ctx->n_ctus * 80U);
    if (io_aux_size < 0x9000U) {
        io_aux_size = 0x9000U;
    }
    io_aux_size = (io_aux_size + 0xfffU) & ~(size_t)0xfffU;

    size_t coloc_size = (size_t)ctx->n_ctus * 256U;
    if (coloc_size < 0x20000U) {
        coloc_size = 0x20000U;
    }
    coloc_size = (coloc_size + 0xfffU) & ~(size_t)0xfffU;

    if (alloc_buf_sysmem(ctx, &ctx->pic_setup_buf, 0xe000ULL, 1, rm_stats, err_buf, err_cap) < 0 ||
        alloc_buf_vidmem(ctx, &ctx->scratch_buf, scratch_size, rm_stats, err_buf, err_cap) < 0 ||
        alloc_buf_sysmem(ctx, &ctx->qm_rc_buf, 0x1000ULL, 1, rm_stats, err_buf, err_cap) < 0 ||
        alloc_buf_sysmem(ctx, &ctx->status_buf, 0x1000ULL, 1, rm_stats, err_buf, err_cap) < 0 ||
        alloc_buf_sysmem(ctx, &ctx->bs_buf, bs_size, 1, rm_stats, err_buf, err_cap) < 0 ||
        alloc_buf_sysmem(ctx, &ctx->io_aux_buf, io_aux_size, 1, rm_stats, err_buf, err_cap) < 0 ||
        alloc_buf_sysmem(ctx, &ctx->input_buf, ctx->surface_size, 1, rm_stats, err_buf, err_cap) < 0 ||
        alloc_buf_vidmem(ctx, &ctx->recon_buf[0], ctx->surface_size, rm_stats, err_buf, err_cap) < 0 ||
        alloc_buf_vidmem(ctx, &ctx->recon_buf[1], ctx->surface_size, rm_stats, err_buf, err_cap) < 0 ||
        alloc_buf_vidmem(ctx, &ctx->coloc_mv_buf[0], coloc_size, rm_stats, err_buf, err_cap) < 0 ||
        alloc_buf_vidmem(ctx, &ctx->coloc_mv_buf[1], coloc_size, rm_stats, err_buf, err_cap) < 0) {
        goto fail;
    }
    if (cfg->is_h264) {
        size_t h264_mv_sz = (size_t)ctx->n_mbs * 512U;
        if (h264_mv_sz < 0x50000U) {
            h264_mv_sz = 0x50000U;
        }
        h264_mv_sz = (h264_mv_sz + 0xffffU) & ~(size_t)0xffffU;
        if (alloc_buf_vidmem(ctx, &ctx->h264_mv_buf, h264_mv_sz, rm_stats, err_buf, err_cap) < 0) {
            goto fail;
        }
    }

    init_qm_rc_buf((uint32_t *)ctx->qm_rc_buf.cpu);
    rc_init_state(ctx);

    if (posix_memalign((void **)&ctx->stage_buf, 64U, ctx->surface_size) != 0) {
        ctx->stage_buf = (uint8_t *)calloc(1U, ctx->surface_size);
    } else {
        memset(ctx->stage_buf, 0, ctx->surface_size);
    }

    /* Optional NVC0B5 Copy Engine vidmem zero-clear (skips gracefully if unavailable) */
    {
        hw_buf_t *const ce_clear_list[] = {
            &ctx->scratch_buf, &ctx->recon_buf[0], &ctx->recon_buf[1],
            &ctx->coloc_mv_buf[0], &ctx->coloc_mv_buf[1],
            cfg->is_h264 ? &ctx->h264_mv_buf : NULL
        };
        ce_init_and_clear_vidmem(ctx, ce_clear_list,
                                 sizeof(ce_clear_list) / sizeof(ce_clear_list[0]), rm_stats);
    }

    /* 5. Submit SET_OBJECT = 0xc2b7 & init ctx (0x0204 = 0x5ffffffe) + verify GPGet advances */
    volatile uint32_t *pb = (volatile uint32_t *)ctx->pb_buf.cpu;
    uint32_t start = ctx->pb_dw_cur;
    pb[ctx->pb_dw_cur++] = pb_hdr_inc(4U, 0x0000U, 1U);
    pb[ctx->pb_dw_cur++] = OPEN_NVENC_RM_CLASS_VIDEO_ENCODER;
    pb[ctx->pb_dw_cur++] = pb_hdr_inc(4U, 0x0204U, 1U);
    pb[ctx->pb_dw_cur++] = 0x5ffffffeU;
    submit_pb_range(ctx, start, ctx->pb_dw_cur - start, 0);

    volatile uint32_t *userd = (volatile uint32_t *)ctx->userd_cpu;
    if (wait_sema_equal(&userd[0x88U / 4U], ctx->gp_put, 500) < 0) {
        snprintf(err_buf, err_cap, "NVC2B7 init GPGet stall (gp_get=%u gp_put=%u)",
                 userd[0x88U / 4U], ctx->gp_put);
        goto fail;
    }

    if (rm_stats) {
        rm_stats->h_client         = ctx->h_client;
        rm_stats->h_device         = ctx->h_device;
        rm_stats->h_subdevice      = ctx->h_subdevice;
        rm_stats->h_channel        = ctx->h_channel;
        rm_stats->h_nvenc_obj      = ctx->h_nvenc;
        rm_stats->nvenc_class      = OPEN_NVENC_RM_CLASS_VIDEO_ENCODER;
        rm_stats->rm_connected     = 1;
        rm_stats->hw_encode_active = 1;
    }

    *out_ctx = ctx;
    return 0;

fail:
    open_nvenc_hw_destroy(ctx, rm_stats);
    return -1;
}

void open_nvenc_hw_destroy(open_nvenc_hw_ctx_t *ctx, open_nvenc_rm_stats_t *rm_stats)
{
    if (!ctx) {
        return;
    }
    if (ctx->h_ce_obj && ctx->h_ce_channel) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, ctx->h_ce_channel, ctx->h_ce_obj, rm_stats);
        ctx->h_ce_obj = 0U;
    }
    if (ctx->ce_userd_mmap_base && ctx->ce_userd_mmap_base != MAP_FAILED) {
        munmap(ctx->ce_userd_mmap_base, ctx->ce_userd_mmap_len ? ctx->ce_userd_mmap_len : 0x1000U);
    } else if (ctx->ce_userd_cpu && ctx->ce_userd_cpu != MAP_FAILED) {
        munmap(ctx->ce_userd_cpu, 0x1000U);
    }
    ctx->ce_userd_mmap_base = NULL;
    ctx->ce_userd_mmap_len  = 0U;
    ctx->ce_userd_cpu       = NULL;
    if (ctx->ce_userd_fd >= 0) {
        if (ctx->ce_mapped) {
            nv_nvos33_t ureq;
            memset(&ureq, 0, sizeof(ureq));
            ureq.hClient        = ctx->h_client;
            ureq.hDevice        = ctx->h_subdevice;
            ureq.hMemory        = ctx->h_ce_channel;
            ureq.pLinearAddress = ctx->ce_userd_rm_addr;
            ureq.flags          = 0x03000000U;
            (void)ioctl(ctx->ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_UNMAP_MEMORY, nv_nvos33_t), &ureq);
            ctx->ce_mapped         = 0;
            ctx->ce_userd_rm_addr  = 0ULL;
        }
        close(ctx->ce_userd_fd);
        ctx->ce_userd_fd = -1;
    }
    if (ctx->h_ce_channel) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, ctx->h_device, ctx->h_ce_channel, rm_stats);
        ctx->h_ce_channel = 0U;
    }

    free_hw_buf(ctx, &ctx->h264_mv_buf, ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->coloc_mv_buf[1], ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->coloc_mv_buf[0], ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->recon_buf[1], ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->recon_buf[0], ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->input_buf, ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->io_aux_buf, ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->bs_buf, ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->status_buf, ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->qm_rc_buf, ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->scratch_buf, ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->pic_setup_buf, ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->sema_buf, ctx->h_vaspace, rm_stats);
    free_hw_buf(ctx, &ctx->err_buf, ctx->h_vaspace, rm_stats);
    free(ctx->stage_buf);
    ctx->stage_buf = NULL;

    if (ctx->h_nvenc) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, ctx->h_channel, ctx->h_nvenc, rm_stats);
        ctx->h_nvenc = 0U;
    }
    if (ctx->userd_mmap_base && ctx->userd_mmap_base != MAP_FAILED) {
        munmap(ctx->userd_mmap_base, ctx->userd_mmap_len ? ctx->userd_mmap_len : 0x1000U);
    } else if (ctx->userd_cpu && ctx->userd_cpu != MAP_FAILED) {
        munmap(ctx->userd_cpu, 0x1000U);
    }
    ctx->userd_mmap_base = NULL;
    ctx->userd_mmap_len  = 0U;
    ctx->userd_cpu       = NULL;
    if (ctx->userd_fd >= 0) {
        if (ctx->channel_mapped) {
            nv_nvos33_t ureq;
            memset(&ureq, 0, sizeof(ureq));
            ureq.hClient        = ctx->h_client;
            ureq.hDevice        = ctx->h_subdevice;
            ureq.hMemory        = ctx->h_channel;
            ureq.pLinearAddress = ctx->userd_rm_addr;
            ureq.flags          = 0x03000000U;
            (void)ioctl(ctx->ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_UNMAP_MEMORY, nv_nvos33_t), &ureq);
            ctx->channel_mapped = 0;
            ctx->userd_rm_addr  = 0ULL;
        }
        close(ctx->userd_fd);
        ctx->userd_fd = -1;
    }
    if (ctx->h_channel) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, ctx->h_device, ctx->h_channel, rm_stats);
        ctx->h_channel = 0U;
    }
    free_hw_buf(ctx, &ctx->pb_buf, ctx->h_mem_virt ? ctx->h_mem_virt : ctx->h_vaspace, rm_stats);
    if (ctx->h_mem_virt) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, ctx->h_device, ctx->h_mem_virt, rm_stats);
        ctx->h_mem_virt = 0U;
    }
    if (ctx->h_ctx_dma) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, ctx->h_device, ctx->h_ctx_dma, rm_stats);
    }
    if (ctx->h_sw_sess) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, ctx->h_subdevice, ctx->h_sw_sess, rm_stats);
    }
    if (ctx->h_vaspace) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, ctx->h_device, ctx->h_vaspace, rm_stats);
    }
    if (ctx->h_subdevice) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, ctx->h_device, ctx->h_subdevice, rm_stats);
    }
    if (ctx->h_device) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, ctx->h_client, ctx->h_device, rm_stats);
    }
    if (ctx->h_client) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, 0U, ctx->h_client, rm_stats);
    }
    free(ctx);
}

/* Compute NVIDIA Pascal 16Bx2 (64B x 8-row GOB, block_height = 2 GOBs = 16 rows) offset. */
static inline size_t gob_bl_offset(uint32_t x_byte, uint32_t y, uint32_t pitch_bytes)
{
    uint32_t width_in_gobs = pitch_bytes >> 6;
    if (width_in_gobs == 0U) {
        width_in_gobs = 1U;
    }
    uint32_t gob_x    = x_byte >> 6;
    uint32_t gob_y    = y >> 3;
    uint32_t by_blk   = gob_y >> 1; /* 2 GOBs per vertical block */
    uint32_t by_in    = gob_y & 1U;
    uint32_t gob_idx  = (by_blk * width_in_gobs + gob_x) * 2U + by_in;
    uint32_t in_gob   = (((x_byte >> 5) & 1U) << 8)
                      | (((y >> 2) & 1U) << 7)
                      | (((x_byte >> 4) & 1U) << 6)
                      | ((y & 3U) << 4)
                      | (x_byte & 15U);
    return (size_t)gob_idx * 512U + (size_t)in_gob;
}

/* Write a full 64-byte GOB row into Pascal 16Bx2 layout via four 16-byte stores. */
static inline void bl16_store_gob_row64(uint8_t *gob_base, uint32_t row_in_gob, const uint8_t *src64)
{
    uint8_t *r0 = gob_base + row_in_gob;
    memcpy(r0 + 0x000U, src64 + 0U,  16U);
    memcpy(r0 + 0x040U, src64 + 16U, 16U);
    memcpy(r0 + 0x100U, src64 + 32U, 16U);
    memcpy(r0 + 0x140U, src64 + 48U, 16U);
}

static void swizzle_plane_linear_to_bl16(uint8_t *dst, size_t dst_avail,
                                         const uint8_t *src, uint32_t src_pitch,
                                         uint32_t w_bytes, uint32_t elem_bytes,
                                         uint32_t h, uint32_t dst_pitch, uint32_t dst_h)
{
    if (dst_pitch < 64U || (dst_pitch & 63U) != 0U || (dst_h & 15U) != 0U || w_bytes < elem_bytes) {
        for (uint32_t y = 0U; y < dst_h; y++) {
            uint32_t sy = (y < h) ? y : (h - 1U);
            const uint8_t *srow = src + (size_t)sy * src_pitch;
            for (uint32_t xb = 0U; xb < dst_pitch; xb++) {
                uint32_t sxb = (xb < w_bytes) ? xb : (w_bytes - elem_bytes + (xb % elem_bytes));
                size_t off = gob_bl_offset(xb, y, dst_pitch);
                if (off < dst_avail) {
                    dst[off] = srow[sxb];
                }
            }
        }
        return;
    }

    uint32_t width_in_gobs = dst_pitch >> 6;
    uint32_t n_gob_y       = dst_h >> 3;
    uint32_t full_gobs     = w_bytes >> 6;
    if (full_gobs > width_in_gobs) {
        full_gobs = width_in_gobs;
    }

    for (uint32_t gob_y = 0U; gob_y < n_gob_y; gob_y++) {
        uint32_t by_blk      = gob_y >> 1;
        uint32_t by_in       = gob_y & 1U;
        size_t   row_gob_idx = (size_t)by_blk * (size_t)width_in_gobs * 2U + (size_t)by_in;
        for (uint32_t y_in = 0U; y_in < 8U; y_in++) {
            uint32_t y          = (gob_y << 3) | y_in;
            uint32_t sy         = (y < h) ? y : (h - 1U);
            const uint8_t *srow = src + (size_t)sy * src_pitch;
            uint32_t row_in_gob = (((y_in >> 2) & 1U) << 7) | ((y_in & 3U) << 4);

            for (uint32_t gx = 0U; gx < full_gobs; gx++) {
                size_t g_off = (row_gob_idx + (size_t)gx * 2U) * 512U;
                if (g_off + 512U <= dst_avail) {
                    bl16_store_gob_row64(dst + g_off, row_in_gob, srow + ((size_t)gx << 6));
                }
            }
            for (uint32_t gx = full_gobs; gx < width_in_gobs; gx++) {
                uint8_t pad64[64];
                uint32_t base_x = gx << 6;
                uint32_t rem    = (base_x < w_bytes) ? (w_bytes - base_x) : 0U;
                if (rem > 0U) {
                    memcpy(pad64, srow + base_x, rem);
                }
                const uint8_t *last_px = srow + (w_bytes - elem_bytes);
                for (uint32_t off = rem; off < 64U; off += elem_bytes) {
                    if (elem_bytes == 1U) {
                        pad64[off] = last_px[0];
                    } else if (elem_bytes == 2U && off + 2U <= 64U) {
                        memcpy(&pad64[off], last_px, 2U);
                    } else if (off + 4U <= 64U) {
                        memcpy(&pad64[off], last_px, 4U);
                    }
                }
                size_t g_off = (row_gob_idx + (size_t)gx * 2U) * 512U;
                if (g_off + 512U <= dst_avail) {
                    bl16_store_gob_row64(dst + g_off, row_in_gob, pad64);
                }
            }
        }
    }
}

static void swizzle_uv444_to_bl16(uint8_t *dst_uv, size_t uv_avail,
                                  const uint8_t *src_u, const uint8_t *src_v,
                                  uint32_t src_pitch, uint32_t w, uint32_t h,
                                  uint32_t uv_pitch, uint32_t ah, int is_10b)
{
    uint32_t width_in_gobs = uv_pitch >> 6;
    uint32_t n_gob_y       = ah >> 3;
    if (width_in_gobs == 0U || n_gob_y == 0U) {
        return;
    }

    for (uint32_t gob_y = 0U; gob_y < n_gob_y; gob_y++) {
        uint32_t by_blk      = gob_y >> 1;
        uint32_t by_in       = gob_y & 1U;
        size_t   row_gob_idx = (size_t)by_blk * (size_t)width_in_gobs * 2U + (size_t)by_in;
        for (uint32_t y_in = 0U; y_in < 8U; y_in++) {
            uint32_t y          = (gob_y << 3) | y_in;
            uint32_t sy         = (y < h) ? y : (h - 1U);
            uint32_t row_in_gob = (((y_in >> 2) & 1U) << 7) | ((y_in & 3U) << 4);

            if (is_10b) {
                const uint16_t *su16 = (const uint16_t *)(src_u + (size_t)sy * src_pitch);
                const uint16_t *sv16 = (const uint16_t *)(src_v + (size_t)sy * src_pitch);
                uint32_t last_uv     = (uint32_t)su16[w - 1U] | ((uint32_t)sv16[w - 1U] << 16);
                uint32_t full_gobs   = w >> 4; /* 16 pixels * 4 B = 64 B */
                if (full_gobs > width_in_gobs) {
                    full_gobs = width_in_gobs;
                }
                for (uint32_t gx = 0U; gx < full_gobs; gx++) {
                    uint32_t buf[16];
                    uint32_t x0 = gx << 4;
                    for (uint32_t k = 0U; k < 16U; k++) {
                        buf[k] = (uint32_t)su16[x0 + k] | ((uint32_t)sv16[x0 + k] << 16);
                    }
                    size_t g_off = (row_gob_idx + (size_t)gx * 2U) * 512U;
                    if (g_off + 512U <= uv_avail) {
                        bl16_store_gob_row64(dst_uv + g_off, row_in_gob, (const uint8_t *)buf);
                    }
                }
                for (uint32_t gx = full_gobs; gx < width_in_gobs; gx++) {
                    uint32_t buf[16];
                    uint32_t x0 = gx << 4;
                    for (uint32_t k = 0U; k < 16U; k++) {
                        uint32_t x = x0 + k;
                        buf[k] = (x < w) ? ((uint32_t)su16[x] | ((uint32_t)sv16[x] << 16)) : last_uv;
                    }
                    size_t g_off = (row_gob_idx + (size_t)gx * 2U) * 512U;
                    if (g_off + 512U <= uv_avail) {
                        bl16_store_gob_row64(dst_uv + g_off, row_in_gob, (const uint8_t *)buf);
                    }
                }
            } else {
                const uint8_t *su8 = src_u + (size_t)sy * src_pitch;
                const uint8_t *sv8 = src_v + (size_t)sy * src_pitch;
                uint16_t last_uv   = (uint16_t)su8[w - 1U] | ((uint16_t)sv8[w - 1U] << 8);
                uint32_t full_gobs = w >> 5; /* 32 pixels * 2 B = 64 B */
                if (full_gobs > width_in_gobs) {
                    full_gobs = width_in_gobs;
                }
                for (uint32_t gx = 0U; gx < full_gobs; gx++) {
                    uint16_t buf[32];
                    uint32_t x0 = gx << 5;
                    for (uint32_t k = 0U; k < 32U; k++) {
                        buf[k] = (uint16_t)su8[x0 + k] | ((uint16_t)sv8[x0 + k] << 8);
                    }
                    size_t g_off = (row_gob_idx + (size_t)gx * 2U) * 512U;
                    if (g_off + 512U <= uv_avail) {
                        bl16_store_gob_row64(dst_uv + g_off, row_in_gob, (const uint8_t *)buf);
                    }
                }
                for (uint32_t gx = full_gobs; gx < width_in_gobs; gx++) {
                    uint16_t buf[32];
                    uint32_t x0 = gx << 5;
                    for (uint32_t k = 0U; k < 32U; k++) {
                        uint32_t x = x0 + k;
                        buf[k] = (x < w) ? ((uint16_t)su8[x] | ((uint16_t)sv8[x] << 8)) : last_uv;
                    }
                    size_t g_off = (row_gob_idx + (size_t)gx * 2U) * 512U;
                    if (g_off + 512U <= uv_avail) {
                        bl16_store_gob_row64(dst_uv + g_off, row_in_gob, (const uint8_t *)buf);
                    }
                }
            }
        }
    }
}

static inline void argb_convert_px(const uint8_t *srow, uint32_t x,
                                   int is_argb, int is_rgb10,
                                   int use_bt601, int full_range,
                                   uint16_t *out_y10, uint16_t *out_u10, uint16_t *out_v10)
{
    int r10, g10, b10;
    if (is_rgb10) {
        uint32_t p32;
        memcpy(&p32, srow + (size_t)x * 4U, sizeof(p32));
        if (is_argb) {
            r10 = (int)((p32 >> 20) & 0x3ffU);
            g10 = (int)((p32 >> 10) & 0x3ffU);
            b10 = (int)(p32 & 0x3ffU);
        } else {
            b10 = (int)((p32 >> 20) & 0x3ffU);
            g10 = (int)((p32 >> 10) & 0x3ffU);
            r10 = (int)(p32 & 0x3ffU);
        }
    } else {
        const uint8_t *px = srow + (size_t)x * 4U;
        r10 = (int)(is_argb ? px[2] : px[0]) << 2;
        g10 = (int)px[1] << 2;
        b10 = (int)(is_argb ? px[0] : px[2]) << 2;
    }

    int yy, uu, vv;
    if (use_bt601) {
        if (full_range) {
            yy = ((77 * r10 + 150 * g10 + 29 * b10 + 128) >> 8);
            uu = ((-43 * r10 - 85 * g10 + 128 * b10 + 128) >> 8) + 512;
            vv = ((128 * r10 - 107 * g10 - 21 * b10 + 128) >> 8) + 512;
        } else {
            yy = ((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 64;
            uu = ((-38 * r10 - 74 * g10 + 112 * b10 + 128) >> 8) + 512;
            vv = ((112 * r10 - 94 * g10 - 18 * b10 + 128) >> 8) + 512;
        }
    } else {
        if (full_range) {
            yy = ((54 * r10 + 183 * g10 + 19 * b10 + 128) >> 8);
            uu = ((-29 * r10 - 99 * g10 + 128 * b10 + 128) >> 8) + 512;
            vv = ((128 * r10 - 116 * g10 - 12 * b10 + 128) >> 8) + 512;
        } else {
            yy = ((47 * r10 + 157 * g10 + 16 * b10 + 128) >> 8) + 64;
            uu = ((-26 * r10 - 86 * g10 + 112 * b10 + 128) >> 8) + 512;
            vv = ((112 * r10 - 102 * g10 - 10 * b10 + 128) >> 8) + 512;
        }
    }
    *out_y10 = (uint16_t)(yy < 0 ? 0 : (yy > 1023 ? 1023 : yy)) << 6;
    *out_u10 = (uint16_t)(uu < 0 ? 0 : (uu > 1023 ? 1023 : uu)) << 6;
    *out_v10 = (uint16_t)(vv < 0 ? 0 : (vv > 1023 ? 1023 : vv)) << 6;
}

static inline uint8_t clamp_u8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void copy_argb_to_bl16(open_nvenc_hw_ctx_t *ctx, uint8_t *dst,
                              const uint8_t *src, uint32_t src_pitch,
                              NV_ENC_BUFFER_FORMAT fmt)
{
    uint32_t w          = ctx->cfg.width;
    uint32_t h          = ctx->cfg.height;
    uint32_t ah         = ctx->aligned_h;
    uint32_t dst_pitch  = ctx->surface_pitch;
    uint8_t *dst_uv     = dst + (size_t)dst_pitch * (size_t)ah;
    size_t   uv_avail   = ctx->surface_size - (size_t)dst_pitch * (size_t)ah;
    uint32_t uv_pitch   = (ctx->cfg.chroma_format_idc == 3U) ? (dst_pitch * 2U) : dst_pitch;
    int      is_444     = (ctx->cfg.chroma_format_idc == 3U);
    int      is_10b     = (ctx->cfg.bit_depth == 10U);
    int      is_argb    = (fmt == NV_ENC_BUFFER_FORMAT_ARGB || fmt == NV_ENC_BUFFER_FORMAT_ARGB10);
    int      is_rgb10   = (fmt == NV_ENC_BUFFER_FORMAT_ARGB10 || fmt == NV_ENC_BUFFER_FORMAT_ABGR10);
    int      use_bt601  = (ctx->cfg.vui_matrix_coeffs == 5U || ctx->cfg.vui_matrix_coeffs == 6U) ? 1 : 0;
    int      full_range = ctx->cfg.vui_full_range ? 1 : 0;

    uint32_t y_gobs     = dst_pitch >> 6;
    uint32_t uv_gobs    = uv_pitch >> 6;
    uint32_t n_gob_y    = ah >> 3;

    if (!is_10b && !is_rgb10) {
        const uint32_t r_off = is_argb ? 2U : 0U;
        const uint32_t b_off = is_argb ? 0U : 2U;
        int cy0, cy1, cy2, cy_off;
        int cu0, cu1, cu2, cu_off;
        int cv0, cv1, cv2, cv_off;
        if (use_bt601) {
            if (full_range) {
                cy0 =  77; cy1 =  150; cy2 =  29; cy_off = 0;
                cu0 = -43; cu1 =  -85; cu2 = 128; cu_off = 128;
                cv0 = 128; cv1 = -107; cv2 = -21; cv_off = 128;
            } else {
                cy0 =  66; cy1 = 129; cy2 =  25; cy_off = 16;
                cu0 = -38; cu1 = -74; cu2 = 112; cu_off = 128;
                cv0 = 112; cv1 = -94; cv2 = -18; cv_off = 128;
            }
        } else {
            if (full_range) {
                cy0 =  54; cy1 =  183; cy2 =  19; cy_off = 0;
                cu0 = -29; cu1 =  -99; cu2 = 128; cu_off = 128;
                cv0 = 128; cv1 = -116; cv2 = -12; cv_off = 128;
            } else {
                cy0 =  47; cy1 =  157; cy2 =  16; cy_off = 16;
                cu0 = -26; cu1 =  -86; cu2 = 112; cu_off = 128;
                cv0 = 112; cv1 = -102; cv2 = -10; cv_off = 128;
            }
        }
        uint32_t full_gobs = w >> 6;
        if (full_gobs > y_gobs) {
            full_gobs = y_gobs;
        }

        for (uint32_t gob_y = 0U; gob_y < n_gob_y; gob_y++) {
            uint32_t by_blk     = gob_y >> 1;
            uint32_t by_in      = gob_y & 1U;
            size_t   y_gob_base = (size_t)by_blk * (size_t)y_gobs * 2U + (size_t)by_in;
            size_t   uv444_base = (size_t)by_blk * (size_t)uv_gobs * 2U + (size_t)by_in;
            for (uint32_t y_in = 0U; y_in < 8U; y_in++) {
                uint32_t y          = (gob_y << 3) | y_in;
                uint32_t sy         = (y < h) ? y : (h - 1U);
                const uint8_t *srow = src + (size_t)sy * src_pitch;
                uint32_t y_row_in   = (((y_in >> 2) & 1U) << 7) | ((y_in & 3U) << 4);
                int      do_nv12_uv = (!is_444 && (y & 1U) == 0U);
                size_t   nv12_gbase = 0U;
                uint32_t nv12_rowin = 0U;
                if (do_nv12_uv) {
                    uint32_t uvy    = y >> 1;
                    uint32_t uv_gy  = uvy >> 3;
                    uint32_t uv_yin = uvy & 7U;
                    nv12_rowin = (((uv_yin >> 2) & 1U) << 7) | ((uv_yin & 3U) << 4);
                    nv12_gbase = (size_t)(uv_gy >> 1) * (size_t)uv_gobs * 2U + (size_t)(uv_gy & 1U);
                }

                for (uint32_t gx = 0U; gx < full_gobs; gx++) {
                    uint8_t        yb[64];
                    uint16_t       ub[32], vb[32];
                    const uint8_t *gp = srow + ((size_t)gx << 8);
                    if (is_444) {
                        for (uint32_t k = 0U; k < 32U; k++) {
                            const uint8_t *p = gp + (size_t)k * 4U;
                            int r = p[r_off], g = p[1], b = p[b_off];
                            yb[k] = clamp_u8(((cy0 * r + cy1 * g + cy2 * b + 128) >> 8) + cy_off);
                            uint8_t u8 = clamp_u8(((cu0 * r + cu1 * g + cu2 * b + 128) >> 8) + cu_off);
                            uint8_t v8 = clamp_u8(((cv0 * r + cv1 * g + cv2 * b + 128) >> 8) + cv_off);
                            ub[k] = (uint16_t)u8 | ((uint16_t)v8 << 8);
                        }
                        for (uint32_t k = 0U; k < 32U; k++) {
                            const uint8_t *p = gp + (size_t)(32U + k) * 4U;
                            int r = p[r_off], g = p[1], b = p[b_off];
                            yb[32U + k] = clamp_u8(((cy0 * r + cy1 * g + cy2 * b + 128) >> 8) + cy_off);
                            uint8_t u8 = clamp_u8(((cu0 * r + cu1 * g + cu2 * b + 128) >> 8) + cu_off);
                            uint8_t v8 = clamp_u8(((cv0 * r + cv1 * g + cv2 * b + 128) >> 8) + cv_off);
                            vb[k] = (uint16_t)u8 | ((uint16_t)v8 << 8);
                        }
                    } else {
                        for (uint32_t k = 0U; k < 64U; k++) {
                            const uint8_t *p = gp + (size_t)k * 4U;
                            int r = p[r_off], g = p[1], b = p[b_off];
                            yb[k] = clamp_u8(((cy0 * r + cy1 * g + cy2 * b + 128) >> 8) + cy_off);
                        }
                        if (do_nv12_uv) {
                            for (uint32_t k = 0U; k < 32U; k++) {
                                const uint8_t *p = gp + (size_t)k * 8U;
                                int r = p[r_off], g = p[1], b = p[b_off];
                                uint8_t u8 = clamp_u8(((cu0 * r + cu1 * g + cu2 * b + 128) >> 8) + cu_off);
                                uint8_t v8 = clamp_u8(((cv0 * r + cv1 * g + cv2 * b + 128) >> 8) + cv_off);
                                ub[k] = (uint16_t)u8 | ((uint16_t)v8 << 8);
                            }
                        }
                    }
                    size_t goff = (y_gob_base + (size_t)gx * 2U) * 512U;
                    if (goff + 512U <= ctx->surface_size) {
                        bl16_store_gob_row64(dst + goff, y_row_in, yb);
                    }
                    if (is_444) {
                        size_t o0 = (uv444_base + (size_t)(gx * 2U + 0U) * 2U) * 512U;
                        size_t o1 = (uv444_base + (size_t)(gx * 2U + 1U) * 2U) * 512U;
                        if (o0 + 512U <= uv_avail) {
                            bl16_store_gob_row64(dst_uv + o0, y_row_in, (const uint8_t *)ub);
                        }
                        if (o1 + 512U <= uv_avail) {
                            bl16_store_gob_row64(dst_uv + o1, y_row_in, (const uint8_t *)vb);
                        }
                    } else if (do_nv12_uv) {
                        size_t o = (nv12_gbase + (size_t)gx * 2U) * 512U;
                        if (o + 512U <= uv_avail) {
                            bl16_store_gob_row64(dst_uv + o, nv12_rowin, (const uint8_t *)ub);
                        }
                    }
                }

                if (full_gobs < y_gobs) {
                    const uint8_t *lp = srow + (size_t)(w - 1U) * 4U;
                    int lr = lp[r_off], lg = lp[1], lb_ = lp[b_off];
                    uint8_t last_y8 = clamp_u8(((cy0 * lr + cy1 * lg + cy2 * lb_ + 128) >> 8) + cy_off);
                    uint8_t last_u8 = clamp_u8(((cu0 * lr + cu1 * lg + cu2 * lb_ + 128) >> 8) + cu_off);
                    uint8_t last_v8 = clamp_u8(((cv0 * lr + cv1 * lg + cv2 * lb_ + 128) >> 8) + cv_off);
                    uint16_t last_uv8 = (uint16_t)last_u8 | ((uint16_t)last_v8 << 8);

                    for (uint32_t gx = full_gobs; gx < y_gobs; gx++) {
                        uint8_t  yb[64];
                        uint16_t ub[32], vb[32];
                        uint32_t x0 = gx << 6;
                        for (uint32_t k = 0U; k < 64U; k++) {
                            uint32_t x = x0 + k;
                            uint8_t  y8 = last_y8, u8 = last_u8, v8 = last_v8;
                            if (x < w) {
                                const uint8_t *p = srow + (size_t)x * 4U;
                                int r = p[r_off], g = p[1], b = p[b_off];
                                y8 = clamp_u8(((cy0 * r + cy1 * g + cy2 * b + 128) >> 8) + cy_off);
                                u8 = clamp_u8(((cu0 * r + cu1 * g + cu2 * b + 128) >> 8) + cu_off);
                                v8 = clamp_u8(((cv0 * r + cv1 * g + cv2 * b + 128) >> 8) + cv_off);
                            }
                            yb[k] = y8;
                            if (is_444) {
                                uint16_t uv_val = (uint16_t)u8 | ((uint16_t)v8 << 8);
                                if (k < 32U) {
                                    ub[k] = uv_val;
                                } else {
                                    vb[k - 32U] = uv_val;
                                }
                            } else if (do_nv12_uv && (k & 1U) == 0U) {
                                ub[k >> 1] = (uint16_t)u8 | ((uint16_t)v8 << 8);
                            }
                        }
                        (void)last_uv8;
                        size_t goff = (y_gob_base + (size_t)gx * 2U) * 512U;
                        if (goff + 512U <= ctx->surface_size) {
                            bl16_store_gob_row64(dst + goff, y_row_in, yb);
                        }
                        if (is_444) {
                            size_t o0 = (uv444_base + (size_t)(gx * 2U + 0U) * 2U) * 512U;
                            size_t o1 = (uv444_base + (size_t)(gx * 2U + 1U) * 2U) * 512U;
                            if (o0 + 512U <= uv_avail) {
                                bl16_store_gob_row64(dst_uv + o0, y_row_in, (const uint8_t *)ub);
                            }
                            if (o1 + 512U <= uv_avail) {
                                bl16_store_gob_row64(dst_uv + o1, y_row_in, (const uint8_t *)vb);
                            }
                        } else if (do_nv12_uv) {
                            size_t o = (nv12_gbase + (size_t)gx * 2U) * 512U;
                            if (o + 512U <= uv_avail) {
                                bl16_store_gob_row64(dst_uv + o, nv12_rowin, (const uint8_t *)ub);
                            }
                        }
                    }
                }
            }
        }
        return;
    }

    for (uint32_t gob_y = 0U; gob_y < n_gob_y; gob_y++) {
        uint32_t by_blk      = gob_y >> 1;
        uint32_t by_in       = gob_y & 1U;
        size_t   y_gob_base  = (size_t)by_blk * (size_t)y_gobs * 2U + (size_t)by_in;
        for (uint32_t y_in = 0U; y_in < 8U; y_in++) {
            uint32_t y          = (gob_y << 3) | y_in;
            uint32_t sy         = (y < h) ? y : (h - 1U);
            const uint8_t *srow = src + (size_t)sy * src_pitch;
            uint32_t y_row_in   = (((y_in >> 2) & 1U) << 7) | ((y_in & 3U) << 4);

            uint16_t last_y, last_u, last_v;
            argb_convert_px(srow, w - 1U, is_argb, is_rgb10, use_bt601, full_range,
                            &last_y, &last_u, &last_v);

            if (is_10b) {
                for (uint32_t gx = 0U; gx < y_gobs; gx++) {
                    uint16_t yb[32];
                    uint32_t ub[16], vb[16];
                    uint32_t x0 = gx << 5;
                    for (uint32_t k = 0U; k < 32U; k++) {
                        uint32_t x = x0 + k;
                        uint16_t y10 = last_y, u10 = last_u, v10 = last_v;
                        if (x < w) {
                            argb_convert_px(srow, x, is_argb, is_rgb10, use_bt601, full_range,
                                            &y10, &u10, &v10);
                        }
                        yb[k] = y10;
                        if (is_444) {
                            uint32_t uv_val = (uint32_t)u10 | ((uint32_t)v10 << 16);
                            if (k < 16U) {
                                ub[k] = uv_val;
                            } else {
                                vb[k - 16U] = uv_val;
                            }
                        } else if ((y & 1U) == 0U && (k & 1U) == 0U) {
                            ub[k >> 1] = (uint32_t)u10 | ((uint32_t)v10 << 16);
                        }
                    }
                    size_t goff = (y_gob_base + (size_t)gx * 2U) * 512U;
                    if (goff + 512U <= ctx->surface_size) {
                        bl16_store_gob_row64(dst + goff, y_row_in, (const uint8_t *)yb);
                    }
                    if (is_444) {
                        size_t g0 = (size_t)by_blk * (size_t)uv_gobs * 2U + (size_t)by_in;
                        size_t o0 = (g0 + (size_t)(gx * 2U + 0U) * 2U) * 512U;
                        size_t o1 = (g0 + (size_t)(gx * 2U + 1U) * 2U) * 512U;
                        if (o0 + 512U <= uv_avail) {
                            bl16_store_gob_row64(dst_uv + o0, y_row_in, (const uint8_t *)ub);
                        }
                        if (o1 + 512U <= uv_avail) {
                            bl16_store_gob_row64(dst_uv + o1, y_row_in, (const uint8_t *)vb);
                        }
                    } else if ((y & 1U) == 0U) {
                        uint32_t uvy        = y >> 1;
                        uint32_t uv_gy      = uvy >> 3;
                        uint32_t uv_yin     = uvy & 7U;
                        uint32_t uv_row_in  = (((uv_yin >> 2) & 1U) << 7) | ((uv_yin & 3U) << 4);
                        size_t   uv_gob_idx = (size_t)(uv_gy >> 1) * (size_t)uv_gobs * 2U + (size_t)(uv_gy & 1U);
                        size_t   o          = (uv_gob_idx + (size_t)gx * 2U) * 512U;
                        if (o + 512U <= uv_avail) {
                            bl16_store_gob_row64(dst_uv + o, uv_row_in, (const uint8_t *)ub);
                        }
                    }
                }
            } else {
                for (uint32_t gx = 0U; gx < y_gobs; gx++) {
                    uint8_t  yb[64];
                    uint16_t ub[32], vb[32];
                    uint32_t x0 = gx << 6;
                    for (uint32_t k = 0U; k < 64U; k++) {
                        uint32_t x = x0 + k;
                        uint16_t y10 = last_y, u10 = last_u, v10 = last_v;
                        if (x < w) {
                            argb_convert_px(srow, x, is_argb, is_rgb10, use_bt601, full_range,
                                            &y10, &u10, &v10);
                        }
                        yb[k] = (uint8_t)(y10 >> 8);
                        if (is_444) {
                            uint16_t uv_val = (uint16_t)(u10 >> 8) | ((uint16_t)(v10 >> 8) << 8);
                            if (k < 32U) {
                                ub[k] = uv_val;
                            } else {
                                vb[k - 32U] = uv_val;
                            }
                        } else if ((y & 1U) == 0U && (k & 1U) == 0U) {
                            ub[k >> 1] = (uint16_t)(u10 >> 8) | ((uint16_t)(v10 >> 8) << 8);
                        }
                    }
                    size_t goff = (y_gob_base + (size_t)gx * 2U) * 512U;
                    if (goff + 512U <= ctx->surface_size) {
                        bl16_store_gob_row64(dst + goff, y_row_in, yb);
                    }
                    if (is_444) {
                        size_t g0 = (size_t)by_blk * (size_t)uv_gobs * 2U + (size_t)by_in;
                        size_t o0 = (g0 + (size_t)(gx * 2U + 0U) * 2U) * 512U;
                        size_t o1 = (g0 + (size_t)(gx * 2U + 1U) * 2U) * 512U;
                        if (o0 + 512U <= uv_avail) {
                            bl16_store_gob_row64(dst_uv + o0, y_row_in, (const uint8_t *)ub);
                        }
                        if (o1 + 512U <= uv_avail) {
                            bl16_store_gob_row64(dst_uv + o1, y_row_in, (const uint8_t *)vb);
                        }
                    } else if ((y & 1U) == 0U) {
                        uint32_t uvy        = y >> 1;
                        uint32_t uv_gy      = uvy >> 3;
                        uint32_t uv_yin     = uvy & 7U;
                        uint32_t uv_row_in  = (((uv_yin >> 2) & 1U) << 7) | ((uv_yin & 3U) << 4);
                        size_t   uv_gob_idx = (size_t)(uv_gy >> 1) * (size_t)uv_gobs * 2U + (size_t)(uv_gy & 1U);
                        size_t   o          = (uv_gob_idx + (size_t)gx * 2U) * 512U;
                        if (o + 512U <= uv_avail) {
                            bl16_store_gob_row64(dst_uv + o, uv_row_in, (const uint8_t *)ub);
                        }
                    }
                }
            }
        }
    }
}

static void copy_surface_to_gpu(open_nvenc_hw_ctx_t *ctx,
                                const uint8_t *src, uint32_t src_pitch,
                                NV_ENC_BUFFER_FORMAT fmt,
                                const uint32_t chroma_offset[2])
{
    uint8_t *dst       = ctx->stage_buf ? ctx->stage_buf : (uint8_t *)ctx->input_buf.cpu;
    uint32_t w         = ctx->cfg.width;
    uint32_t h         = ctx->cfg.height;
    uint32_t ah        = ctx->aligned_h;
    uint32_t dst_pitch = ctx->surface_pitch;
    int      is_444    = (ctx->cfg.chroma_format_idc == 3U);
    int      is_10b    = (ctx->cfg.bit_depth == 10U);
    uint32_t bpp       = is_10b ? 2U : 1U;
    uint32_t row_bytes = w * bpp;

    if (!src || src_pitch == 0U || w == 0U || h == 0U) {
        return;
    }

    if (fmt == NV_ENC_BUFFER_FORMAT_ARGB || fmt == NV_ENC_BUFFER_FORMAT_ABGR ||
        fmt == NV_ENC_BUFFER_FORMAT_ARGB10 || fmt == NV_ENC_BUFFER_FORMAT_ABGR10) {
        copy_argb_to_bl16(ctx, dst, src, src_pitch, fmt);
        if (ctx->stage_buf) {
            memcpy(ctx->input_buf.cpu, ctx->stage_buf, ctx->surface_size);
        }
        return;
    }

    /* Plane 0 (Luma Y in Pascal 16Bx2 block-linear) */
    uint32_t copy_y_bytes = (row_bytes <= src_pitch && row_bytes <= dst_pitch) ? row_bytes : dst_pitch;
    swizzle_plane_linear_to_bl16(dst, ctx->surface_size, src, src_pitch,
                                 copy_y_bytes, bpp, h, dst_pitch, ah);

    if (is_444) {
        /* Interleaved (U, V) semi-planar chroma plane at +dst_pitch*ah, UV pitch = 2 * dst_pitch */
        uint8_t *dst_uv   = dst + (size_t)dst_pitch * (size_t)ah;
        size_t   uv_avail = ctx->surface_size - (size_t)dst_pitch * (size_t)ah;
        uint32_t uv_pitch = dst_pitch * 2U;
        const uint8_t *src_u = (chroma_offset && chroma_offset[0] > 0U)
                                   ? (src + chroma_offset[0])
                                   : (src + (size_t)src_pitch * (size_t)h);
        const uint8_t *src_v = (chroma_offset && chroma_offset[1] > 0U)
                                   ? (src + chroma_offset[1])
                                   : (src_u + (size_t)src_pitch * (size_t)h);
        swizzle_uv444_to_bl16(dst_uv, uv_avail, src_u, src_v, src_pitch,
                              w, h, uv_pitch, ah, is_10b);
    } else {
        /* Semi-planar 4:2:0 NV12 / P010: Y + interleaved UV in Pascal 16Bx2 block-linear */
        uint8_t *dst_uv   = dst + (size_t)dst_pitch * (size_t)ah;
        size_t   uv_avail = ctx->surface_size - (size_t)dst_pitch * (size_t)ah;
        uint32_t uv_h     = h / 2U;
        uint32_t uv_ah    = ctx->uv_aligned_h;
        if (fmt == NV_ENC_BUFFER_FORMAT_IYUV || fmt == NV_ENC_BUFFER_FORMAT_YV12) {
            const uint8_t *p1 = (chroma_offset && chroma_offset[0] > 0U)
                                    ? (src + chroma_offset[0])
                                    : (src + (size_t)src_pitch * (size_t)h);
            uint32_t cp = src_pitch / 2U;
            const uint8_t *p2 = (chroma_offset && chroma_offset[1] > 0U)
                                    ? (src + chroma_offset[1])
                                    : (p1 + (size_t)cp * (size_t)uv_h);
            const uint8_t *su = (fmt == NV_ENC_BUFFER_FORMAT_IYUV) ? p1 : p2;
            const uint8_t *sv = (fmt == NV_ENC_BUFFER_FORMAT_IYUV) ? p2 : p1;
            swizzle_uv444_to_bl16(dst_uv, uv_avail, su, sv, cp,
                                  w / 2U, uv_h, dst_pitch, uv_ah, 0);
        } else {
            const uint8_t *src_uv = (chroma_offset && chroma_offset[0] > 0U)
                                        ? (src + chroma_offset[0])
                                        : (src + (size_t)src_pitch * (size_t)h);
            swizzle_plane_linear_to_bl16(dst_uv, uv_avail, src_uv, src_pitch,
                                         copy_y_bytes, bpp * 2U, uv_h, dst_pitch, uv_ah);
        }
    }

    if (ctx->stage_buf) {
        memcpy(ctx->input_buf.cpu, ctx->stage_buf, ctx->surface_size);
    }
}

size_t open_nvenc_hw_get_bs_capacity(const open_nvenc_hw_ctx_t *ctx)
{
    return ctx ? (size_t)ctx->bs_buf.size : 0U;
}

void open_nvenc_hw_invalidate_refs(open_nvenc_hw_ctx_t *ctx)
{
    if (ctx) {
        ctx->has_ref        = 0;
        ctx->force_next_idr = 1;
    }
}

void open_nvenc_hw_update_cfg(open_nvenc_hw_ctx_t *ctx,
                              uint32_t gop_length,
                              uint32_t qp,
                              uint32_t qp_p,
                              uint32_t rc_mode,
                              uint32_t average_bitrate,
                              uint32_t max_bitrate,
                              uint32_t vbv_buffer_size,
                              uint32_t fps_num,
                              uint32_t fps_den,
                              uint32_t min_qp,
                              uint32_t max_qp,
                              uint32_t target_quality,
                              int force_idr)
{
    if (!ctx) {
        return;
    }
    uint32_t prev_rc         = ctx->cfg.rc_mode;
    ctx->cfg.gop_length      = gop_length;
    ctx->cfg.qp              = qp;
    ctx->cfg.qp_p            = qp_p;
    ctx->cfg.rc_mode         = rc_mode;
    ctx->cfg.average_bitrate = average_bitrate;
    ctx->cfg.max_bitrate     = max_bitrate;
    ctx->cfg.vbv_buffer_size = vbv_buffer_size;
    ctx->cfg.fps_num         = fps_num;
    ctx->cfg.fps_den         = fps_den;
    ctx->cfg.min_qp          = min_qp;
    ctx->cfg.max_qp          = max_qp;
    ctx->cfg.target_quality  = target_quality;

    uint32_t target_bps = average_bitrate ? average_bitrate : max_bitrate;
    if (prev_rc == rc_mode && rc_mode != 0U && target_bps > 0U && ctx->rc_bits_per_frame > 0.0) {
        double fps = (fps_num > 0U && fps_den > 0U) ? ((double)fps_num / (double)fps_den) : 60.0;
        if (fps < 1.0) {
            fps = 60.0;
        }
        ctx->rc_bits_per_frame = (double)target_bps / fps;
        ctx->rc_vbv_size = (vbv_buffer_size > 0U) ? (double)vbv_buffer_size : (double)target_bps;
        if (ctx->rc_vbv_size < ctx->rc_bits_per_frame * 4.0) {
            ctx->rc_vbv_size = ctx->rc_bits_per_frame * 4.0;
        }
        if (ctx->rc_vbv_level > ctx->rc_vbv_size) {
            ctx->rc_vbv_level = ctx->rc_vbv_size * 0.5;
        }
        uint32_t lo = (min_qp > 0U && min_qp <= 51U) ? min_qp : 10U;
        uint32_t hi = (max_qp > lo && max_qp <= 51U) ? max_qp : 46U;
        if (ctx->rc_cur_qp < lo) {
            ctx->rc_cur_qp = lo;
        }
        if (ctx->rc_cur_qp > hi) {
            ctx->rc_cur_qp = hi;
        }
    } else {
        rc_init_state(ctx);
    }

    if (force_idr) {
        open_nvenc_hw_invalidate_refs(ctx);
    }
}

NVENCSTATUS open_nvenc_hw_encode_frame(open_nvenc_hw_ctx_t *ctx,
                                       const uint8_t *src_data,
                                       uint32_t src_pitch,
                                       NV_ENC_BUFFER_FORMAT src_fmt,
                                       const uint32_t chroma_offset[2],
                                       int force_idr,
                                       int emit_spspps,
                                       uint8_t *out_bs,
                                       size_t out_bs_cap,
                                       size_t *out_bs_len,
                                       int *out_is_idr,
                                       uint32_t *out_avg_qp,
                                       open_nvenc_rm_stats_t *rm_stats,
                                       char *err_buf, size_t err_cap)
{
    if (!ctx || !out_bs || !out_bs_len) {
        return NV_ENC_ERR_INVALID_PTR;
    }

    int is_idr = (!ctx->has_ref || force_idr || ctx->force_next_idr ||
                  (ctx->cfg.gop_length > 0U && ctx->gop_poc >= ctx->cfg.gop_length))
                     ? 1
                     : 0;
    uint32_t ref_slot = ctx->last_recon_slot & 1U;
    uint32_t cur_slot = is_idr ? 0U : (ref_slot ^ 1U);
    if (is_idr) {
        ctx->gop_poc        = 0U;
        ctx->force_next_idr = 0;
    }

    /* Reset PB write cursor safely before CE region (+0x1e000 -> 0x7800 DW) */
    if (ctx->pb_dw_cur > 0x7000U) {
        ctx->pb_dw_cur = 0U;
    }

    uint32_t frame_qp = rc_select_frame_qp(ctx, is_idr);
    copy_surface_to_gpu(ctx, src_data, src_pitch, src_fmt, chroma_offset);
    if (ctx->cfg.is_h264) {
        build_pic_setup_h264(ctx, (uint8_t *)ctx->pic_setup_buf.cpu, is_idr, ref_slot, cur_slot, frame_qp);
    } else {
        build_pic_setup(ctx, (uint8_t *)ctx->pic_setup_buf.cpu, is_idr, ref_slot, cur_slot, frame_qp);
    }
    memset(ctx->status_buf.cpu, 0, 0x100U);
    memset(ctx->bs_buf.cpu, 0, 0x200U);

    uint32_t done_token = ++ctx->sema_seq;
    volatile uint32_t *sema_word = (volatile uint32_t *)((uint8_t *)ctx->sema_buf.cpu + 0x1000U);
    *sema_word = 0U;
    uint64_t sema_va = ctx->sema_buf.gpu_va + 0x1000ULL;

    volatile uint32_t *pb = (volatile uint32_t *)ctx->pb_buf.cpu;
    uint32_t start = ctx->pb_dw_cur;

#define EMIT_M(mbyte, v) \
    do { pb[ctx->pb_dw_cur++] = pb_hdr_inc(4U, (mbyte), 1U); pb[ctx->pb_dw_cur++] = (uint32_t)(v); } while (0)

    EMIT_M(0x0200U, ctx->cfg.is_h264 ? 0x00000001U : 0x00000003U);
    EMIT_M(0x0700U, ctx->cfg.is_h264 ? 0x00001303U : 0x00001104U);
    EMIT_M(0x0704U, ctx->frame_num);
    EMIT_M(0x0710U, (uint32_t)(ctx->pic_setup_buf.gpu_va >> 8));
    EMIT_M(0x0720U, (uint32_t)(ctx->scratch_buf.gpu_va >> 8));
    if (ctx->cfg.is_h264) {
        uint64_t mv_half = (uint64_t)(ctx->h264_mv_buf.size / 2U);
        if (!is_idr) {
            EMIT_M(0x0728U, (uint32_t)((ctx->h264_mv_buf.gpu_va + (uint64_t)ref_slot * mv_half) >> 8));
        }
        EMIT_M(0x072cU, (uint32_t)((ctx->h264_mv_buf.gpu_va + (uint64_t)cur_slot * mv_half) >> 8));
    }
    EMIT_M(0x0724U, (uint32_t)(ctx->qm_rc_buf.gpu_va >> 8));
    EMIT_M(0x0730U, (uint32_t)(ctx->recon_buf[cur_slot].gpu_va >> 8));
    EMIT_M(0x074cU, (uint32_t)(ctx->recon_buf[cur_slot].gpu_va >> 8));
    if (!is_idr) {
        EMIT_M(0x0738U, (uint32_t)(ctx->coloc_mv_buf[ref_slot].gpu_va >> 8));
        EMIT_M(0x073cU, (uint32_t)(ctx->coloc_mv_buf[cur_slot].gpu_va >> 8));
        EMIT_M(0x0400U + (ref_slot * 4U), (uint32_t)(ctx->recon_buf[ref_slot].gpu_va >> 8));
        EMIT_M(0x0440U + (ref_slot * 4U), (uint32_t)(ctx->recon_buf[ref_slot].gpu_va >> 8));
    }
    EMIT_M(0x0718U, (uint32_t)(ctx->status_buf.gpu_va >> 8));
    EMIT_M(0x071cU, (uint32_t)(ctx->bs_buf.gpu_va >> 8));
    EMIT_M(0x0734U, (uint32_t)(ctx->input_buf.gpu_va >> 8));
    EMIT_M(0x0740U, (uint32_t)(ctx->input_buf.gpu_va >> 8));
    EMIT_M(0x0744U, (uint32_t)(ctx->input_buf.gpu_va >> 8));
    EMIT_M(0x070cU, (uint32_t)(ctx->io_aux_buf.gpu_va >> 8));
    EMIT_M(0x0300U, 0x00000000U); /* NVC2B7_EXECUTE */

    /* Release completion semaphore (0x0304 = 0 triggers NVC2B7 write) */
    EMIT_M(0x0240U, (uint32_t)(sema_va >> 32));
    EMIT_M(0x0244U, (uint32_t)(sema_va & 0xffffffffU));
    EMIT_M(0x0248U, done_token);
    EMIT_M(0x0304U, 0x00000000U);

#undef EMIT_M

    __sync_synchronize();
    submit_pb_range(ctx, start, ctx->pb_dw_cur - start, 0);

    if (wait_sema_equal(sema_word, done_token, 2000) < 0) {
        volatile uint32_t *st_words = (volatile uint32_t *)ctx->status_buf.cpu;
        volatile uint32_t *userd    = (volatile uint32_t *)ctx->userd_cpu;
        volatile uint32_t *err_w    = (volatile uint32_t *)ctx->err_buf.cpu;
        const char *trace_dir       = getenv("OPEN_NVENC_TRACE_DIR");
        if (trace_dir && trace_dir[0]) {
            char dpath[512];
            snprintf(dpath, sizeof(dpath), "%s/hw_timeout_diag.txt", trace_dir);
            FILE *dfp = fopen(dpath, "a");
            if (dfp) {
                const uint32_t *ps = (const uint32_t *)ctx->pic_setup_buf.cpu;
                fprintf(dfp,
                        "=== NVC2B7 TIMEOUT frame=%u is_idr=%d is_h264=%d ===\n"
                        "vaspace=0x%x chan=0x%x nvenc=0x%x userd_rm=0x%llx gp_get=%u gp_put=%u sema=%u (exp=%u)\n"
                        "status[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x\n"
                        "VAs: pb=0x%llx(0x%llx) sema=0x%llx err=0x%llx ps=0x%llx scr=0x%llx(0x%llx)\n"
                        "     qm=0x%llx st=0x%llx bs=0x%llx(0x%llx) in=0x%llx(0x%llx) r0=0x%llx r1=0x%llx aux=0x%llx\n",
                        ctx->frame_num, is_idr, ctx->cfg.is_h264,
                        ctx->h_vaspace, ctx->h_channel, ctx->h_nvenc,
                        (unsigned long long)ctx->userd_rm_addr,
                        userd ? userd[0x88U / 4U] : 0xffffffffU, ctx->gp_put,
                        *sema_word, done_token,
                        st_words[0], st_words[1], st_words[2], st_words[3],
                        st_words[4], st_words[5], st_words[6], st_words[7],
                        (unsigned long long)ctx->pb_buf.gpu_va, (unsigned long long)ctx->pb_buf.size,
                        (unsigned long long)ctx->sema_buf.gpu_va,
                        (unsigned long long)ctx->err_buf.gpu_va,
                        (unsigned long long)ctx->pic_setup_buf.gpu_va,
                        (unsigned long long)ctx->scratch_buf.gpu_va, (unsigned long long)ctx->scratch_buf.size,
                        (unsigned long long)ctx->qm_rc_buf.gpu_va,
                        (unsigned long long)ctx->status_buf.gpu_va,
                        (unsigned long long)ctx->bs_buf.gpu_va, (unsigned long long)ctx->bs_buf.size,
                        (unsigned long long)ctx->input_buf.gpu_va, (unsigned long long)ctx->input_buf.size,
                        (unsigned long long)ctx->recon_buf[0].gpu_va,
                        (unsigned long long)ctx->recon_buf[1].gpu_va,
                        (unsigned long long)ctx->io_aux_buf.gpu_va);
                for (int si = 0; err_w && si < 10; si++) {
                    const volatile uint32_t *nw = &err_w[si * 4];
                    if (nw[0] || nw[1] || nw[2] || nw[3]) {
                        fprintf(dfp, "err_slot[%d]= %08x %08x info32=%08x status=%08x\n",
                                si, nw[0], nw[1], nw[2], nw[3]);
                    }
                }
                fprintf(dfp,
                        "ps[0x0..0x24]= %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n"
                        "ps[0x64..0x7c]= %08x %08x %08x %08x %08x %08x %08x a4=%08x\n"
                        "ps[0x1a0..0x1bc]= %08x %08x %08x %08x %08x %08x %08x %08x 30c=%08x 5900=%08x\n",
                        ps[0], ps[1], ps[2], ps[3], ps[4], ps[5], ps[6], ps[7], ps[8], ps[9],
                        ps[0x64U / 4U], ps[0x68U / 4U], ps[0x6cU / 4U], ps[0x70U / 4U],
                        ps[0x74U / 4U], ps[0x78U / 4U], ps[0x7cU / 4U], ps[0xa4U / 4U],
                        ps[0x1a0U / 4U], ps[0x1a4U / 4U], ps[0x1a8U / 4U], ps[0x1acU / 4U],
                        ps[0x1b0U / 4U], ps[0x1b4U / 4U], ps[0x1b8U / 4U], ps[0x1bcU / 4U],
                        ps[0x30cU / 4U], ps[0x5900U / 4U]);
                fclose(dfp);
            }
        }
        snprintf(err_buf, err_cap,
                 "NVC2B7 timeout (st=%u b=%u s=%u gp=%u/%u e=%08x/%08x ce=%d)",
                 st_words[1], st_words[2], *sema_word,
                 userd ? userd[0x88U / 4U] : 0xffffffffU, ctx->gp_put,
                 err_w ? err_w[2] : 0U, err_w ? err_w[3] : 0U, ctx->has_ce);
        return NV_ENC_ERR_GENERIC;
    }

    volatile uint32_t *st_words = (volatile uint32_t *)ctx->status_buf.cpu;
    if (wait_sema_equal(&st_words[1], 2U, 200) < 0) {
        snprintf(err_buf, err_cap, "NVC2B7 status_buf[1]=%u (expected 2)", st_words[1]);
        return NV_ENC_ERR_GENERIC;
    }

    uint32_t encoded_bits = st_words[2];
    size_t slice_bytes = (size_t)encoded_bits / 8U;
    if (rm_stats) {
        rm_stats->last_hw_encoded_bits = encoded_bits;
    }

    const uint8_t *bs_base = (const uint8_t *)ctx->bs_buf.cpu;
    size_t bs_off = ctx->cfg.is_h264 ? 0U : 0x100U;
    if (ctx->cfg.is_h264 &&
        !(bs_base[0] == 0x00U && bs_base[1] == 0x00U &&
          (bs_base[2] == 0x01U || (bs_base[2] == 0x00U && bs_base[3] == 0x01U)))) {
        for (size_t s = 0U; s < 0x200U && (s + 3U) < ctx->bs_buf.size; s++) {
            if (bs_base[s] == 0x00U && bs_base[s + 1U] == 0x00U && bs_base[s + 2U] == 0x01U) {
                bs_off = (s > 0U && bs_base[s - 1U] == 0x00U) ? (s - 1U) : s;
                break;
            }
        }
    }
    const uint8_t *hw_slice = bs_base + bs_off;
    if (slice_bytes < 5U || slice_bytes > (ctx->bs_buf.size - bs_off)) {
        snprintf(err_buf, err_cap, "Invalid Falcon encoded_bits=%u", encoded_bits);
        return NV_ENC_ERR_GENERIC;
    }

    /* Emit parameter sets on IDR or OUTPUT_SPSPPS, then 4-byte Annex-B slice */
    size_t hdr_len = 0U;
    if (is_idr || emit_spspps) {
        if (ctx->cfg.is_h264) {
            open_nvenc_h264_hdr_params_t hp;
            memset(&hp, 0, sizeof(hp));
            hp.width                     = ctx->cfg.width;
            hp.height                    = ctx->cfg.height;
            hp.chroma_format_idc         = ctx->cfg.chroma_format_idc;
            hp.level_idc                 = ctx->cfg.level_idc;
            hp.fps_num                   = ctx->cfg.fps_num;
            hp.fps_den                   = ctx->cfg.fps_den;
            hp.video_signal_type_present = ctx->cfg.vui_video_signal_present;
            hp.video_format              = ctx->cfg.vui_video_format;
            hp.video_full_range          = ctx->cfg.vui_full_range;
            hp.colour_desc_present       = ctx->cfg.vui_colour_desc_present;
            hp.colour_primaries          = ctx->cfg.vui_colour_primaries;
            hp.transfer_characteristics  = ctx->cfg.vui_transfer_char;
            hp.matrix_coeffs             = ctx->cfg.vui_matrix_coeffs;
            hdr_len = open_nvenc_h264_write_headers_ex(out_bs, out_bs_cap, &hp);
        } else {
            open_nvenc_hevc_hdr_params_t ep;
            memset(&ep, 0, sizeof(ep));
            ep.width                     = ctx->cfg.width;
            ep.height                    = ctx->cfg.height;
            ep.chroma_format_idc         = ctx->cfg.chroma_format_idc;
            ep.bit_depth                 = ctx->cfg.bit_depth;
            ep.level_idc                 = ctx->cfg.level_idc;
            ep.tier                      = ctx->cfg.tier;
            ep.fps_num                   = ctx->cfg.fps_num;
            ep.fps_den                   = ctx->cfg.fps_den;
            ep.overscan_present          = ctx->cfg.vui_overscan_present;
            ep.overscan_info             = ctx->cfg.vui_overscan_info;
            ep.video_signal_type_present = ctx->cfg.vui_video_signal_present;
            ep.video_format              = ctx->cfg.vui_video_format;
            ep.video_full_range          = ctx->cfg.vui_full_range;
            ep.colour_desc_present       = ctx->cfg.vui_colour_desc_present;
            ep.colour_primaries          = ctx->cfg.vui_colour_primaries;
            ep.transfer_characteristics  = ctx->cfg.vui_transfer_char;
            ep.matrix_coeffs             = ctx->cfg.vui_matrix_coeffs;
            ep.chroma_loc_present        = ctx->cfg.vui_chroma_loc_present;
            ep.chroma_loc_top            = ctx->cfg.vui_chroma_loc_top;
            ep.chroma_loc_bot            = ctx->cfg.vui_chroma_loc_bot;
            ep.timing_present            = ctx->cfg.vui_timing_present;
            ep.num_units_in_tick         = ctx->cfg.vui_num_units_in_tick;
            ep.time_scale                = ctx->cfg.vui_time_scale;
            hdr_len = open_nvenc_hevc_write_headers_ex(out_bs, out_bs_cap, &ep);
        }
        if (hdr_len == 0U) {
            set_err(err_buf, err_cap, "parameter set header generation failed");
            return NV_ENC_ERR_NOT_ENOUGH_BUFFER;
        }
    }

    /* Convert Falcon's 3-byte startcode at +0x100 into 4-byte Annex-B if needed */
    int needs_leading_zero = (hw_slice[0] == 0x00U && hw_slice[1] == 0x00U && hw_slice[2] == 0x01U) ? 1 : 0;
    size_t need_total = hdr_len + (size_t)needs_leading_zero + slice_bytes;
    if (need_total > out_bs_cap) {
        set_err(err_buf, err_cap, "Output bitstream buffer too small for HW slice");
        return NV_ENC_ERR_NOT_ENOUGH_BUFFER;
    }

    size_t pos = hdr_len;
    if (needs_leading_zero) {
        out_bs[pos++] = 0x00U;
    }
    memcpy(out_bs + pos, hw_slice, slice_bytes);
    pos += slice_bytes;

    rc_update_after_frame(ctx, encoded_bits, is_idr);

    ctx->last_recon_slot = cur_slot;
    ctx->has_ref         = 1;
    ctx->frame_num++;
    ctx->gop_poc++;

    if (out_is_idr) {
        *out_is_idr = is_idr;
    }
    if (out_avg_qp) {
        *out_avg_qp = frame_qp;
    }
    *out_bs_len = pos;
    return NV_ENC_SUCCESS;
}

NVENCSTATUS open_nvenc_hw_encode_idr(open_nvenc_hw_ctx_t *ctx,
                                     const uint8_t *src_data,
                                     uint32_t src_pitch,
                                     NV_ENC_BUFFER_FORMAT src_fmt,
                                     uint8_t *out_bs,
                                     size_t out_bs_cap,
                                     size_t *out_bs_len,
                                     open_nvenc_rm_stats_t *rm_stats,
                                     char *err_buf, size_t err_cap)
{
    return open_nvenc_hw_encode_frame(ctx, src_data, src_pitch, src_fmt, NULL,
                                      1, 1, out_bs, out_bs_cap, out_bs_len,
                                      NULL, NULL, rm_stats, err_buf, err_cap);
}
