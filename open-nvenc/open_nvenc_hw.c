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

    uint32_t pb_dw_cur;    /* current dword offset in pb_buf */
    uint32_t gp_put;
    uint32_t sema_seq;

    open_nvenc_hw_init_cfg_t cfg;
    uint32_t aligned_w;
    uint32_t aligned_h;
    uint32_t ctu_w;
    uint32_t ctu_h;
    uint32_t n_ctus;
    uint32_t uv_aligned_h;
    uint32_t surface_pitch;
    size_t   surface_size;

    uint32_t frame_num;
    uint32_t gop_poc;
    uint32_t last_recon_slot;
    int      has_ref;
    int      force_next_idr;
};

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
        void *cpu = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        if (cpu == MAP_FAILED) {
            set_err(err_buf, err_cap, "mmap sysmem failed");
            close(mfd);
            return -1;
        }
        buf->cpu    = cpu;
        buf->map_fd = mfd;
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
    if (buf->cpu && buf->cpu != MAP_FAILED) {
        munmap(buf->cpu, (size_t)buf->size);
        buf->cpu = NULL;
    }
    if (buf->map_fd >= 0) {
        nv_nvos33_t ureq;
        memset(&ureq, 0, sizeof(ureq));
        ureq.hClient = ctx->h_client;
        ureq.hDevice = ctx->h_subdevice;
        ureq.hMemory = buf->h_mem;
        (void)ioctl(ctx->ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_UNMAP_MEMORY, nv_nvos33_t), &ureq);
        close(buf->map_fd);
        buf->map_fd = -1;
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

static void build_pic_setup(const open_nvenc_hw_ctx_t *ctx,
                            uint8_t *buf,
                            int is_idr,
                            uint32_t ref_dpb_slot,
                            uint32_t cur_dpb_slot)
{
    memset(buf, 0, 0xe000U);
    uint32_t *w = (uint32_t *)buf;
    (void)cur_dpb_slot;

    uint32_t width     = ctx->cfg.width;
    uint32_t aligned_h = ctx->aligned_h;
    uint32_t aligned_w = ctx->aligned_w;
    int      is_444    = (ctx->cfg.chroma_format_idc == 3U);
    int      is_10b    = (ctx->cfg.bit_depth == 10U);
    uint32_t qp_i      = (ctx->cfg.qp <= 51U) ? ctx->cfg.qp : 24U;
    uint32_t qp_p      = (ctx->cfg.qp_p > 0U && ctx->cfg.qp_p <= 51U) ? ctx->cfg.qp_p : qp_i;
    uint32_t qp        = is_idr ? qp_i : qp_p;
    uint32_t gop_len   = (ctx->cfg.gop_length > 0U) ? ctx->cfg.gop_length : 0xfaU;

    uint32_t y_stride_bytes = aligned_w * (is_10b ? 2U : 1U);
    uint32_t y_plane_pages  = (y_stride_bytes * aligned_h) >> 8; /* in 256B units */
    uint32_t uv_plane_pages = (y_stride_bytes * ctx->uv_aligned_h) >> 8;
    uint32_t uv_stride_hdr  = is_444 ? (aligned_w * 2U) : aligned_w;
    uint32_t dims_m1        = ((aligned_h - 1U) << 16) | (width - 1U);
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
    w[0x0030cU / 4U] = (ctx->n_ctus > 1024U)
                           ? 0x16698000U
                           : ((ctx->n_ctus > 256U) ? 0x0a1b8000U : 0x09e34000U);

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
    ctx->ctl_fd   = ctl_fd;
    ctx->dev_fd   = dev_fd;
    ctx->userd_fd = -1;
    ctx->cfg      = *cfg;
    ctx->h_next   = 0x80000000U;

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
    ctx->userd_cpu = mmap(NULL, 0x1000U, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->userd_fd, 0);
    if (ctx->userd_cpu == MAP_FAILED) {
        ctx->userd_cpu = NULL;
        set_err(err_buf, err_cap, "mmap USERD failed");
        goto fail;
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

    size_t scratch_size = (size_t)ctx->n_ctus * 1024U;
    if (scratch_size < 0x10000U) {
        scratch_size = 0x10000U;
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

    init_qm_rc_buf((uint32_t *)ctx->qm_rc_buf.cpu);

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

    if (ctx->h_nvenc) {
        (void)rm_free(ctx->ctl_fd, ctx->h_client, ctx->h_channel, ctx->h_nvenc, rm_stats);
        ctx->h_nvenc = 0U;
    }
    if (ctx->userd_cpu && ctx->userd_cpu != MAP_FAILED) {
        munmap(ctx->userd_cpu, 0x1000U);
        ctx->userd_cpu = NULL;
    }
    if (ctx->userd_fd >= 0) {
        if (ctx->channel_mapped) {
            nv_nvos33_t ureq;
            memset(&ureq, 0, sizeof(ureq));
            ureq.hClient = ctx->h_client;
            ureq.hDevice = ctx->h_subdevice;
            ureq.hMemory = ctx->h_channel;
            (void)ioctl(ctx->ctl_fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_UNMAP_MEMORY, nv_nvos33_t), &ureq);
            ctx->channel_mapped = 0;
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

static void copy_surface_to_gpu(open_nvenc_hw_ctx_t *ctx,
                                const uint8_t *src, uint32_t src_pitch,
                                NV_ENC_BUFFER_FORMAT fmt)
{
    uint8_t *dst       = (uint8_t *)ctx->input_buf.cpu;
    uint32_t w         = ctx->cfg.width;
    uint32_t h         = ctx->cfg.height;
    uint32_t ah        = ctx->aligned_h;
    uint32_t dst_pitch = ctx->surface_pitch;
    int      is_444    = (ctx->cfg.chroma_format_idc == 3U);
    int      is_10b    = (ctx->cfg.bit_depth == 10U);
    uint32_t bpp       = is_10b ? 2U : 1U;
    uint32_t row_bytes = w * bpp;

    if (!src || src_pitch == 0U) {
        return;
    }

    memset(dst, 0, ctx->surface_size);

    /* Plane 0 (Luma Y in Pascal 16Bx2 block-linear) */
    uint32_t copy_y_bytes = (row_bytes <= src_pitch && row_bytes <= dst_pitch) ? row_bytes : dst_pitch;
    for (uint32_t y = 0U; y < ah; y++) {
        uint32_t sy = (y < h) ? y : (h - 1U);
        const uint8_t *srow = src + (size_t)sy * src_pitch;
        for (uint32_t xb = 0U; xb < copy_y_bytes; xb++) {
            size_t off = gob_bl_offset(xb, y, dst_pitch);
            if (off < ctx->surface_size) {
                dst[off] = srow[xb];
            }
        }
    }

    if (is_444) {
        /* Interleaved (U, V) semi-planar chroma plane at +dst_pitch*ah, UV pitch = 2 * dst_pitch */
        uint8_t *dst_uv    = dst + (size_t)dst_pitch * (size_t)ah;
        size_t   uv_avail  = ctx->surface_size - (size_t)dst_pitch * (size_t)ah;
        uint32_t uv_pitch  = dst_pitch * 2U;
        const uint8_t *src_u = src + (size_t)src_pitch * (size_t)h;
        const uint8_t *src_v = src_u + (size_t)src_pitch * (size_t)h;
        for (uint32_t y = 0U; y < ah; y++) {
            uint32_t sy = (y < h) ? y : (h - 1U);
            const uint8_t *su = src_u + (size_t)sy * src_pitch;
            const uint8_t *sv = src_v + (size_t)sy * src_pitch;
            for (uint32_t x = 0U; x < w; x++) {
                uint32_t xb = x * bpp * 2U;
                for (uint32_t k = 0U; k < bpp; k++) {
                    size_t off_u = gob_bl_offset(xb + k, y, uv_pitch);
                    size_t off_v = gob_bl_offset(xb + bpp + k, y, uv_pitch);
                    if (off_u < uv_avail) {
                        dst_uv[off_u] = su[(size_t)x * bpp + k];
                    }
                    if (off_v < uv_avail) {
                        dst_uv[off_v] = sv[(size_t)x * bpp + k];
                    }
                }
            }
        }
    } else {
        /* Semi-planar 4:2:0 NV12 / P010: Y + interleaved UV in Pascal 16Bx2 block-linear */
        uint8_t *dst_uv   = dst + (size_t)dst_pitch * (size_t)ah;
        size_t   uv_avail = ctx->surface_size - (size_t)dst_pitch * (size_t)ah;
        uint32_t uv_h     = h / 2U;
        uint32_t uv_ah    = ctx->uv_aligned_h;
        if (fmt == NV_ENC_BUFFER_FORMAT_IYUV || fmt == NV_ENC_BUFFER_FORMAT_YV12) {
            const uint8_t *p1 = src + (size_t)src_pitch * (size_t)h;
            uint32_t cp = src_pitch / 2U;
            const uint8_t *p2 = p1 + (size_t)cp * (size_t)uv_h;
            const uint8_t *su = (fmt == NV_ENC_BUFFER_FORMAT_IYUV) ? p1 : p2;
            const uint8_t *sv = (fmt == NV_ENC_BUFFER_FORMAT_IYUV) ? p2 : p1;
            for (uint32_t y = 0U; y < uv_ah; y++) {
                uint32_t sy = (y < uv_h) ? y : (uv_h - 1U);
                for (uint32_t x = 0U; x < w / 2U; x++) {
                    size_t off_u = gob_bl_offset(x * 2U, y, dst_pitch);
                    size_t off_v = gob_bl_offset(x * 2U + 1U, y, dst_pitch);
                    if (off_u < uv_avail) {
                        dst_uv[off_u] = su[(size_t)sy * cp + x];
                    }
                    if (off_v < uv_avail) {
                        dst_uv[off_v] = sv[(size_t)sy * cp + x];
                    }
                }
            }
        } else {
            const uint8_t *src_uv = src + (size_t)src_pitch * (size_t)h;
            for (uint32_t y = 0U; y < uv_ah; y++) {
                uint32_t sy = (y < uv_h) ? y : (uv_h - 1U);
                const uint8_t *srow = src_uv + (size_t)sy * src_pitch;
                for (uint32_t xb = 0U; xb < copy_y_bytes; xb++) {
                    size_t off = gob_bl_offset(xb, y, dst_pitch);
                    if (off < uv_avail) {
                        dst_uv[off] = srow[xb];
                    }
                }
            }
        }
    }
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
                              int force_idr)
{
    if (!ctx) {
        return;
    }
    ctx->cfg.gop_length = gop_length;
    ctx->cfg.qp         = qp;
    ctx->cfg.qp_p       = qp_p;
    if (force_idr) {
        open_nvenc_hw_invalidate_refs(ctx);
    }
}

NVENCSTATUS open_nvenc_hw_encode_frame(open_nvenc_hw_ctx_t *ctx,
                                       const uint8_t *src_data,
                                       uint32_t src_pitch,
                                       NV_ENC_BUFFER_FORMAT src_fmt,
                                       int force_idr,
                                       int emit_spspps,
                                       uint8_t *out_bs,
                                       size_t out_bs_cap,
                                       size_t *out_bs_len,
                                       int *out_is_idr,
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

    /* Reset PB write cursor when wrapping around safely */
    if (ctx->pb_dw_cur > 0x7000U) {
        ctx->pb_dw_cur = 0U;
    }

    copy_surface_to_gpu(ctx, src_data, src_pitch, src_fmt);
    build_pic_setup(ctx, (uint8_t *)ctx->pic_setup_buf.cpu, is_idr, ref_slot, cur_slot);
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

    EMIT_M(0x0200U, 0x00000003U);
    EMIT_M(0x0700U, 0x00001104U); /* HEVC */
    EMIT_M(0x0704U, ctx->frame_num);
    EMIT_M(0x0710U, (uint32_t)(ctx->pic_setup_buf.gpu_va >> 8));
    EMIT_M(0x0720U, (uint32_t)(ctx->scratch_buf.gpu_va >> 8));
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
        snprintf(err_buf, err_cap, "NVC2B7_EXECUTE timeout (status[1]=%u bits=%u sema=%u)",
                 st_words[1], st_words[2], *sema_word);
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

    const uint8_t *hw_slice = (const uint8_t *)ctx->bs_buf.cpu + 0x100U;
    if (slice_bytes < 5U || slice_bytes > (ctx->bs_buf.size - 0x100U)) {
        snprintf(err_buf, err_cap, "Invalid Falcon encoded_bits=%u", encoded_bits);
        return NV_ENC_ERR_GENERIC;
    }

    /* Emit VPS+SPS+PPS on IDR or OUTPUT_SPSPPS, then 4-byte Annex-B slice */
    size_t hdr_len = 0U;
    if (is_idr || emit_spspps) {
        hdr_len = open_nvenc_hevc_write_headers(out_bs, out_bs_cap,
                                                ctx->cfg.width, ctx->cfg.height,
                                                ctx->cfg.chroma_format_idc,
                                                ctx->cfg.bit_depth);
        if (hdr_len == 0U) {
            set_err(err_buf, err_cap, "open_nvenc_hevc_write_headers failed");
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

    ctx->last_recon_slot = cur_slot;
    ctx->has_ref         = 1;
    ctx->frame_num++;
    ctx->gop_poc++;

    if (out_is_idr) {
        *out_is_idr = is_idr;
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
    return open_nvenc_hw_encode_frame(ctx, src_data, src_pitch, src_fmt,
                                      1, 1, out_bs, out_bs_cap, out_bs_len,
                                      NULL, rm_stats, err_buf, err_cap);
}
