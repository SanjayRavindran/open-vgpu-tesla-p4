/* SPDX-License-Identifier: MIT OR GPL-2.0 */
#include "open_vgpu_guest_rpc.h"
#include "open_vgpu_plugin.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * In-Process Host Plugin VMIOP Harness + Guest DMA Page Arena
 * ------------------------------------------------------------------------- */
#define MAX_ARENA_PAGES 16U
#define MAX_HOST_REGIONS 8U

static gp104_vgpu_dev_t g_host_dev;
static void *g_cfg_priv = NULL;
static vmiop_emul_access_fn_t g_cfg_cb = NULL;

typedef struct {
    uint64_t start;
    uint64_t len;
    void    *priv;
    vmiop_emul_access_fn_t cb;
} loopback_region_t;

static loopback_region_t g_regions[MAX_HOST_REGIONS];
static uint32_t g_region_cnt = 0U;

static struct {
    int      in_use;
    uint64_t gpa;
    uint8_t  bytes[4096];
} g_dma_pages[MAX_ARENA_PAGES];

static uint32_t g_msi_count = 0U;
static uint32_t g_pin_calls = 0U;
static uint64_t g_pinned_pages = 0ULL;
static uint32_t g_unpin_calls = 0U;
static uint64_t g_unpinned_pages = 0ULL;
static uint32_t g_map_region_adds = 0U;
static uint32_t g_map_region_removes = 0U;
static int      g_open_fds = 0;

uint32_t vmiop_log(uint32_t level, const char *fmt, ...)
{
    (void)level; (void)fmt;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_config_get(uint32_t dev_handle, const char *key, char **out_val)
{
    (void)dev_handle;
    if (!out_val || !key) return VMIOP_ERROR_INVAL;
    if (strcmp(key, "frame_rate_limiter") == 0) {
        *out_val = strdup("1");
        return VMIOP_SUCCESS;
    }
    *out_val = NULL;
    return VMIOP_ERROR_NOT_FOUND;
}

uint32_t vmiop_register_emul_device_v2(uint32_t handle_in, void *priv, void *access_cb,
                                       void *tag, uint32_t *handle_out)
{
    (void)tag;
    g_cfg_priv = priv;
    g_cfg_cb   = (vmiop_emul_access_fn_t)access_cb;
    if (handle_out) *handle_out = handle_in;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_unregister_emul_device(uint32_t handle)
{
    (void)handle;
    g_cfg_priv = NULL;
    g_cfg_cb   = NULL;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_create_guest_region_v2(uint32_t dev_handle, uint64_t length, void *access_cb,
                                      void *priv, int space, uint32_t *out_id)
{
    (void)dev_handle; (void)space;
    if (g_region_cnt >= MAX_HOST_REGIONS || !out_id) return VMIOP_ERROR_NO_MEMORY;
    uint32_t idx = g_region_cnt++;
    g_regions[idx].start = UINT64_MAX;
    g_regions[idx].len   = length;
    g_regions[idx].priv  = priv;
    g_regions[idx].cb    = (vmiop_emul_access_fn_t)access_cb;
    *out_id = idx + 1U;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_relocate_guest_region(uint32_t region_id, uint64_t start)
{
    if (region_id == 0U || region_id > g_region_cnt) return VMIOP_ERROR_NOT_FOUND;
    g_regions[region_id - 1U].start = start;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_map_guest_memory_v2(uint32_t dev_handle, uint64_t gpa, uint64_t size,
                                   void **out_hva, uint32_t prot_flags, uint32_t *out_id)
{
    (void)dev_handle; (void)size; (void)prot_flags;
    for (uint32_t i = 0U; i < MAX_ARENA_PAGES; i++) {
        if (g_dma_pages[i].in_use && g_dma_pages[i].gpa == gpa) {
            *out_hva = g_dma_pages[i].bytes;
            *out_id  = i + 1U;
            return VMIOP_SUCCESS;
        }
    }
    return VMIOP_ERROR_NOT_FOUND;
}

uint32_t vmiop_unmap_guest_memory(uint32_t mem_id)
{
    (void)mem_id;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_get_guest_id(uint64_t out_uuid[2])
{
    if (!out_uuid) return VMIOP_ERROR_INVAL;
    out_uuid[0] = 0x1111222233334444ULL;
    out_uuid[1] = 0x5555666677778888ULL;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_pin_guest_pages_v2(uint32_t dev_handle, const uint64_t *gpfn,
                                  uint64_t *hpfn, uint32_t count)
{
    (void)dev_handle;
    for (uint32_t i = 0U; i < count; i++) {
        hpfn[i] = gpfn[i] + 0x200000ULL;
    }
    g_pin_calls++;
    g_pinned_pages += count;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_map_guest_region(uint32_t region_id, uint64_t pmmio_start,
                                uint64_t reserved, uint64_t size,
                                uint64_t vmmio_start, int add)
{
    (void)pmmio_start; (void)reserved; (void)vmmio_start;
    if (region_id == 0U || size == 0ULL) return VMIOP_ERROR_INVAL;
    if (add == 1) g_map_region_adds++;
    else if (add == 0) g_map_region_removes++;
    else return VMIOP_ERROR_INVAL;
    return VMIOP_SUCCESS;
}

static uint32_t host_inject_msi(uint64_t h, uint64_t a, uint32_t b)
{
    (void)h; (void)a; (void)b;
    g_msi_count++;
    return VMIOP_SUCCESS;
}
static uint32_t host_update_lfb(void) { return VMIOP_SUCCESS; }
static uint32_t host_unpin_pages(uint64_t h, uint64_t *p, uint64_t u, uint32_t c)
{
    (void)h; (void)p; (void)u;
    g_unpin_calls++;
    g_unpinned_pages += c;
    return VMIOP_SUCCESS;
}
static uint32_t host_guest_id_type(void) { return 1U; }

/* -------------------------------------------------------------------------
 * Mock Host RM Driver Sys-Ops (/dev/nvidiactl + /dev/nvidia0 + GR static)
 * ------------------------------------------------------------------------- */
static uint32_t g_next_host_client = 0xc1d09000U;

static int lb_open(const char *path, int flags)
{
    (void)flags;
    if (strcmp(path, "/dev/nvidiactl") == 0) { g_open_fds++; return 200; }
    if (strcmp(path, "/dev/nvidia0") == 0) { g_open_fds++; return 201; }
    return -1;
}

static int lb_close(int fd)
{
    if (fd == 200 || fd == 201) g_open_fds--;
    return 0;
}

static int lb_ioctl(int fd, unsigned long req, void *arg)
{
    (void)fd;
    uint32_t nr = (uint32_t)_IOC_NR(req);
    if (nr == NV_ESC_CHECK_VERSION_STR || nr == NV_ESC_REGISTER_FD ||
        nr == NV_ESC_IOCTL_XFER_CMD) {
        return 0;
    }
    if (nr == NV_ESC_CARD_INFO) {
        nv_ioctl_card_info_t *cards = (nv_ioctl_card_info_t *)arg;
        cards[0].valid = 1U;
        cards[0].bus = 1U;
        cards[0].vendor_id = 0x10deU;
        cards[0].device_id = 0x1bb3U;
        cards[0].gpu_id = 0x100U;
        return 0;
    }
    if (nr == NV_ESC_RM_ALLOC) {
        if ((uint32_t)_IOC_SIZE(req) == sizeof(NVOS21_ROOT_PARAMETERS)) {
            NVOS21_ROOT_PARAMETERS *p = (NVOS21_ROOT_PARAMETERS *)arg;
            p->hObjectNew = g_next_host_client++;
            p->status = NV_OK;
            return 0;
        }
        NVOS64_PARAMETERS *p = (NVOS64_PARAMETERS *)arg;
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_FREE) {
        ((NVOS00_PARAMETERS *)arg)->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_MAP_MEMORY) {
        NVOS33_PARAMETERS *p = (NVOS33_PARAMETERS *)arg;
        p->pLinearAddress = 0x7f100000ULL;
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_UNMAP_MEMORY) {
        ((NVOS34_PARAMETERS *)arg)->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_MAP_MEMORY_DMA) {
        NVOS46_PARAMETERS *p = (NVOS46_PARAMETERS *)arg;
        if (p->dmaOffset == 0ULL) p->dmaOffset = 0x300000000ULL;
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_UNMAP_MEMORY_DMA) {
        ((NVOS47_PARAMETERS *)arg)->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_CONTROL) {
        NVOS54_PARAMETERS *p = (NVOS54_PARAMETERS *)arg;
        uint8_t *prm = (uint8_t *)(uintptr_t)p->params;
        if (p->cmd == NV2080_CTRL_CMD_BUS_GET_PCI_INFO) {
            uint32_t ids[2] = { 0x1bb310deU, 0x11d810deU };
            memcpy(prm, ids, sizeof(ids));
        } else if (p->cmd == NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE) {
            uint32_t m = 3U;
            memcpy(prm, &m, sizeof(m));
        } else if (p->cmd == NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_TYPE_INFO) {
            NVA081_CTRL_VGPU_CONFIG_GET_VGPU_TYPE_INFO_PARAMS *ti =
                (NVA081_CTRL_VGPU_CONFIG_GET_VGPU_TYPE_INFO_PARAMS *)prm;
            snprintf(ti->vgpuName, sizeof(ti->vgpuName), "GRID P4-4Q");
            ti->fbLength = 0x100000000ULL;
            ti->frlConfig = 60U;
            uint64_t usable = 0xec000000ULL;
            memcpy(&ti->tail[0], &usable, sizeof(usable));
        } else if (p->cmd == NVA082_CTRL_CMD_GET_ENCODER_CAPACITY) {
            uint32_t cap = 100U;
            memcpy(prm, &cap, sizeof(cap));
        } else if (p->cmd == NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR) {
            uint64_t pmmio = 0xfef000000ULL;
            memcpy(prm + 0x08, &pmmio, sizeof(pmmio));
        } else if (p->cmd == NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_SM_ORDER) {
            uint32_t w = 0x134000a1U;
            memcpy(prm, &w, 4U);
        } else if (p->cmd == NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_INFO) {
            uint32_t words[5] = { 0x134000a1U, 0x134a1000U, 4U, 20U, 2560U };
            memcpy(prm, words, sizeof(words));
        } else if (p->cmd == NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_PDB_PROPS) {
            uint32_t w = 0x00001234U;
            memcpy(prm, &w, 4U);
        }
        p->status = NV_OK;
        return 0;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Guest-Side BAR0 / PCI Config / DMA Callbacks Wiring Into Host Plugin
 * ------------------------------------------------------------------------- */
static uint32_t guest_pci_cfg_read32(void *priv, uint64_t off)
{
    (void)priv;
    uint32_t val = 0U, dummy = 0U;
    assert(g_cfg_cb(g_cfg_priv, 0U, VGPU_EMUL_CONFIG_SPACE, off, 4U, &val, &dummy) == 0U);
    return val;
}

static uint32_t guest_bar0_read32(void *priv, uint64_t off)
{
    (void)priv;
    uint32_t val = 0U, dummy = 0U;
    assert(g_regions[0].cb(g_regions[0].priv, 0U, VGPU_EMUL_MMIO, off, 4U, &val, &dummy) == 0U);
    return val;
}

static void guest_bar0_write32(void *priv, uint64_t off, uint32_t val)
{
    (void)priv;
    uint32_t dummy = 0U;
    assert(g_regions[0].cb(g_regions[0].priv, 1U, VGPU_EMUL_MMIO, off, 4U, &val, &dummy) == 0U);
}

static void *guest_alloc_dma_page(void *priv, uint64_t *out_gpa)
{
    (void)priv;
    for (uint32_t i = 0U; i < MAX_ARENA_PAGES; i++) {
        if (!g_dma_pages[i].in_use) {
            g_dma_pages[i].in_use = 1;
            g_dma_pages[i].gpa    = 0x200000ULL + (uint64_t)i * 0x1000ULL;
            memset(g_dma_pages[i].bytes, 0, sizeof(g_dma_pages[i].bytes));
            *out_gpa = g_dma_pages[i].gpa;
            return g_dma_pages[i].bytes;
        }
    }
    return NULL;
}

static void guest_free_dma_page(void *priv, void *va, uint64_t gpa)
{
    (void)priv; (void)va;
    for (uint32_t i = 0U; i < MAX_ARENA_PAGES; i++) {
        if (g_dma_pages[i].in_use && g_dma_pages[i].gpa == gpa) {
            g_dma_pages[i].in_use = 0;
            return;
        }
    }
}

int main(void)
{
    printf("Running clean-room Host <-> Guest BAR0 0x8f000 VRPC Loopback Test (VGX 0x23.0x05)...\n");

    memset(&g_dma_pages, 0, sizeof(g_dma_pages));
    open_vgpu_rm_sys_ops_t rm_ops = {
        .open_fn = lb_open, .close_fn = lb_close, .ioctl_fn = lb_ioctl
    };
    open_vgpu_rm_set_sys_ops(&rm_ops);

    gp104_dev_init_defaults(&g_host_dev, 1U, 65U); /* GRID P4-4Q */
    snprintf(g_host_dev.gpu_pci_bdf, sizeof(g_host_dev.gpu_pci_bdf), "0000:01:00.0");

    static vmiop_env_v2_t env = {
        .inject_msi        = host_inject_msi,
        .update_lfb        = host_update_lfb,
        .unpin_pages       = host_unpin_pages,
        .get_guest_id_type = host_guest_id_type,
    };
    assert(gp104_dev_start(&g_host_dev, &env) == VMIOP_SUCCESS);

    /* Simulate PCI BAR relocation performed by OVMF/Linux PCI core */
    uint32_t dummy = 0U;
    uint32_t b0 = 0xc1000000U;
    uint32_t b1_lo = 0x0000000cU, b1_hi = 0x8U;
    assert(g_cfg_cb(g_cfg_priv, 1U, VGPU_EMUL_CONFIG_SPACE, 0x10ULL, 4U, &b0, &dummy) == 0U);
    assert(g_cfg_cb(g_cfg_priv, 1U, VGPU_EMUL_CONFIG_SPACE, 0x14ULL, 4U, &b1_lo, &dummy) == 0U);
    assert(g_cfg_cb(g_cfg_priv, 1U, VGPU_EMUL_CONFIG_SPACE, 0x18ULL, 4U, &b1_hi, &dummy) == 0U);

    open_vgpu_guest_transport_t g_ops = {
        .priv           = NULL,
        .pci_cfg_read32 = guest_pci_cfg_read32,
        .bar0_read32    = guest_bar0_read32,
        .bar0_write32   = guest_bar0_write32,
        .alloc_dma_page = guest_alloc_dma_page,
        .free_dma_page  = guest_free_dma_page,
    };

    open_vgpu_guest_ctx_t guest;
    assert(open_vgpu_guest_init(&guest, &g_ops) == 0);
    assert(guest.pci_id == 0x1bb310deU && guest.subsys_id == 0x120610deU);
    assert(guest.pmc_boot0 == 0x134000a1U && guest.virtual_fb_bytes == 0xec000000ULL);
    assert(strcmp(guest.vendor_cap_tag, "535.309.01r539_7") == 0);

    /* 1) Verify pre-init VRPC rejects 0x50 with NV_ERR_NOT_SUPPORTED (0x56) */
    uint8_t chunk_probe[16] = { 0 };
    uint32_t st = 0U;
    assert(open_vgpu_guest_vrpc_call(&guest, 0x50U, chunk_probe, sizeof(chunk_probe), &st) == 0);
    assert(st == 0x56U);

    /* 2) Guest VGX 0x23.0x05 handshake (0x01 + 0x40) */
    assert(open_vgpu_guest_handshake(&guest, "580.196.07", "r580_00", &st) == 0 && st == 0U);

    /* 3) Guest pulls full 17-chunk 0x50 VGPU_STATIC_INFO (67,416 B) */
    uint8_t *blob_50 = (uint8_t *)calloc(1U, OPEN_VGPU_GUEST_STATIC_INFO_LEN);
    open_vgpu_guest_static_summary_t s_info;
    assert(blob_50 != NULL);
    assert(open_vgpu_guest_fetch_static_info(&guest, blob_50, &s_info, &st) == 0 && st == 0U);
    assert(s_info.usable_fb_bytes == 0xec000000ULL);
    assert(s_info.bar1_bytes == 256ULL * 1024ULL * 1024ULL);
    assert(s_info.vgpu_type_id == 65U);
    assert(s_info.pci_device_id == 0x1bb3U);
    assert(s_info.frl_fps == 60U);
    assert(strcmp(s_info.host_driver_ver, NV_VGPU_DRIVER_VERSION_STR) == 0);
    assert(strcmp(s_info.profile_name, "GRID P4-4Q") == 0);
    assert(strcmp(s_info.guest_driver_ver, "580.196.07") == 0);
    assert(strcmp(s_info.guest_branch_ver, "r580_00") == 0);
    free(blob_50);

    /* 4) Guest pulls full 8-chunk 0x9c GR_STATIC_INFO (31,536 B) */
    uint8_t *blob_9c = (uint8_t *)calloc(1U, OPEN_VGPU_GUEST_GR_INFO_LEN);
    open_vgpu_guest_gr_summary_t gr_info;
    assert(blob_9c != NULL);
    assert(open_vgpu_guest_fetch_gr_static_info(&guest, blob_9c, &gr_info, &st) == 0 && st == 0U);
    assert(gr_info.pmc_boot0 == 0x134000a1U);
    assert(gr_info.gpc_count == 4U && gr_info.tpc_count == 20U && gr_info.cuda_cores == 2560U);
    assert(gr_info.pdb_props_word0 == 0x00001234U);
    free(blob_9c);

    /* 5) Full Guest Client / Device / Memory / BAR1 / MMU / Channel Lifecycle */
    uint32_t h_cli = 0xc1d00077U, h_dev = 0xaa010001U, h_sub = 0xaa010002U;
    uint32_t h_vid = 0xbb010001U, h_sys = 0xbb010002U, h_virt = 0xbb010003U;
    uint32_t h_dma = 0xdd010001U, h_chan = 0xcc010001U;

    assert(open_vgpu_guest_alloc_root(&guest, h_cli, &st) == 0 && st == 0U);
    assert(open_vgpu_guest_alloc_device(&guest, h_cli, h_dev, &st) == 0 && st == 0U);
    assert(open_vgpu_guest_alloc_subdevice(&guest, h_cli, h_dev, h_sub, &st) == 0 && st == 0U);

    assert(open_vgpu_guest_alloc_vidmem(&guest, h_cli, h_dev, h_vid, 0x10000ULL, 0x100ULL, &st) == 0 && st == 0U);
    uint64_t gpfns[4] = { 0x200ULL, 0x201ULL, 0x202ULL, 0x203ULL };
    assert(open_vgpu_guest_alloc_sysmem(&guest, h_cli, h_dev, h_sys, 0x4000ULL, gpfns, 4U, &st) == 0 && st == 0U);
    assert(open_vgpu_guest_alloc_virtmem(&guest, h_cli, h_dev, h_virt, 0x100000ULL, &st) == 0 && st == 0U);

    uint64_t bar1_off = 0ULL;
    assert(open_vgpu_guest_map_bar1(&guest, h_cli, h_dev, h_vid, 0x10000ULL, &bar1_off, &st) == 0 && st == 0U);

    assert(open_vgpu_guest_alloc_ctx_dma(&guest, h_cli, h_dma, h_sys, 0x3fffULL, &st) == 0 && st == 0U);
    uint64_t dma_off = 0x4000ULL;
    assert(open_vgpu_guest_map_dma(&guest, h_cli, h_dev, h_dma, h_sys, 0x4000ULL, &dma_off, &st) == 0 && st == 0U);
    assert(dma_off == 0x4000ULL);

    assert(open_vgpu_guest_fill_pte_mem(&guest, h_cli, h_dev, 4U, &st) == 0 && st == 0U);
    assert(open_vgpu_guest_update_pde_2(&guest, h_cli, h_dev, &st) == 0 && st == 0U);
    assert(open_vgpu_guest_update_gpu_pdes(&guest, h_cli, h_dev, 0x400000000ULL, 1U, &st) == 0 && st == 0U);

    assert(open_vgpu_guest_alloc_channel_dma(&guest, h_cli, h_dev, h_chan,
                                             OPEN_VGPU_GUEST_CLASS_CHANNEL_GPFIFO, 64U,
                                             0x802000000ULL, 0xfef200000ULL, &st) == 0 && st == 0U);

    /* 6) Allocate Pascal GP104 engine objects (NVENC, Compute, 3D, DMA Copy, NVDEC) on GPFIFO channel */
    const struct {
        uint32_t h_obj;
        uint32_t h_cls;
    } eng_objs[] = {
        { 0xee010001U, OPEN_VGPU_GUEST_CLASS_VIDEO_ENCODER },
        { 0xee010002U, OPEN_VGPU_GUEST_CLASS_COMPUTE       },
        { 0xee010003U, OPEN_VGPU_GUEST_CLASS_3D            },
        { 0xee010004U, OPEN_VGPU_GUEST_CLASS_DMA_COPY      },
        { 0xee010005U, OPEN_VGPU_GUEST_CLASS_VIDEO_DECODER },
    };
    for (size_t i = 0U; i < sizeof(eng_objs) / sizeof(eng_objs[0]); i++) {
        uint8_t eng_params[32] = { 0 };
        assert(open_vgpu_guest_alloc_object(&guest, h_cli, h_chan, eng_objs[i].h_obj,
                                            eng_objs[i].h_cls, eng_params,
                                            (uint32_t)sizeof(eng_params), &st) == 0 && st == 0U);
    }

    assert(open_vgpu_guest_idle_channels(&guest, h_cli, &st) == 0 && st == 0U);

    for (size_t i = 0U; i < sizeof(eng_objs) / sizeof(eng_objs[0]); i++) {
        assert(open_vgpu_guest_free_obj(&guest, h_cli, h_chan, eng_objs[i].h_obj, &st) == 0 && st == 0U);
    }

    assert(open_vgpu_guest_unmap_bar1(&guest, h_cli, h_dev, h_vid, 0U, bar1_off, &st) == 0 && st == 0U);
    assert(open_vgpu_guest_update_gpu_pdes(&guest, h_cli, h_dev, 0x400000000ULL, 0U, &st) == 0 && st == 0U);
    assert(open_vgpu_guest_unmap_dma(&guest, h_cli, h_dev, h_dma, h_sys, dma_off, &st) == 0 && st == 0U);
    assert(open_vgpu_guest_free_obj(&guest, h_cli, h_dev, h_chan, &st) == 0 && st == 0U);
    assert(open_vgpu_guest_free_obj(&guest, h_cli, h_dev, h_vid, &st) == 0 && st == 0U);
    assert(open_vgpu_guest_free_obj(&guest, h_cli, h_dev, h_sys, &st) == 0 && st == 0U);
    assert(open_vgpu_guest_free_obj(&guest, h_cli, h_cli, h_cli, &st) == 0 && st == 0U);

    uint64_t total_vrpcs = guest.vrpc_calls;
    open_vgpu_guest_shutdown(&guest);
    gp104_dev_shutdown(&g_host_dev);
    open_vgpu_rm_reset_sys_ops();

    for (uint32_t i = 0U; i < MAX_ARENA_PAGES; i++) {
        assert(!g_dma_pages[i].in_use);
    }
    assert(g_open_fds == 0);
    assert(g_pin_calls == 1U && g_pinned_pages == 4ULL && g_unpinned_pages == 4ULL);
    assert(g_map_region_adds == 2U && g_map_region_removes == 2U);
    assert(g_msi_count == total_vrpcs);

    printf("  [PASS] Clean-room Host <-> Guest loopback completed %llu circular VRPCs (0x50 67416B, 0x9c 31536B, BAR1+USERD+PFNs zero leaks)!\n",
           (unsigned long long)total_vrpcs);
    return 0;
}
