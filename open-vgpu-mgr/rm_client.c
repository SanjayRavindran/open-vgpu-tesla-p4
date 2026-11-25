#include "rm_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define NV_CTL_MINOR 255U

typedef struct rm_map_entry {
    void *linear_addr;
    size_t mmap_len;
    uint64_t rm_linear_addr;
    uint32_t hClient;
    uint32_t hDevice;
    uint32_t hMemory;
    uint32_t is_fixed;
    uint32_t fd_type;
    int refcnt;
    struct rm_map_entry *prev;
    struct rm_map_entry *next;
} rm_map_entry_t;

typedef struct rm_event_fd {
    int fd;
    struct rm_event_fd *prev;
    struct rm_event_fd *next;
} rm_event_fd_t;

typedef struct rm_dev_alloc {
    uint32_t hClient;
    uint32_t hDevice;
    uint32_t hParent;
    int fd;
    rm_event_fd_t *event_fds;
    rm_map_entry_t *mappings;
    struct rm_dev_alloc *next;
} rm_dev_alloc_t;

typedef struct {
    int fd;
    uint32_t minor;
    int refcnt;
} rm_card_fd_t;

static int default_open(const char *path, int flags)
{
    return open(path, flags, 0);
}

static int default_close(int fd)
{
    return close(fd);
}

static int default_ioctl(int fd, unsigned long req, void *arg)
{
    return ioctl(fd, req, arg);
}

static void *default_mmap64(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
    return mmap64(addr, len, prot, flags, fd, (off64_t)off);
}

static rm_sys_ops_t g_sys = {
    .open_fn   = default_open,
    .close_fn  = default_close,
    .ioctl_fn  = default_ioctl,
    .mmap64_fn = default_mmap64,
    .munmap_fn = munmap,
};

static const char *g_proc_devices_path = "/proc/devices";
static const char *g_nvidia_params_path = "/proc/driver/nvidia/params";

static volatile int g_rm_spinlock = 0;
static int g_ctl_fd = -1;
static unsigned int g_client_refcnt = 0;
static nv_ioctl_card_info_t g_card_info[NV_MAX_PROBED_GPUS];
static rm_card_fd_t g_card_fds[NV_MAX_PROBED_GPUS];
static rm_dev_alloc_t *g_devs = NULL;
static rm_event_fd_t *g_ctl_events = NULL;
static rm_map_entry_t *g_ctl_mappings = NULL;

static void rm_spin_lock(void)
{
    unsigned int tries = 0;
    while (__sync_lock_test_and_set(&g_rm_spinlock, 1) != 0) {
        if ((++tries & 0xffU) == 0U) {
            struct timespec ts = { 0, 2000000 };
            nanosleep(&ts, NULL);
        }
    }
}

static void rm_spin_unlock(void)
{
    __sync_lock_release(&g_rm_spinlock);
}

void rm_set_sys_ops(const rm_sys_ops_t *ops)
{
    if (!ops) {
        rm_reset_sys_ops();
        return;
    }
    g_sys.open_fn   = ops->open_fn   ? ops->open_fn   : default_open;
    g_sys.close_fn  = ops->close_fn  ? ops->close_fn  : default_close;
    g_sys.ioctl_fn  = ops->ioctl_fn  ? ops->ioctl_fn  : default_ioctl;
    g_sys.mmap64_fn = ops->mmap64_fn ? ops->mmap64_fn : default_mmap64;
    g_sys.munmap_fn = ops->munmap_fn ? ops->munmap_fn : munmap;
}

void rm_reset_sys_ops(void)
{
    g_sys.open_fn   = default_open;
    g_sys.close_fn  = default_close;
    g_sys.ioctl_fn  = default_ioctl;
    g_sys.mmap64_fn = default_mmap64;
    g_sys.munmap_fn = munmap;
}

void rm_set_proc_paths(const char *proc_devices, const char *nvidia_params)
{
    g_proc_devices_path = proc_devices ? proc_devices : "/proc/devices";
    g_nvidia_params_path = nvidia_params ? nvidia_params : "/proc/driver/nvidia/params";
}

static NV_STATUS errno_to_rm_status(int err)
{
    static const NV_STATUS map[23] = {
        0x1bU, 0x59U, 0x59U, 0x66U, 0x59U, 0x26U, 0x59U, 0x59U,
        0x59U, 0x59U, 0x66U, 0x1aU, 0x1bU, 0x59U, 0x59U, 0x59U,
        0x59U, 0x59U, 0x26U, 0x59U, 0x59U, 0x1fU, 0x1aU
    };
    if (err >= 1 && err <= 23) {
        return map[err - 1];
    }
    return NV_ERR_OPERATING_SYSTEM;
}

static int rm_escape_ioctl(int fd, uint32_t cmd, uint32_t size, unsigned long req, void *params)
{
    for (;;) {
        int rc;
        if (size <= 0x1000U) {
            rc = g_sys.ioctl_fn(fd, req, params);
        } else {
            nv_ioctl_xfer_t xfer;
            xfer.cmd  = cmd;
            xfer.size = size;
            xfer.ptr  = (NvP64)(uintptr_t)params;
            rc = g_sys.ioctl_fn(fd, NV_IOCTL(NV_ESC_IOCTL_XFER_CMD, sizeof(xfer)), &xfer);
        }
        if (rc >= 0) {
            return rc;
        }
        if (errno != EINTR && errno != EAGAIN) {
            return -1;
        }
    }
}

static NV_STATUS rm_retry_escape(int fd, uint32_t cmd, uint32_t size, unsigned long req,
                                 void *params, uint32_t *status_out)
{
    time_t start_t = time(NULL);
    for (;;) {
        if (rm_escape_ioctl(fd, cmd, size, req, params) < 0) {
            return NV_ERR_OPERATING_SYSTEM;
        }
        if (*status_out != NV_ERR_BUSY_RETRY) {
            return NV_OK;
        }
        time_t elapsed = time(NULL) - start_t;
        struct timespec ts;
        if (elapsed < 4) {
            ts.tv_sec = 0;
            ts.tv_nsec = 100000000L;
        } else if (elapsed < 60) {
            ts.tv_sec = 1;
            ts.tv_nsec = 0;
        } else if (elapsed < 86400) {
            ts.tv_sec = 10;
            ts.tv_nsec = 0;
        } else {
            return NV_ERR_TIMEOUT;
        }
        nanosleep(&ts, NULL);
    }
}

int rm_set_cloexec(int fd)
{
    if (fd < 0) {
        return -1;
    }
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) {
        flags = 0;
    } else if (flags & FD_CLOEXEC) {
        return fd;
    }
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        int saved = errno > 0 ? errno : 1;
        g_sys.close_fn(fd);
        errno = saved;
        return -saved;
    }
    return fd;
}

int rm_open_rw_cloexec(const char *path)
{
    for (;;) {
        int fd = g_sys.open_fn(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            return rm_set_cloexec(fd);
        }
        if (errno == EINTR || errno == EAGAIN) {
            continue;
        }
        fd = g_sys.open_fn(path, O_RDWR);
        if (fd >= 0) {
            return rm_set_cloexec(fd);
        }
        if (errno == EINTR || errno == EAGAIN) {
            continue;
        }
        return -1;
    }
}

int rm_get_chardev_major(const char *dev_name)
{
    FILE *fp = fopen(g_proc_devices_path, "r");
    if (!fp) {
        return -1;
    }
    char line[256];
    int in_char = 0;
    int major_num = -1;
    size_t name_len = strlen(dev_name);

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (!in_char) {
            if (strcmp(line, "Character devices:\n") == 0) {
                in_char = 1;
            }
            continue;
        }
        if (line[0] == '\n' || line[0] == '\0') {
            break;
        }
        unsigned int num = 0;
        char parsed[128] = {0};
        if (sscanf(line, " %u %127s", &num, parsed) == 2) {
            if (strlen(parsed) == name_len && strcmp(parsed, dev_name) == 0) {
                major_num = (int)num;
                break;
            }
        }
    }
    fclose(fp);
    return major_num;
}

void rm_read_params_file(const char *params_path,
                         unsigned int *uid_out,
                         unsigned int *gid_out,
                         unsigned int *mode_out,
                         int *modify_out)
{
    *uid_out = 0;
    *gid_out = 0;
    *mode_out = 0666U;
    *modify_out = 1;
    if (!params_path || !*params_path) {
        return;
    }
    FILE *fp = fopen(params_path, "r");
    if (!fp) {
        return;
    }
    char key[64];
    unsigned int val = 0;
    while (fscanf(fp, "%63[^:]: %u\n", key, &val) == 2) {
        if (strcmp(key, "DeviceFileUID") == 0) {
            *uid_out = val;
        } else if (strcmp(key, "DeviceFileGID") == 0) {
            *gid_out = val;
        } else if (strcmp(key, "DeviceFileMode") == 0) {
            *mode_out = val;
        } else if (strcmp(key, "ModifyDeviceFiles") == 0 ||
                   strcmp(key, "DeviceFileModify") == 0) {
            *modify_out = (int)val;
        }
    }
    fclose(fp);
}

static int ensure_char_node_with_major(int major_num, unsigned int minor_num, const char *path)
{
    if (!path || !*path || major_num < 0) {
        return 0;
    }
    unsigned int uid = 0, gid = 0, mode = 0666U;
    int modify = 1;
    rm_read_params_file(g_nvidia_params_path, &uid, &gid, &mode, &modify);

    dev_t expected = makedev((unsigned int)major_num, minor_num);
    struct stat st;
    if (modify == 1) {
        int exists = (stat(path, &st) == 0);
        if (!exists) {
            if (mknod(path, (mode_t)(S_IFCHR | (mode & 0777U)), expected) != 0) {
                return 0;
            }
            if (chmod(path, (mode_t)(mode & 0777U)) != 0 ||
                chown(path, (uid_t)uid, (gid_t)gid) != 0) {
                unlink(path);
                return 0;
            }
        } else {
            if (!S_ISCHR(st.st_mode) || st.st_rdev != expected) {
                if (remove(path) != 0 ||
                    mknod(path, (mode_t)(S_IFCHR | (mode & 0777U)), expected) != 0 ||
                    chmod(path, (mode_t)(mode & 0777U)) != 0 ||
                    chown(path, (uid_t)uid, (gid_t)gid) != 0) {
                    return 0;
                }
            } else if ((st.st_mode & 0777U) != (mode & 0777U) ||
                       st.st_uid != (uid_t)uid || st.st_gid != (gid_t)gid) {
                if (chmod(path, (mode_t)(mode & 0777U)) != 0 ||
                    chown(path, (uid_t)uid, (gid_t)gid) != 0) {
                    return 0;
                }
            }
        }
    }

    if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode)) {
        return 0;
    }
    if (strncmp(path, "/dev/", 5) == 0) {
        char link_path[128];
        char target[128];
        snprintf(link_path, sizeof(link_path), "/dev/char/%d:%u", major_num, minor_num);
        snprintf(target, sizeof(target), "../%s", path + 5);
        remove(link_path);
        if (symlink(target, link_path) != 0) {
            struct stat lst;
            if (stat(link_path, &lst) != 0 || lst.st_ino != st.st_ino) {
                return 0;
            }
        }
    }
    return 1;
}

int rm_ensure_vgpu_chardev(int minor)
{
    int major_num = rm_get_chardev_major("nvidia-vgpu-vfio");
    if (major_num < 0) {
        return 0;
    }
    char path[128];
    if (minor == 0) {
        snprintf(path, sizeof(path), "/dev/nvidia-vgpuctl");
    } else {
        snprintf(path, sizeof(path), "/dev/nvidia-vgpu%d", minor);
    }
    return ensure_char_node_with_major(major_num, (unsigned int)minor, path);
}

int rm_ensure_nvidia_chardev(unsigned int minor)
{
    char path[128];
    if (minor == NV_CTL_MINOR) {
        snprintf(path, sizeof(path), "/dev/nvidiactl");
    } else {
        snprintf(path, sizeof(path), "/dev/nvidia%u", minor);
    }
    return ensure_char_node_with_major(195, minor, path);
}

static void init_card_fds_locked(void)
{
    memset(g_card_fds, 0, sizeof(g_card_fds));
    for (size_t i = 0; i < NV_MAX_PROBED_GPUS; i++) {
        g_card_fds[i].fd = -1;
    }
}

static void close_all_card_fds_locked(void)
{
    for (size_t i = 0; i < NV_MAX_PROBED_GPUS; i++) {
        if (g_card_fds[i].fd >= 0) {
            g_sys.close_fn(g_card_fds[i].fd);
        }
    }
    init_card_fds_locked();
}

static void detect_proc_driver_version(char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s", NV_VGPU_DRIVER_VERSION_STR);
    FILE *fp = fopen("/proc/driver/nvidia/version", "r");
    if (!fp) return;
    char line[256];
    if (fgets(line, sizeof(line), fp)) {
        const char *p = strstr(line, "Kernel Module");
        if (p) {
            p += 13;
            while (*p == ' ' || *p == '\t') p++;
            size_t i = 0;
            while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' && i + 1 < out_sz) {
                out[i] = p[i];
                i++;
            }
            if (i > 0) out[i] = '\0';
        }
    }
    fclose(fp);
}

static int init_ctl_locked(NV_STATUS *status_out)
{
    *status_out = NV_OK;
    if (g_client_refcnt > 0) {
        g_client_refcnt++;
        return 1;
    }

    init_card_fds_locked();

    /* Only try ensuring /dev/nvidiactl if not already accessible */
    if (access("/dev/nvidiactl", R_OK | W_OK) != 0) {
        rm_ensure_nvidia_chardev(NV_CTL_MINOR);
    }

    int fd = rm_open_rw_cloexec("/dev/nvidiactl");
    if (fd < 0) {
        *status_out = errno_to_rm_status(errno > 0 ? errno : 1);
        return 0;
    }

    nv_ioctl_rm_api_version_t ver;
    memset(&ver, 0, sizeof(ver));
    detect_proc_driver_version(ver.versionString, sizeof(ver.versionString));
    const char *no_check = getenv("__RM_NO_VERSION_CHECK");
    if (no_check && no_check[0] == '1') {
        ver.cmd = 0x31U;
    }
    if (!no_check || no_check[0] != '2') {
        if (rm_escape_ioctl(fd, NV_ESC_CHECK_VERSION_STR, sizeof(ver),
                            NV_IOCTL(NV_ESC_CHECK_VERSION_STR, sizeof(ver)), &ver) < 0) {
            g_sys.close_fn(fd);
            *status_out = 0x6aU;
            return 0;
        }
    }

    memset(g_card_info, 0, sizeof(g_card_info));
    if (rm_escape_ioctl(fd, NV_ESC_CARD_INFO, sizeof(g_card_info),
                        NV_IOCTL(NV_ESC_CARD_INFO, sizeof(g_card_info)), g_card_info) < 0) {
        g_sys.close_fn(fd);
        *status_out = NV_ERR_OPERATING_SYSTEM;
        return 0;
    }

    g_ctl_fd = fd;
    g_client_refcnt = 1;
    return 1;
}

static void shutdown_ctl_locked(void)
{
    if (g_client_refcnt == 0) {
        return;
    }
    g_client_refcnt--;
    if (g_client_refcnt == 0) {
        if (g_ctl_fd >= 0) {
            g_sys.close_fn(g_ctl_fd);
            g_ctl_fd = -1;
        }
        close_all_card_fds_locked();
    }
}

static rm_dev_alloc_t *find_dev_alloc_locked(uint32_t hClient, uint32_t hDev)
{
    for (rm_dev_alloc_t *d = g_devs; d != NULL; d = d->next) {
        if (d->hClient == hClient && (d->hDevice == hDev || d->hParent == hDev)) {
            return d;
        }
    }
    return NULL;
}

static int find_gpu_slot_for_id(uint32_t hClient, uint32_t target_gpu_id)
{
    uint32_t attached[NV_MAX_PROBED_GPUS];
    memset(attached, 0, sizeof(attached));
    if (rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS,
                   attached, sizeof(attached)) != NV_OK) {
        return -1;
    }
    for (int i = 0; i < NV_MAX_PROBED_GPUS; i++) {
        if (!g_card_info[i].valid) {
            continue;
        }
        NV0000_CTRL_GPU_GET_ID_INFO_PARAMS info;
        memset(&info, 0, sizeof(info));
        info.gpuId = g_card_info[i].gpu_id;
        for (int j = 0; j < NV_MAX_PROBED_GPUS; j++) {
            if (attached[j] == info.gpuId) {
                if (rm_control(hClient, hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO,
                               &info, sizeof(info)) == NV_OK &&
                    info.deviceInstance == target_gpu_id &&
                    info.subDeviceInstance == 0) {
                    return i;
                }
                break;
            }
        }
    }
    return NV_MAX_PROBED_GPUS;
}

static int open_gpu_minor_fd(unsigned int minor_num, NV_STATUS *status_out)
{
    char path[128];
    snprintf(path, sizeof(path), "/dev/nvidia%u", minor_num);
    if (access(path, R_OK | W_OK) != 0) {
        rm_ensure_nvidia_chardev(minor_num);
    }
    int fd = rm_open_rw_cloexec(path);
    if (fd < 0) {
        *status_out = errno_to_rm_status(errno > 0 ? errno : 1);
        return -1;
    }
    uint32_t ctl = (uint32_t)g_ctl_fd;
    if (rm_escape_ioctl(fd, NV_ESC_REGISTER_FD, sizeof(ctl),
                        NV_IOCTL(NV_ESC_REGISTER_FD, sizeof(ctl)), &ctl) < 0) {
        g_sys.close_fn(fd);
        *status_out = NV_ERR_GENERIC;
        return -1;
    }
    *status_out = NV_OK;
    return fd;
}

static void unmap_entry(rm_map_entry_t **head, rm_map_entry_t *e)
{
    e->refcnt--;
    if (e->refcnt > 0) {
        return;
    }
    if (e->prev) {
        e->prev->next = e->next;
    } else if (*head == e) {
        *head = e->next;
    }
    if (e->next) {
        e->next->prev = e->prev;
    }
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) {
        pg = 4096;
    }
    uintptr_t aligned = (uintptr_t)e->linear_addr & ~((uintptr_t)pg - 1ULL);
    if (!e->is_fixed) {
        g_sys.munmap_fn((void *)aligned, e->mmap_len);
    } else {
        g_sys.mmap64_fn((void *)aligned, e->mmap_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    }
    free(e);
}

static void unmap_all_list(rm_map_entry_t **head)
{
    rm_map_entry_t *cur = *head;
    while (cur) {
        rm_map_entry_t *nxt = cur->next;
        long pg = sysconf(_SC_PAGESIZE);
        if (pg <= 0) pg = 4096;
        uintptr_t aligned = (uintptr_t)cur->linear_addr & ~((uintptr_t)pg - 1ULL);
        g_sys.munmap_fn((void *)aligned, cur->mmap_len);
        free(cur);
        cur = nxt;
    }
    *head = NULL;
}

static rm_map_entry_t *track_mmap(int fd, rm_map_entry_t **head, uint64_t len,
                                  uint64_t off, uint64_t hint, int is_fixed,
                                  int prot, uint32_t hDevice, uint32_t hMemory, uint32_t fd_type)
{
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) {
        pg = 4096;
    }
    size_t align_len = (size_t)(((off & ((uint64_t)pg - 1ULL)) + len + (uint64_t)pg - 1ULL) &
                                ~((uint64_t)pg - 1ULL));
    int flags = MAP_SHARED | (hint ? MAP_FIXED : 0);
    if (hint && ((off & ((uint64_t)pg - 1ULL)) != (hint & ((uint64_t)pg - 1ULL)))) {
        return NULL;
    }
    void *mapped = g_sys.mmap64_fn((void *)(uintptr_t)(hint & ~((uint64_t)pg - 1ULL)),
                                   align_len, prot, flags, fd, 0);
    if (mapped == MAP_FAILED) {
        return NULL;
    }
    void *user_ptr = (void *)((uintptr_t)mapped | (uintptr_t)(off & ((uint64_t)pg - 1ULL)));
    rm_map_entry_t *e = (rm_map_entry_t *)calloc(1, sizeof(*e));
    if (!e) {
        if (!is_fixed) {
            g_sys.munmap_fn(mapped, align_len);
        }
        return NULL;
    }
    e->linear_addr = user_ptr;
    e->mmap_len = align_len;
    e->rm_linear_addr = off;
    e->hDevice = hDevice;
    e->hMemory = hMemory;
    e->is_fixed = (uint32_t)(is_fixed != 0);
    e->fd_type = fd_type;
    e->refcnt = 1;
    e->next = *head;
    if (*head) {
        (*head)->prev = e;
    }
    *head = e;
    return e;
}

static void close_event_fd_in_list(rm_event_fd_t **head, int fd)
{
    for (rm_event_fd_t *e = *head; e != NULL; e = e->next) {
        if (e->fd == fd) {
            if (e->prev) e->prev->next = e->next;
            else *head = e->next;
            if (e->next) e->next->prev = e->prev;
            free(e);
            g_sys.close_fn(fd);
            return;
        }
    }
}

NV_STATUS rm_alloc_client(uint32_t *out_client)
{
    if (!out_client) {
        return 0x3dU;
    }
    rm_spin_lock();
    NV_STATUS st = NV_OK;
    if (!init_ctl_locked(&st)) {
        rm_spin_unlock();
        return st;
    }
    *out_client = 0;
    NVOS21_ROOT_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    rm_spin_unlock();

    st = rm_retry_escape(g_ctl_fd, NV_ESC_RM_ALLOC, sizeof(p),
                         NV_IOCTL(NV_ESC_RM_ALLOC, sizeof(p)), &p, &p.status);
    if (st == NV_OK && p.status == NV_OK) {
        *out_client = p.hObjectNew;
        return NV_OK;
    }
    rm_spin_lock();
    shutdown_ctl_locked();
    rm_spin_unlock();
    return (st != NV_OK) ? st : p.status;
}

NV_STATUS rm_alloc_object(uint32_t hClient, uint32_t hParent, uint32_t hNew,
                          uint32_t hClass, void *pParams, uint32_t paramsSize)
{
    if (hNew == 0) {
        return 0x3dU;
    }
    rm_dev_alloc_t *dev_rec = NULL;
    if (hClass == NV01_DEVICE_0) {
        if (!pParams) {
            return NV_ERR_INVALID_ARGUMENT;
        }
        uint32_t dev_inst = *(const uint32_t *)pParams;
        int slot = find_gpu_slot_for_id(hClient, dev_inst);
        if (slot < 0 || slot >= NV_MAX_PROBED_GPUS) {
            return NV_ERR_OPERATING_SYSTEM;
        }
        rm_spin_lock();
        if (g_client_refcnt == 0) {
            rm_spin_unlock();
            return NV_ERR_INVALID_ARGUMENT;
        }
        if (g_card_fds[slot].fd < 0) {
            NV_STATUS open_st = NV_OK;
            int fd = open_gpu_minor_fd(g_card_info[slot].minor_number, &open_st);
            if (fd < 0) {
                rm_spin_unlock();
                return open_st;
            }
            g_card_fds[slot].fd = fd;
            g_card_fds[slot].minor = g_card_info[slot].minor_number;
        }
        g_card_fds[slot].refcnt++;
        dev_rec = (rm_dev_alloc_t *)calloc(1, sizeof(*dev_rec));
        if (!dev_rec) {
            rm_spin_unlock();
            return NV_ERR_NO_MEMORY;
        }
        dev_rec->hClient = hClient;
        dev_rec->hDevice = hNew;
        dev_rec->hParent = hParent;
        dev_rec->fd = g_card_fds[slot].fd;
        dev_rec->next = g_devs;
        g_devs = dev_rec;
        rm_spin_unlock();
    }

    NVOS64_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot = hClient;
    p.hObjectParent = hParent;
    p.hObjectNew = hNew;
    p.hClass = hClass;
    p.pAllocParms = (NvP64)(uintptr_t)pParams;
    p.paramsSize = paramsSize;

    NV_STATUS st = rm_retry_escape(g_ctl_fd, NV_ESC_RM_ALLOC, sizeof(p),
                                   NV_IOCTL(NV_ESC_RM_ALLOC, sizeof(p)), &p, &p.status);
    if (st != NV_OK || p.status != NV_OK) {
        if (dev_rec) {
            rm_free(hClient, hClient, hNew);
        }
        return (st != NV_OK) ? st : p.status;
    }
    return NV_OK;
}

NV_STATUS rm_alloc_memory(uint32_t hClient, uint32_t hParent, uint32_t hMemory,
                          uint32_t hClass, uint32_t flags,
                          void **pSysMemInOut, uint64_t *limitInOut)
{
    if (!limitInOut || !pSysMemInOut) {
        return 0x3dU;
    }
    rm_spin_lock();
    rm_dev_alloc_t *d = find_dev_alloc_locked(hClient, hParent);
    int dev_fd = d ? d->fd : -1;
    rm_spin_unlock();
    if (dev_fd < 0) {
        return 0x22U;
    }

    NVOS02_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot = hClient;
    p.hObjectParent = hParent;
    p.hObjectNew = hMemory;
    p.hClass = hClass;
    p.flags = flags;
    if (hClass == NV01_MEMORY_LIST_SYSTEM || hClass == 0x71U) {
        p.pMemory = (NvP64)(uintptr_t)(*pSysMemInOut);
    }
    p.limit = *limitInOut;
    p.fd = -1;

    NV_STATUS st = rm_retry_escape(dev_fd, NV_ESC_RM_ALLOC_MEMORY, sizeof(p),
                                   NV_IOCTL(NV_ESC_RM_ALLOC_MEMORY, sizeof(p)), &p, &p.status);
    if (st != NV_OK) {
        return st;
    }
    if (p.status == NV_OK) {
        *limitInOut = p.limit;
    }
    return p.status;
}

NV_STATUS rm_control(uint32_t hClient, uint32_t hObject, uint32_t cmd,
                     void *pParams, uint32_t paramsSize)
{
    NVOS54_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hClient = hClient;
    p.hObject = hObject;
    p.cmd = cmd;
    p.params = (NvP64)(uintptr_t)pParams;
    p.paramsSize = paramsSize;

    NV_STATUS st = rm_retry_escape(g_ctl_fd, NV_ESC_RM_CONTROL, sizeof(p),
                                   NV_IOCTL(NV_ESC_RM_CONTROL, sizeof(p)), &p, &p.status);
    if (st != NV_OK) {
        return st;
    }
    if (p.status == NV_OK) {
        if (cmd == NV0000_CTRL_CMD_GPU_ATTACH_IDS) {
            rm_spin_lock();
            const int32_t *ids = (const int32_t *)pParams;
            for (int i = 0; i < NV_MAX_PROBED_GPUS; i++) {
                if (!g_card_info[i].valid) continue;
                int match = (ids[0] == 0xffff);
                for (int j = 0; !match && j < NV_MAX_PROBED_GPUS && ids[j] != -1; j++) {
                    if ((int32_t)g_card_info[i].gpu_id == ids[j]) {
                        match = 1;
                    }
                }
                if (match && g_card_fds[i].fd < 0) {
                    NV_STATUS open_st = NV_OK;
                    int fd = open_gpu_minor_fd(g_card_info[i].minor_number, &open_st);
                    if (fd >= 0) {
                        g_card_fds[i].fd = fd;
                        g_card_fds[i].minor = g_card_info[i].minor_number;
                    }
                }
            }
            rm_spin_unlock();
        } else if (cmd == NV0000_CTRL_CMD_GPU_DETACH_IDS) {
            rm_spin_lock();
            const int32_t *ids = (const int32_t *)pParams;
            for (int i = 0; i < NV_MAX_PROBED_GPUS; i++) {
                if (!g_card_info[i].valid) continue;
                int match = (ids[0] == 0xffff && g_card_fds[i].refcnt == 0);
                for (int j = 0; !match && j < NV_MAX_PROBED_GPUS && ids[j] != -1; j++) {
                    if ((int32_t)g_card_info[i].gpu_id == ids[j]) {
                        match = 1;
                    }
                }
                if (match && g_card_fds[i].fd >= 0) {
                    g_sys.close_fn(g_card_fds[i].fd);
                    g_card_fds[i].fd = -1;
                }
            }
            rm_spin_unlock();
        }
    }
    return p.status;
}

NV_STATUS rm_free(uint32_t hClient, uint32_t hParent, uint32_t hObject)
{
    rm_spin_lock();
    for (rm_dev_alloc_t *d = g_devs; d != NULL; d = d->next) {
        if (d->hClient == hClient) {
            if (hClient == hObject) {
                unmap_all_list(&d->mappings);
            } else if (d->hDevice == hObject) {
                unmap_all_list(&d->mappings);
            } else if (d->hParent == hParent || d->hDevice == hParent) {
                for (;;) {
                    rm_map_entry_t *found = NULL;
                    for (rm_map_entry_t *m = d->mappings; m != NULL; m = m->next) {
                        if (m->hMemory == hObject) {
                            found = m;
                            break;
                        }
                    }
                    if (!found) break;
                    unmap_entry(&d->mappings, found);
                }
            }
        }
    }
    rm_spin_unlock();

    NVOS00_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot = hClient;
    p.hObjectParent = hParent;
    p.hObjectOld = hObject;

    NV_STATUS st = rm_retry_escape(g_ctl_fd, NV_ESC_RM_FREE, sizeof(p),
                                   NV_IOCTL(NV_ESC_RM_FREE, sizeof(p)), &p, &p.status);
    if (st != NV_OK || p.status != NV_OK) {
        return (st != NV_OK) ? st : p.status;
    }

    rm_spin_lock();
    rm_dev_alloc_t **pp = &g_devs;
    while (*pp) {
        rm_dev_alloc_t *d = *pp;
        if (d->hClient == hClient &&
            (hClient == hObject || d->hDevice == hObject || d->hParent == hObject)) {
            *pp = d->next;
            unmap_all_list(&d->mappings);
            while (d->event_fds) {
                close_event_fd_in_list(&d->event_fds, d->event_fds->fd);
            }
            for (int i = 0; i < NV_MAX_PROBED_GPUS; i++) {
                if (g_card_fds[i].fd == d->fd && g_card_fds[i].refcnt > 0) {
                    g_card_fds[i].refcnt--;
                }
            }
            free(d);
        } else {
            pp = &(*pp)->next;
        }
    }
    if (hClient == hObject) {
        shutdown_ctl_locked();
    }
    rm_spin_unlock();
    return NV_OK;
}

NV_STATUS rm_map_memory(uint32_t hClient, uint32_t hDevice, uint32_t hMemory,
                        uint64_t offset, uint64_t length,
                        void **pLinearOut, uint32_t flags)
{
    if (!pLinearOut) {
        return 0x3dU;
    }

    NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TO_FD_PARAMS exp;
    memset(&exp, 0, sizeof(exp));
    exp.memoryHandle = hMemory;
    exp.flags = flags;
    NV_STATUS st = rm_control(hClient, hClient, NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECT_TO_FD,
                              &exp, sizeof(exp));
    if (st != NV_OK) {
        return st;
    }

    int prot = PROT_READ | PROT_WRITE;
    if ((flags & 3U) == 1U) {
        prot = PROT_READ;
    } else if ((flags & 3U) == 2U) {
        prot = PROT_WRITE;
    }
    if ((flags & 0x40000U) == 0U) {
        *pLinearOut = NULL;
    }

    rm_spin_lock();
    rm_dev_alloc_t *d = find_dev_alloc_locked(hClient, hDevice);
    if (exp.fdType != 1U && !d) {
        rm_spin_unlock();
        return 0x22U;
    }

    int map_fd = -1;
    NV_STATUS fd_st = NV_OK;
    if (exp.fdType == 1U) {
        if (access("/dev/nvidiactl", R_OK | W_OK) != 0) {
            rm_ensure_nvidia_chardev(NV_CTL_MINOR);
        }
        map_fd = rm_open_rw_cloexec("/dev/nvidiactl");
        if (map_fd < 0) fd_st = errno_to_rm_status(errno > 0 ? errno : 1);
    } else {
        int slot = -1;
        for (int i = 0; i < NV_MAX_PROBED_GPUS; i++) {
            if (g_card_fds[i].fd == d->fd) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            rm_spin_unlock();
            return 0x22U;
        }
        map_fd = open_gpu_minor_fd(g_card_fds[slot].minor, &fd_st);
    }
    if (map_fd < 0) {
        rm_spin_unlock();
        return fd_st;
    }

    NVOS33_PARAMETERS mp;
    memset(&mp, 0, sizeof(mp));
    mp.hClient = hClient;
    mp.hDevice = hDevice;
    mp.hMemory = hMemory;
    mp.offset = offset;
    mp.length = length;
    mp.flags = flags;
    mp.fd = map_fd;

    st = rm_retry_escape(g_ctl_fd, NV_ESC_RM_MAP_MEMORY, sizeof(mp),
                         NV_IOCTL(NV_ESC_RM_MAP_MEMORY, sizeof(mp)), &mp, &mp.status);
    if (st != NV_OK || mp.status != NV_OK) {
        g_sys.close_fn(map_fd);
        rm_spin_unlock();
        return (st != NV_OK) ? st : mp.status;
    }

    rm_map_entry_t **list_head = d ? &d->mappings : &g_ctl_mappings;
    rm_map_entry_t *e = track_mmap(map_fd, list_head, length,
                                   (uint64_t)(uintptr_t)mp.pLinearAddress,
                                   (uint64_t)(uintptr_t)*pLinearOut,
                                   (int)((flags >> 19) & 1U), prot,
                                   hDevice, hMemory, exp.fdType);
    if (!e) {
        NVOS34_PARAMETERS unp;
        memset(&unp, 0, sizeof(unp));
        unp.hClient = hClient;
        unp.hDevice = hDevice;
        unp.hMemory = hMemory;
        unp.flags = flags;
        rm_retry_escape(g_ctl_fd, NV_ESC_RM_UNMAP_MEMORY, sizeof(unp),
                        NV_IOCTL(NV_ESC_RM_UNMAP_MEMORY, sizeof(unp)), &unp, &unp.status);
        g_sys.close_fn(map_fd);
        rm_spin_unlock();
        return NV_ERR_NO_MEMORY;
    }

    if (exp.fdType == 2U) {
        NVOS56_PARAMETERS up;
        memset(&up, 0, sizeof(up));
        up.hClient = hClient;
        up.hDevice = hDevice;
        up.hMemory = hMemory;
        up.pNewCpuAddress = (NvP64)(uintptr_t)e->linear_addr;
        rm_retry_escape(g_ctl_fd, NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO, sizeof(up),
                        NV_IOCTL(NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO, sizeof(up)), &up, &up.status);
    }

    g_sys.close_fn(map_fd);
    *pLinearOut = e->linear_addr;
    rm_spin_unlock();
    return NV_OK;
}

NV_STATUS rm_unmap_memory(uint32_t hClient, uint32_t hDevice, uint32_t hMemory,
                          void *linearAddr, uint32_t flags)
{
    rm_spin_lock();
    rm_dev_alloc_t *d = find_dev_alloc_locked(hClient, hDevice);
    if (!d && hClient != hDevice) {
        rm_spin_unlock();
        return NV_ERR_OPERATING_SYSTEM;
    }
    rm_map_entry_t *found = NULL;
    rm_map_entry_t **found_head = NULL;
    if (d) {
        for (rm_map_entry_t *m = d->mappings; m != NULL; m = m->next) {
            if (m->linear_addr == linearAddr) {
                found = m;
                found_head = &d->mappings;
                break;
            }
        }
    }
    if (!found) {
        for (rm_map_entry_t *m = g_ctl_mappings; m != NULL; m = m->next) {
            if (m->linear_addr == linearAddr) {
                found = m;
                found_head = &g_ctl_mappings;
                break;
            }
        }
    }

    NVOS34_PARAMETERS unp;
    memset(&unp, 0, sizeof(unp));
    unp.hClient = hClient;
    unp.hDevice = hDevice;
    unp.hMemory = hMemory;
    if (found) {
        unp.pLinearAddress = (found->fd_type == 2U)
                                 ? (NvP64)(uintptr_t)linearAddr
                                 : (NvP64)found->rm_linear_addr;
    }
    unp.flags = flags;
    rm_spin_unlock();

    NV_STATUS st = rm_retry_escape(g_ctl_fd, NV_ESC_RM_UNMAP_MEMORY, sizeof(unp),
                                   NV_IOCTL(NV_ESC_RM_UNMAP_MEMORY, sizeof(unp)), &unp, &unp.status);
    if (st == NV_OK && unp.status == NV_OK && found && found_head) {
        rm_spin_lock();
        unmap_entry(found_head, found);
        rm_spin_unlock();
    }
    return (st != NV_OK) ? st : unp.status;
}

NV_STATUS rm_alloc_os_event(uint32_t hClient, uint32_t hParent, int *out_fd)
{
    if (!out_fd) {
        return 0x3dU;
    }
    *out_fd = -1;
    if (access("/dev/nvidiactl", R_OK | W_OK) != 0) {
        rm_ensure_nvidia_chardev(NV_CTL_MINOR);
    }
    int fd = rm_open_rw_cloexec("/dev/nvidiactl");
    if (fd < 0) {
        return errno_to_rm_status(errno > 0 ? errno : 1);
    }
    uint32_t ctl = (uint32_t)g_ctl_fd;
    if (rm_escape_ioctl(fd, NV_ESC_REGISTER_FD, sizeof(ctl),
                        NV_IOCTL(NV_ESC_REGISTER_FD, sizeof(ctl)), &ctl) < 0) {
        g_sys.close_fn(fd);
        return NV_ERR_GENERIC;
    }

    nv_ioctl_alloc_os_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.hClient = hClient;
    ev.hDevice = hParent;
    ev.fd = (uint32_t)fd;
    int rc = rm_escape_ioctl(fd, NV_ESC_ALLOC_OS_EVENT, sizeof(ev),
                             NV_IOCTL(NV_ESC_ALLOC_OS_EVENT, sizeof(ev)), &ev);
    if (rc < 0 || ev.status != NV_OK) {
        g_sys.close_fn(fd);
        return (rc < 0) ? NV_ERR_OPERATING_SYSTEM : ev.status;
    }

    rm_event_fd_t *rec = (rm_event_fd_t *)calloc(1, sizeof(*rec));
    if (!rec) {
        g_sys.close_fn(fd);
        return NV_ERR_NO_MEMORY;
    }
    rec->fd = fd;

    rm_spin_lock();
    rm_dev_alloc_t *d = find_dev_alloc_locked(hClient, hParent);
    rm_event_fd_t **head = d ? &d->event_fds : &g_ctl_events;
    rec->next = *head;
    if (*head) (*head)->prev = rec;
    *head = rec;
    rm_spin_unlock();

    *out_fd = fd;
    return NV_OK;
}

NV_STATUS rm_free_os_event(uint32_t hClient, uint32_t hParent, int fd)
{
    nv_ioctl_alloc_os_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.hClient = hClient;
    ev.hDevice = hParent;
    ev.fd = (uint32_t)fd;

    int rc = rm_escape_ioctl(fd, NV_ESC_FREE_OS_EVENT, sizeof(ev),
                             NV_IOCTL(NV_ESC_FREE_OS_EVENT, sizeof(ev)), &ev);
    NV_STATUS st = (rc < 0) ? NV_ERR_OPERATING_SYSTEM : ev.status;
    rm_spin_lock();
    rm_dev_alloc_t *d = find_dev_alloc_locked(hClient, hParent);
    if (d) {
        close_event_fd_in_list(&d->event_fds, fd);
    } else {
        close_event_fd_in_list(&g_ctl_events, fd);
    }
    rm_spin_unlock();
    return st;
}

NV_STATUS vfio_vgpu_ioctl(int fd, unsigned long req, void *params, uint32_t size)
{
    if (fd < 0) {
        return 0x40U;
    }
    NV_VGPU_DEV_IOCTL_PARAMS wrap;
    memset(&wrap, 0, sizeof(wrap));
    wrap.pCmdParams = (NvP64)(uintptr_t)params;
    wrap.size = size;
    if (g_sys.ioctl_fn(fd, req, &wrap) != 0) {
        return NV_ERR_OPERATING_SYSTEM;
    }
    return wrap.status;
}

void rm_reset_after_fork(void)
{
    g_rm_spinlock = 0;
    rm_spin_lock();
    rm_dev_alloc_t *d = g_devs;
    while (d) {
        rm_dev_alloc_t *nxt = d->next;
        unmap_all_list(&d->mappings);
        while (d->event_fds) {
            close_event_fd_in_list(&d->event_fds, d->event_fds->fd);
        }
        if (d->fd >= 0) {
            g_sys.close_fn(d->fd);
        }
        free(d);
        d = nxt;
    }
    g_devs = NULL;
    unmap_all_list(&g_ctl_mappings);
    while (g_ctl_events) {
        close_event_fd_in_list(&g_ctl_events, g_ctl_events->fd);
    }
    close_all_card_fds_locked();
    if (g_ctl_fd >= 0) {
        g_sys.close_fn(g_ctl_fd);
        g_ctl_fd = -1;
    }
    g_client_refcnt = 0;
    memset(g_card_info, 0, sizeof(g_card_info));
    rm_spin_unlock();
}
