#include "vmiop_env.h"
#include "rm_client.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#define RUN_DIR_PATH "/var/run/nvidia-vgpu-mgr"
#define PID_FILE_PATH "/var/run/nvidia-vgpu-mgr/nvidia-vgpu-mgr.pid"

static volatile sig_atomic_t g_stop_requested = 0;
static int g_pid_fd = -1;
static int g_created_run_dir = 0;
static int g_ctl_dev_fd = -1;

static uint32_t g_hClient = 0;
static uint32_t g_hEvBind = 0;
static uint32_t g_hEvUnbind = 0;
static int g_fd_rm_bind = -1;
static int g_fd_rm_unbind = -1;
static int g_fd_ctl_open = -1;
static int g_fd_ctl_create = -1;
static int g_fd_ctl_delete = -1;
static int g_fd_ctl_instances = -1;

static uint32_t g_qemu_pid = 0;
static char     g_vm_name[128];
static uint32_t g_worker_stop = 0;

static void safe_copy_str(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1U;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* -------------------------------------------------------------------------
 * Ioctl Wrappers (/dev/nvidia-vgpuctl & /dev/nvidia-vgpu*)
 * ------------------------------------------------------------------------- */
static int vgpu_ctl_ioctl(unsigned long req, uint32_t size, void *params)
{
    if (g_ctl_dev_fd < 0) return NV_ERR_INVALID_STATE;
    NV_VGPU_DEV_IOCTL_PARAMS wrap;
    memset(&wrap, 0, sizeof(wrap));
    wrap.pCmdParams = (NvP64)(uintptr_t)params;
    wrap.size = size;
    if (ioctl(g_ctl_dev_fd, req, &wrap) != 0) {
        vmiop_log(1, "vmiop_env_log: Ctrl ioctl on nvidia-vgpu-vfio module failed. 0x%x\n",
                  NV_ERR_OPERATING_SYSTEM);
        return NV_ERR_OPERATING_SYSTEM;
    }
    if (wrap.status != NV_OK) {
        vmiop_log(1, "vmiop_env_log: Ctrl ioctl on nvidia-vgpu-vfio module failed. 0x%x\n",
                  wrap.status);
    }
    return (int)wrap.status;
}

static int create_vfio_event_fd(int *out_fd, uint32_t event_type, uint32_t inst, int is_ctl)
{
    int fd = -1;
    const char *path = is_ctl ? "/dev/nvidia-vgpuctl" : vmiope_get_instance(inst)->dev_path;
    fd = rm_open_rw_cloexec(path);
    if (fd < 0) {
        *out_fd = -1;
        return 0x38;
    }
    *out_fd = fd;

    uint32_t ev_t = event_type;
    NV_VGPU_DEV_IOCTL_PARAMS wrap;
    memset(&wrap, 0, sizeof(wrap));
    wrap.pCmdParams = (NvP64)(uintptr_t)&ev_t;
    wrap.size = sizeof(ev_t);
    unsigned long req = is_ctl ? NV_VFIO_VGPU_CTLDEV_EVENT_CREATE : NV_VFIO_VGPU_EVENT_CREATE;
    if (ioctl(fd, req, &wrap) != 0) {
        close(fd);
        *out_fd = -1;
        return NV_ERR_OPERATING_SYSTEM;
    }
    if (wrap.status != NV_OK) {
        close(fd);
        *out_fd = -1;
        return (int)wrap.status;
    }
    return NV_OK;
}

static void destroy_vfio_event_fd(int *fd_ptr, uint32_t event_type, int is_ctl)
{
    if (!fd_ptr || *fd_ptr < 0) return;
    uint32_t ev_t = event_type;
    NV_VGPU_DEV_IOCTL_PARAMS wrap;
    memset(&wrap, 0, sizeof(wrap));
    wrap.pCmdParams = (NvP64)(uintptr_t)&ev_t;
    wrap.size = sizeof(ev_t);
    unsigned long req = is_ctl ? NV_VFIO_VGPU_CTLDEV_EVENT_DESTROY : NV_VFIO_VGPU_EVENT_DESTROY;
    if (ioctl(*fd_ptr, req, &wrap) != 0 || wrap.status != NV_OK) {
        vmiop_log(1, "vmiop_env_log: Failed to destroy event. id: 0x%x \n", event_type);
    }
    close(*fd_ptr);
    *fd_ptr = -1;
}

/* -------------------------------------------------------------------------
 * Signal Handling & Daemon Shutdown Cleanup
 * ------------------------------------------------------------------------- */
static void daemon_sig_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        g_stop_requested = 1;
    }
}

static void worker_sig_handler(int sig)
{
    (void)sig;
    g_worker_stop = 1U;
}

static void cleanup_and_exit(int code)
{
    if (g_pid_fd >= 0) {
        if (lockf(g_pid_fd, F_ULOCK, 0) < 0) {
            vmiop_log(1, "vmiop_env_log: Failed to unlock pid file (%s)\n", strerror(errno));
        }
        if (close(g_pid_fd) < 0) {
            vmiop_log(1, "vmiop_env_log: Failed to close pid file (%s)\n", strerror(errno));
        }
        g_pid_fd = -1;
        if (unlink(PID_FILE_PATH) < 0 && errno != ENOENT) {
            vmiop_log(1, "vmiop_env_log: Failed to unlink pid file (%s)\n", strerror(errno));
        }
    }
    if (g_created_run_dir) {
        if (rmdir(RUN_DIR_PATH) < 0 && errno != ENOENT && errno != EACCES) {
            vmiop_log(1, "vmiop_env_log: Failed to remove %s (%s)\n", RUN_DIR_PATH, strerror(errno));
        }
    }
    closelog();
    exit(code);
}

/* -------------------------------------------------------------------------
 * GPU Probe & vGPU Types Registration
 * ------------------------------------------------------------------------- */
static int register_vgpu_types_for_gpu(uint32_t hClient, uint32_t hDevice,
                                       uint32_t gpu_id, uint8_t sriov_en,
                                       uint32_t first_vf_off)
{
    uint32_t hSub = ((hClient + gpu_id) << 8) | 0xdae00002U;
    NV2080_ALLOC_PARAMETERS sub_p;
    memset(&sub_p, 0, sizeof(sub_p));
    NV_STATUS st = rm_alloc_object(hClient, hDevice, hSub, NV20_SUBDEVICE_0, &sub_p, sizeof(sub_p));
    if (st != NV_OK) return (int)st;

    uint32_t hCfg = ((hClient + gpu_id) << 8) | 0xdae00003U;
    st = rm_alloc_object(hClient, hSub, hCfg, NVA081_VGPU_CONFIG, NULL, 0);
    if (st != NV_OK) {
        rm_free(hClient, hDevice, hSub);
        return (int)st;
    }

    NVA081_CTRL_VGPU_CONFIG_GET_SUPPORTED_VGPU_TYPES_PARAMS sup;
    memset(&sup, 0, sizeof(sup));
    st = rm_control(hClient, hCfg, NVA081_CTRL_CMD_VGPU_CONFIG_GET_SUPPORTED_VGPU_TYPES,
                    &sup, sizeof(sup));
    if (st != NV_OK) {
        rm_free(hClient, hSub, hCfg);
        rm_free(hClient, hDevice, hSub);
        return (int)st;
    }

    uint32_t ntypes = sup.numVgpuTypes;
    type_info_t *tinfo = NULL;
    if (ntypes > 0U) {
        if (ntypes > NV_MAX_VGPU_TYPE_PER_GPU) ntypes = NV_MAX_VGPU_TYPE_PER_GPU;
        if (vmiop_memory_alloc_internal(ntypes * sizeof(type_info_t), (void **)&tinfo, 1) != 0) {
            rm_free(hClient, hSub, hCfg);
            rm_free(hClient, hDevice, hSub);
            return NV_ERR_NO_MEMORY;
        }
        NVA081_CTRL_VGPU_CONFIG_GET_VGPU_TYPE_INFO_PARAMS tip;
        for (uint32_t i = 0; i < ntypes; i++) {
            memset(&tip, 0, sizeof(tip));
            tip.vgpuType = sup.vgpuTypes[i];
            st = rm_control(hClient, hCfg, NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_TYPE_INFO,
                            &tip, sizeof(tip));
            if (st != NV_OK) {
                vmiop_log(1, "vmiop_env_log: Failed to get type info 0x%x on GPU 0x%x: 0x%x\n",
                          tip.vgpuType, gpu_id, st);
                vmiop_memory_free_internal(tinfo, ntypes * sizeof(type_info_t));
                rm_free(hClient, hSub, hCfg);
                rm_free(hClient, hDevice, hSub);
                return (int)st;
            }
            tinfo[i].vgpu_type_id    = tip.vgpuType;
            tinfo[i].num_heads       = tip.numHeads;
            tinfo[i].max_instance    = tip.maxInstance;
            tinfo[i].max_resolutionx = tip.maxResolutionX;
            tinfo[i].max_resolutiony = tip.maxResolutionY;
            tinfo[i].frl_config      = tip.frlConfig;
            tinfo[i].profileSize     = (uint32_t)(tip.fbLength >> 20);
            memcpy(tinfo[i].vgpu_name, tip.vgpuName, NV_VGPU_STRING_BUFFER_SIZE);
        }
    }

    NV_VFIO_VGPU_CTLDEV_SET_TYPES_INFO_PARAMS set_p;
    memset(&set_p, 0, sizeof(set_p));
    set_p.gpu_pci_id      = gpu_id;
    set_p.num_vgpu_types  = ntypes;
    set_p.first_vf_offset = first_vf_off;
    set_p.sriov_enabled   = sriov_en;
    set_p.type_info       = tinfo;
    int rc = vgpu_ctl_ioctl(NV_VFIO_VGPU_CTLDEV_SET_TYPES_INFO, sizeof(set_p), &set_p);
    if (tinfo) vmiop_memory_free_internal(tinfo, ntypes * sizeof(type_info_t));
    rm_free(hClient, hSub, hCfg);
    rm_free(hClient, hDevice, hSub);
    return rc;
}

static int probe_gpus(uint32_t hClient, int is_daemon_init, uint32_t *out_count, uint32_t *gpu_ids)
{
    int32_t probed[64];
    memset(probed, 0xff, sizeof(probed));

    uint32_t total = 0;
    if (is_daemon_init) {
        NV_STATUS st = rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_GET_PROBED_IDS,
                                  probed, sizeof(probed));
        if (st != NV_OK) {
            vmiop_log(1, "vmiop_env_log: Failed to get probed IDs: 0x%x\n", st);
            return (int)st;
        }
        *out_count = 0;
        while (total < NV_MAX_PROBED_GPUS && probed[total] != -1) total++;
        if (total == 0) return NV_OK;
    } else {
        total = 1;
        probed[0] = (int32_t)gpu_ids[0];
    }

    for (uint32_t i = 0; i < total; i++) {
        uint32_t gpu_id = (uint32_t)probed[i];
        if (gpu_id == UINT32_MAX) continue;

        NV0000_CTRL_GPU_GET_PCI_INFO_PARAMS pci_p;
        memset(&pci_p, 0, sizeof(pci_p));
        pci_p.gpuId = gpu_id;
        NV_STATUS st = rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_GET_PCI_INFO,
                                  &pci_p, sizeof(pci_p));
        if (st != NV_OK) {
            vmiop_log(1, "vmiop_env_log: Failed to get PCI info: 0x%x (gpuId 0x%x)\n", st, gpu_id);
            return (int)st;
        }

        int32_t attach_ids[33];
        memset(attach_ids, 0, sizeof(attach_ids));
        attach_ids[0] = (int32_t)gpu_id;
        attach_ids[1] = -1;
        st = rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_ATTACH_IDS,
                        attach_ids, sizeof(attach_ids));
        if (st != NV_OK) {
            vmiop_log(1, "vmiop_env_log: Failed to attach device: 0x%x (gpuId 0x%x) \n", st, gpu_id);
            return (int)st;
        }

        NV0000_CTRL_GPU_GET_ID_INFO_PARAMS id_p;
        memset(&id_p, 0, sizeof(id_p));
        id_p.gpuId = gpu_id;
        st = rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO, &id_p, sizeof(id_p));
        if (st != NV_OK) {
            vmiop_log(1, "vmiop_env_log: Failed to get gpu ID info: 0x%x (gpuId 0x%x)\n", st, gpu_id);
            attach_ids[0] = (int32_t)gpu_id;
            attach_ids[1] = -1;
            rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_DETACH_IDS, attach_ids, 0x80U);
            return (int)st;
        }

        uint32_t hDev = ((hClient + gpu_id) << 8) | 0xdae00001U;
        NV0080_ALLOC_PARAMETERS dev_p;
        memset(&dev_p, 0, sizeof(dev_p));
        dev_p.deviceId = id_p.deviceInstance;
        dev_p.hClientShare = hClient;
        st = rm_alloc_object(hClient, hClient, hDev, NV01_DEVICE_0, &dev_p, sizeof(dev_p));
        if (st != NV_OK) {
            vmiop_log(1, "vmiop_env_log: Failed to allocate device: 0x%x (hDevice 0x%x, gpuId 0x%x)\n",
                      st, hDev, gpu_id);
            rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_DETACH_IDS, attach_ids, 0x80U);
            return (int)st;
        }

        NV2080_CTRL_GPU_GET_SRIOV_CAPS_PARAMS sriov_p;
        memset(&sriov_p, 0, sizeof(sriov_p));
        st = rm_control(hClient, hDev, NV2080_CTRL_CMD_GPU_GET_SRIOV_CAPS,
                        &sriov_p, sizeof(sriov_p));
        if (st != NV_OK) {
            vmiop_log(1, "vmiop_env_log: Failed to query SRIOV caps info for GPU: 0x%x 0x%x\n",
                      gpu_id, st);
            rm_free(hClient, hClient, hDev);
            rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_DETACH_IDS, attach_ids, 0x80U);
            return (int)st;
        }

        uint32_t persist = 0;
        st = rm_control(hClient, hDev, NV2080_CTRL_CMD_GPU_GET_PERSISTENCE_STATE,
                        &persist, sizeof(persist));
        if (st != NV_OK) {
            vmiop_log(1, "vmiop_env_log: Failed to query persistence state: 0x%x (gpuId 0x%x)\n",
                      st, gpu_id);
            rm_free(hClient, hClient, hDev);
            rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_DETACH_IDS, attach_ids, 0x80U);
            return (int)st;
        }
        if (persist == 1U) {
            uint32_t zero = 0;
            st = rm_control(hClient, hDev, NV2080_CTRL_CMD_GPU_SET_PERSISTENCE_STATE,
                            &zero, sizeof(zero));
            if (st != NV_OK) {
                vmiop_log(1, "vmiop_env_log: Failed to set persistence state: 0x%x (gpuId 0x%x)\n",
                          st, gpu_id);
                rm_free(hClient, hClient, hDev);
                rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_DETACH_IDS, attach_ids, 0x80U);
                return (int)st;
            }
            if (!is_daemon_init) {
                NV_VFIO_VGPU_CTLDEV_PROBE_GPU_PARAMS pp;
                memset(&pp, 0, sizeof(pp));
                pp.num_pgpus = 1U;
                pp.is_daemon_init = 0;
                pp.gpu_id[0] = gpu_id;
                vgpu_ctl_ioctl(NV_VFIO_VGPU_CTLDEV_PROBE_GPU, sizeof(pp), &pp);
            }
        }

        if (is_daemon_init) {
            int trc = register_vgpu_types_for_gpu(hClient, hDev, gpu_id,
                                                  sriov_p.sriovEnabled, sriov_p.firstVfOffset);
            if (trc == 0) {
                gpu_ids[*out_count] = gpu_id;
                (*out_count)++;
            } else {
                vmiop_log(1, "vmiop_env_log: Failed to get vGPU type infos on GPU 0x%x. 0x%x\n",
                          gpu_id, trc);
            }
        }

        rm_free(hClient, hClient, hDev);
        attach_ids[0] = (int32_t)gpu_id;
        attach_ids[1] = -1;
        st = rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_DETACH_IDS, attach_ids, 0x80U);
        if (st != NV_OK) {
            vmiop_log(1, "vmiop_env_log: Failed to detach gpu: 0x%x (gpuId 0x%x)\n", st, gpu_id);
            return (int)st;
        }
    }
    return NV_OK;
}

static int alloc_rm_os_event(uint32_t hClient, uint32_t hObj, uint32_t notify_idx,
                             uint32_t action, int *out_fd)
{
    NV_STATUS st = rm_alloc_os_event(hClient, hClient, out_fd);
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: Failed to allocate event fd 0x%x\n", st);
        return (int)st;
    }

    NV0005_ALLOC_PARAMETERS evp;
    memset(&evp, 0, sizeof(evp));
    evp.hParentClient = hClient;
    evp.hClass        = NV01_EVENT_OS_EVENT;
    evp.notifyIndex   = notify_idx | NV01_EVENT_WITHOUT_EVENT_DATA;
    evp.data          = (NvP64)(uintptr_t)(uint32_t)(*out_fd);
    st = rm_alloc_object(hClient, hClient, hObj, NV01_EVENT, &evp, sizeof(evp));
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: Failed to allocate event 0x%x\n", st);
        rm_free_os_event(hClient, hClient, *out_fd);
        *out_fd = -1;
        return (int)st;
    }

    NV0000_CTRL_EVENT_SET_NOTIFICATION_PARAMS act_p;
    act_p.notifyIndex = notify_idx;
    act_p.action = action;
    st = rm_control(hClient, hClient, NV0000_CTRL_CMD_EVENT_SET_NOTIFICATION,
                    &act_p, sizeof(act_p));
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: Set notification action failed 0x%x\n", st);
        rm_free(hClient, hClient, hObj);
        rm_free_os_event(hClient, hClient, *out_fd);
        *out_fd = -1;
        return (int)st;
    }
    return NV_OK;
}

/* -------------------------------------------------------------------------
 * Per-VM Child Worker ("vgpu") Implementation
 * ------------------------------------------------------------------------- */
static void format_uuid_str(const uint8_t u[VM_UUID_SIZE], char out[40])
{
    static const int perm[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    size_t pos = 0;
    for (int i = 0; i < 16; i++) {
        snprintf(out + pos, 40U - pos, "%02x", u[perm[i]]);
        pos += 2;
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            out[pos++] = '-';
        }
    }
    out[pos] = '\0';
}

static void parse_uuid_str_bytes(const char *s, uint8_t out[VM_UUID_SIZE])
{
    memset(out, 0, VM_UUID_SIZE);
    size_t si = 0;
    for (int i = 0; i < 16; i++) {
        if (!s[si] || !s[si + 1]) break;
        unsigned int byte_val = 0;
        sscanf(s + si, "%2x", &byte_val);
        out[i] = (uint8_t)byte_val;
        si += 2;
        if (i == 3 || i == 5 || i == 7 || i == 9) si++;
    }
}

static int read_qemu_cmdline_flag(uint32_t pid, const char *flag, char *out, int out_len)
{
    char path[1024];
    snprintf(path, sizeof(path), "/proc/%u/cmdline", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0x16;

    char tok[4096];
    size_t ti = 0;
    int matched = 0;
    int overflow = 0;
    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (ch == '\0') {
            tok[ti] = '\0';
            if (matched) {
                fclose(fp);
                if (overflow) return 2;
                safe_copy_str(out, (size_t)out_len, tok);
                return 0;
            }
            if (!overflow && strcmp(tok, flag) == 0) matched = 1;
            ti = 0;
            overflow = 0;
        } else if (ti < sizeof(tok) - 1U) {
            tok[ti++] = (char)ch;
        } else {
            overflow = 1;
        }
    }
    fclose(fp);
    return 0x57;
}

static void apply_smp_affinity_from_cfg(const char *cfg)
{
    char work[1024];
    safe_copy_str(work, sizeof(work), cfg);

    char *p = work;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';

        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            const char *val = eq + 1;
            /* trim key whitespace */
            char *kend = eq;
            while (kend > p && isspace((unsigned char)kend[-1])) *--kend = '\0';
            if (strcmp(p, "smp_affinity") == 0) {
                while (*val && (isspace((unsigned char)*val) || *val == '=' || *val == 'x' || *val == 'X')) {
                    if (*val == '0' && (val[1] == 'x' || val[1] == 'X')) {
                        val += 2;
                        continue;
                    }
                    val++;
                }
                if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) val += 2;
                long ncpus = sysconf(_SC_NPROCESSORS_CONF);
                if (ncpus <= 0) ncpus = 64;
                cpu_set_t *set = CPU_ALLOC(4096);
                if (set) {
                    size_t sz = CPU_ALLOC_SIZE(4096);
                    CPU_ZERO_S(sz, set);
                    size_t vlen = strlen(val);
                    int bit_base = 0;
                    for (ssize_t idx = (ssize_t)vlen - 1; idx >= 0 && bit_base < ncpus; idx--) {
                        char hex_c[2] = { val[idx], '\0' };
                        unsigned int nibble = 0;
                        if (sscanf(hex_c, "%1x", &nibble) == 1) {
                            for (int b = 0; b < 4 && bit_base + b < ncpus; b++) {
                                if ((nibble & (1U << b)) && (bit_base + b) < 4096) {
                                    CPU_SET_S((size_t)(bit_base + b), sz, set);
                                }
                            }
                        }
                        bit_base += 4;
                    }
                    if (sched_setaffinity(0, sz, set) < 0) {
                        vmiop_log(1, "vmiop_env_log: Setting CPU affinity failed for vgpu process %s\n",
                                  strerror(errno));
                    }
                    CPU_FREE(set);
                }
                return;
            }
        }
        if (!comma) break;
        p = comma + 1;
    }
}

static uint32_t extract_vgpu_type_id(const char *cfg)
{
    char work[1024];
    safe_copy_str(work, sizeof(work), cfg);
    uint32_t found_id = 0;
    char *p = work;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            char *kend = eq;
            while (kend > p && isspace((unsigned char)kend[-1])) *--kend = '\0';
            if (strcmp(p, "vgpu_type_id") == 0) {
                unsigned int v = 0;
                if (sscanf(eq + 1, "%u", &v) == 1) found_id = v;
            }
        }
        if (!comma) break;
        p = comma + 1;
    }
    return found_id;
}

static int vfio_inst_ioctl(uint32_t inst, unsigned long req, void *params, uint32_t size)
{
    vmiope_instance_t *in = vmiope_get_instance(inst);
    if (!in || in->vgpu_fd < 0) return NV_ERR_OPERATING_SYSTEM;
    NV_STATUS st = vfio_vgpu_ioctl(in->vgpu_fd, req, params, size);
    return (int)st;
}

static int create_device_events(uint32_t inst)
{
    vmiope_instance_t *in = vmiope_get_instance(inst);
    int rc;
    if ((rc = create_vfio_event_fd(&in->fd_shutdown, NV_VFIO_VGPU_EVENT_VM_SHUTDOWN, inst, 0)) != 0) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Error allocating event for stop action\n", inst);
        return rc;
    }
    if ((rc = create_vfio_event_fd(&in->fd_reg_access, NV_VFIO_VGPU_EVENT_VM_REG_ACCESS, inst, 0)) != 0) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Error allocating event for register access\n", inst);
        return rc;
    }
    if ((rc = create_vfio_event_fd(&in->fd_bar_info, NV_VFIO_VGPU_EVENT_BAR_INFO, inst, 0)) != 0) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Error allocating event for bar info query\n", inst);
        return rc;
    }
    if ((rc = create_vfio_event_fd(&in->fd_reboot, NV_VFIO_VGPU_EVENT_VM_REBOOT, inst, 0)) != 0) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Error allocating event for reset action\n", inst);
    }
    if ((rc = create_vfio_event_fd(&in->fd_console, NV_VFIO_VGPU_EVENT_CONSOLE_INFO, inst, 0)) != 0) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Error allocating event for console event\n", inst);
        return rc;
    }
    if (vmiope_get_migration_cap_byte() == 1U) {
        if ((rc = create_vfio_event_fd(&in->fd_mig_state, NV_VFIO_VGPU_EVENT_MIGRATION_STATE, inst, 0)) != 0) return rc;
        if ((rc = create_vfio_event_fd(&in->fd_mig_read, NV_VFIO_VGPU_EVENT_READ_DEVICE_BUFFER, inst, 0)) != 0) return rc;
        if ((rc = create_vfio_event_fd(&in->fd_mig_write, NV_VFIO_VGPU_EVENT_WRITE_DEVICE_BUFFER, inst, 0)) != 0) return rc;
        if ((rc = create_vfio_event_fd(&in->fd_mig_size, NV_VFIO_VGPU_EVENT_MIGRATION_DATA_SIZE, inst, 0)) != 0) return rc;
    }
    if (inst == 0U) {
        if ((rc = create_vfio_event_fd(&in->fd_attach, NV_VFIO_VGPU_EVENT_ATTACH_DEVICE, 0, 0)) != 0) {
            vmiop_log(1, "vmiop_env_log: (0x%x): Error allocating event for attach device\n", 0);
            return rc;
        }
    }
    return NV_OK;
}

static void teardown_device_instance(uint32_t inst)
{
    vmiope_instance_t *in = vmiope_get_instance(inst);
    if (!in) return;

    destroy_vfio_event_fd(&in->fd_shutdown,   NV_VFIO_VGPU_EVENT_VM_SHUTDOWN, 0);
    destroy_vfio_event_fd(&in->fd_reg_access, NV_VFIO_VGPU_EVENT_VM_REG_ACCESS, 0);
    destroy_vfio_event_fd(&in->fd_bar_info,   NV_VFIO_VGPU_EVENT_BAR_INFO, 0);
    destroy_vfio_event_fd(&in->fd_reboot,     NV_VFIO_VGPU_EVENT_VM_REBOOT, 0);
    destroy_vfio_event_fd(&in->fd_console,    NV_VFIO_VGPU_EVENT_CONSOLE_INFO, 0);
    if (vmiope_get_migration_cap_byte() == 1U) {
        destroy_vfio_event_fd(&in->fd_mig_state, NV_VFIO_VGPU_EVENT_MIGRATION_STATE, 0);
        destroy_vfio_event_fd(&in->fd_mig_read,  NV_VFIO_VGPU_EVENT_READ_DEVICE_BUFFER, 0);
        destroy_vfio_event_fd(&in->fd_mig_write, NV_VFIO_VGPU_EVENT_WRITE_DEVICE_BUFFER, 0);
        destroy_vfio_event_fd(&in->fd_mig_size,  NV_VFIO_VGPU_EVENT_MIGRATION_DATA_SIZE, 0);
    }
    if (inst == 0U) {
        destroy_vfio_event_fd(&in->fd_attach, NV_VFIO_VGPU_EVENT_ATTACH_DEVICE, 0);
    }

    NV_VFIO_VGPU_CONSOLE_BUFFER_PARAMS cb;
    memset(&cb, 0, sizeof(cb));
    if (in->vgpu_fd >= 0 && in->console.buffer) {
        int rc = vfio_inst_ioctl(inst, NV_VFIO_VGPU_FREE_CONSOLE_BUFFER, &cb, sizeof(cb));
        if (rc != 0) {
            vmiop_log(1, "vmiop_env_log: (0x%x): error: failed to free console buffer 0x%x \n",
                      inst, rc);
        }
    }

    vmiope_cleanup_instance(inst);

    if (in->hClient) {
        if (in->hSmcPartRef) {
            rm_free(in->hClient, in->hSubDevice, in->hSmcPartRef);
            in->hSmcPartRef = 0;
        }
        if (in->hVgpuConfig) {
            rm_free(in->hClient, in->hSubDevice, in->hVgpuConfig);
            in->hVgpuConfig = 0;
        }
        if (in->hSubDevice) {
            rm_free(in->hClient, in->hDevice, in->hSubDevice);
            in->hSubDevice = 0;
        }
        if (in->hDevice) {
            rm_free(in->hClient, in->hClient, in->hDevice);
            in->hDevice = 0;
        }
        rm_free(in->hClient, in->hClient, in->hClient);
        in->hClient = 0;
    }
}

static int init_device_instance(uint32_t inst, const uint8_t uuid[VM_UUID_SIZE],
                                const char *cfg_params, uint16_t vgpu_id,
                                uint32_t gpu_pci_id, uint32_t gpu_pci_bdf)
{
    if (!rm_ensure_vgpu_chardev((int)vgpu_id)) {
        return 0x26;
    }
    char dev_path[128];
    snprintf(dev_path, sizeof(dev_path), "/dev/nvidia-vgpu%u", (unsigned)vgpu_id);
    int vfd = rm_open_rw_cloexec(dev_path);
    if (vfd < 0) {
        return 0x38;
    }

    vmiope_init_instance_slot(inst, vfd, vgpu_id, gpu_pci_id, gpu_pci_bdf, uuid, cfg_params);
    vmiope_instance_t *in = vmiope_get_instance(inst);

    NV_STATUS st = rm_alloc_client(&in->hClient);
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to allocate resource manager client: 0x%x\n",
                  inst, st);
        return (int)st;
    }

    char uuid_str[40], bdf_str[32];
    format_uuid_str(uuid, uuid_str);
    snprintf(bdf_str, sizeof(bdf_str), "%02x:%02x:%02x.%01x",
             in->pci_domain, in->pci_bus, in->pci_dev, in->pci_func);
    vmiop_log(2, "vmiop_env_log: (0x%x): Received start call from nvidia-vgpu-vfio module: mdev uuid %s GPU PCI id %s config params %s\n",
              inst, uuid_str, bdf_str, cfg_params);

    int32_t attach_ids[33];
    memset(attach_ids, 0, sizeof(attach_ids));
    attach_ids[0] = (int32_t)gpu_pci_id;
    attach_ids[1] = -1;
    st = rm_control(in->hClient, in->hClient, NV0000_CTRL_CMD_GPU_ATTACH_IDS,
                    attach_ids, sizeof(attach_ids));
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: (0x%x): error: failed to attach device: 0x%x\n", inst, st);
        return (int)st;
    }

    NV0000_CTRL_GPU_GET_ID_INFO_PARAMS id_p;
    memset(&id_p, 0, sizeof(id_p));
    id_p.gpuId = gpu_pci_id;
    st = rm_control(in->hClient, in->hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO, &id_p, sizeof(id_p));
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: (0x%x): error: failed to obtain GPU ID information: 0x%x\n",
                  inst, st);
        return (int)st;
    }

    in->hDevice = ((in->hClient + gpu_pci_id) << 8) | 1U;
    NV0080_ALLOC_PARAMETERS dev_p;
    memset(&dev_p, 0, sizeof(dev_p));
    dev_p.deviceId = id_p.deviceInstance;
    dev_p.hClientShare = in->hClient;
    st = rm_alloc_object(in->hClient, in->hClient, in->hDevice, NV01_DEVICE_0, &dev_p, sizeof(dev_p));
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to allocate device: 0x%x\n", inst, st);
        return (int)st;
    }

    in->hSubDevice = ((in->hClient + gpu_pci_id) << 8) | 2U;
    NV2080_ALLOC_PARAMETERS sub_p;
    memset(&sub_p, 0, sizeof(sub_p));
    st = rm_alloc_object(in->hClient, in->hDevice, in->hSubDevice, NV20_SUBDEVICE_0, &sub_p, sizeof(sub_p));
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to allocate subdevice: 0x%x\n", inst, st);
        return (int)st;
    }

    in->hVgpuConfig = ((in->hClient + gpu_pci_id) << 8) | 3U;
    st = rm_alloc_object(in->hClient, in->hSubDevice, in->hVgpuConfig, NVA081_VGPU_CONFIG, NULL, 0);
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: (0x%x): error: vGPU config class allocation failed: 0x%x\n",
                  inst, st);
        return (int)st;
    }

    uint32_t vgpu_type_id = extract_vgpu_type_id(cfg_params);
    if (vgpu_type_id == 0U) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Required parameter vgpuType_id not found", inst);
        return 0x31;
    }

    int rc = vmiope_process_configuration(cfg_params, vgpu_type_id, bdf_str, inst);
    if (rc != 0) {
        rc = (int)vmiop_err_to_rm_status((uint32_t)rc);
        vmiop_log(1, "vmiop_env_log: (0x%x): vmiope_process_configuration failed with 0x%x\n",
                  inst, rc);
        return rc;
    }

    if (in->swizz_enabled) {
        struct {
            uint64_t rsv0;
            uint32_t swizzId;
            uint32_t rsv1;
        } smc_p;
        memset(&smc_p, 0, sizeof(smc_p));
        smc_p.swizzId = in->swizz_id;
        in->hSmcPartRef = ((in->hClient + gpu_pci_id) << 8) | 5U;
        st = rm_alloc_object(in->hClient, in->hSubDevice, in->hSmcPartRef,
                             AMPERE_SMC_PARTITION_REF, &smc_p, sizeof(smc_p));
        if (st != NV_OK) {
            vmiop_log(1, "vmiop_env_log: (0x%x): Failed to allocate SMC PARTITION: 0x%x for swizzId: %d\n",
                      inst, st, in->swizz_id);
            in->hSmcPartRef = 0;
            return (int)st;
        }
    }

    rc = vmiope_register_plugin_internal(&vmiop_presentation, &in->pres_handle);
    if (rc != 0) {
        rc = (int)vmiop_err_to_rm_status((uint32_t)rc);
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to register Presentation plugin 0x%x", inst, rc);
        return rc;
    }
    rc = vmiope_connect_plugins_internal(in->dev_handle, in->pres_handle);
    if (rc != 0) {
        rc = (int)vmiop_err_to_rm_status((uint32_t)rc);
        vmiop_log(1, "vmiop_env_log: (0x%x): Connect plugins failed with 0x%x\n", inst, rc);
        return rc;
    }

    /* Console VNC shared buffer allocation */
    NV_VFIO_VGPU_CONSOLE_BUFFER_PARAMS cb_p;
    cb_p.size = NV_VFIO_VGPU_CONSOLE_SURFACE_SIZE_MAX;
    in->console.buffer_size = NV_VFIO_VGPU_CONSOLE_SURFACE_SIZE_MAX;
    int c_rc = vfio_inst_ioctl(inst, NV_VFIO_VGPU_ALLOC_CONSOLE_BUFFER, &cb_p, sizeof(cb_p));
    if (c_rc == 0) {
        void *mapped = mmap(NULL, cb_p.size - 1ULL, PROT_READ | PROT_WRITE, MAP_SHARED,
                            in->vgpu_fd, (off_t)NV_VGPU_DEVICE_CONSOLE_REGION_OFFSET);
        if (mapped == MAP_FAILED) {
            vmiop_log(1, "vmiop_env_log: (0x%x): error: failed to mmap console buffer \n", inst);
            return NV_ERR_OPERATING_SYSTEM;
        }
        in->console.buffer = mapped;
    } else if (c_rc == (int)NV_ERR_NOT_SUPPORTED) {
        vmiop_log(2, "vmiop_env_log: (0x%x): Console VNC not supported by kernel. \n", inst);
    } else {
        vmiop_log(1, "vmiop_env_log: (0x%x): error: failed to alloc console buffer 0x%x \n",
                  inst, c_rc);
        return c_rc;
    }

    return create_device_events(inst);
}

static int send_attach_ack_and_vm_name(uint32_t inst, int status_in)
{
    vmiope_instance_t *in = vmiope_get_instance(inst);
    int st_out = status_in;
    if (st_out == 0) {
        NVA081_CTRL_VGPU_CONFIG_SET_VM_NAME_PARAMS nm;
        memset(&nm, 0, sizeof(nm));
        memcpy(nm.mdevUuid, in->mdev_uuid, VM_UUID_SIZE);
        safe_copy_str(nm.vmName, sizeof(nm.vmName), g_vm_name);
        NV_STATUS rm_st = rm_control(in->hClient, in->hVgpuConfig,
                                     NVA081_CTRL_CMD_VGPU_CONFIG_SET_VM_NAME, &nm, sizeof(nm));
        if (rm_st != NV_OK) {
            vmiop_log(1, "vmiop_env_log: Failed to set VM name 0x%x\n", rm_st);
        }
        st_out = (int)rm_st;
    }

    NV_VFIO_VGPU_ATTACH_DEVICE_ACK_PARAMS ack;
    memset(&ack, 0, sizeof(ack));
    ack.instanceId = inst;
    ack.returnStatus = (NV_STATUS)st_out;
    int rc = vfio_inst_ioctl(inst, NV_VFIO_VGPU_ATTACH_DEVICE_ACK, &ack, sizeof(ack));
    if (rc != 0) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to notify device open status 0x%x\n",
                  inst, rc);
    }
    return rc;
}

static void handle_bar_info_event(uint32_t inst)
{
    vmiope_instance_t *in = vmiope_get_instance(inst);
    NVA081_CTRL_VGPU_CONFIG_GET_BAR_INFO_PARAMS rp;
    memset(&rp, 0, sizeof(rp));
    rp.gpuPciId = in->gpu_pci_id;
    memcpy(rp.mdevUuid, in->mdev_uuid, VM_UUID_SIZE);
    memcpy(rp.configParams, in->config_params, VGPU_CONFIG_PARAMS_MAX_LENGTH);

    NV_VFIO_VGPU_BAR_INFO_SET_DATA_PARAMS sp;
    memset(&sp, 0, sizeof(sp));
    NV_STATUS st = rm_control(in->hClient, in->hVgpuConfig,
                              NVA081_CTRL_CMD_VGPU_CONFIG_GET_BAR_INFO, &rp, sizeof(rp));
    if (st == NV_OK) {
        memcpy(sp.barSizes, rp.barSizes, sizeof(sp.barSizes));
        memcpy(sp.sparseOffsets, rp.sparseOffsets, sizeof(sp.sparseOffsets));
        memcpy(sp.sparseSizes, rp.sparseSizes, sizeof(sp.sparseSizes));
        sp.sparseCount = rp.sparseCount;
    } else {
        vmiop_log(1, "vmiop_env_log: Failed to get bar info from RM, 0x%x\n", st);
    }
    sp.returnStatus = st;
    int rc = vfio_inst_ioctl(inst, NV_VFIO_VGPU_BAR_INFO_SET_DATA, &sp, sizeof(sp));
    if (rc != 0) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to set bar info, 0x%x\n", inst, rc);
    }
}

static void handle_migration_state_event(uint32_t inst)
{
    vmiope_instance_t *in = vmiope_get_instance(inst);
    NV_VFIO_VGPU_MIGRATION_STATE_PARAMS gs;
    memset(&gs, 0, sizeof(gs));
    int rc = vfio_inst_ioctl(inst, NV_VFIO_VGPU_GET_MIGRATION_STATE, &gs, sizeof(gs));
    if (rc != 0) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to get migration state err: 0x%x \n", inst, rc);
        NV_VFIO_VGPU_NOTIFY_MIGRATION_STATE_STATUS_PARAMS ns = { .returnStatus = (NV_STATUS)rc };
        vfio_inst_ioctl(inst, NV_VFIO_VGPU_NOTIFY_MIGRATION_STATE_STATUS, &ns, sizeof(ns));
        return;
    }

    static const uint32_t mig_map[3] = { 1U, 2U, 3U };
    uint32_t prev_stage = in->mig_stage;
    uint32_t next_stage = 0U;
    if (gs.deviceState >= 2U && gs.deviceState <= 4U) {
        next_stage = mig_map[gs.deviceState - 2U];
    }
    in->mig_stage = next_stage;
    uint32_t status_out = 0;

    if (prev_stage != next_stage) {
        uint32_t notify_st = (next_stage == 0U && (prev_stage == 2U || prev_stage == 3U))
                                 ? 3U : next_stage;
        int mrc = vmiope_plugin_notify_mig_state(inst, notify_st);
        status_out = (uint32_t)vmiop_err_to_rm_status((uint32_t)mrc);
        if (prev_stage == 1U && next_stage == 0U && in->staging_buffer) {
            munmap(in->staging_buffer, NV_VFIO_VGPU_MIGRATION_REGION_SIZE_MMAP);
            in->staging_buffer = NULL;
        }
        if ((next_stage == 1U || next_stage == 2U) && !in->staging_buffer) {
            void *p = mmap(NULL, NV_VFIO_VGPU_MIGRATION_REGION_SIZE_MMAP,
                           PROT_READ | PROT_WRITE, MAP_SHARED, in->vgpu_fd,
                           (off_t)NV_VGPU_DEVICE_STAGING_REGION_OFFSET);
            if (p != MAP_FAILED) in->staging_buffer = p;
            else status_out = 0x1eU;
        } else if (next_stage == 3U && !in->mig_buffer) {
            void *p = mmap(NULL, NV_VFIO_VGPU_MIGRATION_REGION_SIZE_MMAP,
                           PROT_READ | PROT_WRITE, MAP_SHARED, in->vgpu_fd,
                           (off_t)NV_VGPU_DEVICE_MIGRATION_REGION_OFFSET);
            if (p != MAP_FAILED) in->mig_buffer = p;
            else status_out = 0x1eU;
        }
    }

    NV_VFIO_VGPU_NOTIFY_MIGRATION_STATE_STATUS_PARAMS ns = { .returnStatus = status_out };
    rc = vfio_inst_ioctl(inst, NV_VFIO_VGPU_NOTIFY_MIGRATION_STATE_STATUS, &ns, sizeof(ns));
    if (rc != 0) {
        vmiop_log(1, "vmiop_env_log: (0x%x): Failed to wake up the migration waitqueue with status: 0x%x \n",
                  inst, rc);
    }
}

static void add_fd_to_set(int fd, fd_set *fds, int *max_fd)
{
    if (fd >= 0 && fd < FD_SETSIZE) {
        FD_SET(fd, fds);
        if (fd > *max_fd) *max_fd = fd;
    }
}

static int run_worker_process(char *argv0,
                              const NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA_PARAMS *ev)
{
    apply_smp_affinity_from_cfg((const char *)ev->vgpu_params);
    size_t a0_len = strlen(argv0);
    if (a0_len >= 4U) {
        memcpy(argv0, "vgpu", 4U);
        if (a0_len > 4U) argv0[4] = '\0';
    }
    prctl(PR_SET_NAME, "vgpu", 0, 0, 0);

    rm_reset_after_fork();
    if (g_ctl_dev_fd >= 0) { close(g_ctl_dev_fd); g_ctl_dev_fd = -1; }
    if (g_pid_fd >= 0) { close(g_pid_fd); g_pid_fd = -1; }
    if (g_fd_rm_bind >= 0) { close(g_fd_rm_bind); g_fd_rm_bind = -1; }
    if (g_fd_rm_unbind >= 0) { close(g_fd_rm_unbind); g_fd_rm_unbind = -1; }
    if (g_fd_ctl_open >= 0) { close(g_fd_ctl_open); g_fd_ctl_open = -1; }
    if (g_fd_ctl_create >= 0) { close(g_fd_ctl_create); g_fd_ctl_create = -1; }
    if (g_fd_ctl_delete >= 0) { close(g_fd_ctl_delete); g_fd_ctl_delete = -1; }
    if (g_fd_ctl_instances >= 0) { close(g_fd_ctl_instances); g_fd_ctl_instances = -1; }

    sigset_t bmask;
    sigemptyset(&bmask);
    sigaddset(&bmask, SIGCHLD);
    sigaddset(&bmask, SIGTSTP);
    sigaddset(&bmask, SIGTTOU);
    sigaddset(&bmask, SIGTTIN);
    sigprocmask(SIG_BLOCK, &bmask, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = worker_sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    g_qemu_pid = ev->qemu_pid;
    vmiope_set_instance_count(0);

    char uuid_str[40];
    if (read_qemu_cmdline_flag(g_qemu_pid, "-uuid", uuid_str, sizeof(uuid_str)) != 0) {
        snprintf(uuid_str, sizeof(uuid_str), "00000000-0000-0000-0000-%012lx",
                 (unsigned long)g_qemu_pid);
        vmiop_log(2, "vmiop_env_log: Failed to get VM UUID from QEMU command-line, generating UUID from PID: %s\n",
                  uuid_str);
    }
    uint8_t guest_uuid[VM_UUID_SIZE];
    parse_uuid_str_bytes(uuid_str, guest_uuid);
    vmiope_set_guest_uuid(guest_uuid);

    if (vmiope_initialize_env() != VMIOP_SUCCESS) {
        vmiop_log(1, "vmiop_env_log: vmiope_initialize failed\n");
        return 1;
    }

    char raw_name[128];
    memset(raw_name, 0, sizeof(raw_name));
    if (read_qemu_cmdline_flag(g_qemu_pid, "-name", raw_name, sizeof(raw_name)) == 0) {
        const char *gpos = strstr(raw_name, "guest=");
        const char *src = gpos ? (gpos + 6) : raw_name;
        safe_copy_str(g_vm_name, sizeof(g_vm_name), src);
        char *comma = strchr(g_vm_name, ',');
        if (comma) *comma = '\0';
    } else {
        safe_copy_str(g_vm_name, sizeof(g_vm_name), uuid_str);
    }

    int init_rc = init_device_instance(0U, ev->vgpu_name, (const char *)ev->vgpu_params,
                                       (uint16_t)ev->vgpu_id, ev->gpu_pci_id, ev->gpu_pci_bdf);
    if (init_rc != 0) {
        send_attach_ack_and_vm_name(0U, 1);
        teardown_device_instance(0U);
        close(vmiope_get_instance(0U)->vgpu_fd);
        return 1;
    }
    vmiope_set_instance_count(1U);
    if (send_attach_ack_and_vm_name(0U, 0) != 0) {
        teardown_device_instance(0U);
        close(vmiope_get_instance(0U)->vgpu_fd);
        return 1;
    }

    /* Enter worker event loop (g_env_lock held on entry to worker loop like 0x115e00) */
    while (!g_worker_stop && vmiope_get_instance_count() > 0U && vmiope_get_instance(0U)->active) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int max_fd = -1;
        int has_any = 0;

        uint32_t cnt = vmiope_get_instance_count();
        for (uint32_t i = 0; i < cnt && i < VMIOP_MAX_INSTANCES; i++) {
            vmiope_instance_t *in = vmiope_get_instance(i);
            if (!in->active) continue;
            has_any = 1;
            add_fd_to_set(in->fd_shutdown, &rfds, &max_fd);
            add_fd_to_set(in->fd_reg_access, &rfds, &max_fd);
            add_fd_to_set(in->fd_bar_info, &rfds, &max_fd);
            add_fd_to_set(in->fd_reboot, &rfds, &max_fd);
            add_fd_to_set(in->fd_console, &rfds, &max_fd);
            if (i == 0U) add_fd_to_set(in->fd_attach, &rfds, &max_fd);
            if (vmiope_get_migration_cap_byte() == 1U) {
                add_fd_to_set(in->fd_mig_state, &rfds, &max_fd);
                add_fd_to_set(in->fd_mig_read, &rfds, &max_fd);
                add_fd_to_set(in->fd_mig_write, &rfds, &max_fd);
                add_fd_to_set(in->fd_mig_size, &rfds, &max_fd);
            }
        }
        if (!has_any) break;

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        vmiope_env_unlock(0);
        int sel = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        vmiope_env_lock(NULL);

        if (sel < 0) {
            if (errno == EINTR) continue;
            vmiop_log(1, "vmiop_env_log: select() failed 0x%x (%s)!\n", errno, strerror(errno));
            return 1;
        }
        if (sel == 0) {
            for (uint32_t i = 0; i < cnt && i < VMIOP_MAX_INSTANCES; i++) {
                vmiope_instance_t *in = vmiope_get_instance(i);
                if (in->active && in->console.started) {
                    vmiope_console_stop(i);
                }
            }
            continue;
        }

        uint64_t now_ns = 0;
        vmiop_thread_get_time(&now_ns);

        for (uint32_t i = 0; i < cnt && i < VMIOP_MAX_INSTANCES; i++) {
            vmiope_instance_t *in = vmiope_get_instance(i);
            if (!in->active) continue;

            if (in->fd_console >= 0 && !FD_ISSET(in->fd_console, &rfds) &&
                in->console.started && (now_ns - in->console.last_update_ns) > 999999999ULL) {
                vmiope_console_stop(i);
            }

            if (in->fd_shutdown >= 0 && FD_ISSET(in->fd_shutdown, &rfds)) {
                in->shutting_down = 1U;
                vmiope_console_stop(i);
                teardown_device_instance(i);
                NV_VFIO_VGPU_NOTIFY_POWER_OP_PARAMS po = { .returnStatus = NV_OK };
                vfio_inst_ioctl(i, NV_VFIO_VGPU_NOTIFY_POWER_OP, &po, sizeof(po));
                close(in->vgpu_fd);
                in->vgpu_fd = -1;
                if (i == 0U) return 0;
                continue;
            }

            if (in->fd_reg_access >= 0 && FD_ISSET(in->fd_reg_access, &rfds)) {
                vmiope_handle_reg_access(i);
            }
            if (in->fd_bar_info >= 0 && FD_ISSET(in->fd_bar_info, &rfds)) {
                handle_bar_info_event(i);
            }
            if (in->fd_reboot >= 0 && FD_ISSET(in->fd_reboot, &rfds)) {
                vmiope_console_stop(i);
                in->resetting = 1U;
                vmiope_plugin_reset(i);
                in->resetting = 0U;
                vmiope_console_start(i);
                NV_VFIO_VGPU_NOTIFY_POWER_OP_PARAMS po = { .returnStatus = NV_OK };
                vfio_inst_ioctl(i, NV_VFIO_VGPU_NOTIFY_POWER_OP, &po, sizeof(po));
            }
            if (i == 0U && in->fd_attach >= 0 && FD_ISSET(in->fd_attach, &rfds)) {
                NV_VFIO_VGPU_GET_ATTACH_DEVICE_DATA_PARAMS ad;
                memset(&ad, 0, sizeof(ad));
                int arc = vfio_inst_ioctl(0U, NV_VFIO_VGPU_GET_ATTACH_DEVICE_DATA, &ad, sizeof(ad));
                uint32_t new_idx = vmiope_get_instance_count();
                if (arc == 0 && new_idx < VMIOP_MAX_INSTANCES) {
                    int irc = init_device_instance(new_idx, ad.mdevUuid,
                                                   (const char *)ad.configParams,
                                                   ad.vgpuId, ad.gpuPciId, ad.gpuPciBdf);
                    int send_rc = send_attach_ack_and_vm_name(new_idx, irc == 0 ? 0 : 1);
                    if (irc == 0 && send_rc == 0) {
                        vmiope_set_instance_count(new_idx + 1U);
                    } else {
                        teardown_device_instance(new_idx);
                        close(vmiope_get_instance(new_idx)->vgpu_fd);
                    }
                }
            }
            if (in->fd_console >= 0 && FD_ISSET(in->fd_console, &rfds)) {
                if (!in->console.started) vmiope_console_start(i);
                vmiop_thread_get_time(&in->console.last_update_ns);
                NV_VFIO_VGPU_CONSOLE_SURFACE_PARAMS sp;
                sp.width    = in->console.width;
                sp.height   = in->console.height;
                sp.depth    = in->console.bpp;
                sp.size     = in->console.stride * in->console.height;
                sp.lineSize = in->console.stride;
                vfio_inst_ioctl(i, NV_VFIO_VGPU_SET_CONSOLE_SURFACE_PROP, &sp, sizeof(sp));
            }
            if (in->fd_mig_state >= 0 && FD_ISSET(in->fd_mig_state, &rfds)) {
                handle_migration_state_event(i);
            }
            if (in->fd_mig_read >= 0 && FD_ISSET(in->fd_mig_read, &rfds)) {
                NV_VFIO_VGPU_GET_MIGRATION_BUFFER_SIZE_PARAMS gbs = {0};
                if (vfio_inst_ioctl(i, NV_VFIO_VGPU_GET_MIGRATION_BUFFER_SIZE, &gbs, sizeof(gbs)) == 0) {
                    NV_VFIO_VGPU_SET_MIGRATION_BUFFER_SIZE_PARAMS sbs = {0};
                    int prc = vmiope_plugin_read_dev_buf(i, in->staging_buffer, gbs.bufferSize,
                                                         &sbs.written, &sbs.pending);
                    sbs.returnStatus = vmiop_err_to_rm_status((uint32_t)prc);
                    vfio_inst_ioctl(i, NV_VFIO_VGPU_SET_MIGRATION_BUFFER_SIZE, &sbs, sizeof(sbs));
                }
            }
            if (in->fd_mig_write >= 0 && FD_ISSET(in->fd_mig_write, &rfds)) {
                NV_VFIO_VGPU_GET_MIGRATION_BUFFER_SIZE_PARAMS gbs = {0};
                int rc = vfio_inst_ioctl(i, NV_VFIO_VGPU_GET_MIGRATION_BUFFER_SIZE, &gbs, sizeof(gbs));
                uint32_t st = (uint32_t)rc;
                if (rc == 0) {
                    int prc = vmiope_plugin_write_dev_buf(i, in->mig_buffer, gbs.bufferSize);
                    st = vmiop_err_to_rm_status((uint32_t)prc);
                }
                NV_VFIO_VGPU_NOTIFY_MIGRATION_STATE_STATUS_PARAMS ns = { .returnStatus = st };
                vfio_inst_ioctl(i, NV_VFIO_VGPU_NOTIFY_MIGRATION_STATE_STATUS, &ns, sizeof(ns));
            }
            if (in->fd_mig_size >= 0 && FD_ISSET(in->fd_mig_size, &rfds)) {
                NV_VFIO_VGPU_MIGRATION_DATA_SIZE_PARAMS ds = {0};
                int prc = vmiope_plugin_get_mig_data_size(i, &ds.size);
                ds.returnStatus = vmiop_err_to_rm_status((uint32_t)prc);
                vfio_inst_ioctl(i, NV_VFIO_VGPU_MIGRATION_DATA_SIZE, &ds, sizeof(ds));
            }
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Main Daemon Event Handlers
 * ------------------------------------------------------------------------- */
static void handle_ctl_create(void)
{
    NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA_PARAMS ev;
    memset(&ev, 0, sizeof(ev));
    ev.event_type = NV_VFIO_VGPU_CTLDEV_EVENT_CREATE_DEVICE;
    if (vgpu_ctl_ioctl(NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA, sizeof(ev), &ev) != 0) return;

    NV0000_CTRL_VGPU_CREATE_DEVICE_PARAMS cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.mdevUuid, ev.vgpu_name, VM_UUID_SIZE);
    cp.gpuPciId   = ev.gpu_pci_id;
    cp.gpuPciBdf  = ev.gpu_pci_bdf;
    cp.vgpuTypeId = ev.vgpu_type_id;

    NV_STATUS st = rm_control(g_hClient, g_hClient, NV0000_CTRL_CMD_VGPU_CREATE_DEVICE,
                              &cp, sizeof(cp));
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: Failed to create device on GPU 0x%x 0x%x\n",
                  ev.gpu_pci_bdf, st);
    }

    NV_VFIO_VGPU_CTLDEV_SET_EVENT_DATA_PARAMS sev;
    memset(&sev, 0, sizeof(sev));
    sev.event_type = NV_VFIO_VGPU_CTLDEV_EVENT_CREATE_DEVICE;
    sev.return_status = st;
    sev.vgpu_id = cp.vgpuId;
    vgpu_ctl_ioctl(NV_VFIO_VGPU_CTLDEV_SET_EVENT_DATA, sizeof(sev), &sev);
}

static void handle_ctl_delete(void)
{
    NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA_PARAMS ev;
    memset(&ev, 0, sizeof(ev));
    ev.event_type = NV_VFIO_VGPU_CTLDEV_EVENT_DELETE_DEVICE;
    if (vgpu_ctl_ioctl(NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA, sizeof(ev), &ev) != 0) return;

    NV0000_CTRL_VGPU_DELETE_DEVICE_PARAMS dp;
    memset(&dp, 0, sizeof(dp));
    memcpy(dp.mdevUuid, ev.vgpu_name, VM_UUID_SIZE);
    dp.vgpuId = (uint16_t)ev.vgpu_id;

    NV_STATUS st = rm_control(g_hClient, g_hClient, NV0000_CTRL_CMD_VGPU_DELETE_DEVICE,
                              &dp, sizeof(dp));
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: Failed to delete vGPU device 0x%x\n", st);
    }

    NV_VFIO_VGPU_CTLDEV_SET_EVENT_DATA_PARAMS sev;
    memset(&sev, 0, sizeof(sev));
    sev.event_type = NV_VFIO_VGPU_CTLDEV_EVENT_DELETE_DEVICE;
    sev.return_status = st;
    vgpu_ctl_ioctl(NV_VFIO_VGPU_CTLDEV_SET_EVENT_DATA, sizeof(sev), &sev);
}

static void handle_ctl_get_instances(void)
{
    NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA_PARAMS ev;
    memset(&ev, 0, sizeof(ev));
    ev.event_type = NV_VFIO_VGPU_CTLDEV_EVENT_GET_INSTANCES;
    if (vgpu_ctl_ioctl(NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA, sizeof(ev), &ev) != 0) return;

    NV0000_CTRL_VGPU_GET_INSTANCES_PARAMS gi;
    memset(&gi, 0, sizeof(gi));
    gi.gpuPciId     = ev.gpu_pci_id;
    gi.gpuPciBdf    = ev.gpu_pci_bdf;
    gi.numVgpuTypes = ev.num_vgpu_types;
    uint32_t copy_n = ev.num_vgpu_types;
    if (copy_n > NV_MAX_VGPU_TYPE_PER_GPU) copy_n = NV_MAX_VGPU_TYPE_PER_GPU;
    for (uint32_t i = 0; i < copy_n; i++) {
        gi.vgpuTypeIds[i] = ev.vgpu_type_ids[i];
    }

    NV_STATUS st = rm_control(g_hClient, g_hClient, NV0000_CTRL_CMD_VGPU_GET_INSTANCES,
                              &gi, sizeof(gi));
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: Failed to get instances for GPU 0x%x, 0x%x\n",
                  ev.gpu_pci_bdf, st);
    }

    NV_VFIO_VGPU_CTLDEV_SET_EVENT_DATA_PARAMS sev;
    memset(&sev, 0, sizeof(sev));
    sev.event_type = NV_VFIO_VGPU_CTLDEV_EVENT_GET_INSTANCES;
    sev.return_status = st;
    uint32_t out_n = gi.numVgpuTypes;
    if (out_n > NV_MAX_VGPU_TYPE_PER_GPU) out_n = NV_MAX_VGPU_TYPE_PER_GPU;
    sev.num_vgpu_types = out_n;
    for (uint32_t i = 0; i < out_n; i++) {
        sev.vgpu_type_ids[i] = gi.vgpuTypeIds[i];
        sev.available_instances[i] = gi.availableInstances[i];
    }
    vgpu_ctl_ioctl(NV_VFIO_VGPU_CTLDEV_SET_EVENT_DATA, sizeof(sev), &sev);
}

static void handle_ctl_open(char *argv0)
{
    NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA_PARAMS ev;
    memset(&ev, 0, sizeof(ev));
    ev.event_type = NV_VFIO_VGPU_CTLDEV_EVENT_OPEN_DEVICE;
    if (vgpu_ctl_ioctl(NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA, sizeof(ev), &ev) != 0) return;

    pid_t pid = fork();
    if (pid < 0) {
        vmiop_log(1, "vmiop_env_log: fork first child failed 0x%x (%s)\n", errno, strerror(errno));
        return;
    }
    if (pid == 0) {
        pid_t gpid = fork();
        if (gpid == 0) {
            int rc = run_worker_process(argv0, &ev);
            _exit(rc);
        }
        if (gpid < 0) {
            vmiop_log(1, "vmiop_env_log: forking child's child failed 0x%x (%s)\n",
                      errno, strerror(errno));
        }
        _exit(0);
    }
    int wstatus = 0;
    while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {}
}

static void handle_rm_bind_event(void)
{
    NV0000_CTRL_EVENT_GET_EVENT_DATA_PARAMS ed;
    memset(&ed, 0, sizeof(ed));
    NV_STATUS st = rm_control(g_hClient, g_hClient, NV0000_CTRL_CMD_EVENT_GET_EVENT_DATA,
                              &ed, sizeof(ed));
    if (st != NV_OK || ed.event != 3U) return;

    pid_t pid = fork();
    if (pid == 0) {
        rm_reset_after_fork();
        uint32_t cli = 0;
        if (rm_alloc_client(&cli) == NV_OK) {
            uint32_t dummy_cnt = 1;
            uint32_t gpu_id = ed.gpuId;
            int rc = probe_gpus(cli, 0, &dummy_cnt, &gpu_id);
            NV0000_CTRL_VGPU_NOTIFY_EVENT_PARAMS np = { .status = (uint32_t)rc, .gpuPciId = gpu_id };
            rm_control(cli, cli, NV0000_CTRL_CMD_VGPU_NOTIFY_EVENT, &np, sizeof(np));
            rm_free(cli, cli, cli);
        }
        _exit(0);
    }
}

static void handle_rm_unbind_event(void)
{
    NV0000_CTRL_EVENT_GET_EVENT_DATA_PARAMS ed;
    memset(&ed, 0, sizeof(ed));
    NV_STATUS st = rm_control(g_hClient, g_hClient, NV0000_CTRL_CMD_EVENT_GET_EVENT_DATA,
                              &ed, sizeof(ed));
    if (st != NV_OK || ed.event != 2U) {
        vmiop_log(1, "vmiop_env_log: Failed to query unbind event status: 0x%x event: 0x%x\n",
                  st, ed.event);
        return;
    }

    NV_VFIO_VGPU_CTLDEV_UNREGISTER_PARAMS up = { .gpu_id = ed.gpuId };
    int rc = vgpu_ctl_ioctl(NV_VFIO_VGPU_CTLDEV_UNREGISTER, sizeof(up), &up);
    NV0000_CTRL_VGPU_NOTIFY_EVENT_PARAMS np = { .status = (uint32_t)rc, .gpuPciId = ed.gpuId };
    rm_control(g_hClient, g_hClient, NV0000_CTRL_CMD_VGPU_NOTIFY_EVENT, &np, sizeof(np));
}

int main(int argc, char **argv)
{
    int foreground = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-f|--foreground] [-h|--help]\n", argv[0]);
            return 0;
        }
    }

    sigset_t bmask;
    sigemptyset(&bmask);
    sigaddset(&bmask, SIGCHLD);
    sigaddset(&bmask, SIGTSTP);
    sigaddset(&bmask, SIGTTOU);
    sigaddset(&bmask, SIGTTIN);
    sigprocmask(SIG_BLOCK, &bmask, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = daemon_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    if (!foreground) {
        pid_t dpid = fork();
        if (dpid < 0) {
            vmiop_log(1, "vmiop_env_log: fork failed (%s)\n", strerror(errno));
            return 1;
        }
        if (dpid > 0) return 0;

        umask(0);
        if (setsid() < 0) {
            vmiop_log(1, "vmiop_env_log: setsid failed (%s)\n", strerror(errno));
            return 1;
        }
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
    }

    setlogmask(LOG_UPTO(LOG_DEBUG));
    openlog("nvidia-vgpu-mgr", LOG_PID, LOG_DAEMON);

    if (chdir("/") < 0) {
        vmiop_log(1, "vmiop_env_log: chdir(\"/\") failed (%s)\n", strerror(errno));
    }
    if (mkdir(RUN_DIR_PATH, 0755) == 0) {
        g_created_run_dir = 1;
    }
    if (access(RUN_DIR_PATH, R_OK | W_OK) < 0) {
        vmiop_log(1, "vmiop_env_log: Failed to access %s (%s)\n", RUN_DIR_PATH, strerror(errno));
        cleanup_and_exit(1);
    }

    g_pid_fd = open(PID_FILE_PATH, O_RDWR | O_CREAT, 0644);
    if (g_pid_fd < 0) {
        vmiop_log(1, "vmiop_env_log: Failed to open pid file (%s)\n", strerror(errno));
        cleanup_and_exit(1);
    }
    if (lockf(g_pid_fd, F_TLOCK, 0) < 0) {
        vmiop_log(1, "vmiop_env_log: Failed to lock pid file (%s)\n", strerror(errno));
        cleanup_and_exit(1);
    }
    if (ftruncate(g_pid_fd, 0) == 0) {
        char pidbuf[32];
        int plen = snprintf(pidbuf, sizeof(pidbuf), "%d", getpid());
        if (write(g_pid_fd, pidbuf, (size_t)plen) != plen) {
            vmiop_log(1, "vmiop_env_log: Failed to write pid (%s)\n", strerror(errno));
            cleanup_and_exit(1);
        }
    }

    NV_STATUS st = rm_alloc_client(&g_hClient);
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: Failed to initialize RM client: 0x%x\n", st);
        cleanup_and_exit(1);
    }

    if (!rm_ensure_vgpu_chardev(0)) {
        rm_free(g_hClient, g_hClient, g_hClient);
        cleanup_and_exit(1);
    }
    g_ctl_dev_fd = rm_open_rw_cloexec("/dev/nvidia-vgpuctl");
    if (g_ctl_dev_fd < 0) {
        rm_free(g_hClient, g_hClient, g_hClient);
        cleanup_and_exit(1);
    }

    NV0000_CTRL_SYSTEM_GET_BUILD_VERSION_V2_PARAMS bv;
    memset(&bv, 0, sizeof(bv));
    st = rm_control(g_hClient, g_hClient, NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION_V2,
                    &bv, sizeof(bv));
    if (st != NV_OK) {
        vmiop_log(1, "vmiop_env_log: Failed to get driver version: 0x%x\n", st);
        close(g_ctl_dev_fd);
        rm_free(g_hClient, g_hClient, g_hClient);
        cleanup_and_exit(1);
    }

    NV_VFIO_VGPU_CTLDEV_VERSION_CHECK_PARAMS vc;
    memset(&vc, 0, sizeof(vc));
    safe_copy_str(vc.version_string, sizeof(vc.version_string), bv.driverVersionBuffer);
    if (vgpu_ctl_ioctl(NV_VFIO_VGPU_CTLDEV_VERSION_CHECK, sizeof(vc), &vc) != 0) {
        close(g_ctl_dev_fd);
        rm_free(g_hClient, g_hClient, g_hClient);
        cleanup_and_exit(1);
    }

    uint32_t gpu_ids[NV_MAX_PROBED_GPUS];
    memset(gpu_ids, 0, sizeof(gpu_ids));
    uint32_t num_pgpus = 0;
    if (probe_gpus(g_hClient, 1, &num_pgpus, gpu_ids) != 0) {
        close(g_ctl_dev_fd);
        rm_free(g_hClient, g_hClient, g_hClient);
        cleanup_and_exit(1);
    }

    g_hEvBind   = 0xdae00401U;
    g_hEvUnbind = 0xdae00402U;
    if (alloc_rm_os_event(g_hClient, g_hEvBind,   3U, 2U, &g_fd_rm_bind) != 0 ||
        alloc_rm_os_event(g_hClient, g_hEvUnbind, 2U, 2U, &g_fd_rm_unbind) != 0 ||
        create_vfio_event_fd(&g_fd_ctl_open,      NV_VFIO_VGPU_CTLDEV_EVENT_OPEN_DEVICE,   0, 1) != 0 ||
        create_vfio_event_fd(&g_fd_ctl_create,    NV_VFIO_VGPU_CTLDEV_EVENT_CREATE_DEVICE, 0, 1) != 0 ||
        create_vfio_event_fd(&g_fd_ctl_delete,    NV_VFIO_VGPU_CTLDEV_EVENT_DELETE_DEVICE, 0, 1) != 0 ||
        create_vfio_event_fd(&g_fd_ctl_instances, NV_VFIO_VGPU_CTLDEV_EVENT_GET_INSTANCES, 0, 1) != 0) {
        goto shutdown_daemon;
    }

    vmiop_log(2, "vmiop_env_log: Event fds open=%d create=%d delete=%d instances=%d bind=%d unbind=%d\n",
              g_fd_ctl_open, g_fd_ctl_create, g_fd_ctl_delete, g_fd_ctl_instances,
              g_fd_rm_bind, g_fd_rm_unbind);

    {
        NV_VFIO_VGPU_CTLDEV_PROBE_GPU_PARAMS pp;
        memset(&pp, 0, sizeof(pp));
        pp.num_pgpus = num_pgpus;
        pp.is_daemon_init = 1U;
        for (uint32_t i = 0; i < num_pgpus && i < NV_MAX_PROBED_GPUS; i++) {
            pp.gpu_id[i] = gpu_ids[i];
        }
        if (vgpu_ctl_ioctl(NV_VFIO_VGPU_CTLDEV_PROBE_GPU, sizeof(pp), &pp) != 0) {
            goto shutdown_daemon;
        }
    }

    while (!g_stop_requested) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int max_fd = -1;
        add_fd_to_set(g_fd_ctl_open, &rfds, &max_fd);
        add_fd_to_set(g_fd_rm_bind, &rfds, &max_fd);
        add_fd_to_set(g_fd_rm_unbind, &rfds, &max_fd);
        add_fd_to_set(g_fd_ctl_create, &rfds, &max_fd);
        add_fd_to_set(g_fd_ctl_delete, &rfds, &max_fd);
        add_fd_to_set(g_fd_ctl_instances, &rfds, &max_fd);

        int sel = select(max_fd + 1, &rfds, NULL, NULL, NULL);
        if (sel < 0) {
            if (errno == EINTR) continue;
            vmiop_log(1, "vmiop_env_log: select() failed 0x%x (%s)\n", errno, strerror(errno));
            break;
        }
        if (FD_ISSET(g_fd_ctl_open, &rfds))      handle_ctl_open(argv[0]);
        if (FD_ISSET(g_fd_rm_bind, &rfds))       handle_rm_bind_event();
        if (FD_ISSET(g_fd_rm_unbind, &rfds))     handle_rm_unbind_event();
        if (FD_ISSET(g_fd_ctl_create, &rfds))    handle_ctl_create();
        if (FD_ISSET(g_fd_ctl_delete, &rfds))    handle_ctl_delete();
        if (FD_ISSET(g_fd_ctl_instances, &rfds)) handle_ctl_get_instances();
    }

shutdown_daemon:
    destroy_vfio_event_fd(&g_fd_ctl_open,      NV_VFIO_VGPU_CTLDEV_EVENT_OPEN_DEVICE,   1);
    destroy_vfio_event_fd(&g_fd_ctl_create,    NV_VFIO_VGPU_CTLDEV_EVENT_CREATE_DEVICE, 1);
    destroy_vfio_event_fd(&g_fd_ctl_delete,    NV_VFIO_VGPU_CTLDEV_EVENT_DELETE_DEVICE, 1);
    destroy_vfio_event_fd(&g_fd_ctl_instances, NV_VFIO_VGPU_CTLDEV_EVENT_GET_INSTANCES, 1);
    if (g_fd_rm_bind >= 0) {
        rm_free(g_hClient, g_hClient, g_hEvBind);
        rm_free_os_event(g_hClient, g_hClient, g_fd_rm_bind);
        g_fd_rm_bind = -1;
    }
    if (g_fd_rm_unbind >= 0) {
        rm_free(g_hClient, g_hClient, g_hEvUnbind);
        rm_free_os_event(g_hClient, g_hClient, g_fd_rm_unbind);
        g_fd_rm_unbind = -1;
    }
    if (g_ctl_dev_fd >= 0) {
        close(g_ctl_dev_fd);
        g_ctl_dev_fd = -1;
    }
    if (g_hClient) {
        rm_free(g_hClient, g_hClient, g_hClient);
        g_hClient = 0;
    }
    cleanup_and_exit(0);
    return 0;
}
