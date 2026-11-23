#define _GNU_SOURCE
#include "gridd/env_fingerprint.h"
#include "gridd/json_util.h"
#include "gridd/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void append_quoted_csv(char *dst, size_t dst_sz, const char *item) {
    if (!dst || !item || !*item) return;
    size_t cur = strlen(dst);
    size_t ilen = strlen(item);
    size_t extra = (cur > 0 ? 1U : 0U) + ilen + 2U;
    if (cur + extra >= dst_sz) return;
    if (cur > 0) {
        dst[cur++] = ',';
    }
    dst[cur++] = '"';
    memcpy(dst + cur, item, ilen);
    cur += ilen;
    dst[cur++] = '"';
    dst[cur] = '\0';
}

static bool csv_contains(const char *csv, const char *item) {
    char needle[300];
    snprintf(needle, sizeof(needle), "\"%s\"", item);
    return strstr(csv, needle) != NULL;
}

static void collect_network_info(char *mac_csv, size_t mac_sz, char *ip_csv, size_t ip_sz) {
    mac_csv[0] = '\0';
    ip_csv[0] = '\0';

    const char *env_ip = getenv("NODE_IP");
    if (env_ip && *env_ip) {
        append_quoted_csv(ip_csv, ip_sz, env_ip);
    }

    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) != 0) {
        LOG_E("Failed to retrieve adapter information (%d)", errno);
        return;
    }

    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_name) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;

        int fam = ifa->ifa_addr->sa_family;
        if (fam == AF_PACKET) {
            struct sockaddr_ll *sll = (struct sockaddr_ll *)ifa->ifa_addr;
            if (sll->sll_halen == 6) {
                char mac[32];
                snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                         sll->sll_addr[0], sll->sll_addr[1], sll->sll_addr[2],
                         sll->sll_addr[3], sll->sll_addr[4], sll->sll_addr[5]);
                if (strcmp(mac, "00:00:00:00:00:00") != 0 && !csv_contains(mac_csv, mac)) {
                    append_quoted_csv(mac_csv, mac_sz, mac);
                }
            }
        } else if ((!env_ip || !*env_ip) && (fam == AF_INET || fam == AF_INET6)) {
            char ip[INET6_ADDRSTRLEN];
            const void *addr_ptr = (fam == AF_INET)
                ? (const void *)&((struct sockaddr_in *)ifa->ifa_addr)->sin_addr
                : (const void *)&((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr;
            if (inet_ntop(fam, addr_ptr, ip, sizeof(ip)) && !csv_contains(ip_csv, ip)) {
                append_quoted_csv(ip_csv, ip_sz, ip);
            }
        }
    }

    freeifaddrs(ifaddr);
}

static void parse_os_release(char *os_platform, size_t plat_sz, char *os_version, size_t ver_sz) {
    os_platform[0] = '\0';
    os_version[0] = '\0';
    FILE *fp = fopen("/etc/os-release", "r");
    if (!fp) return;

    char name[64] = {0};
    char ver_id[64] = {0};
    char ver[128] = {0};
    char line[512];

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (strncmp(line, "NAME=\"", 6) == 0) {
            snprintf(name, sizeof(name), "%.60s", line + 6);
            char *q = strchr(name, '"');
            if (q) *q = '\0';
        } else if (strncmp(line, "VERSION_ID=\"", 12) == 0) {
            snprintf(ver_id, sizeof(ver_id), "%.60s", line + 12);
            char *q = strchr(ver_id, '"');
            if (q) *q = '\0';
        } else if (strncmp(line, "VERSION=\"", 9) == 0) {
            snprintf(ver, sizeof(ver), "%.120s", line + 9);
            char *q = strchr(ver, '"');
            if (q) *q = '\0';
        }
    }
    fclose(fp);

    if (name[0] && ver_id[0]) {
        snprintf(os_platform, plat_sz, "%.60s %.60s", name, ver_id);
    } else if (name[0]) {
        snprintf(os_platform, plat_sz, "%.120s", name);
    }
    if (ver[0]) {
        snprintf(os_version, ver_sz, "%.120s", ver);
    }
}

static void read_cpu_topology(uint32_t *out_sockets, uint32_t *out_cores) {
    *out_sockets = 1;
    *out_cores = 1;
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) {
        LOG_E("Failed to fetch CPU information");
        return;
    }

    int max_phys_id = -1;
    int cpu_cores = 0;
    int processors = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "processor", 9) == 0) {
            processors++;
        } else if (strncmp(line, "physical id", 11) == 0) {
            char *colon = strchr(line, ':');
            if (colon) {
                int id = atoi(colon + 1);
                if (id > max_phys_id) max_phys_id = id;
            }
        } else if (strncmp(line, "cpu cores", 9) == 0) {
            char *colon = strchr(line, ':');
            if (colon) {
                int c = atoi(colon + 1);
                if (c > cpu_cores) cpu_cores = c;
            }
        }
    }
    fclose(fp);

    uint32_t sockets = (max_phys_id >= 0) ? (uint32_t)(max_phys_id + 1) : 1U;
    uint32_t cores = (cpu_cores > 0) ? (uint32_t)cpu_cores * sockets
                                     : (processors > 0 ? (uint32_t)processors : 1U);
    *out_sockets = sockets;
    *out_cores = cores;
}

static bool read_trimmed_file(const char *path, char *out, size_t out_sz) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    if (!fgets(out, (int)out_sz, fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ')) {
        out[--len] = '\0';
    }
    return len > 0;
}

bool gridd_env_read_machine_uuid(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    if (read_trimmed_file("/sys/class/dmi/id/product_uuid", out, out_sz)) return true;
    if (read_trimmed_file("/etc/machine-id", out, out_sz)) return true;
    LOG_D("Failed to read UUID of instance from SMBIOS");
    snprintf(out, out_sz, "00000000-0000-0000-0000-000000000000");
    return false;
}

gridd_status_t gridd_env_collect(gridd_rm_client_t *rm, gridd_environment_t *env) {
    if (!env) return GRIDD_ERR_INVALID_PARAM;
    memset(env, 0, sizeof(*env));

    collect_network_info(env->mac_address_list, sizeof(env->mac_address_list),
                         env->ip_address_list, sizeof(env->ip_address_list));

    const char *env_host = getenv("NODE_NAME");
    if (env_host && *env_host) {
        snprintf(env->hostname, sizeof(env->hostname), "%s", env_host);
    } else if (gethostname(env->hostname, sizeof(env->hostname) - 1) != 0) {
        LOG_E("Failed to read hostname (%s)", strerror(errno));
        snprintf(env->hostname, sizeof(env->hostname), "localhost");
    }

    parse_os_release(env->os_platform, sizeof(env->os_platform),
                     env->os_version, sizeof(env->os_version));
    read_cpu_topology(&env->cpu_sockets, &env->physical_cores);
    gridd_env_read_machine_uuid(env->client_platform_id, sizeof(env->client_platform_id));
    snprintf(env->hv_platform, sizeof(env->hv_platform), "%s",
             (rm && rm->is_vgpu_guest) ? "vGPU" : "BareMetal");

    if (rm) {
        gridd_rm_get_telemetry(rm, env->guest_driver_version, sizeof(env->guest_driver_version),
                               env->host_driver_version, sizeof(env->host_driver_version));
        if (gridd_rm_query_pci_ids(rm) == GRIDD_OK) {
            for (uint32_t i = 0; i < rm->gpu_count; ++i) {
                char pci_str[32];
                snprintf(pci_str, sizeof(pci_str), "%X-%X",
                         rm->gpus[i].pci_vendor_id, rm->gpus[i].pci_device_id);
                append_quoted_csv(env->gpu_id_list, sizeof(env->gpu_id_list), pci_str);
            }
        }
    } else {
        snprintf(env->host_driver_version, sizeof(env->host_driver_version), "NA");
    }

    return GRIDD_OK;
}
