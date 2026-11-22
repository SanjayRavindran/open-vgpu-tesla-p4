#include "vmiop_env.h"
#include "rm_client.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Internal data structures
 * ------------------------------------------------------------------------- */
#define SLAB_CHUNK_ELEMS 128U
#define MAX_PEER_CONNS   5U
#define MAX_PRES_SLOTS   16U
#define DEFAULT_SEARCH_PATH "/usr/lib64:/usr/lib/x86_64-linux-gnu:/usr/lib/aarch64-linux-gnu/"

typedef struct slab_block {
    struct slab_block *next;
    uint32_t           base_index;
    uint32_t           pad;
    uint8_t            data[];
} slab_block_t;

typedef struct {
    slab_block_t *head;
    uint32_t      next_base;
    int         (*is_free_fn)(const void *elem);
    uint32_t      elem_size;
} slab_pool_t;

enum {
    SYNC_UNUSED   = 0,
    SYNC_MUTEX    = 1,
    SYNC_COND     = 2,
    SYNC_SPINLOCK = 3,
};

typedef struct {
    pthread_mutex_t    mutex;    /* +0x00 (40 bytes) */
    pthread_cond_t     cond;     /* +0x28 (48 bytes) */
    uint8_t            pad[4];
    int                kind;     /* SYNC_*; reused as sync_kind */
} sync_slot_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int             in_use;
    int             signaled;
} thread_event_slot_t;

typedef struct {
    pthread_t tid;
    void    (*fn)(uint32_t handle, void *arg);
    void     *arg;
    uint32_t  handle;
    int       in_use;
    int       joined;
} thread_slot_t;

typedef struct config_entry {
    struct config_entry *next;
    struct config_entry *prev;
    char key[1024];
    char val[1024];
} config_entry_t;

typedef struct space_range {
    struct space_range   *next;
    uint64_t              start;
    uint64_t              end;
    vmiop_emul_access_fn_t cb;
    void                 *priv;
} space_range_t;

typedef struct {
    int                    kind; /* VGPU_EMUL_* */
    uint64_t               start;
    uint64_t               end;
    uint64_t               len;
    vmiop_emul_access_fn_t cb;
    void                  *priv;
    int                    in_use;
} region_slot_t;

typedef struct {
    uint32_t  slot_id;
    void     *hva;
    uint64_t *gpfn;
    uint64_t  gpa;
    uint64_t  length;
    uint32_t  prot_flags;
    int       mapped;
} guest_mem_slot_t;

typedef struct emul_dev_rec {
    struct emul_dev_rec   *next;
    struct emul_dev_rec   *prev;
    void                  *priv;
    vmiop_emul_access_fn_t cb;
    void                  *tag;
    uint32_t               dev_handle;
} emul_dev_rec_t;

typedef struct plugin_node {
    struct plugin_node   *next;
    struct plugin_node   *prev;
    const vmiop_plugin_t *plugin;
    uint32_t              id;
    uint32_t              downstream_count;
    struct plugin_node   *downstream[MAX_PEER_CONNS];
    uint32_t              upstream_count;
    struct plugin_node   *upstream[MAX_PEER_CONNS];
    void                 *dl_handle;
    uint32_t              dl_idx;
} plugin_node_t;

typedef struct {
    uint32_t handle;
    uint32_t seq;
    uint32_t pending_msg_flag;
    uint32_t last_ack;
    uint32_t cur_state;
} pres_slot_t;

typedef struct {
    vmiope_instance_t inst;
    config_entry_t   *cfg_head;
    slab_pool_t       region_pool;
    slab_pool_t       mem_pool;
    pthread_mutex_t   region_lock;
    pthread_mutex_t   mem_lock;
    space_range_t    *cfg_spaces;
    space_range_t    *mmio_spaces;
    space_range_t    *ioport_spaces;
} inst_bundle_t;

/* -------------------------------------------------------------------------
 * Global Environment State
 * ------------------------------------------------------------------------- */
static pthread_mutex_t g_env_lock     = PTHREAD_MUTEX_INITIALIZER;
static int             g_env_locked   = 0;
static pthread_t       g_env_owner    = 0;

static pthread_mutex_t g_sync_lock    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_plugin_lock  = PTHREAD_MUTEX_INITIALIZER;

static int is_sync_free(const void *p)   { return ((const sync_slot_t *)p)->kind == SYNC_UNUSED; }
static int is_tev_free(const void *p)    { return ((const thread_event_slot_t *)p)->in_use == 0; }
static int is_thr_free(const void *p)    { return ((const thread_slot_t *)p)->in_use == 0; }
static int is_reg_free(const void *p)    { return ((const region_slot_t *)p)->in_use == 0; }
static int is_mem_free(const void *p)    { return ((const guest_mem_slot_t *)p)->mapped == 0; }

static slab_pool_t g_sync_pool   = { NULL, 0, is_sync_free, sizeof(sync_slot_t) };
static slab_pool_t g_tev_pool    = { NULL, 0, is_tev_free,  sizeof(thread_event_slot_t) };
static slab_pool_t g_thr_pool    = { NULL, 0, is_thr_free,  sizeof(thread_slot_t) };

static plugin_node_t        *g_plugins        = NULL;
static uint32_t              g_next_plugin_id = 1;
static const vmiop_plugin_t *g_display_plugin = NULL;
static char                 *g_search_path    = NULL;
static uint8_t               g_guest_uuid[VM_UUID_SIZE];
static uint8_t               g_migration_cap  = 0;
static int                   g_vga_state      = 0;
static uint32_t              g_instance_count = 0;

static emul_dev_rec_t       *g_emul_devs      = NULL;
static pres_slot_t           g_pres_slots[MAX_PRES_SLOTS];
static inst_bundle_t         g_bundles[VMIOP_MAX_INSTANCES];

static vmiope_log_hook_fn_t   g_log_hook      = NULL;
static int default_vfio_ioctl(int fd, unsigned long req, void *arg) { return ioctl(fd, req, arg); }
static vmiope_vfio_ioctl_fn_t g_vfio_ioctl    = default_vfio_ioctl;

/* -------------------------------------------------------------------------
 * Optional Env-Gated Live Trace Tap (OPEN_VGPU_TRACE_DIR)
 * ------------------------------------------------------------------------- */
#define TRACE_MAX_MEMS       128U
#define TRACE_MMIO_CAP       4000U
#define TRACE_HEX_MAX_BYTES  128U
#define TRACE_VRPC_MAGIC     0x43505256U

typedef struct {
    int      in_use;
    uint32_t mem_id;
    uint64_t gpa;
    uint64_t size;
    void    *hva;
} trace_guest_mem_t;

typedef struct {
    uint32_t send_lo, send_hi;
    uint32_t recv_lo, recv_hi;
    uint32_t ev_lo, ev_hi;
    uint32_t uvm_send_lo, uvm_send_hi;
    uint64_t mmio_non8f_logged;
    uint64_t mmio_non8f_suppressed;
    uint64_t mmio_8f_count;
    uint64_t pci_cfg_count;
    uint64_t pin_calls;
    uint64_t pin_pages;
    uint64_t msi_count;
    uint32_t last_req_seq;
    uint32_t last_req_func;
    int      has_req;
    uint32_t last_rep_seq;
    uint32_t last_rep_func;
    uint32_t last_rep_res;
    int      has_rep;
} trace_inst_stats_t;

static pthread_mutex_t    g_trace_lock      = PTHREAD_MUTEX_INITIALIZER;
static int                g_trace_init_done = 0;
static FILE              *g_trace_fp        = NULL;
static trace_guest_mem_t  g_trace_mems[VMIOP_MAX_INSTANCES][TRACE_MAX_MEMS];
static trace_inst_stats_t g_trace_stats[VMIOP_MAX_INSTANCES];

static uint64_t trace_mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0ULL;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void trace_ensure_init_locked(void)
{
    if (g_trace_init_done) return;
    g_trace_init_done = 1;
    const char *dir = getenv("OPEN_VGPU_TRACE_DIR");
    if (!dir || dir[0] == '\0') return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/vgpu_trace_pid%ld.jsonl", dir, (long)getpid());
    FILE *fp = fopen(path, "a");
    if (!fp) return;
    g_trace_fp = fp;
    fprintf(g_trace_fp, "{\"ts\":%llu,\"ev\":\"trace_start\",\"pid\":%ld}\n",
            (unsigned long long)trace_mono_ns(), (long)getpid());
    fflush(g_trace_fp);
}

static int trace_active(void)
{
    if (!g_trace_init_done) {
        pthread_mutex_lock(&g_trace_lock);
        trace_ensure_init_locked();
        pthread_mutex_unlock(&g_trace_lock);
    }
    return g_trace_fp != NULL;
}

static int trace_safe_read_mem(const void *hva, void *dst, size_t len)
{
    if (!hva || !dst || len == 0U || len > 0x1000U) return -1;
    uintptr_t page_start = ((uintptr_t)hva) & ~(uintptr_t)0xfffULL;
    uintptr_t page_end   = (((uintptr_t)hva) + len - 1ULL) & ~(uintptr_t)0xfffULL;
    if (msync((void *)page_start, (size_t)(page_end - page_start + 0x1000ULL), MS_ASYNC) != 0) {
        return -1;
    }
    memcpy(dst, hva, len);
    return 0;
}

static void trace_bytes_to_hex(const uint8_t *src, size_t len, char *out, size_t out_sz)
{
    static const char hex[] = "0123456789abcdef";
    size_t pos = 0U;
    for (size_t i = 0; i < len && (pos + 2U) < out_sz; i++) {
        out[pos++] = hex[(src[i] >> 4) & 0xfU];
        out[pos++] = hex[src[i] & 0xfU];
    }
    out[pos] = '\0';
}

static void trace_json_escaped(FILE *fp, const char *s)
{
    fputc('"', fp);
    if (s) {
        for (const char *p = s; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') {
                fputc('\\', fp);
                fputc((int)c, fp);
            } else if (c >= 0x20U) {
                fputc((int)c, fp);
            } else {
                fputc(' ', fp);
            }
        }
    }
    fputc('"', fp);
}

static void trace_mem_add(uint32_t inst, uint32_t mem_id, uint64_t gpa, uint64_t size,
                          uint32_t prot, void *hva)
{
    if (!trace_active() || inst >= VMIOP_MAX_INSTANCES) return;
    pthread_mutex_lock(&g_trace_lock);
    if (g_trace_fp) {
        for (uint32_t i = 0; i < TRACE_MAX_MEMS; i++) {
            if (!g_trace_mems[inst][i].in_use) {
                g_trace_mems[inst][i].in_use = 1;
                g_trace_mems[inst][i].mem_id = mem_id;
                g_trace_mems[inst][i].gpa    = gpa;
                g_trace_mems[inst][i].size   = size;
                g_trace_mems[inst][i].hva    = hva;
                break;
            }
        }
        fprintf(g_trace_fp,
                "{\"ts\":%llu,\"ev\":\"mem_map\",\"inst\":%u,\"id\":\"0x%x\","
                "\"gpa\":\"0x%llx\",\"size\":\"0x%llx\",\"prot\":%u}\n",
                (unsigned long long)trace_mono_ns(), inst, mem_id,
                (unsigned long long)gpa, (unsigned long long)size, prot);
        fflush(g_trace_fp);
    }
    pthread_mutex_unlock(&g_trace_lock);
}

static void trace_mem_remove(uint32_t inst, uint32_t mem_id)
{
    if (!trace_active() || inst >= VMIOP_MAX_INSTANCES) return;
    pthread_mutex_lock(&g_trace_lock);
    if (g_trace_fp) {
        for (uint32_t i = 0; i < TRACE_MAX_MEMS; i++) {
            if (g_trace_mems[inst][i].in_use && g_trace_mems[inst][i].mem_id == mem_id) {
                fprintf(g_trace_fp,
                        "{\"ts\":%llu,\"ev\":\"mem_unmap\",\"inst\":%u,\"id\":\"0x%x\",\"gpa\":\"0x%llx\"}\n",
                        (unsigned long long)trace_mono_ns(), inst, mem_id,
                        (unsigned long long)g_trace_mems[inst][i].gpa);
                fflush(g_trace_fp);
                memset(&g_trace_mems[inst][i], 0, sizeof(g_trace_mems[inst][i]));
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_trace_lock);
}

static void trace_scan_vrpc_pages(uint32_t inst, const char *phase)
{
    if (!trace_active() || inst >= VMIOP_MAX_INSTANCES) return;
    pthread_mutex_lock(&g_trace_lock);
    if (g_trace_fp) {
        trace_inst_stats_t *st = &g_trace_stats[inst];
        for (uint32_t i = 0; i < TRACE_MAX_MEMS; i++) {
            const trace_guest_mem_t *m = &g_trace_mems[inst][i];
            if (!m->in_use || !m->hva || m->size < 0x20ULL || m->size > 0x200000ULL) continue;
            uint8_t buf[TRACE_HEX_MAX_BYTES];
            memset(buf, 0, sizeof(buf));
            if (trace_safe_read_mem(m->hva, buf, 0x20U) != 0) continue;
            uint32_t ver, sig, len, fn, res, spare, seq, flags;
            memcpy(&ver,   buf + 0x00, 4);
            memcpy(&sig,   buf + 0x04, 4);
            memcpy(&len,   buf + 0x08, 4);
            memcpy(&fn,    buf + 0x0c, 4);
            memcpy(&res,   buf + 0x10, 4);
            memcpy(&spare, buf + 0x14, 4);
            memcpy(&seq,   buf + 0x18, 4);
            memcpy(&flags, buf + 0x1c, 4);
            if (sig != TRACE_VRPC_MAGIC || len < 0x20U || (uint64_t)len > m->size) continue;

            size_t dump_len = (len < TRACE_HEX_MAX_BYTES) ? (size_t)len : TRACE_HEX_MAX_BYTES;
            if (dump_len > 0x20U) {
                if (trace_safe_read_mem(m->hva, buf, dump_len) != 0) dump_len = 0x20U;
            }
            char hexbuf[TRACE_HEX_MAX_BYTES * 2U + 1U];
            trace_bytes_to_hex(buf, dump_len, hexbuf, sizeof(hexbuf));

            if (res == 0xffffffffU) {
                if (!st->has_req || st->last_req_seq != seq || st->last_req_func != fn) {
                    st->has_req = 1;
                    st->last_req_seq = seq;
                    st->last_req_func = fn;
                    fprintf(g_trace_fp,
                            "{\"ts\":%llu,\"ev\":\"vrpc_req\",\"inst\":%u,\"phase\":\"%s\","
                            "\"gpa\":\"0x%llx\",\"ver\":\"0x%x\",\"func\":\"0x%x\","
                            "\"len\":%u,\"seq\":%u,\"flags\":\"0x%x\",\"hex\":\"%s\"}\n",
                            (unsigned long long)trace_mono_ns(), inst, phase ? phase : "",
                            (unsigned long long)m->gpa, ver, fn, len, seq, flags, hexbuf);
                    fflush(g_trace_fp);
                }
            } else {
                if (!st->has_rep || st->last_rep_seq != seq ||
                    st->last_rep_func != fn || st->last_rep_res != res) {
                    st->has_rep = 1;
                    st->last_rep_seq = seq;
                    st->last_rep_func = fn;
                    st->last_rep_res = res;
                    fprintf(g_trace_fp,
                            "{\"ts\":%llu,\"ev\":\"vrpc_rep\",\"inst\":%u,\"phase\":\"%s\","
                            "\"gpa\":\"0x%llx\",\"ver\":\"0x%x\",\"func\":\"0x%x\","
                            "\"len\":%u,\"status\":\"0x%x\",\"spare\":\"0x%x\",\"seq\":%u,"
                            "\"flags\":\"0x%x\",\"hex\":\"%s\"}\n",
                            (unsigned long long)trace_mono_ns(), inst, phase ? phase : "",
                            (unsigned long long)m->gpa, ver, fn, len, res, spare, seq, flags, hexbuf);
                    fflush(g_trace_fp);
                }
            }
        }
    }
    pthread_mutex_unlock(&g_trace_lock);
}

static void trace_close_all_locked(void)
{
    if (g_trace_fp) {
        fprintf(g_trace_fp, "{\"ts\":%llu,\"ev\":\"trace_stop\",\"pid\":%ld}\n",
                (unsigned long long)trace_mono_ns(), (long)getpid());
        fflush(g_trace_fp);
        fclose(g_trace_fp);
        g_trace_fp = NULL;
    }
    memset(g_trace_mems, 0, sizeof(g_trace_mems));
    memset(g_trace_stats, 0, sizeof(g_trace_stats));
    g_trace_init_done = 0;
}

static int do_vfio_ioctl(int fd, unsigned long req, void *params, uint32_t size, NV_STATUS *out_status)
{
    if (fd < 0) {
        if (out_status) *out_status = NV_ERR_GENERIC;
        return -1;
    }
    NV_VGPU_DEV_IOCTL_PARAMS wrap;
    memset(&wrap, 0, sizeof(wrap));
    wrap.pCmdParams = (NvP64)(uintptr_t)params;
    wrap.size = size;
    if (g_vfio_ioctl(fd, req, &wrap) != 0) {
        if (out_status) *out_status = NV_ERR_OPERATING_SYSTEM;
        return -1;
    }
    if (out_status) *out_status = wrap.status;
    return 0;
}

/* -------------------------------------------------------------------------
 * Slab Allocator & Reentrant Env Lock
 * ------------------------------------------------------------------------- */
static uint32_t slab_alloc_elem(slab_pool_t *pool, uint32_t *out_id, void **out_ptr)
{
    for (slab_block_t *blk = pool->head; blk != NULL; blk = blk->next) {
        for (uint32_t i = 0; i < SLAB_CHUNK_ELEMS; i++) {
            void *elem = blk->data + (size_t)i * pool->elem_size;
            if (pool->is_free_fn(elem)) {
                memset(elem, 0, pool->elem_size);
                *out_id = blk->base_index + i + 1U;
                *out_ptr = elem;
                return VMIOP_SUCCESS;
            }
        }
    }

    size_t total = sizeof(slab_block_t) + (size_t)SLAB_CHUNK_ELEMS * pool->elem_size;
    slab_block_t *new_blk = (slab_block_t *)calloc(1, total);
    if (!new_blk) {
        *out_id = 0;
        *out_ptr = NULL;
        return VMIOP_ERROR_NO_MEMORY;
    }
    new_blk->base_index = pool->next_base;
    pool->next_base += SLAB_CHUNK_ELEMS;
    if (!pool->head) {
        pool->head = new_blk;
    } else {
        slab_block_t *tail = pool->head;
        while (tail->next) tail = tail->next;
        tail->next = new_blk;
    }
    void *elem = new_blk->data;
    memset(elem, 0, pool->elem_size);
    *out_id = new_blk->base_index + 1U;
    *out_ptr = elem;
    return VMIOP_SUCCESS;
}

static void *slab_lookup(slab_pool_t *pool, uint32_t id)
{
    if (id == 0) return NULL;
    uint32_t idx = id - 1U;
    for (slab_block_t *blk = pool->head; blk != NULL; blk = blk->next) {
        if (idx >= blk->base_index && idx < blk->base_index + SLAB_CHUNK_ELEMS) {
            void *elem = blk->data + (size_t)(idx - blk->base_index) * pool->elem_size;
            return pool->is_free_fn(elem) ? NULL : elem;
        }
    }
    return NULL;
}

static void slab_destroy_pool(slab_pool_t *pool)
{
    slab_block_t *blk = pool->head;
    while (blk) {
        slab_block_t *nxt = blk->next;
        free(blk);
        blk = nxt;
    }
    pool->head = NULL;
    pool->next_base = 0;
}

void vmiope_env_lock(int *was_locked_out)
{
    pthread_t self = pthread_self();
    if (!g_env_locked || !pthread_equal(g_env_owner, self)) {
        pthread_mutex_lock(&g_env_lock);
        g_env_locked = 1;
        g_env_owner = self;
        if (was_locked_out) *was_locked_out = 0;
    } else if (was_locked_out) {
        *was_locked_out = 1;
    }
}

static void env_unlock_if_owner(int *was_unlocked_out)
{
    pthread_t self = pthread_self();
    if (g_env_locked && pthread_equal(g_env_owner, self)) {
        g_env_locked = 0;
        pthread_mutex_unlock(&g_env_lock);
        if (was_unlocked_out) *was_unlocked_out = 1;
    } else if (was_unlocked_out) {
        *was_unlocked_out = 0;
    }
}

void vmiope_env_unlock(int was_locked)
{
    if (was_locked == 0) {
        env_unlock_if_owner(NULL);
    }
}

static uint32_t lock_acquire_submutex(int *was_unlocked_env, pthread_mutex_t *mtx)
{
    env_unlock_if_owner(was_unlocked_env);
    if (pthread_mutex_lock(mtx) != 0) {
        if (*was_unlocked_env) vmiope_env_lock(NULL);
        return VMIOP_ERROR_NO_MEMORY;
    }
    return VMIOP_SUCCESS;
}

static int find_inst_by_dev_handle(uint32_t dev_handle)
{
    for (uint32_t i = 0; i <= g_instance_count && i < VMIOP_MAX_INSTANCES; i++) {
        if (g_bundles[i].inst.dev_handle == dev_handle && dev_handle != 0) {
            return (int)i;
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Internal Range Lists (PCI Config, IO Port, MMIO)
 * ------------------------------------------------------------------------- */
static int range_list_insert(space_range_t **head, uint64_t start, uint64_t end,
                             vmiop_emul_access_fn_t cb, void *priv)
{
    for (space_range_t *r = *head; r != NULL; r = r->next) {
        if ((r->start <= start && start <= r->end) ||
            (r->start <= end && end <= r->end)) {
            return -1;
        }
    }
    space_range_t *nr = (space_range_t *)calloc(1, sizeof(*nr));
    if (!nr) return -1;
    nr->start = start;
    nr->end   = end;
    nr->cb    = cb;
    nr->priv  = priv;
    nr->next  = *head;
    *head = nr;
    return 0;
}

static void range_list_remove(space_range_t **head, uint64_t start)
{
    space_range_t *cur = *head;
    space_range_t *prev = NULL;
    while (cur) {
        if (cur->start == start) {
            if (prev) prev->next = cur->next;
            else *head = cur->next;
            free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static void range_list_clear(space_range_t **head)
{
    space_range_t *cur = *head;
    while (cur) {
        space_range_t *nxt = cur->next;
        free(cur);
        cur = nxt;
    }
    *head = NULL;
}

static uint32_t packed_bdf_key(const vmiope_instance_t *in)
{
    return ((uint32_t)in->pci_bus << 16) | ((uint32_t)in->pci_dev << 8) | in->pci_func;
}

/* -------------------------------------------------------------------------
 * Exported Core VMIOP API
 * ------------------------------------------------------------------------- */
uint32_t vmiop_get_guest_id_type(void)
{
    return 1U;
}

uint32_t vmiop_get_page_size(uint64_t *page_size)
{
    *page_size = 0x1000ULL;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_get_guest_id(uint64_t out_uuid[2])
{
    if (!out_uuid) return VMIOP_ERROR_INVAL;
    memcpy(out_uuid, g_guest_uuid, VM_UUID_SIZE);
    return VMIOP_SUCCESS;
}

uint32_t vmiop_memory_alloc_internal(size_t size, void **out_ptr, int zero)
{
    if (!out_ptr) return VMIOP_ERROR_INVAL;
    void *p = zero ? calloc(size ? size : 1U, 1U) : malloc(size ? size : 1U);
    *out_ptr = p;
    return p ? VMIOP_SUCCESS : VMIOP_ERROR_NO_MEMORY;
}

uint32_t vmiop_memory_free_internal(void *ptr, size_t size)
{
    (void)size;
    if (!ptr) return VMIOP_ERROR_INVAL;
    free(ptr);
    return VMIOP_SUCCESS;
}

uint32_t vmiop_buffer_alloc(vmiop_buffer_t **out_buf, uint32_t src_class,
                            uint32_t dst_class, uint32_t count, uint32_t data_len)
{
    if (!out_buf) return VMIOP_ERROR_INVAL;
    if (src_class > 9U || dst_class > 9U || count == 0U) {
        *out_buf = NULL;
        return VMIOP_ERROR_INVAL;
    }
    size_t hdr = ((size_t)count + 4U) * 16U;
    size_t total = (hdr + (size_t)data_len + 7U) & ~(size_t)7U;
    uint8_t *raw = (uint8_t *)calloc(total, 1U);
    if (!raw) {
        *out_buf = NULL;
        return VMIOP_ERROR_NO_MEMORY;
    }
    vmiop_buffer_t *b = (vmiop_buffer_t *)raw;
    b->src_class = src_class;
    b->dst_class = dst_class;
    b->release = vmiop_buffer_free;
    b->refcnt = 1U;
    b->element_count = count;
    b->elements = (vmiop_iov_t *)(raw + 0x40);
    b->total_alloc_bytes = (uint32_t)total;
    b->elements[0].base = raw + hdr;
    b->elements[0].length = data_len;
    *out_buf = b;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_buffer_free(vmiop_buffer_t *buf)
{
    if (!buf || buf->element_count == 0U) return VMIOP_ERROR_INVAL;
    if (buf->refcnt > 1U) {
        buf->refcnt--;
        return VMIOP_SUCCESS;
    }
    size_t hdr = ((size_t)buf->element_count + 4U) * 16U;
    if ((size_t)buf->total_alloc_bytes < hdr) return VMIOP_ERROR_INVAL;
    free(buf);
    return VMIOP_SUCCESS;
}

static uint32_t vmiop_buffer_read_bytes(const vmiop_buffer_t *buf, uint32_t offset,
                                        uint32_t len, void *scratch, const void **out_ptr)
{
    if (!buf || !scratch || !out_ptr) return VMIOP_ERROR_INVAL;
    uint32_t cnt = buf->element_count;
    vmiop_iov_t *iov = buf->elements;
    uint8_t *dst = NULL;
    uint32_t rem = len;
    uint32_t off = offset;

    if (cnt == 0U || rem == 0U) return VMIOP_SUCCESS;
    for (uint32_t i = 0; i < cnt && rem > 0U; i++) {
        uint64_t ilen = iov[i].length;
        if ((uint64_t)off < ilen) {
            uint32_t avail = (uint32_t)(ilen - off);
            const uint8_t *src = (const uint8_t *)iov[i].base + off;
            if (!dst && avail >= rem) {
                *out_ptr = src;
                return VMIOP_SUCCESS;
            }
            if (!dst) {
                *out_ptr = scratch;
                dst = (uint8_t *)scratch;
            }
            uint32_t take = (avail < rem) ? avail : rem;
            memcpy(dst, src, take);
            dst += take;
            rem -= take;
            off = 0;
        } else {
            off -= (uint32_t)ilen;
        }
    }
    return VMIOP_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Mutex / Spinlock / Condvar / Thread Event / Thread
 * ------------------------------------------------------------------------- */
static uint32_t alloc_sync_kind(uint32_t *out_id, int kind)
{
    if (!out_id) return VMIOP_ERROR_INVAL;
    int unlocked = 0;
    uint32_t st = lock_acquire_submutex(&unlocked, &g_sync_lock);
    if (st != VMIOP_SUCCESS) return st;

    void *ptr = NULL;
    uint32_t id = 0;
    st = slab_alloc_elem(&g_sync_pool, &id, &ptr);
    if (st == VMIOP_SUCCESS) {
        sync_slot_t *s = (sync_slot_t *)ptr;
        int rc = 0;
        if (kind == SYNC_SPINLOCK) {
            rc = pthread_spin_init((pthread_spinlock_t *)&s->mutex, PTHREAD_PROCESS_PRIVATE);
        } else {
            rc = pthread_mutex_init(&s->mutex, NULL);
            if (rc == 0 && kind == SYNC_COND) {
                rc = pthread_cond_init(&s->cond, NULL);
            }
        }
        if (rc == 0) {
            s->kind = kind;
            *out_id = id;
        } else {
            *out_id = 0;
            st = VMIOP_ERROR_NO_MEMORY;
        }
    }
    pthread_mutex_unlock(&g_sync_lock);
    if (unlocked) vmiope_env_lock(NULL);
    return st;
}

uint32_t vmiop_lock_alloc(uint32_t *out_id)     { return alloc_sync_kind(out_id, SYNC_MUTEX); }
uint32_t vmiop_spinlock_alloc(uint32_t *out_id) { return alloc_sync_kind(out_id, SYNC_SPINLOCK); }
uint32_t vmiop_cv_alloc(uint32_t *out_id)       { return alloc_sync_kind(out_id, SYNC_COND); }

uint32_t vmiop_lock_free(uint32_t id)
{
    int unlocked = 0;
    uint32_t st = lock_acquire_submutex(&unlocked, &g_sync_lock);
    if (st != VMIOP_SUCCESS) return st;
    sync_slot_t *s = (sync_slot_t *)slab_lookup(&g_sync_pool, id);
    st = VMIOP_ERROR_INVAL;
    if (s && s->kind == SYNC_MUTEX && pthread_mutex_destroy(&s->mutex) == 0) {
        s->kind = SYNC_UNUSED;
        st = VMIOP_SUCCESS;
    }
    pthread_mutex_unlock(&g_sync_lock);
    if (unlocked) vmiope_env_lock(NULL);
    return st;
}

uint32_t vmiop_lock(uint32_t id, int trylock)
{
    sync_slot_t *s = (sync_slot_t *)slab_lookup(&g_sync_pool, id);
    if (!s || s->kind != SYNC_MUTEX) return VMIOP_ERROR_NOT_FOUND;
    int rc = trylock ? pthread_mutex_trylock(&s->mutex) : pthread_mutex_lock(&s->mutex);
    if (rc == 0) return VMIOP_SUCCESS;
    return (rc == EBUSY) ? VMIOP_ERROR_TIMEOUT : VMIOP_ERROR_INVAL;
}

uint32_t vmiop_unlock(uint32_t id)
{
    sync_slot_t *s = (sync_slot_t *)slab_lookup(&g_sync_pool, id);
    if (!s || s->kind != SYNC_MUTEX) return VMIOP_ERROR_NOT_FOUND;
    return (pthread_mutex_unlock(&s->mutex) == 0) ? VMIOP_SUCCESS : VMIOP_ERROR_INVAL;
}

uint32_t vmiop_spinlock_free(uint32_t id)
{
    int unlocked = 0;
    uint32_t st = lock_acquire_submutex(&unlocked, &g_sync_lock);
    if (st != VMIOP_SUCCESS) return st;
    sync_slot_t *s = (sync_slot_t *)slab_lookup(&g_sync_pool, id);
    st = VMIOP_ERROR_INVAL;
    if (s && s->kind == SYNC_SPINLOCK &&
        pthread_spin_destroy((pthread_spinlock_t *)&s->mutex) == 0) {
        s->kind = SYNC_UNUSED;
        st = VMIOP_SUCCESS;
    }
    pthread_mutex_unlock(&g_sync_lock);
    if (unlocked) vmiope_env_lock(NULL);
    return st;
}

uint32_t vmiop_spin_lock(uint32_t id, int trylock)
{
    sync_slot_t *s = (sync_slot_t *)slab_lookup(&g_sync_pool, id);
    if (!s || s->kind != SYNC_SPINLOCK) return VMIOP_ERROR_NOT_FOUND;
    int rc = trylock ? pthread_spin_trylock((pthread_spinlock_t *)&s->mutex)
                     : pthread_spin_lock((pthread_spinlock_t *)&s->mutex);
    if (rc == 0) return VMIOP_SUCCESS;
    return (rc == EBUSY) ? VMIOP_ERROR_TIMEOUT : VMIOP_ERROR_INVAL;
}

uint32_t vmiop_spin_unlock(uint32_t id)
{
    sync_slot_t *s = (sync_slot_t *)slab_lookup(&g_sync_pool, id);
    if (!s || s->kind != SYNC_SPINLOCK) return VMIOP_ERROR_NOT_FOUND;
    return (pthread_spin_unlock((pthread_spinlock_t *)&s->mutex) == 0)
               ? VMIOP_SUCCESS : VMIOP_ERROR_INVAL;
}

uint32_t vmiop_cv_free(uint32_t id)
{
    int unlocked = 0;
    uint32_t st = lock_acquire_submutex(&unlocked, &g_sync_lock);
    if (st != VMIOP_SUCCESS) return st;
    sync_slot_t *s = (sync_slot_t *)slab_lookup(&g_sync_pool, id);
    st = VMIOP_ERROR_INVAL;
    if (s && s->kind == SYNC_COND &&
        pthread_cond_destroy(&s->cond) == 0 &&
        pthread_mutex_destroy(&s->mutex) == 0) {
        s->kind = SYNC_UNUSED;
        st = VMIOP_SUCCESS;
    }
    pthread_mutex_unlock(&g_sync_lock);
    if (unlocked) vmiope_env_lock(NULL);
    return st;
}

static void ns_to_timespec(uint64_t abs_ns, struct timespec *ts)
{
    uint64_t secs = abs_ns / 1000000000ULL;
    ts->tv_sec = (secs > (uint64_t)0x7fffffffULL) ? (time_t)0x7fffffffLL : (time_t)secs;
    ts->tv_nsec = (long)(abs_ns % 1000000000ULL);
}

uint32_t vmiop_cv_wait(uint32_t lock_id, uint32_t cv_id, uint64_t abs_ns)
{
    sync_slot_t *lk = (sync_slot_t *)slab_lookup(&g_sync_pool, lock_id);
    sync_slot_t *cv = (sync_slot_t *)slab_lookup(&g_sync_pool, cv_id);
    if (!lk || lk->kind != SYNC_MUTEX || !cv || cv->kind != SYNC_COND) {
        return VMIOP_ERROR_NOT_FOUND;
    }
    int rc;
    if (abs_ns == UINT64_MAX) {
        rc = pthread_cond_wait(&cv->cond, &lk->mutex);
    } else {
        struct timespec ts;
        ns_to_timespec(abs_ns, &ts);
        rc = pthread_cond_timedwait(&cv->cond, &lk->mutex, &ts);
        if (rc == ETIMEDOUT) return VMIOP_ERROR_TIMEOUT;
    }
    return (rc == 0) ? VMIOP_SUCCESS : VMIOP_ERROR_INVAL;
}

uint32_t vmiop_cv_signal(uint32_t cv_id)
{
    sync_slot_t *cv = (sync_slot_t *)slab_lookup(&g_sync_pool, cv_id);
    if (!cv || cv->kind != SYNC_COND) return VMIOP_ERROR_NOT_FOUND;
    return (pthread_cond_signal(&cv->cond) == 0) ? VMIOP_SUCCESS : VMIOP_ERROR_INVAL;
}

uint32_t vmiop_cv_broadcast(uint32_t cv_id)
{
    sync_slot_t *cv = (sync_slot_t *)slab_lookup(&g_sync_pool, cv_id);
    if (!cv || cv->kind != SYNC_COND) return VMIOP_ERROR_NOT_FOUND;
    return (pthread_cond_broadcast(&cv->cond) == 0) ? VMIOP_SUCCESS : VMIOP_ERROR_INVAL;
}

uint32_t vmiop_thread_event_alloc(uint32_t *out_id)
{
    if (!out_id) return VMIOP_ERROR_INVAL;
    int unlocked = 0;
    uint32_t st = lock_acquire_submutex(&unlocked, &g_sync_lock);
    if (st != VMIOP_SUCCESS) return st;

    void *ptr = NULL;
    uint32_t id = 0;
    st = slab_alloc_elem(&g_tev_pool, &id, &ptr);
    if (st == VMIOP_SUCCESS) {
        thread_event_slot_t *te = (thread_event_slot_t *)ptr;
        if (pthread_mutex_init(&te->mutex, NULL) == 0 &&
            pthread_cond_init(&te->cond, NULL) == 0) {
            te->in_use = 1;
            te->signaled = 0;
            *out_id = id;
        } else {
            *out_id = 0;
            st = VMIOP_ERROR_NO_MEMORY;
        }
    }
    pthread_mutex_unlock(&g_sync_lock);
    if (unlocked) vmiope_env_lock(NULL);
    return st;
}

uint32_t vmiop_thread_event_free(uint32_t id)
{
    int unlocked = 0;
    uint32_t st = lock_acquire_submutex(&unlocked, &g_sync_lock);
    if (st != VMIOP_SUCCESS) return st;

    thread_event_slot_t *te = (thread_event_slot_t *)slab_lookup(&g_tev_pool, id);
    st = VMIOP_ERROR_NOT_FOUND;
    if (te && te->in_use) {
        pthread_mutex_lock(&te->mutex);
        te->in_use = 0;
        pthread_mutex_unlock(&te->mutex);
        pthread_cond_destroy(&te->cond);
        pthread_mutex_destroy(&te->mutex);
        st = VMIOP_SUCCESS;
    }
    pthread_mutex_unlock(&g_sync_lock);
    if (unlocked) vmiope_env_lock(NULL);
    return st;
}

uint32_t vmiop_thread_event_wait(uint32_t id, uint64_t abs_ns, int clear)
{
    thread_event_slot_t *te = (thread_event_slot_t *)slab_lookup(&g_tev_pool, id);
    if (!te || !te->in_use) return VMIOP_ERROR_NOT_FOUND;

    pthread_mutex_lock(&te->mutex);
    int rc = 0;
    while (!te->signaled && te->in_use) {
        if (abs_ns == UINT64_MAX) {
            rc = pthread_cond_wait(&te->cond, &te->mutex);
        } else {
            struct timespec ts;
            ns_to_timespec(abs_ns, &ts);
            rc = pthread_cond_timedwait(&te->cond, &te->mutex, &ts);
            if (rc != 0) break;
        }
    }
    uint32_t ret = VMIOP_SUCCESS;
    if (!te->in_use) {
        ret = VMIOP_ERROR_NOT_FOUND;
    } else if (te->signaled) {
        if (clear) te->signaled = 0;
    } else if (rc == ETIMEDOUT) {
        ret = VMIOP_ERROR_TIMEOUT;
    } else {
        ret = VMIOP_ERROR_INVAL;
    }
    pthread_mutex_unlock(&te->mutex);
    return ret;
}

uint32_t vmiop_thread_event_post(uint32_t id, int single)
{
    thread_event_slot_t *te = (thread_event_slot_t *)slab_lookup(&g_tev_pool, id);
    if (!te || !te->in_use) return VMIOP_ERROR_NOT_FOUND;

    pthread_mutex_lock(&te->mutex);
    te->signaled = 1;
    if (single) pthread_cond_signal(&te->cond);
    else pthread_cond_broadcast(&te->cond);
    pthread_mutex_unlock(&te->mutex);
    return VMIOP_SUCCESS;
}

uint32_t vmiop_thread_get_time(uint64_t *out_ns)
{
    if (!out_ns) return VMIOP_ERROR_INVAL;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return VMIOP_ERROR_RESOURCE;
    *out_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    return VMIOP_SUCCESS;
}

static void *thread_trampoline(void *ctx)
{
    thread_slot_t *slot = (thread_slot_t *)ctx;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) {
        return NULL;
    }
    /* Wait until creator publishes slot->handle and releases g_sync_lock */
    pthread_mutex_lock(&g_sync_lock);
    void (*fn)(uint32_t, void *) = slot->fn;
    uint32_t handle = slot->handle;
    void *arg = slot->arg;
    pthread_mutex_unlock(&g_sync_lock);

    if (fn) {
        fn(handle, arg);
    }
    return NULL;
}

uint32_t vmiop_thread_alloc(void *arg, void (*fn)(uint32_t handle, void *arg), uint32_t *out_handle)
{
    if (!fn || !out_handle) return VMIOP_ERROR_INVAL;
    int unlocked = 0;
    uint32_t st = lock_acquire_submutex(&unlocked, &g_sync_lock);
    if (st != VMIOP_SUCCESS) return st;

    void *ptr = NULL;
    uint32_t id = 0;
    st = slab_alloc_elem(&g_thr_pool, &id, &ptr);
    if (st == VMIOP_SUCCESS) {
        thread_slot_t *ts = (thread_slot_t *)ptr;
        ts->fn = fn;
        ts->arg = arg;
        ts->handle = id;
        ts->in_use = 1;
        ts->joined = 0;
        if (pthread_create(&ts->tid, NULL, thread_trampoline, ts) == 0) {
            *out_handle = id;
        } else {
            ts->in_use = 0;
            *out_handle = 0;
            st = VMIOP_ERROR_NO_MEMORY;
        }
    }
    pthread_mutex_unlock(&g_sync_lock);
    if (unlocked) vmiope_env_lock(NULL);
    return st;
}

uint32_t vmiop_thread_join(uint32_t handle)
{
    int unlocked = 0;
    uint32_t st = lock_acquire_submutex(&unlocked, &g_sync_lock);
    if (st != VMIOP_SUCCESS) return st;

    thread_slot_t *ts = (thread_slot_t *)slab_lookup(&g_thr_pool, handle);
    pthread_t tid = 0;
    int valid = (ts && ts->in_use && !ts->joined);
    if (valid) {
        tid = ts->tid;
        ts->joined = 1;
    }
    pthread_mutex_unlock(&g_sync_lock);
    if (unlocked) vmiope_env_lock(NULL);

    if (!valid) return VMIOP_ERROR_NOT_FOUND;

    int rc = pthread_join(tid, NULL);

    lock_acquire_submutex(&unlocked, &g_sync_lock);
    ts = (thread_slot_t *)slab_lookup(&g_thr_pool, handle);
    if (ts) ts->in_use = 0;
    pthread_mutex_unlock(&g_sync_lock);
    if (unlocked) vmiope_env_lock(NULL);

    return (rc == 0) ? VMIOP_SUCCESS : VMIOP_ERROR_INVAL;
}

/* -------------------------------------------------------------------------
 * Logging & Value Conversion
 * ------------------------------------------------------------------------- */
uint32_t vmiop_log(uint32_t level, const char *fmt, ...)
{
    static const int prio_map[5] = { LOG_CRIT, LOG_ERR, LOG_NOTICE, LOG_INFO, LOG_DEBUG };
    static const char *const lbl_map[5] = { "fatal", "error", "notice", "info", "debug" };

    int was_locked = 0;
    vmiope_env_lock(&was_locked);

    int prio = (level < 5U) ? prio_map[level] : LOG_CRIT;
    const char *lbl = (level < 5U) ? lbl_map[level] : "fatal";

    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);

    if (g_log_hook) {
        g_log_hook(level, lbl, msg);
    } else {
        syslog(prio, "%s: %s", lbl, msg);
    }

    if (was_locked == 0) {
        vmiope_env_unlock(0);
    }
    return VMIOP_SUCCESS;
}

uint32_t vmiop_convert_value(uint32_t in_type, const void *in_buf, size_t in_size,
                             uint32_t out_type, void *out_buf, size_t out_size)
{
    if (!in_buf || !out_buf) return VMIOP_ERROR_INVAL;
    if ((in_type < 2U && in_size != 8U) || (in_type == 2U && in_size == 0U) || in_type > 2U) {
        return VMIOP_ERROR_INVAL;
    }
    if ((out_type < 2U && out_size != 8U) || (out_type == 2U && out_size == 0U) || out_type > 2U) {
        return VMIOP_ERROR_INVAL;
    }

    if (in_type == 1U) {
        int64_t val = *(const int64_t *)in_buf;
        if (out_type == 1U) {
            *(int64_t *)out_buf = val;
            return VMIOP_SUCCESS;
        }
        if (out_type == 0U) {
            if (val < 0) return VMIOP_ERROR_NO_MEMORY;
            *(uint64_t *)out_buf = (uint64_t)val;
            return VMIOP_SUCCESS;
        }
        int n = snprintf((char *)out_buf, out_size, "%lld", (long long)val);
        return (n >= 0 && (size_t)n < out_size) ? VMIOP_SUCCESS : VMIOP_ERROR_NO_MEMORY;
    }

    if (in_type == 0U) {
        uint64_t val = *(const uint64_t *)in_buf;
        if (out_type == 0U) {
            *(uint64_t *)out_buf = val;
            return VMIOP_SUCCESS;
        }
        if (out_type == 1U) {
            if ((int64_t)val < 0) return VMIOP_ERROR_NO_MEMORY;
            *(int64_t *)out_buf = (int64_t)val;
            return VMIOP_SUCCESS;
        }
        int n = snprintf((char *)out_buf, out_size, "%llu", (unsigned long long)val);
        return (n >= 0 && (size_t)n < out_size) ? VMIOP_SUCCESS : VMIOP_ERROR_NO_MEMORY;
    }

    /* in_type == 2 (cstr) */
    if (out_type == 2U) {
        size_t lim = (in_size < out_size) ? in_size : out_size;
        strncpy((char *)out_buf, (const char *)in_buf, lim);
        return (((const char *)out_buf)[lim - 1U] == '\0')
                   ? VMIOP_SUCCESS : VMIOP_ERROR_NO_MEMORY;
    }

    if (!memchr(in_buf, 0, in_size)) return VMIOP_ERROR_INVAL;
    const char *s = (const char *)in_buf;
    unsigned long long uval = 0;
    if (out_type == 0U) {
        if (sscanf(s, "0x%llx", &uval) == 1 ||
            sscanf(s, "0%llo", &uval) == 1 ||
            sscanf(s, "%llu", &uval) == 1) {
            *(uint64_t *)out_buf = (uint64_t)uval;
            return VMIOP_SUCCESS;
        }
        return VMIOP_ERROR_INVAL;
    } else {
        long long ival = 0;
        if (sscanf(s, "%lli", &ival) == 1) {
            *(int64_t *)out_buf = (int64_t)ival;
            return VMIOP_SUCCESS;
        }
        return VMIOP_ERROR_INVAL;
    }
}

/* -------------------------------------------------------------------------
 * Configuration Parsing & Lookup
 * ------------------------------------------------------------------------- */
static size_t config_token_len(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = 0;
    while (s[len] && s[len] != ',') len++;
    return len;
}

static uint32_t read_cfg_atom(const char **p, char *out)
{
    if (!p || !*p || !out) return VMIOP_ERROR_INVAL;
    const char *s = *p;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0' || *s == '#') {
        *p = s;
        return VMIOP_ERROR_NOT_FOUND;
    }
    char quote = '\0';
    if (*s == '"' || *s == '\'') {
        quote = *s++;
        if (*s == '\0' || *s == '#') return VMIOP_ERROR_INVAL;
    }
    while (*s && *s != '#') {
        if (!quote && isspace((unsigned char)*s)) {
            *out = '\0';
            *p = s;
            return VMIOP_SUCCESS;
        }
        if (quote && *s == quote) {
            s++;
            *out = '\0';
            *p = s;
            return VMIOP_SUCCESS;
        }
        if (*s == '\\') {
            s++;
            if (*s == '\0') break;
        }
        if (*s == '=') {
            s++;
            *out = '\0';
            *p = s;
            return VMIOP_SUCCESS;
        }
        *out++ = *s++;
    }
    if (quote) return VMIOP_ERROR_INVAL;
    *out = '\0';
    *p = s;
    return VMIOP_SUCCESS;
}

const char *vmiope_lookup_config_raw(const char *prefixed_key, uint32_t inst)
{
    if (!prefixed_key || inst >= VMIOP_MAX_INSTANCES) return NULL;
    for (config_entry_t *e = g_bundles[inst].cfg_head; e != NULL; e = e->next) {
        if (strncmp(prefixed_key, e->key, 1024U) == 0) {
            return e->val;
        }
    }
    return NULL;
}

int vmiope_parse_config(const char *cfg, uint32_t inst)
{
    if (!cfg || inst >= VMIOP_MAX_INSTANCES) return VMIOP_ERROR_INVAL;
    size_t slen = strlen(cfg);
    char *compact = (char *)malloc(slen + 1U);
    if (!compact) return VMIOP_ERROR_NO_MEMORY;

    size_t out_i = 0;
    for (size_t i = 0; cfg[i] != '\0'; i++) {
        if (!isspace((unsigned char)cfg[i])) {
            compact[out_i++] = cfg[i];
        }
    }
    compact[out_i] = '\0';

    const char *cur = compact;
    for (;;) {
        size_t tok_len = config_token_len(cur);
        if (tok_len == 0) break;
        if (tok_len >= 0x3f9U) {
            vmiop_log(1, "vmiop_env_log: (0x%x): Config element length greater than max length", inst);
            break;
        }
        char line[1032];
        snprintf(line, tok_len + 9U, "plugin0.%s", cur);
        if (line[0] != '#' && line[0] != '\n') {
            config_entry_t *ent = (config_entry_t *)calloc(1, sizeof(*ent));
            if (!ent) {
                free(compact);
                return VMIOP_ERROR_NO_MEMORY;
            }
            const char *lp = line;
            if (read_cfg_atom(&lp, ent->key) != 0 || read_cfg_atom(&lp, ent->val) != 0) {
                free(ent);
                free(compact);
                return VMIOP_ERROR_INVAL;
            }
            if (vmiope_lookup_config_raw(ent->key, inst) != NULL) {
                free(ent);
            } else {
                ent->next = g_bundles[inst].cfg_head;
                if (ent->next) ent->next->prev = ent;
                g_bundles[inst].cfg_head = ent;
            }
        }
        cur += tok_len;
        if (*cur == ',') cur++;
        if (*cur == '\0') break;
    }
    free(compact);
    return VMIOP_SUCCESS;
}

void vmiope_free_config(uint32_t inst)
{
    if (inst >= VMIOP_MAX_INSTANCES) return;
    config_entry_t *e = g_bundles[inst].cfg_head;
    while (e) {
        config_entry_t *nxt = e->next;
        free(e);
        e = nxt;
    }
    g_bundles[inst].cfg_head = NULL;
}

uint32_t vmiop_config_get(uint32_t dev_handle, const char *key, char **out_val)
{
    if (!out_val || !key || dev_handle == 0U) return VMIOP_ERROR_INVAL;
    int inst = find_inst_by_dev_handle(dev_handle);
    if (inst < 0) return VMIOP_ERROR_NOT_FOUND;

    char full_key[1024];
    snprintf(full_key, sizeof(full_key) - 1U, "plugin0.%s", key);
    full_key[sizeof(full_key) - 1U] = '\0';

    const char *found = vmiope_lookup_config_raw(full_key, (uint32_t)inst);
    if (!found) {
        *out_val = NULL;
        return VMIOP_ERROR_NOT_FOUND;
    }
    size_t len = strlen(found);
    char *copy = (char *)calloc(len + 1U, 1U);
    if (!copy) {
        *out_val = NULL;
        return VMIOP_ERROR_NO_MEMORY;
    }
    memcpy(copy, found, len + 1U);
    *out_val = copy;
    return VMIOP_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Plugin Registry & Inter-Plugin Messaging
 * ------------------------------------------------------------------------- */
int vmiope_register_plugin_internal(const vmiop_plugin_t *plugin, uint32_t *out_handle)
{
    if (!out_handle) return VMIOP_ERROR_INVAL;
    if (!plugin) {
        *out_handle = 0;
        return VMIOP_ERROR_INVAL;
    }
    int was_locked = 0;
    vmiope_env_lock(&was_locked);

    if (g_next_plugin_id == UINT32_MAX) {
        *out_handle = 0;
        if (was_locked == 0) vmiope_env_unlock(0);
        return VMIOP_ERROR_NO_MEMORY;
    }
    plugin_node_t *node = (plugin_node_t *)calloc(1, sizeof(*node));
    if (!node) {
        *out_handle = 0;
        if (was_locked == 0) vmiope_env_unlock(0);
        return VMIOP_ERROR_NO_MEMORY;
    }
    uint32_t id = g_next_plugin_id++;
    node->plugin = plugin;
    node->id = id;
    node->next = g_plugins;
    if (node->next) node->next->prev = node;
    g_plugins = node;

    uint32_t rc = VMIOP_SUCCESS;
    if (plugin->init_routine) {
        vmiope_env_unlock(0);
        rc = plugin->init_routine(id);
        vmiope_env_lock(NULL);
    }
    if (rc != VMIOP_SUCCESS) {
        if (node->prev) node->prev->next = node->next;
        else g_plugins = node->next;
        if (node->next) node->next->prev = node->prev;
        free(node);
        *out_handle = 0;
    } else {
        *out_handle = id;
    }

    if (was_locked == 0) vmiope_env_unlock(0);
    return (int)rc;
}

static plugin_node_t *find_plugin_node(uint32_t id)
{
    for (plugin_node_t *n = g_plugins; n != NULL; n = n->next) {
        if (n->id == id) return n;
    }
    return NULL;
}

int vmiope_unregister_plugin_internal(uint32_t handle)
{
    int was_locked = 0;
    vmiope_env_lock(&was_locked);
    plugin_node_t *n = find_plugin_node(handle);
    if (!n) {
        if (was_locked == 0) vmiope_env_unlock(0);
        return VMIOP_ERROR_NOT_FOUND;
    }
    if (n->prev) n->prev->next = n->next;
    else g_plugins = n->next;
    if (n->next) n->next->prev = n->prev;
    if (n->dl_handle) dlclose(n->dl_handle);
    free(n);
    if (was_locked == 0) vmiope_env_unlock(0);
    return VMIOP_SUCCESS;
}

static int add_peer_edge(uint32_t *count, plugin_node_t *peers[MAX_PEER_CONNS], plugin_node_t *peer)
{
    uint32_t c = *count;
    if (c >= MAX_PEER_CONNS) return VMIOP_ERROR_NO_MEMORY;
    uint32_t ic = peer->plugin->input_classes;
    for (uint32_t i = 0; i < c; i++) {
        if (peers[i]->plugin->input_classes & ic) {
            return VMIOP_ERROR_EXISTS;
        }
    }
    peers[c] = peer;
    *count = c + 1U;
    return VMIOP_SUCCESS;
}

int vmiope_connect_plugins_internal(uint32_t upstream_id, uint32_t downstream_id)
{
    if (upstream_id == 0U || downstream_id == 0U) return VMIOP_ERROR_INVAL;
    int was_locked = 0;
    vmiope_env_lock(&was_locked);

    plugin_node_t *up = find_plugin_node(upstream_id);
    plugin_node_t *dn = find_plugin_node(downstream_id);
    int rc = VMIOP_ERROR_INVAL;
    if (!up || !dn) {
        rc = VMIOP_ERROR_NOT_FOUND;
    } else if (up->plugin->connect_downstream_required && dn->plugin->connect_upstream_required) {
        rc = add_peer_edge(&up->downstream_count, up->downstream, dn);
        if (rc == VMIOP_SUCCESS) {
            rc = add_peer_edge(&dn->upstream_count, dn->upstream, up);
        }
    }

    if (was_locked == 0) vmiope_env_unlock(0);
    return rc;
}

uint32_t vmiop_find_plugin(uint32_t *id_inout, const vmiop_plugin_t **out_plugin)
{
    if (!id_inout || !out_plugin) return VMIOP_ERROR_INVAL;
    int was_locked = 0;
    vmiope_env_lock(&was_locked);

    plugin_node_t *best = NULL;
    for (plugin_node_t *n = g_plugins; n != NULL; n = n->next) {
        if (n->id > *id_inout) {
            if (!best || n->id < best->id) best = n;
        }
    }
    uint32_t rc;
    if (!best) {
        *id_inout = 0;
        *out_plugin = NULL;
        rc = VMIOP_ERROR_NOT_FOUND;
    } else {
        *id_inout = best->id;
        *out_plugin = best->plugin;
        rc = VMIOP_SUCCESS;
    }

    if (was_locked == 0) vmiope_env_unlock(0);
    return rc;
}

uint32_t vmiop_deliver_message(uint32_t src_id, vmiop_buffer_t *buf, uint32_t dir)
{
    if (!buf || src_id == 0U) return VMIOP_ERROR_INVAL;
    int was_locked = 0;
    vmiope_env_lock(&was_locked);

    plugin_node_t *src = find_plugin_node(src_id);
    if (!src) {
        if (was_locked == 0) vmiope_env_unlock(0);
        return VMIOP_ERROR_NOT_FOUND;
    }

    plugin_node_t **list = (dir == 1U) ? src->upstream : src->downstream;
    uint32_t cnt = (dir == 1U) ? src->upstream_count : src->downstream_count;
    uint32_t mask = 1U << (buf->dst_class & 31U);
    plugin_node_t *target = NULL;
    for (uint32_t i = 0; i < cnt; i++) {
        if (list[i]->plugin->input_classes & mask) {
            target = list[i];
            break;
        }
    }
    if (!target || !target->plugin->deliver_message) {
        if (was_locked == 0) vmiope_env_unlock(0);
        return VMIOP_ERROR_NOT_FOUND;
    }

    uint32_t (*deliver_fn)(uint32_t, vmiop_buffer_t *) = target->plugin->deliver_message;
    uint32_t target_id = target->id;
    vmiope_env_unlock(0);
    uint32_t rc = deliver_fn(target_id, buf);
    if (was_locked != 0) vmiope_env_lock(NULL);
    return rc;
}

/* -------------------------------------------------------------------------
 * Emulated Devices, Guest Regions, Guest Memory, Pinning, Interrupts
 * ------------------------------------------------------------------------- */
uint32_t vmiop_register_emul_device(void) { return VMIOP_ERROR_INVAL; }

uint32_t vmiop_register_emul_device_v2(uint32_t handle_in, void *priv, void *access_cb,
                                       void *tag, uint32_t *handle_out)
{
    if (!handle_out) return VMIOP_ERROR_INVAL;
    *handle_out = handle_in;
    int inst = find_inst_by_dev_handle(handle_in);
    if (inst < 0) return VMIOP_ERROR_NOT_FOUND;
    if (!access_cb || !tag) return VMIOP_ERROR_INVAL;

    int was_locked = 0;
    vmiope_env_lock(&was_locked);

    emul_dev_rec_t *rec = (emul_dev_rec_t *)calloc(1, sizeof(*rec));
    if (!rec) {
        if (was_locked == 0) vmiope_env_unlock(0);
        return VMIOP_ERROR_NO_MEMORY;
    }
    rec->priv = priv;
    rec->cb = (vmiop_emul_access_fn_t)access_cb;
    rec->tag = tag;
    rec->dev_handle = handle_in;

    uint32_t key = packed_bdf_key(&g_bundles[inst].inst);
    if (range_list_insert(&g_bundles[inst].cfg_spaces, key, key, rec->cb, rec->priv) < 0) {
        vmiop_log(1, "vmiop_env_log: (0x%x): failed to register PCI config space", inst);
        free(rec);
        if (was_locked == 0) vmiope_env_unlock(0);
        return VMIOP_SUCCESS;
    }

    rec->next = g_emul_devs;
    if (rec->next) rec->next->prev = rec;
    g_emul_devs = rec;
    if (was_locked == 0) vmiope_env_unlock(0);
    if (trace_active()) {
        pthread_mutex_lock(&g_trace_lock);
        if (g_trace_fp) {
            fprintf(g_trace_fp,
                    "{\"ts\":%llu,\"ev\":\"reg_emul\",\"inst\":%d,\"dev_h\":%u,\"tag\":\"%s\"}\n",
                    (unsigned long long)trace_mono_ns(), inst, handle_in, (const char *)tag);
            fflush(g_trace_fp);
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    return VMIOP_SUCCESS;
}

uint32_t vmiop_unregister_emul_device(uint32_t handle)
{
    int was_locked = 0;
    vmiope_env_lock(&was_locked);

    uint32_t rc = VMIOP_ERROR_INVAL;
    if (handle != 0U) {
        rc = VMIOP_ERROR_NOT_FOUND;
        for (emul_dev_rec_t *rec = g_emul_devs; rec != NULL; rec = rec->next) {
            if (rec->dev_handle == handle) {
                int inst = find_inst_by_dev_handle(handle);
                if (inst >= 0) {
                    range_list_remove(&g_bundles[inst].cfg_spaces,
                                      packed_bdf_key(&g_bundles[inst].inst));
                    if (rec->prev) rec->prev->next = rec->next;
                    else g_emul_devs = rec->next;
                    if (rec->next) rec->next->prev = rec->prev;
                    free(rec);
                    rc = VMIOP_SUCCESS;
                }
                break;
            }
        }
    }

    if (was_locked == 0) vmiope_env_unlock(0);
    return rc;
}

uint32_t vmiop_create_guest_region(void) { return VMIOP_ERROR_INVAL; }

uint32_t vmiop_create_guest_region_v2(uint32_t dev_handle, uint64_t length, void *access_cb,
                                      void *priv, int space, uint32_t *out_id)
{
    if (!out_id || length == 0ULL) return VMIOP_ERROR_INVAL;
    if (space == VGPU_EMUL_MMIO && ((length + 0xfffULL) < 0x1000ULL)) {
        return VMIOP_ERROR_INVAL;
    }
    int inst = find_inst_by_dev_handle(dev_handle);
    if (inst < 0) return VMIOP_ERROR_NOT_FOUND;

    inst_bundle_t *b = &g_bundles[inst];
    int unlocked = 0;
    uint32_t st = lock_acquire_submutex(&unlocked, &b->region_lock);
    if (st != VMIOP_SUCCESS) return st;

    void *ptr = NULL;
    uint32_t slot = 0;
    st = slab_alloc_elem(&b->region_pool, &slot, &ptr);
    if (st == VMIOP_SUCCESS) {
        region_slot_t *r = (region_slot_t *)ptr;
        r->kind = space;
        r->start = UINT64_MAX;
        r->end = UINT64_MAX;
        r->len = length;
        r->cb = (vmiop_emul_access_fn_t)access_cb;
        r->priv = priv;
        r->in_use = 1;
        *out_id = ((uint32_t)inst << 24) | (slot & 0xffffffU);
    }
    pthread_mutex_unlock(&b->region_lock);
    if (unlocked) vmiope_env_lock(NULL);
    if (st == VMIOP_SUCCESS && trace_active()) {
        pthread_mutex_lock(&g_trace_lock);
        if (g_trace_fp) {
            fprintf(g_trace_fp,
                    "{\"ts\":%llu,\"ev\":\"region_create\",\"inst\":%d,\"id\":\"0x%x\","
                    "\"space\":%d,\"len\":\"0x%llx\"}\n",
                    (unsigned long long)trace_mono_ns(), inst, *out_id, space,
                    (unsigned long long)length);
            fflush(g_trace_fp);
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    return st;
}

uint32_t vmiop_relocate_guest_region(uint32_t region_id, uint64_t start)
{
    uint32_t inst = region_id >> 24;
    if (inst >= VMIOP_MAX_INSTANCES) return VMIOP_ERROR_NOT_FOUND;
    inst_bundle_t *b = &g_bundles[inst];

    int unlocked = 0;
    uint32_t st = lock_acquire_submutex(&unlocked, &b->region_lock);
    if (st != VMIOP_SUCCESS) return st;

    region_slot_t *r = (region_slot_t *)slab_lookup(&b->region_pool, region_id & 0xffffffU);
    if (!r) {
        pthread_mutex_unlock(&b->region_lock);
        if (unlocked) vmiope_env_lock(NULL);
        return VMIOP_ERROR_NOT_FOUND;
    }
    if (r->start == start) {
        pthread_mutex_unlock(&b->region_lock);
        if (unlocked) vmiope_env_lock(NULL);
        return VMIOP_SUCCESS;
    }

    space_range_t **list_head = (r->kind == VGPU_EMUL_IO_PORT) ? &b->ioport_spaces
                              : (r->kind == VGPU_EMUL_MMIO)    ? &b->mmio_spaces
                                                               : NULL;
    if (!list_head) {
        pthread_mutex_unlock(&b->region_lock);
        if (unlocked) vmiope_env_lock(NULL);
        return VMIOP_ERROR_INVAL;
    }
    if (r->start != UINT64_MAX) {
        range_list_remove(list_head, r->start);
    }
    if (start == UINT64_MAX) {
        r->start = UINT64_MAX;
        r->end = UINT64_MAX;
    } else {
        if (r->kind == VGPU_EMUL_IO_PORT && (start >= 0x10000ULL || start + r->len > 0x10000ULL)) {
            r->start = UINT64_MAX;
            r->end = UINT64_MAX;
            st = VMIOP_ERROR_INVAL;
        } else if (range_list_insert(list_head, start, start + r->len - 1ULL, r->cb, r->priv) < 0) {
            r->start = UINT64_MAX;
            r->end = UINT64_MAX;
            st = VMIOP_ERROR_INVAL;
        } else {
            r->start = start;
            r->end = start + r->len - 1ULL;
        }
    }

    pthread_mutex_unlock(&b->region_lock);
    if (unlocked) vmiope_env_lock(NULL);
    if (st == VMIOP_SUCCESS && trace_active()) {
        pthread_mutex_lock(&g_trace_lock);
        if (g_trace_fp) {
            fprintf(g_trace_fp,
                    "{\"ts\":%llu,\"ev\":\"region_reloc\",\"inst\":%u,\"id\":\"0x%x\",\"start\":\"0x%llx\"}\n",
                    (unsigned long long)trace_mono_ns(), inst, region_id,
                    (unsigned long long)start);
            fflush(g_trace_fp);
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    return st;
}

uint32_t vmiop_map_guest_region(uint32_t dev_handle_or_id, uint64_t pmmio_start,
                                uint64_t reserved, uint64_t size, uint64_t vmmio_start, int add)
{
    (void)reserved;
    if (size == 0ULL || add == 2) return VMIOP_ERROR_INVAL;
    uint32_t inst = dev_handle_or_id >> 24;
    if (inst >= VMIOP_MAX_INSTANCES) return VMIOP_ERROR_NOT_FOUND;

    NV_VFIO_VGPU_MMIO_MAPPING_PARAMS p;
    memset(&p, 0, sizeof(p));
    p.virtualMMIOStart  = vmmio_start;
    p.physicalMMIOStart = pmmio_start;
    p.mmioSize          = (uint32_t)size;
    unsigned long req = (add == 1) ? NV_VFIO_VGPU_ADD_MMIO_MAPPING
                                   : NV_VFIO_VGPU_REMOVE_MMIO_MAPPING;
    NV_STATUS st = NV_OK;
    int rc = do_vfio_ioctl(g_bundles[inst].inst.vgpu_fd, req, &p, sizeof(p), &st);
    if (rc < 0 || st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to %s mapping from 0x%llx to 0x%llx of length 0x%llx error 0x%x",
                  inst, (add == 1) ? "add" : "remove",
                  (unsigned long long)vmmio_start,
                  (unsigned long long)pmmio_start,
                  (unsigned long long)size, st);
        return VMIOP_ERROR_INVAL;
    }
    if (trace_active()) {
        pthread_mutex_lock(&g_trace_lock);
        if (g_trace_fp) {
            fprintf(g_trace_fp,
                    "{\"ts\":%llu,\"ev\":\"region_map\",\"inst\":%u,\"add\":%d,"
                    "\"vmmio\":\"0x%llx\",\"pmmio\":\"0x%llx\",\"size\":\"0x%llx\"}\n",
                    (unsigned long long)trace_mono_ns(), inst, add,
                    (unsigned long long)vmmio_start, (unsigned long long)pmmio_start,
                    (unsigned long long)size);
            fflush(g_trace_fp);
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    return VMIOP_SUCCESS;
}

uint32_t vmiop_update_guest_lfb(uint64_t unused, int64_t addr)
{
    (void)unused;
    return (addr == -1) ? VMIOP_ERROR_NOT_FOUND : VMIOP_SUCCESS;
}

uint32_t vmiop_map_guest_memory(void) { return VMIOP_ERROR_INVAL; }

uint32_t vmiop_map_guest_memory_v2(uint32_t dev_handle, uint64_t gpa, uint64_t size,
                                   void **out_hva, uint32_t prot_flags, uint32_t *out_id)
{
    if (!out_hva || !out_id || size == 0ULL) return VMIOP_ERROR_INVAL;
    int inst = find_inst_by_dev_handle(dev_handle);
    if (inst < 0) return VMIOP_ERROR_NOT_FOUND;

    inst_bundle_t *b = &g_bundles[inst];
    int unlocked = 0;
    uint32_t st = lock_acquire_submutex(&unlocked, &b->mem_lock);
    if (st != VMIOP_SUCCESS) return st;

    void *ptr = NULL;
    uint32_t slot_id = 0;
    st = slab_alloc_elem(&b->mem_pool, &slot_id, &ptr);
    if (st != VMIOP_SUCCESS) {
        pthread_mutex_unlock(&b->mem_lock);
        if (unlocked) vmiope_env_lock(NULL);
        return st;
    }
    guest_mem_slot_t *m = (guest_mem_slot_t *)ptr;
    m->mapped = 1; /* reserve slot while mapping outside lock */
    pthread_mutex_unlock(&b->mem_lock);
    if (unlocked) vmiope_env_lock(NULL);

    uint32_t page_count = (uint32_t)((size + 0xfffULL) >> 12);
    size_t pfn_bytes = (size_t)page_count * sizeof(uint64_t);
    uint64_t *gpfn = NULL;
    uint64_t *hpfn = NULL;
    uint8_t *mem_list = NULL;
    void *hva = NULL;

    if (vmiop_memory_alloc_internal(pfn_bytes, (void **)&gpfn, 1) != 0) {
        st = VMIOP_ERROR_NO_MEMORY;
        goto fail;
    }
    if (vmiop_memory_alloc_internal(pfn_bytes, (void **)&hpfn, 1) != 0) {
        st = VMIOP_ERROR_NO_MEMORY;
        goto fail;
    }
    for (uint32_t i = 0; i < page_count; i++) {
        gpfn[i] = (gpa >> 12) + i;
    }

    NV_VFIO_VGPU_PIN_PAGES_PARAMS pin_p;
    memset(&pin_p, 0, sizeof(pin_p));
    pin_p.pGuestPfnMem = (NvP64)(uintptr_t)gpfn;
    pin_p.pHostPfnMem  = (NvP64)(uintptr_t)hpfn;
    pin_p.pageCount    = page_count;
    pin_p.addrType     = GET_HOST_PFN;
    NV_STATUS rm_st = NV_OK;
    if (do_vfio_ioctl(b->inst.vgpu_fd, NV_VFIO_VGPU_PIN_PAGES, &pin_p, sizeof(pin_p), &rm_st) < 0 ||
        rm_st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to translate guest PFN to host PFN. status: 0x%x",
                  inst, rm_st);
        st = VMIOP_ERROR_INVAL;
        goto fail;
    }

    size_t list_sz = pfn_bytes + 0x88U;
    if (vmiop_memory_alloc_internal(list_sz, (void **)&mem_list, 1) != 0) {
        st = VMIOP_ERROR_NO_MEMORY;
        goto fail_unpin;
    }
    *(uint32_t *)(mem_list + 0x40) = page_count;
    memcpy(mem_list + 0x80, hpfn, pfn_bytes);

    void *list_arg = mem_list;
    uint64_t limit = (uint64_t)page_count * 0x1000ULL - 1ULL;
    rm_st = rm_alloc_memory(b->inst.hClient, b->inst.hDevice, slot_id,
                            NV01_MEMORY_LIST_SYSTEM, 0x1010U, &list_arg, &limit);
    if (rm_st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to allocate system memory list hMemory 0x%x, errorCode 0x%x",
                  inst, slot_id, rm_st);
        st = VMIOP_ERROR_INVAL;
        goto fail_unpin;
    }

    rm_st = rm_map_memory(b->inst.hClient, b->inst.hDevice, slot_id, 0,
                          (uint64_t)page_count << 12, &hva, 0);
    if (rm_st != NV_OK || !hva) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to create user space mapping: rmStatus 0x%x",
                  inst, rm_st);
        rm_free(b->inst.hClient, b->inst.hClient, slot_id);
        st = VMIOP_ERROR_INVAL;
        goto fail_unpin;
    }

    m->slot_id = slot_id;
    m->hva = hva;
    m->gpfn = gpfn;
    m->gpa = gpa;
    m->length = size;
    m->prot_flags = prot_flags;
    m->mapped = 1;

    *out_id = ((uint32_t)inst << 24) | (slot_id & 0xffffffU);
    *out_hva = hva;

    trace_mem_add((uint32_t)inst, *out_id, gpa, size, prot_flags, hva);
    trace_scan_vrpc_pages((uint32_t)inst, "mem_map");

    vmiop_memory_free_internal(hpfn, pfn_bytes);
    vmiop_memory_free_internal(mem_list, list_sz);
    return VMIOP_SUCCESS;

fail_unpin:
    {
        NV_VFIO_VGPU_UNPIN_PAGES_PARAMS unp;
        memset(&unp, 0, sizeof(unp));
        unp.pGuestPfnMem = (NvP64)(uintptr_t)gpfn;
        unp.pageCount = page_count;
        unp.addrType = GET_HOST_PFN;
        do_vfio_ioctl(b->inst.vgpu_fd, NV_VFIO_VGPU_UNPIN_PAGES, &unp, sizeof(unp), &rm_st);
    }
fail:
    if (mem_list) vmiop_memory_free_internal(mem_list, pfn_bytes + 0x88U);
    if (hpfn) vmiop_memory_free_internal(hpfn, pfn_bytes);
    if (gpfn) vmiop_memory_free_internal(gpfn, pfn_bytes);
    m->mapped = 0;
    return st;
}

uint32_t vmiop_unmap_guest_memory(uint32_t mem_id)
{
    uint32_t inst = mem_id >> 24;
    if (inst >= VMIOP_MAX_INSTANCES) return VMIOP_ERROR_NOT_FOUND;
    inst_bundle_t *b = &g_bundles[inst];

    int unlocked = 0;
    uint32_t st = lock_acquire_submutex(&unlocked, &b->mem_lock);
    if (st != VMIOP_SUCCESS) return st;

    guest_mem_slot_t *m = (guest_mem_slot_t *)slab_lookup(&b->mem_pool, mem_id & 0xffffffU);
    void *hva = NULL;
    uint64_t *gpfn = NULL;
    uint64_t len = 0;
    if (m && m->mapped && m->hva && m->length && m->gpfn) {
        hva = m->hva;
        gpfn = m->gpfn;
        len = m->length;
        m->hva = NULL;
        m->gpfn = NULL;
        m->length = 0;
        m->mapped = 0;
    }
    pthread_mutex_unlock(&b->mem_lock);
    if (unlocked) vmiope_env_lock(NULL);

    if (!gpfn) return VMIOP_ERROR_NOT_FOUND;

    trace_scan_vrpc_pages(inst, "mem_unmap");
    trace_mem_remove(inst, mem_id);

    uint32_t slot_id = mem_id & 0xffffffU;
    NV_STATUS rm_st = rm_unmap_memory(b->inst.hClient, b->inst.hDevice, slot_id, hva, 0);
    if (rm_st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to unmap system memory: 0x%x", inst, rm_st);
    }

    uint32_t page_count = (uint32_t)((len + 0xfffULL) >> 12);
    uint32_t done = 0;
    uint32_t ret = (rm_st != NV_OK) ? VMIOP_ERROR_INVAL : VMIOP_SUCCESS;
    while (done < page_count) {
        uint32_t chunk = page_count - done;
        if (chunk > 0x20000U) chunk = 0x20000U;
        NV_VFIO_VGPU_UNPIN_PAGES_PARAMS unp;
        memset(&unp, 0, sizeof(unp));
        unp.pGuestPfnMem = (NvP64)(uintptr_t)(gpfn + done);
        unp.pageCount = chunk;
        unp.addrType = GET_HOST_PFN;
        NV_STATUS ust = NV_OK;
        if (do_vfio_ioctl(b->inst.vgpu_fd, NV_VFIO_VGPU_UNPIN_PAGES, &unp, sizeof(unp), &ust) < 0 ||
            ust != NV_OK) {
            ret = VMIOP_ERROR_INVAL;
        }
        done += chunk;
    }

    rm_free(b->inst.hClient, b->inst.hDevice, slot_id);
    vmiop_memory_free_internal(gpfn, (size_t)page_count * sizeof(uint64_t));
    return ret;
}

uint32_t vmiop_pin_guest_pages(void) { return VMIOP_ERROR_INVAL; }

uint32_t vmiop_pin_guest_pages_v2(uint32_t dev_handle, const uint64_t *gpfn,
                                  uint64_t *hpfn, uint32_t count)
{
    if (!gpfn || !hpfn) return VMIOP_ERROR_INVAL;
    if (count == 0U) return VMIOP_SUCCESS;
    int inst = find_inst_by_dev_handle(dev_handle);
    if (inst < 0) return VMIOP_ERROR_NOT_FOUND;

    VGPU_ADDR_TYPE_T addr_type = (g_bundles[inst].inst.pci_dev || g_bundles[inst].inst.pci_func)
                                     ? GET_HOST_PFN : GET_IOVA;
    uint64_t *in_buf = NULL;
    uint64_t *out_buf = NULL;
    size_t bytes = (size_t)count * sizeof(uint64_t);
    if (vmiop_memory_alloc_internal(bytes, (void **)&in_buf, 1) != 0) return VMIOP_ERROR_NO_MEMORY;
    if (vmiop_memory_alloc_internal(bytes, (void **)&out_buf, 1) != 0) {
        vmiop_memory_free_internal(in_buf, bytes);
        return VMIOP_ERROR_NO_MEMORY;
    }

    uint32_t done = 0;
    uint32_t ret = VMIOP_SUCCESS;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > 0x20000U) chunk = 0x20000U;
        memcpy(in_buf, gpfn + done, (size_t)chunk * sizeof(uint64_t));

        NV_VFIO_VGPU_PIN_PAGES_PARAMS pin_p;
        memset(&pin_p, 0, sizeof(pin_p));
        pin_p.pGuestPfnMem = (NvP64)(uintptr_t)in_buf;
        pin_p.pHostPfnMem  = (NvP64)(uintptr_t)out_buf;
        pin_p.pageCount    = chunk;
        pin_p.addrType     = addr_type;

        NV_STATUS rm_st = NV_OK;
        if (do_vfio_ioctl(g_bundles[inst].inst.vgpu_fd, NV_VFIO_VGPU_PIN_PAGES, &pin_p, sizeof(pin_p), &rm_st) < 0 ||
            rm_st != NV_OK) {
            vmiop_log(1, "vmiop_env_log: (0x%x): Failed to translate guest PTEs into host PTE: status: 0x%x",
                      inst, rm_st);
            ret = VMIOP_ERROR_INVAL;
            break;
        }
        memcpy(hpfn + done, out_buf, (size_t)chunk * sizeof(uint64_t));
        done += chunk;
    }

    vmiop_memory_free_internal(in_buf, bytes);
    vmiop_memory_free_internal(out_buf, bytes);
    if (ret == VMIOP_SUCCESS && trace_active() && (uint32_t)inst < VMIOP_MAX_INSTANCES) {
        pthread_mutex_lock(&g_trace_lock);
        g_trace_stats[inst].pin_calls++;
        g_trace_stats[inst].pin_pages += count;
        pthread_mutex_unlock(&g_trace_lock);
    }
    return ret;
}

static uint32_t unpin_guest_pages_internal(uint32_t inst, const uint64_t *gpfn, uint32_t count)
{
    if (!gpfn) return VMIOP_ERROR_INVAL;
    if (count == 0U || inst >= VMIOP_MAX_INSTANCES) return VMIOP_SUCCESS;

    /* Per 0x114470: skip unpin ioctl during VM reset or shutdown teardown */
    if (g_bundles[inst].inst.resetting != 0U || g_bundles[inst].inst.shutting_down != 0U) {
        return VMIOP_SUCCESS;
    }

    VGPU_ADDR_TYPE_T addr_type = (g_bundles[inst].inst.pci_dev || g_bundles[inst].inst.pci_func)
                                     ? GET_HOST_PFN : GET_IOVA;
    uint64_t *tmp = NULL;
    size_t bytes = (size_t)count * sizeof(uint64_t);
    if (vmiop_memory_alloc_internal(bytes, (void **)&tmp, 1) != 0) return VMIOP_ERROR_NO_MEMORY;

    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > 0x20000U) chunk = 0x20000U;
        memcpy(tmp, gpfn + done, (size_t)chunk * sizeof(uint64_t));

        NV_VFIO_VGPU_UNPIN_PAGES_PARAMS unp;
        memset(&unp, 0, sizeof(unp));
        unp.pGuestPfnMem = (NvP64)(uintptr_t)tmp;
        unp.pageCount    = chunk;
        unp.addrType     = addr_type;
        NV_STATUS rm_st  = NV_OK;
        if (do_vfio_ioctl(g_bundles[inst].inst.vgpu_fd, NV_VFIO_VGPU_UNPIN_PAGES, &unp, sizeof(unp), &rm_st) < 0 ||
            rm_st != NV_OK) {
            vmiop_log(1, "vmiop_env_log: (0x%x): Failed to unpin guest pfn gpfn base: 0x%llx, status: 0x%x",
                      inst, (unsigned long long)gpfn[done], rm_st);
        }
        done += chunk;
    }

    vmiop_memory_free_internal(tmp, bytes);
    return VMIOP_SUCCESS;
}

uint32_t vmiop_control_interrupt(uint32_t dev_handle, uint32_t idx, int enable)
{
    (void)enable;
    int inst = find_inst_by_dev_handle(dev_handle);
    if (inst < 0) return VMIOP_ERROR_NOT_FOUND;

    int was_locked = 0;
    vmiope_env_lock(&was_locked);
    uint32_t ret = (idx < 4U) ? VMIOP_SUCCESS : VMIOP_ERROR_INVAL;
    if (ret == VMIOP_SUCCESS) {
        NV_STATUS rm_st = NV_OK;
        do_vfio_ioctl(g_bundles[inst].inst.vgpu_fd, NV_VFIO_VGPU_INJECT_INTERRUPT, NULL, 0, &rm_st);
    }
    if (was_locked == 0) vmiope_env_unlock(0);
    return ret;
}

uint32_t vmiop_set_su(void)  { return VMIOP_SUCCESS; }
uint32_t vmiop_drop_su(void) { return VMIOP_SUCCESS; }

uint32_t vmiop_set_vm_migration_cap(uint8_t enabled)
{
    g_migration_cap = enabled;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_set_swizzid(uint32_t dev_handle, uint8_t enable, uint32_t swizz_id)
{
    int inst = find_inst_by_dev_handle(dev_handle);
    if (inst < 0) return VMIOP_ERROR_NOT_FOUND;
    g_bundles[inst].inst.swizz_enabled = enable;
    g_bundles[inst].inst.swizz_id = swizz_id;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_set_guest_dirty_pages(uint64_t count, const vmiop_dirty_range_t *ranges)
{
    if (!ranges) return VMIOP_ERROR_INVAL;
    if (count == 0ULL) return VMIOP_SUCCESS;
    if (!g_bundles[0].inst.pci_dev && !g_bundles[0].inst.pci_func) return VMIOP_SUCCESS;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t n = ranges[i].count;
        size_t bytes = (size_t)n * sizeof(uint64_t);
        uint64_t *g = NULL, *h = NULL;
        if (vmiop_memory_alloc_internal(bytes, (void **)&g, 1) != 0) return VMIOP_ERROR_NO_MEMORY;
        if (vmiop_memory_alloc_internal(bytes, (void **)&h, 1) != 0) {
            vmiop_memory_free_internal(g, bytes);
            return VMIOP_ERROR_NO_MEMORY;
        }
        for (uint64_t j = 0; j < n; j++) {
            g[j] = ranges[i].start_gpfn + j;
        }
        uint32_t rc = vmiop_pin_guest_pages_v2(g_bundles[0].inst.dev_handle, g, h, (uint32_t)n);
        vmiop_memory_free_internal(g, bytes);
        vmiop_memory_free_internal(h, bytes);
        if (rc != VMIOP_SUCCESS) return rc;
    }
    return VMIOP_SUCCESS;
}

uint32_t vmiop_unhandled_config_access(uint32_t dev_handle, int is_write,
                                       uint64_t offset, uint64_t width, void *data)
{
    int inst = find_inst_by_dev_handle(dev_handle);
    if (inst < 0) return VMIOP_ERROR_NOT_FOUND;
    if (width != 1ULL && width != 2ULL && width != 4ULL) return VMIOP_ERROR_INVAL;
    if (!data) return VMIOP_ERROR_INVAL;

    int unlocked = 0;
    env_unlock_if_owner(&unlocked);

    NV_VFIO_VGPU_VF_REG_ACCESS_HW_PARAMS p;
    memset(&p, 0, sizeof(p));
    p.offset = offset;
    p.width = (uint32_t)width;
    p.emulSpace = VGPU_EMUL_CONFIG_SPACE;
    p.isWrite = (is_write == 1) ? 1U : 0U;
    if (p.isWrite) {
        memcpy(p.data, data, (size_t)width);
    }

    NV_STATUS rm_st = NV_OK;
    int rc = do_vfio_ioctl(g_bundles[inst].inst.vgpu_fd,
                           NV_VFIO_VGPU_VF_REG_ACCESS_HW, &p, sizeof(p), &rm_st);
    uint32_t ret = (rc == 0 && rm_st == NV_OK) ? VMIOP_SUCCESS : VMIOP_ERROR_NO_MEMORY;
    if (ret == VMIOP_SUCCESS && !p.isWrite) {
        memcpy(data, p.data, (size_t)width);
    }
    if (unlocked) vmiope_env_lock(NULL);
    return ret;
}

uint32_t vmiop_vga_init(void)               { return VMIOP_SUCCESS; }
uint32_t vmiop_vga_shutdown(void)           { return VMIOP_SUCCESS; }
uint32_t vmiop_vga_set_callback_table(void) { return VMIOP_SUCCESS; }
uint32_t vmiop_vga_ioport_access(void)      { return VMIOP_SUCCESS; }
void     vmiop_vga_set_VGA_state(int st)    { if (g_vga_state != st) g_vga_state = st; }
int      vmiop_vga_in_VGA_state(void)       { return g_vga_state; }
uint32_t vmiop_vga_update_frame_buffer(int present) { return present ? VMIOP_SUCCESS : VMIOP_ERROR_NOT_FOUND; }

/* -------------------------------------------------------------------------
 * Console / Presentation Plugin
 * ------------------------------------------------------------------------- */
static uint32_t bpp_to_fmt(uint8_t bpp, uint8_t is_bgr)
{
    if (bpp == 15U) return 2U;
    if (bpp == 16U) return 3U;
    if (bpp == 8U)  return 1U;
    return is_bgr ? 5U : 4U;
}

void vmiope_console_update_props(uint32_t inst, const uint32_t *props5)
{
    if (inst >= VMIOP_MAX_INSTANCES) return;
    vmiope_console_state_t *c = &g_bundles[inst].inst.console;
    if (!props5 || g_vga_state != 0) {
        c->width = 0;
        c->height = 0;
        c->stride = 0;
        c->surface_id = 0;
        c->pos_x = 0;
        c->pos_y = 0;
        c->is_bgr = 0;
        return;
    }
    if (props5[2] == 0 && props5[3] == 0) {
        c->blanked = 1;
        if (c->buffer && c->buffer_size) memset(c->buffer, 0, c->buffer_size);
        return;
    }
    c->blanked    = 0;
    c->surface_id = props5[0];
    c->width      = props5[2];
    c->height     = props5[3];
    c->stride     = props5[4];
    c->pos_x      = props5[8];
    c->pos_y      = props5[9];
    c->is_bgr     = props5[11];
}

void vmiope_console_start(uint32_t inst)
{
    if (inst >= VMIOP_MAX_INSTANCES) return;
    vmiope_instance_t *in = &g_bundles[inst].inst;
    uint64_t one = 1ULL;
    if (vmiop_presentation.set_attribute(in->pres_handle, "vnc_console_state", 0, &one, 0) == 0) {
        in->console.started = 1;
    } else {
        vmiope_console_update_props(inst, NULL);
    }
}

void vmiope_console_stop(uint32_t inst)
{
    if (inst >= VMIOP_MAX_INSTANCES) return;
    vmiope_instance_t *in = &g_bundles[inst].inst;
    uint64_t zero = 0ULL;
    if (vmiop_presentation.set_attribute(in->pres_handle, "vnc_console_state", 0, &zero, 0) == 0) {
        in->console.started = 0;
    }
}

static uint32_t pres_init(uint32_t handle)
{
    if (handle == 0U) return VMIOP_ERROR_INVAL;
    int was_locked = 0;
    vmiope_env_lock(&was_locked);
    uint32_t rc = VMIOP_ERROR_NOT_FOUND;
    for (uint32_t i = 0; i < MAX_PRES_SLOTS; i++) {
        if (g_pres_slots[i].handle == 0U) {
            memset(&g_pres_slots[i], 0, sizeof(g_pres_slots[i]));
            g_pres_slots[i].handle = handle;
            g_pres_slots[i].pending_msg_flag = 1U;
            g_pres_slots[i].cur_state = 1U;
            rc = VMIOP_SUCCESS;
            break;
        }
    }
    if (was_locked == 0) vmiope_env_unlock(0);
    return rc;
}

static uint32_t pres_shutdown(uint32_t handle)
{
    for (uint32_t i = 0; i < MAX_PRES_SLOTS; i++) {
        if (g_pres_slots[i].handle == handle) {
            memset(&g_pres_slots[i], 0, sizeof(g_pres_slots[i]));
            break;
        }
    }
    return VMIOP_SUCCESS;
}

static uint32_t pres_get_attribute(uint32_t handle, const char *name,
                                   uint32_t type, void *val, uint32_t len)
{
    (void)handle; (void)name; (void)type; (void)val; (void)len;
    return VMIOP_ERROR_NOT_FOUND;
}

static uint32_t pres_set_attribute(uint32_t handle, const char *name,
                                   uint32_t type, void *val, uint32_t len)
{
    (void)len;
    if (handle == 0U || !val || !name) return VMIOP_ERROR_INVAL;
    for (uint32_t i = 0; i < MAX_PRES_SLOTS; i++) {
        if (g_pres_slots[i].handle == handle && type == 0U &&
            strcmp(name, "vnc_console_state") == 0) {
            vmiop_buffer_t *msg_buf = NULL;
            if (vmiop_buffer_alloc(&msg_buf, 9U, 1U, 1U, 0x28U) != VMIOP_SUCCESS) {
                return VMIOP_ERROR_NO_MEMORY;
            }
            uint8_t *p = (uint8_t *)msg_buf->elements[0].base;
            *(uint32_t *)(p + 0x00) = 0x4f494d56U; /* "VMIO" */
            *(uint32_t *)(p + 0x04) = 0x00010000U;
            *(uint32_t *)(p + 0x08) = 0x24U;
            *(uint32_t *)(p + 0x0c) = 9U;
            *(uint32_t *)(p + 0x10) = g_pres_slots[i].seq++;
            *(uint32_t *)(p + 0x18) = 7U;
            *(uint32_t *)(p + 0x1c) = 4U;
            *(uint32_t *)(p + 0x20) = g_pres_slots[i].cur_state;
            *(uint32_t *)(p + 0x24) = (uint32_t)(*(const uint64_t *)val);
            uint32_t rc = vmiop_deliver_message(handle, msg_buf, 1U);
            vmiop_buffer_free(msg_buf);
            return rc;
        }
    }
    return VMIOP_ERROR_NOT_FOUND;
}

static uint32_t pres_deliver_message(uint32_t handle, vmiop_buffer_t *buf)
{
    static const uint8_t fmt_to_bpp[8] = { 0, 8, 15, 16, 32, 32, 0, 0 };

    uint32_t slot_idx = MAX_PRES_SLOTS;
    for (uint32_t i = 0; i < MAX_PRES_SLOTS; i++) {
        if (g_pres_slots[i].handle == handle) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx >= MAX_PRES_SLOTS) return VMIOP_ERROR_NOT_FOUND;
    if (!buf || buf->element_count == 0U || !buf->elements) return VMIOP_ERROR_INVAL;
    if (buf->elements[0].length <= 0x23U) return VMIOP_ERROR_INVAL;

    uint8_t *hdr = (uint8_t *)buf->elements[0].base;
    if (!hdr) return VMIOP_ERROR_INVAL;
    uint32_t hdr_len = *(uint32_t *)(hdr + 0x08);
    if (buf->elements[0].length < hdr_len || *(uint32_t *)(hdr + 0x0c) != 1U) {
        return VMIOP_ERROR_INVAL;
    }

    pres_slot_t *slot = &g_pres_slots[slot_idx];
    uint32_t op = *(uint32_t *)(hdr + 0x18);
    if (op == 6U) {
        *(uint32_t *)(hdr + 0x24) = 0U;
    }
    if (op == 5U) {
        if (*(uint32_t *)(hdr + 0x20) != slot->cur_state) return VMIOP_ERROR_NOT_FOUND;
    } else if (op == 4U || op == 1U) {
        if (*(uint32_t *)(hdr + 0x20) != slot->cur_state) return VMIOP_ERROR_NOT_FOUND;
        uint32_t scratch[5];
        const void *cfg_ptr = NULL;
        uint32_t rc = vmiop_buffer_read_bytes(buf, 0x24U, 0x14U, scratch, &cfg_ptr);
        if (rc != VMIOP_SUCCESS) return rc;
        const uint32_t *in_cfg = (const uint32_t *)cfg_ptr;
        if (in_cfg[0] != slot->cur_state) return VMIOP_ERROR_INVAL;

        uint32_t fmt_idx = in_cfg[4] < 8U ? in_cfg[4] : 0U;
        uint8_t bpp = fmt_to_bpp[fmt_idx];
        uint32_t w = in_cfg[2];
        uint32_t h = in_cfg[3];
        uint32_t inst_idx = slot_idx;
        for (uint32_t k = 0; k < VMIOP_MAX_INSTANCES; k++) {
            if (g_bundles[k].inst.pres_handle == handle) {
                inst_idx = k;
                break;
            }
        }
        vmiope_console_state_t *c = &g_bundles[inst_idx].inst.console;

        int was_locked = 0;
        vmiope_env_lock(&was_locked);
        buf->pad30 = 0;
        c->surface_id      = in_cfg[0];
        c->width           = w;
        c->height          = h;
        c->format          = in_cfg[4];
        c->stride          = w * in_cfg[4];
        c->bpp             = bpp;
        c->bytes_per_pixel = (bpp + 7U) >> 3;
        c->depth           = (in_cfg[3] * 32U) >> 3;
        if (was_locked == 0) vmiope_env_unlock(0);

        if (op == 1U) {
            uint32_t expected_fmt = bpp_to_fmt((uint8_t)c->bpp, 0);
            uint32_t payload_sz = *(uint32_t *)(hdr + 0x1c) - 0x14U;
            uint32_t exp_bytes = c->stride * c->height;
            if (!c->buffer) return VMIOP_ERROR_INVAL;
            if (c->format != expected_fmt || payload_sz != exp_bytes) {
                return VMIOP_ERROR_INVAL;
            }
            const void *px_ptr = NULL;
            rc = vmiop_buffer_read_bytes(buf, 0x38U, payload_sz, c->buffer, &px_ptr);
            if (rc != VMIOP_SUCCESS) return rc;
            if (px_ptr != c->buffer && payload_sz > 0U && payload_sz <= c->buffer_size) {
                memcpy(c->buffer, px_ptr, payload_sz);
            }
            if (g_vga_state && c->buffer && c->buffer_size) {
                memset(c->buffer, 0, c->buffer_size);
            }
        }
    }

    if (slot->pending_msg_flag != 0U) {
        vmiop_buffer_t *req_buf = NULL;
        if (vmiop_buffer_alloc(&req_buf, 9U, 1U, 1U, 0x38U) == VMIOP_SUCCESS) {
            uint8_t *rp = (uint8_t *)req_buf->elements[0].base;
            *(uint32_t *)(rp + 0x00) = 0x4f494d56U;
            *(uint32_t *)(rp + 0x04) = 0x00010000U;
            *(uint32_t *)(rp + 0x08) = 0x24U;
            *(uint32_t *)(rp + 0x0c) = 9U;
            *(uint32_t *)(rp + 0x10) = slot->seq++;
            *(uint32_t *)(rp + 0x18) = 3U;
            *(uint32_t *)(rp + 0x1c) = 0x14U;
            *(uint32_t *)(rp + 0x20) = slot->cur_state;
            uint32_t inst_idx = slot_idx;
        for (uint32_t k = 0; k < VMIOP_MAX_INSTANCES; k++) {
            if (g_bundles[k].inst.pres_handle == handle) {
                inst_idx = k;
                break;
            }
        }
        vmiope_console_state_t *c = &g_bundles[inst_idx].inst.console;
            *(uint32_t *)(rp + 0x28) = c->width;
            *(uint32_t *)(rp + 0x2c) = c->height;
            *(uint32_t *)(rp + 0x30) = bpp_to_fmt((uint8_t)c->depth, 0);
            uint32_t rc = vmiop_deliver_message(slot->handle, req_buf, 1U);
            if (rc == VMIOP_SUCCESS) {
                slot->pending_msg_flag = 0U;
            } else {
                slot->seq--;
            }
            vmiop_buffer_free(req_buf);
        }
    }
    return VMIOP_SUCCESS;
}

vmiop_plugin_t vmiop_presentation = {
    .length = 0x68U,
    .version = 0x20000U,
    .signature = VMIOP_PLUGIN_SIGNATURE,
    .name = "presentation",
    .plugin_class = 9U,
    .input_classes = 0x202U,
    .connect_downstream_required = 0U,
    .connect_upstream_required = 1U,
    .init_routine = pres_init,
    .shutdown_routine = pres_shutdown,
    .get_attribute = pres_get_attribute,
    .set_attribute = pres_set_attribute,
    .deliver_message = pres_deliver_message,
};

/* -------------------------------------------------------------------------
 * libnvidia-vgpu.so VER_5_0 / VER_2_0 Loader & Migration Hook Wrappers
 * ------------------------------------------------------------------------- */
static uint32_t vmiop_env_inject_msi_cb(uint64_t dev_handle, uint64_t a, uint32_t b)
{
    (void)a; (void)b;
    int inst = find_inst_by_dev_handle((uint32_t)dev_handle);
    if (inst < 0) return VMIOP_ERROR_NOT_FOUND;
    int was_locked = 0;
    vmiope_env_lock(&was_locked);
    NV_STATUS rm_st = NV_OK;
    do_vfio_ioctl(g_bundles[inst].inst.vgpu_fd, NV_VFIO_VGPU_INJECT_INTERRUPT, NULL, 0, &rm_st);
    if (was_locked == 0) vmiope_env_unlock(0);
    if (trace_active() && (uint32_t)inst < VMIOP_MAX_INSTANCES) {
        pthread_mutex_lock(&g_trace_lock);
        g_trace_stats[inst].msi_count++;
        pthread_mutex_unlock(&g_trace_lock);
        trace_scan_vrpc_pages((uint32_t)inst, "msi");
    }
    return VMIOP_SUCCESS;
}

static uint32_t vmiop_env_update_lfb_cb(void)
{
    return VMIOP_SUCCESS;
}

static uint32_t vmiop_env_unpin_pages_cb(uint64_t dev_handle, uint64_t *gpfn,
                                         uint64_t unused, uint32_t count)
{
    (void)unused;
    if (count == 0U) return VMIOP_SUCCESS;
    int inst = find_inst_by_dev_handle((uint32_t)dev_handle);
    if (inst < 0) return VMIOP_ERROR_NOT_FOUND;
    uint32_t rc = unpin_guest_pages_internal((uint32_t)inst, gpfn, count);
    if (rc != 0U) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to unpin guest pfn list. errorCode: 0x%x",
                  inst, rc);
        return VMIOP_ERROR_INVAL;
    }
    return VMIOP_SUCCESS;
}

static uint32_t open_plugin_so(const char *base_name, void **out_dl)
{
    *out_dl = NULL;
    if (!base_name) return VMIOP_ERROR_INVAL;
    size_t blen = strlen(base_name);
    if (blen > 0xffcU) return VMIOP_ERROR_RESOURCE;

    char path[4096];
    snprintf(path, sizeof(path), "%s.so", base_name);
    void *h = dlopen(path, RTLD_NOW);
    if (h) {
        *out_dl = h;
        return VMIOP_SUCCESS;
    }
    vmiop_log(2, "vmiop_env_log: vmioplugin dlopen: %s\n", dlerror());

    if (!g_search_path) return VMIOP_ERROR_INVAL;
    const char *cur = g_search_path;
    while (*cur) {
        const char *colon = strchr(cur, ':');
        size_t dir_len = colon ? (size_t)(colon - cur) : strlen(cur);
        if (dir_len > 0) {
            if (blen + dir_len > 0xffbU) return VMIOP_ERROR_RESOURCE;
            memcpy(path, cur, dir_len);
            snprintf(path + dir_len, sizeof(path) - dir_len, "/%s.so", base_name);
            h = dlopen(path, RTLD_NOW);
            if (h) {
                *out_dl = h;
                return VMIOP_SUCCESS;
            }
            vmiop_log(2, "vmiop_env_log: vmioplugin dlopen: %s\n", dlerror());
        }
        if (!colon) break;
        cur = colon + 1;
    }
    vmiop_log(1, "vmiop_env_log: Unable to find %s on host in %s or via ldconfig \n",
              base_name, g_search_path);
    return VMIOP_ERROR_NOT_FOUND;
}

int vmiope_process_configuration(const char *cfg, uint32_t vgpu_type_id,
                                 const char *pci_bdf_str, uint32_t inst)
{
    if (!cfg || !pci_bdf_str || inst >= VMIOP_MAX_INSTANCES) return VMIOP_ERROR_INVAL;

    int was_locked = 0;
    vmiope_env_lock(&was_locked);

    int rc = vmiope_parse_config(cfg, inst);
    if (rc != 0) {
        if (was_locked == 0) vmiope_env_unlock(0);
        return rc;
    }

    char lib_name[1024];
    const char *env_plug = getenv("OPEN_VGPU_PLUGIN_LIB");
    if (env_plug && env_plug[0] != '\0' && strlen(env_plug) < sizeof(lib_name)) {
        snprintf(lib_name, sizeof(lib_name), "%s", env_plug);
    } else {
        snprintf(lib_name, sizeof(lib_name), "libnvidia-vgpu");
    }

    if (trace_active()) {
        pthread_mutex_lock(&g_trace_lock);
        if (g_trace_fp) {
            fprintf(g_trace_fp, "{\"ts\":%llu,\"ev\":\"plug_load\",\"inst\":%u,\"vgpu_type\":%u,\"lib\":",
                    (unsigned long long)trace_mono_ns(), inst, vgpu_type_id);
            trace_json_escaped(g_trace_fp, lib_name);
            fprintf(g_trace_fp, ",\"bdf\":");
            trace_json_escaped(g_trace_fp, pci_bdf_str);
            fputs("}\n", g_trace_fp);
            fflush(g_trace_fp);
        }
        pthread_mutex_unlock(&g_trace_lock);
    }

    if (inst == 0U) {
        const char *num_s = vmiope_lookup_config_raw("numPlugins", inst);
        if (num_s) {
            uint64_t np = 1ULL;
            if (vmiop_convert_value(2U, num_s, strlen(num_s) + 1U, 0U, &np, 8U) == 0 && np > 0x14U) {
                if (was_locked == 0) vmiope_env_unlock(0);
                return VMIOP_ERROR_INVAL;
            }
        }
        const char *custom_path = vmiope_lookup_config_raw("plugin0.plugin_path", inst);
        if (custom_path) {
            vmiope_set_search_path(custom_path);
        }

        void *dl = NULL;
        rc = (int)open_plugin_so(lib_name, &dl);
        if (rc != 0) {
            if (was_locked == 0) vmiope_env_unlock(0);
            return rc;
        }
        const vmiop_plugin_t *plug = (const vmiop_plugin_t *)dlvsym(dl, "vmiop_display_vmiop_plugin", "VER_5_0");
        if (!plug || !plug->signature || strcmp(plug->signature, VMIOP_PLUGIN_SIGNATURE) != 0 ||
            !plug->name || plug->length != 0xa8U || plug->version != 0x50000U) {
            dlclose(dl);
            if (was_locked == 0) vmiope_env_unlock(0);
            return VMIOP_ERROR_NOT_FOUND;
        }
        g_display_plugin = plug;

        uint64_t *env_tbl = (uint64_t *)dlvsym(dl, "vmiop_display_vmiop_env", "VER_2_0");
        if (env_tbl) {
            env_tbl[0] = (uint64_t)(uintptr_t)vmiop_env_inject_msi_cb;
            env_tbl[1] = (uint64_t)(uintptr_t)vmiop_env_update_lfb_cb;
            *(uint32_t *)&env_tbl[2] = 0U;
            env_tbl[3] = (uint64_t)(uintptr_t)vmiop_env_unpin_pages_cb;
            env_tbl[4] = (uint64_t)(uintptr_t)vmiop_get_guest_id_type;
            vmiop_log(2, "vmiop_env_log: Initialized vmiop_display_vmiop_env VER_2_0");
        }

        plugin_node_t *node = (plugin_node_t *)calloc(1, sizeof(*node));
        if (!node || g_next_plugin_id == UINT32_MAX) {
            free(node);
            dlclose(dl);
            if (was_locked == 0) vmiope_env_unlock(0);
            return VMIOP_ERROR_NO_MEMORY;
        }
        uint32_t dev_h = g_next_plugin_id++;
        node->id = dev_h;
        node->plugin = plug;
        node->dl_handle = dl;
        node->next = g_plugins;
        if (node->next) node->next->prev = node;
        g_plugins = node;

        g_bundles[inst].inst.dev_handle = dev_h;
        g_bundles[inst].region_pool.is_free_fn = is_reg_free;
        g_bundles[inst].region_pool.elem_size = sizeof(region_slot_t);
        pthread_mutex_init(&g_bundles[inst].region_lock, NULL);
        g_bundles[inst].mem_pool.is_free_fn = is_mem_free;
        g_bundles[inst].mem_pool.elem_size = sizeof(guest_mem_slot_t);
        pthread_mutex_init(&g_bundles[inst].mem_lock, NULL);

        uint64_t vtype64 = vgpu_type_id;
        rc = (int)plug->set_attribute(dev_h, "vgpu_type_id", 0U, &vtype64, 8U);
        if (rc == 0) {
            rc = (int)plug->set_attribute(dev_h, "gpu-pci-id", 2U, (void *)pci_bdf_str, 0x400U);
        }
        if (rc == 0) {
            const uint8_t *uuid_ptr = g_bundles[inst].inst.mdev_uuid;
            rc = (int)plug->set_attribute(dev_h, "vgpu-dev-name", 3U, &uuid_ptr, 0x10U);
        }
        if (rc == 0 && plug->get_attribute) {
            uint64_t cap = 0;
            if (plug->get_attribute(dev_h, "vmiop_vgpu_cap", 0U, &cap, 8U) == 0 && (cap & 1ULL)) {
                uint64_t one = 1ULL;
                if (plug->set_attribute(dev_h, "vmiop_vmm_migration_supported", 0U, &one, 8U) != 0) {
                    vmiop_log(2, "vmiop_env_log: Fail to enable migration support");
                }
            }
        }
        if (rc == 0 && plug->init_routine) {
            vmiope_env_unlock(0);
            rc = (int)plug->init_routine(dev_h);
            vmiope_env_lock(NULL);
        }
        if (rc != 0) {
            g_bundles[inst].inst.dev_handle = 0;
            if (node->prev) node->prev->next = node->next;
            else g_plugins = node->next;
            if (node->next) node->next->prev = node->prev;
            free(node);
            dlclose(dl);
        }
    } else {
        if (!g_display_plugin) {
            if (was_locked == 0) vmiope_env_unlock(0);
            return VMIOP_ERROR_NOT_FOUND;
        }
        plugin_node_t *node = (plugin_node_t *)calloc(1, sizeof(*node));
        if (!node || g_next_plugin_id == UINT32_MAX) {
            free(node);
            if (was_locked == 0) vmiope_env_unlock(0);
            return VMIOP_ERROR_NO_MEMORY;
        }
        uint32_t dev_h = g_next_plugin_id++;
        node->id = dev_h;
        node->plugin = g_display_plugin;
        node->next = g_plugins;
        if (node->next) node->next->prev = node;
        g_plugins = node;
        g_bundles[inst].inst.dev_handle = dev_h;

        uint64_t vtype64 = vgpu_type_id;
        rc = (int)g_display_plugin->set_attribute(dev_h, "vgpu_type_id", 0U, &vtype64, 8U);
        if (rc == 0) {
            rc = (int)g_display_plugin->set_attribute(dev_h, "gpu-pci-id", 2U, (void *)pci_bdf_str, 0x400U);
        }
        if (rc == 0) {
            const uint8_t *uuid_ptr = g_bundles[inst].inst.mdev_uuid;
            rc = (int)g_display_plugin->set_attribute(dev_h, "vgpu-dev-name", 3U, &uuid_ptr, 0x10U);
        }
        if (rc == 0 && g_display_plugin->init_routine) {
            vmiope_env_unlock(0);
            rc = (int)g_display_plugin->init_routine(dev_h);
            vmiope_env_lock(NULL);
        }
        if (rc != 0) {
            g_bundles[inst].inst.dev_handle = 0;
            if (node->prev) node->prev->next = node->next;
            else g_plugins = node->next;
            if (node->next) node->next->prev = node->prev;
            free(node);
        }
    }

    if (was_locked == 0) vmiope_env_unlock(0);
    return rc;
}

/* -------------------------------------------------------------------------
 * Emulated Register / Space Access Dispatcher
 * ------------------------------------------------------------------------- */
int vmiope_dispatch_emul_space(uint32_t inst, VGPU_EMUL_SPACE_T space,
                               uint64_t offset, uint32_t width,
                               int is_write, uint8_t data[4])
{
    if (inst >= VMIOP_MAX_INSTANCES || (space != VGPU_EMUL_CONFIG_SPACE &&
        space != VGPU_EMUL_IO_PORT && space != VGPU_EMUL_MMIO)) {
        return -1;
    }

    space_range_t *head = (space == VGPU_EMUL_CONFIG_SPACE) ? g_bundles[inst].cfg_spaces
                        : (space == VGPU_EMUL_MMIO)         ? g_bundles[inst].mmio_spaces
                                                            : g_bundles[inst].ioport_spaces;
    space_range_t *target = NULL;
    if (space == VGPU_EMUL_MMIO) {
        for (space_range_t *r = head; r != NULL; r = r->next) {
            if (r->start <= offset && offset <= r->end) {
                target = r;
                break;
            }
        }
    } else {
        target = head;
    }
    if (!target || !target->cb) {
        vmiop_log(1, "vmiop_env_log: (0x%x): No space found for %s at offset 0x%llx",
                  inst, is_write ? "write" : "read", (unsigned long long)offset);
        return -1;
    }

    uint64_t bar_off = offset & 0xffffffULL;
    if (space == VGPU_EMUL_MMIO && is_write &&
        (bar_off == 0x8f050ULL || bar_off == 0x8f0c0ULL)) {
        trace_scan_vrpc_pages(inst, "doorbell_pre");
    }

    uint32_t rem = width;
    uint64_t off = offset;
    uint32_t pos = 0;
    while (rem > 0U && pos < 4U) {
        uint32_t chunk = 1U;
        if (rem >= 4U && (off & 3ULL) == 0ULL) chunk = 4U;
        else if (rem >= 2U && (off & 1ULL) == 0ULL) chunk = 2U;

        uint32_t val = 0;
        if (is_write) {
            memcpy(&val, data + pos, chunk);
        }

        int was_unlocked = 0;
        env_unlock_if_owner(&was_unlocked);
        uint32_t dummy = 0;
        uint32_t cb_rc = target->cb(target->priv, is_write ? 1U : 0U,
                                    (uint32_t)space, off, chunk, &val, &dummy);
        if (was_unlocked) vmiope_env_lock(NULL);

        if (!is_write) {
            if (cb_rc != 0U) val = 0xffffffffU;
            memcpy(data + pos, &val, chunk);
        }
        if (trace_active()) {
            pthread_mutex_lock(&g_trace_lock);
            if (g_trace_fp) {
                trace_inst_stats_t *ts = &g_trace_stats[inst];
                if (space == VGPU_EMUL_CONFIG_SPACE) {
                    ts->pci_cfg_count++;
                    fprintf(g_trace_fp,
                            "{\"ts\":%llu,\"ev\":\"pci_cfg\",\"inst\":%u,\"rw\":\"%c\","
                            "\"off\":\"0x%llx\",\"w\":%u,\"val\":\"0x%x\",\"rc\":%u}\n",
                            (unsigned long long)trace_mono_ns(), inst, is_write ? 'w' : 'r',
                            (unsigned long long)off, chunk, val, cb_rc);
                } else if (space == VGPU_EMUL_MMIO) {
                    uint64_t chunk_bar_off = off & 0xffffffULL;
                    int is_8f = (chunk_bar_off >= 0x8f000ULL && chunk_bar_off < 0x90000ULL);
                    if (is_8f) {
                        ts->mmio_8f_count++;
                        if (is_write) {
                            uint32_t r = (uint32_t)(chunk_bar_off - 0x8f000ULL);
                            if (r == 0x00U) ts->send_lo = val;
                            else if (r == 0x04U) ts->send_hi = val;
                            else if (r == 0x10U) ts->recv_lo = val;
                            else if (r == 0x14U) ts->recv_hi = val;
                            else if (r == 0x30U) ts->ev_lo = val;
                            else if (r == 0x34U) ts->ev_hi = val;
                            else if (r == 0xb0U) ts->uvm_send_lo = val;
                            else if (r == 0xb4U) ts->uvm_send_hi = val;
                        }
                        fprintf(g_trace_fp,
                                "{\"ts\":%llu,\"ev\":\"mmio\",\"inst\":%u,\"rw\":\"%c\","
                                "\"off\":\"0x%llx\",\"bar_off\":\"0x%llx\",\"w\":%u,\"val\":\"0x%x\",\"rc\":%u}\n",
                                (unsigned long long)trace_mono_ns(), inst, is_write ? 'w' : 'r',
                                (unsigned long long)off, (unsigned long long)chunk_bar_off, chunk, val, cb_rc);
                    } else if (ts->mmio_non8f_logged < TRACE_MMIO_CAP) {
                        ts->mmio_non8f_logged++;
                        fprintf(g_trace_fp,
                                "{\"ts\":%llu,\"ev\":\"mmio\",\"inst\":%u,\"rw\":\"%c\","
                                "\"off\":\"0x%llx\",\"bar_off\":\"0x%llx\",\"w\":%u,\"val\":\"0x%x\",\"rc\":%u}\n",
                                (unsigned long long)trace_mono_ns(), inst, is_write ? 'w' : 'r',
                                (unsigned long long)off, (unsigned long long)chunk_bar_off, chunk, val, cb_rc);
                    } else {
                        ts->mmio_non8f_suppressed++;
                    }
                }
            }
            pthread_mutex_unlock(&g_trace_lock);
        }
        off += chunk;
        pos += chunk;
        rem -= chunk;
    }

    if (space == VGPU_EMUL_MMIO && is_write &&
        (bar_off == 0x8f050ULL || bar_off == 0x8f0c0ULL || bar_off == 0x8f054ULL)) {
        trace_scan_vrpc_pages(inst, (bar_off == 0x8f054ULL) ? "msg_ctl" : "doorbell_post");
    }
    return 0;
}

int vmiope_handle_reg_access(uint32_t inst)
{
    if (inst >= VMIOP_MAX_INSTANCES) return -1;
    vmiope_instance_t *in = &g_bundles[inst].inst;

    NV_VFIO_VGPU_REG_ACCESS_GET_DATA_PARAMS gp;
    memset(&gp, 0, sizeof(gp));
    NV_STATUS st = NV_OK;
    if (do_vfio_ioctl(in->vgpu_fd, NV_VFIO_VGPU_REG_ACCESS_GET_DATA, &gp, sizeof(gp), &st) < 0 ||
        st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to get vgpu register access data: 0x%x\n",
                  inst, st);
        return -1;
    }

    NV_VFIO_VGPU_REG_ACCESS_SET_DATA_PARAMS sp;
    memset(&sp, 0, sizeof(sp));
    if (gp.emulSpace != VGPU_EMUL_CONFIG_SPACE &&
        gp.emulSpace != VGPU_EMUL_IO_PORT &&
        gp.emulSpace != VGPU_EMUL_MMIO) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Unable to process type %d register access\n",
                  inst, gp.emulSpace);
        return -1;
    }

    uint8_t buf[4];
    memcpy(buf, gp.data, 4U);
    if (vmiope_dispatch_emul_space(inst, gp.emulSpace, gp.offset, gp.width, gp.isWrite ? 1 : 0, buf) != 0) {
        return -1;
    }
    if (!gp.isWrite && gp.width <= 4U) {
        memcpy(sp.data, buf, gp.width);
    }

    if (do_vfio_ioctl(in->vgpu_fd, NV_VFIO_VGPU_REG_ACCESS_SET_DATA, &sp, sizeof(sp), &st) < 0 ||
        st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to set vgpu register access data: 0x%x\n",
                  inst, st);
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Plugin Shutdown & Migration Helpers
 * ------------------------------------------------------------------------- */
int vmiope_plugin_reset(uint32_t inst)
{
    if (inst >= VMIOP_MAX_INSTANCES || !g_display_plugin || !g_display_plugin->reset_device) {
        return VMIOP_ERROR_NOT_FOUND;
    }
    uint32_t dev_h = g_bundles[inst].inst.dev_handle;
    if (!dev_h) return VMIOP_ERROR_NOT_FOUND;

    int unlocked = 0;
    env_unlock_if_owner(&unlocked);
    uint32_t rc = g_display_plugin->reset_device(dev_h);
    if (unlocked) vmiope_env_lock(NULL);
    return (int)rc;
}

int vmiope_plugin_notify_mig_state(uint32_t inst, uint32_t state)
{
    if (inst >= VMIOP_MAX_INSTANCES || !g_display_plugin || !g_display_plugin->notify_migration_state) {
        return VMIOP_ERROR_NOT_FOUND;
    }
    int unlocked = 0;
    env_unlock_if_owner(&unlocked);
    uint32_t rc = g_display_plugin->notify_migration_state(g_bundles[inst].inst.dev_handle, state);
    if (unlocked) vmiope_env_lock(NULL);
    return (int)rc;
}

int vmiope_plugin_read_dev_buf(uint32_t inst, void *buf, uint64_t max_len,
                               uint64_t *written, uint64_t *pending)
{
    if (inst >= VMIOP_MAX_INSTANCES || !g_display_plugin || !g_display_plugin->read_device_buffer) {
        return VMIOP_ERROR_NOT_FOUND;
    }
    uint8_t dummy[8] = {0};
    int unlocked = 0;
    env_unlock_if_owner(&unlocked);
    uint32_t rc = g_display_plugin->read_device_buffer(g_bundles[inst].inst.dev_handle,
                                                       buf, max_len, written, pending, dummy);
    if (unlocked) vmiope_env_lock(NULL);
    return (int)rc;
}

int vmiope_plugin_write_dev_buf(uint32_t inst, void *buf, uint64_t len)
{
    if (inst >= VMIOP_MAX_INSTANCES || !g_display_plugin || !g_display_plugin->write_device_buffer) {
        return VMIOP_ERROR_NOT_FOUND;
    }
    int unlocked = 0;
    env_unlock_if_owner(&unlocked);
    uint32_t rc = g_display_plugin->write_device_buffer(g_bundles[inst].inst.dev_handle, buf, len, 0ULL);
    if (unlocked) vmiope_env_lock(NULL);
    return (int)rc;
}

int vmiope_plugin_get_mig_data_size(uint32_t inst, uint64_t *size_out)
{
    if (inst >= VMIOP_MAX_INSTANCES || !g_display_plugin || !g_display_plugin->get_migration_data_size) {
        return VMIOP_ERROR_NOT_FOUND;
    }
    int unlocked = 0;
    env_unlock_if_owner(&unlocked);
    uint32_t rc = g_display_plugin->get_migration_data_size(g_bundles[inst].inst.dev_handle, size_out);
    if (unlocked) vmiope_env_lock(NULL);
    return (int)rc;
}

/* -------------------------------------------------------------------------
 * Initialization, Lifecycle & Cleanup Helpers
 * ------------------------------------------------------------------------- */
void vmiope_set_log_hook(vmiope_log_hook_fn_t hook)     { g_log_hook = hook; }
void vmiope_set_vfio_ioctl_fn(vmiope_vfio_ioctl_fn_t f) { g_vfio_ioctl = f ? f : default_vfio_ioctl; }

void vmiope_set_guest_uuid(const uint8_t uuid[VM_UUID_SIZE])
{
    if (uuid) memcpy(g_guest_uuid, uuid, VM_UUID_SIZE);
    else memset(g_guest_uuid, 0, VM_UUID_SIZE);
}

void vmiope_set_search_path(const char *path)
{
    free(g_search_path);
    g_search_path = path ? strdup(path) : NULL;
}

uint32_t vmiope_initialize_env(void)
{
    vmiope_env_lock(NULL);
    vmiope_set_search_path(DEFAULT_SEARCH_PATH);
    return VMIOP_SUCCESS;
}

vmiope_instance_t *vmiope_get_instance(uint32_t inst)
{
    return (inst < VMIOP_MAX_INSTANCES) ? &g_bundles[inst].inst : NULL;
}

uint32_t vmiope_get_instance_count(void)        { return g_instance_count; }
void     vmiope_set_instance_count(uint32_t c)  { g_instance_count = c; }
void     vmiope_set_migration_cap_byte(uint8_t v) { g_migration_cap = v; }
uint8_t  vmiope_get_migration_cap_byte(void)    { return g_migration_cap; }

void vmiope_init_instance_slot(uint32_t inst, int vgpu_fd, uint16_t vgpu_id,
                               uint32_t gpu_pci_id, uint32_t gpu_pci_bdf,
                               const uint8_t uuid[VM_UUID_SIZE], const char *cfg_params)
{
    if (inst >= VMIOP_MAX_INSTANCES) return;
    inst_bundle_t *b = &g_bundles[inst];
    memset(&b->inst, 0, sizeof(b->inst));
    b->inst.active = 1;
    b->inst.inst_idx = inst;
    b->inst.vgpu_fd = vgpu_fd;
    b->inst.vgpu_id = vgpu_id;
    b->inst.gpu_pci_id = gpu_pci_id;
    b->inst.gpu_pci_bdf = gpu_pci_bdf;
    b->inst.pci_domain = gpu_pci_bdf >> 16;
    b->inst.pci_bus    = (uint8_t)(gpu_pci_bdf >> 8);
    b->inst.pci_dev    = (uint8_t)((gpu_pci_bdf >> 3) & 0x1fU);
    b->inst.pci_func   = (uint8_t)(gpu_pci_bdf & 7U);
    if (uuid) memcpy(b->inst.mdev_uuid, uuid, VM_UUID_SIZE);
    if (cfg_params) snprintf(b->inst.config_params, sizeof(b->inst.config_params), "%s", cfg_params);
    snprintf(b->inst.dev_path, sizeof(b->inst.dev_path), "/dev/nvidia-vgpu%u", (unsigned)vgpu_id);

    b->inst.fd_shutdown   = -1;
    b->inst.fd_reg_access = -1;
    b->inst.fd_bar_info   = -1;
    b->inst.fd_reboot     = -1;
    b->inst.fd_console    = -1;
    b->inst.fd_attach     = -1;
    b->inst.fd_mig_state  = -1;
    b->inst.fd_mig_read   = -1;
    b->inst.fd_mig_write  = -1;
    b->inst.fd_mig_size   = -1;
    b->inst.swizz_id      = UINT32_MAX;

    b->region_pool.is_free_fn = is_reg_free;
    b->region_pool.elem_size = sizeof(region_slot_t);
    pthread_mutex_init(&b->region_lock, NULL);
    b->mem_pool.is_free_fn = is_mem_free;
    b->mem_pool.elem_size = sizeof(guest_mem_slot_t);
    if (!b->mem_pool.head) b->mem_pool.next_base = 1U;
    pthread_mutex_init(&b->mem_lock, NULL);
}

void vmiope_cleanup_instance(uint32_t inst)
{
    if (inst >= VMIOP_MAX_INSTANCES) return;
    inst_bundle_t *b = &g_bundles[inst];
    if (g_display_plugin && g_display_plugin->shutdown_routine && b->inst.dev_handle) {
        int unlocked = 0;
        env_unlock_if_owner(&unlocked);
        g_display_plugin->shutdown_routine(b->inst.dev_handle);
        if (unlocked) vmiope_env_lock(NULL);
    }
    if (b->inst.pres_handle) {
        vmiope_unregister_plugin_internal(b->inst.pres_handle);
        b->inst.pres_handle = 0;
    }
    vmiope_free_config(inst);
    range_list_clear(&b->cfg_spaces);
    range_list_clear(&b->mmio_spaces);
    range_list_clear(&b->ioport_spaces);
    slab_destroy_pool(&b->region_pool);
    slab_destroy_pool(&b->mem_pool);
    if (b->inst.mig_buffer) {
        munmap(b->inst.mig_buffer, NV_VFIO_VGPU_MIGRATION_REGION_SIZE_MMAP);
        b->inst.mig_buffer = NULL;
    }
    if (b->inst.staging_buffer) {
        munmap(b->inst.staging_buffer, NV_VFIO_VGPU_MIGRATION_REGION_SIZE_MMAP);
        b->inst.staging_buffer = NULL;
    }
    if (b->inst.console.buffer) {
        munmap(b->inst.console.buffer, NV_VFIO_VGPU_CONSOLE_SURFACE_SIZE_MAX - 1U);
        b->inst.console.buffer = NULL;
    }
    if (b->inst.active) {
        pthread_mutex_lock(&g_trace_lock);
        if (g_trace_fp) {
            const trace_inst_stats_t *ts = &g_trace_stats[inst];
            fprintf(g_trace_fp,
                    "{\"ts\":%llu,\"ev\":\"inst_summary\",\"inst\":%u,"
                    "\"pin_calls\":%llu,\"pin_pages\":%llu,\"msi_count\":%llu,"
                    "\"pci_cfg_count\":%llu,\"mmio_8f_count\":%llu,"
                    "\"mmio_non8f_logged\":%llu,\"mmio_non8f_suppressed\":%llu}\n",
                    (unsigned long long)trace_mono_ns(), inst,
                    (unsigned long long)ts->pin_calls,
                    (unsigned long long)ts->pin_pages,
                    (unsigned long long)ts->msi_count,
                    (unsigned long long)ts->pci_cfg_count,
                    (unsigned long long)ts->mmio_8f_count,
                    (unsigned long long)ts->mmio_non8f_logged,
                    (unsigned long long)ts->mmio_non8f_suppressed);
            fflush(g_trace_fp);
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    b->inst.active = 0;
}

void vmiope_reset_all(void)
{
    g_env_locked = 0;
    g_env_owner = 0;
    pthread_mutex_init(&g_env_lock, NULL);
    pthread_mutex_init(&g_sync_lock, NULL);
    pthread_mutex_init(&g_plugin_lock, NULL);

    while (g_plugins) {
        plugin_node_t *nxt = g_plugins->next;
        if (g_plugins->dl_handle) dlclose(g_plugins->dl_handle);
        free(g_plugins);
        g_plugins = nxt;
    }
    while (g_emul_devs) {
        emul_dev_rec_t *nxt = g_emul_devs->next;
        free(g_emul_devs);
        g_emul_devs = nxt;
    }
    for (uint32_t i = 0; i < VMIOP_MAX_INSTANCES; i++) {
        vmiope_cleanup_instance(i);
        memset(&g_bundles[i], 0, sizeof(g_bundles[i]));
    }
    slab_destroy_pool(&g_sync_pool);
    slab_destroy_pool(&g_tev_pool);
    slab_destroy_pool(&g_thr_pool);
    memset(g_pres_slots, 0, sizeof(g_pres_slots));
    memset(g_guest_uuid, 0, sizeof(g_guest_uuid));
    free(g_search_path);
    g_search_path = NULL;
    g_display_plugin = NULL;
    g_next_plugin_id = 1;
    g_migration_cap = 0;
    g_vga_state = 0;
    g_instance_count = 0;
    g_log_hook = NULL;
    g_vfio_ioctl = default_vfio_ioctl;
    pthread_mutex_lock(&g_trace_lock);
    trace_close_all_locked();
    pthread_mutex_unlock(&g_trace_lock);
}
