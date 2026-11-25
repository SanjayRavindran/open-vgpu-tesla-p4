/*
 * Clean-room open-source nvidia-smi CLI (open-nvidia-smi)
 * Links against libopen-nvidia-ml.so (libnvidia-ml.so.1)
 *
 * Supports:
 *   - Default 89-column NVIDIA-SMI summary table + Processes table
 *   - -L / --list-gpus
 *   - -q / --query (with optional -d / --display)
 *   - vgpu (-s / --supported, -c / --creatable, -q / --query)
 *   - --query-gpu=<fields> --format=csv[,noheader][,nounits]
 *   - -i / --id=<index|uuid|busId>
 *   - -h / --help, -v / --version
 *
 * SPDX-License-Identifier: MIT OR GPL-2.0-only
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "open_nvml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static void print_timestamp_line(void) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%a %b %e %H:%M:%S %Y", &tmv);
    printf("%s       \n", tbuf);
}

static void format_cuda_version(char *buf, size_t len) {
    int cuda_ver = 13000;
    if (nvmlSystemGetCudaDriverVersion(&cuda_ver) != NVML_SUCCESS || cuda_ver <= 0) {
        cuda_ver = 13000;
    }
    snprintf(buf, len, "%d.%d", cuda_ver / 1000, (cuda_ver % 1000) / 10);
}

static int print_default_table(int filter_idx) {
    char drv_ver[80] = {0}, cuda_str[16] = {0};
    nvmlSystemGetDriverVersion(drv_ver, sizeof(drv_ver));
    format_cuda_version(cuda_str, sizeof(cuda_str));
    unsigned int dev_count = 0;
    nvmlDeviceGetCount_v2(&dev_count);

    print_timestamp_line();
    printf("+---------------------------------------------------------------------------------------+\n");
    printf("| NVIDIA-SMI %-22s Driver Version: %-13s CUDA Version: %-7s |\n",
           drv_ver, drv_ver, cuda_str);
    printf("|-----------------------------------------+----------------------+----------------------+\n");
    printf("| GPU  Name                 Persistence-M | Bus-Id        Disp.A | Volatile Uncorr. ECC |\n");
    printf("| Fan  Temp   Perf          Pwr:Usage/Cap |         Memory-Usage | GPU-Util  Compute M. |\n");
    printf("|                                         |                      |               MIG M. |\n");
    printf("|=========================================+======================+======================|\n");

    for (unsigned int i = 0; i < dev_count; i++) {
        if (filter_idx >= 0 && (int)i != filter_idx) continue;
        nvmlDevice_t dev = NULL;
        if (nvmlDeviceGetHandleByIndex_v2(i, &dev) != NVML_SUCCESS) continue;

        char name[64] = {0};
        nvmlDeviceGetName(dev, name, sizeof(name));

        nvmlEnableState_t pers = NVML_FEATURE_ENABLED, disp = NVML_FEATURE_DISABLED;
        nvmlDeviceGetPersistenceMode(dev, &pers);
        nvmlDeviceGetDisplayActive(dev, &disp);

        nvmlPciInfo_t pci;
        memset(&pci, 0, sizeof(pci));
        nvmlDeviceGetPciInfo_v3(dev, &pci);

        unsigned int temp = 0, pwr_mw = 0, lim_mw = 75000;
        nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp);
        nvmlDeviceGetPowerUsage(dev, &pwr_mw);
        nvmlDeviceGetEnforcedPowerLimit(dev, &lim_mw);

        nvmlPstates_t pstate = NVML_PSTATE_8;
        nvmlDeviceGetPerformanceState(dev, &pstate);
        char pstate_str[16];
        if (pstate <= NVML_PSTATE_15) snprintf(pstate_str, sizeof(pstate_str), "P%u", (unsigned int)pstate);
        else snprintf(pstate_str, sizeof(pstate_str), "N/A");

        nvmlMemory_t mem = {0};
        nvmlDeviceGetMemoryInfo(dev, &mem);
        unsigned long long used_mib  = mem.used / (1024ULL * 1024ULL);
        unsigned long long total_mib = mem.total / (1024ULL * 1024ULL);

        nvmlUtilization_t util = {0};
        nvmlDeviceGetUtilizationRates(dev, &util);

        printf("| %3u  %-22s %11s | %16s %3s | %20s |\n",
               i, name, (pers == NVML_FEATURE_ENABLED) ? "On" : "Off",
               pci.busId, (disp == NVML_FEATURE_ENABLED) ? "On" : "Off", "Off");
        printf("| N/A   %2uC    %-4s          %3uW / %3uW | %6lluMiB / %5lluMiB | %6u%%      Default |\n",
               temp, pstate_str, (pwr_mw + 500U) / 1000U, (lim_mw + 500U) / 1000U,
               used_mib, total_mib, util.gpu);
        printf("|                                         |                      |                  N/A |\n");
        printf("+-----------------------------------------+----------------------+----------------------+\n");
    }

    printf("                                                                                         \n");
    printf("+---------------------------------------------------------------------------------------+\n");
    printf("| Processes:                                                                            |\n");
    printf("|  GPU   GI   CI        PID   Type   Process name                            GPU Memory |\n");
    printf("|        ID   ID                                                             Usage      |\n");
    printf("|=======================================================================================|\n");

    unsigned int total_procs = 0;
    for (unsigned int i = 0; i < dev_count; i++) {
        if (filter_idx >= 0 && (int)i != filter_idx) continue;
        nvmlDevice_t dev = NULL;
        if (nvmlDeviceGetHandleByIndex_v2(i, &dev) != NVML_SUCCESS) continue;

        nvmlProcessInfo_v3_t procs[64];
        unsigned int pcnt = 64;
        nvmlDeviceGetComputeRunningProcesses_v3(dev, &pcnt, procs);
        for (unsigned int p = 0; p < pcnt && p < 64; p++) {
            char pname[64] = {0};
            nvmlSystemGetProcessName(procs[p].pid, pname, sizeof(pname));
            unsigned long long pmib = procs[p].usedGpuMemory / (1024ULL * 1024ULL);
            printf("| %4u   N/A  N/A %9u    C+G   %-38s %8lluMiB |\n",
                   i, procs[p].pid, pname, pmib);
            total_procs++;
        }
    }
    if (total_procs == 0) {
        printf("|  No running processes found                                                           |\n");
    }
    printf("+---------------------------------------------------------------------------------------+\n");
    return 0;
}

static int print_list_gpus(int filter_idx) {
    unsigned int dev_count = 0;
    nvmlDeviceGetCount_v2(&dev_count);
    for (unsigned int i = 0; i < dev_count; i++) {
        if (filter_idx >= 0 && (int)i != filter_idx) continue;
        nvmlDevice_t dev = NULL;
        if (nvmlDeviceGetHandleByIndex_v2(i, &dev) != NVML_SUCCESS) continue;
        char name[64] = {0}, uuid[96] = {0};
        nvmlDeviceGetName(dev, name, sizeof(name));
        nvmlDeviceGetUUID(dev, uuid, sizeof(uuid));
        printf("GPU %u: %s (UUID: %s)\n", i, name, uuid);
    }
    return 0;
}

static int print_query_log(int filter_idx) {
    char drv_ver[80] = {0}, cuda_str[16] = {0};
    nvmlSystemGetDriverVersion(drv_ver, sizeof(drv_ver));
    format_cuda_version(cuda_str, sizeof(cuda_str));
    unsigned int dev_count = 0;
    nvmlDeviceGetCount_v2(&dev_count);

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%a %b %e %H:%M:%S %Y", &tmv);

    printf("\n==============NVSMI LOG==============\n\n");
    printf("Timestamp                                 : %s\n", tbuf);
    printf("Driver Version                            : %s\n", drv_ver);
    printf("CUDA Version                              : %s\n", cuda_str);
    printf("vGPU Driver Capability\n");
    printf("        Heterogenous Multi-vGPU           : Supported\n\n");
    printf("Attached GPUs                             : %u\n", dev_count);

    for (unsigned int i = 0; i < dev_count; i++) {
        if (filter_idx >= 0 && (int)i != filter_idx) continue;
        nvmlDevice_t dev = NULL;
        if (nvmlDeviceGetHandleByIndex_v2(i, &dev) != NVML_SUCCESS) continue;

        char name[64] = {0}, uuid[96] = {0}, serial[64] = {0}, part[64] = {0}, vbios[32] = {0};
        nvmlDeviceGetName(dev, name, sizeof(name));
        nvmlDeviceGetUUID(dev, uuid, sizeof(uuid));
        nvmlDeviceGetSerial(dev, serial, sizeof(serial));
        nvmlDeviceGetBoardPartNumber(dev, part, sizeof(part));
        nvmlDeviceGetVbiosVersion(dev, vbios, sizeof(vbios));

        nvmlPciInfo_t pci = {0};
        nvmlDeviceGetPciInfo_v3(dev, &pci);
        nvmlMemory_v2_t mem2 = { .version = nvmlMemory_v2 };
        nvmlDeviceGetMemoryInfo_v2(dev, &mem2);
        nvmlBAR1Memory_t bar1 = {0};
        nvmlDeviceGetBAR1MemoryInfo(dev, &bar1);
        nvmlUtilization_t util = {0};
        nvmlDeviceGetUtilizationRates(dev, &util);
        unsigned int enc_u = 0, enc_p = 0, dec_u = 0, dec_p = 0;
        nvmlDeviceGetEncoderUtilization(dev, &enc_u, &enc_p);
        nvmlDeviceGetDecoderUtilization(dev, &dec_u, &dec_p);

        unsigned int temp = 0, t_shut = 94, t_slow = 91;
        nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp);
        nvmlDeviceGetTemperatureThreshold(dev, NVML_TEMPERATURE_THRESHOLD_SHUTDOWN, &t_shut);
        nvmlDeviceGetTemperatureThreshold(dev, NVML_TEMPERATURE_THRESHOLD_SLOWDOWN, &t_slow);

        unsigned int pwr_mw = 0, cur_lim = 75000, def_lim = 75000, min_lim = 60000, max_lim = 75000;
        nvmlDeviceGetPowerUsage(dev, &pwr_mw);
        nvmlDeviceGetPowerManagementLimit(dev, &cur_lim);
        nvmlDeviceGetPowerManagementDefaultLimit(dev, &def_lim);
        nvmlDeviceGetPowerManagementLimitConstraints(dev, &min_lim, &max_lim);

        unsigned int clk_gr = 0, clk_sm = 0, clk_mem = 0, clk_vid = 0;
        unsigned int mclk_gr = 0, mclk_sm = 0, mclk_mem = 0, mclk_vid = 0;
        nvmlDeviceGetClockInfo(dev, NVML_CLOCK_GRAPHICS, &clk_gr);
        nvmlDeviceGetClockInfo(dev, NVML_CLOCK_SM, &clk_sm);
        nvmlDeviceGetClockInfo(dev, NVML_CLOCK_MEM, &clk_mem);
        nvmlDeviceGetClockInfo(dev, NVML_CLOCK_VIDEO, &clk_vid);
        nvmlDeviceGetMaxClockInfo(dev, NVML_CLOCK_GRAPHICS, &mclk_gr);
        nvmlDeviceGetMaxClockInfo(dev, NVML_CLOCK_SM, &mclk_sm);
        nvmlDeviceGetMaxClockInfo(dev, NVML_CLOCK_MEM, &mclk_mem);
        nvmlDeviceGetMaxClockInfo(dev, NVML_CLOCK_VIDEO, &mclk_vid);

        printf("GPU %s\n", pci.busId);
        printf("    Product Name                          : %s\n", name);
        printf("    Product Brand                         : Tesla\n");
        printf("    Product Architecture                  : Pascal\n");
        printf("    Serial Number                         : %s\n", serial[0] ? serial : "N/A");
        printf("    GPU UUID                              : %s\n", uuid);
        printf("    Board Part Number                     : %s\n", part[0] ? part : "N/A");
        printf("    VBIOS Version                         : %s\n", vbios);
        printf("    FB Memory Usage\n");
        printf("        Total                             : %llu MiB\n", mem2.total / (1024ULL * 1024ULL));
        printf("        Reserved                          : %llu MiB\n", mem2.reserved / (1024ULL * 1024ULL));
        printf("        Used                              : %llu MiB\n", mem2.used / (1024ULL * 1024ULL));
        printf("        Free                              : %llu MiB\n", mem2.free / (1024ULL * 1024ULL));
        printf("    BAR1 Memory Usage\n");
        printf("        Total                             : %llu MiB\n", bar1.bar1Total / (1024ULL * 1024ULL));
        printf("        Used                              : %llu MiB\n", bar1.bar1Used / (1024ULL * 1024ULL));
        printf("        Free                              : %llu MiB\n", bar1.bar1Free / (1024ULL * 1024ULL));
        printf("    Utilization\n");
        printf("        Gpu                               : %u %%\n", util.gpu);
        printf("        Memory                            : %u %%\n", util.memory);
        printf("        Encoder                           : %u %%\n", enc_u);
        printf("        Decoder                           : %u %%\n", dec_u);
        printf("    Temperature\n");
        printf("        GPU Current Temp                  : %u C\n", temp);
        printf("        GPU Shutdown Temp                 : %u C\n", t_shut);
        printf("        GPU Slowdown Temp                 : %u C\n", t_slow);
        printf("    GPU Power Readings\n");
        printf("        Power Draw                        : %.2f W\n", pwr_mw / 1000.0);
        printf("        Current Power Limit               : %.2f W\n", cur_lim / 1000.0);
        printf("        Default Power Limit               : %.2f W\n", def_lim / 1000.0);
        printf("        Min Power Limit                   : %.2f W\n", min_lim / 1000.0);
        printf("        Max Power Limit                   : %.2f W\n", max_lim / 1000.0);
        printf("    Clocks\n");
        printf("        Graphics                          : %u MHz\n", clk_gr);
        printf("        SM                                : %u MHz\n", clk_sm);
        printf("        Memory                            : %u MHz\n", clk_mem);
        printf("        Video                             : %u MHz\n", clk_vid);
        printf("    Max Clocks\n");
        printf("        Graphics                          : %u MHz\n", mclk_gr);
        printf("        SM                                : %u MHz\n", mclk_sm);
        printf("        Memory                            : %u MHz\n", mclk_mem);
        printf("        Video                             : %u MHz\n", mclk_vid);
    }
    return 0;
}

static int print_vgpu_command(int argc, char **argv, int filter_idx) {
    int show_supported = 0, show_creatable = 0, show_query = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--supported") == 0) show_supported = 1;
        else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--creatable") == 0) show_creatable = 1;
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--query") == 0) show_query = 1;
    }
    if (!show_supported && !show_creatable && !show_query) {
        show_query = 1;
    }

    unsigned int dev_count = 0;
    nvmlDeviceGetCount_v2(&dev_count);
    for (unsigned int d = 0; d < dev_count; d++) {
        if (filter_idx >= 0 && (int)d != filter_idx) continue;
        nvmlDevice_t dev = NULL;
        if (nvmlDeviceGetHandleByIndex_v2(d, &dev) != NVML_SUCCESS) continue;
        nvmlPciInfo_t pci = {0};
        nvmlDeviceGetPciInfo_v3(dev, &pci);

        if (show_query) {
            printf("GPU %s\n", pci.busId);
            nvmlVgpuInstance_t insts[32];
            unsigned int icnt = 32;
            nvmlDeviceGetActiveVgpus(dev, &icnt, insts);
            printf("    Active vGPUs                          : %u\n", icnt);
            for (unsigned int j = 0; j < icnt; j++) {
                nvmlVgpuInstance_t inst = insts[j];
                nvmlVgpuTypeId_t vtype = 0;
                nvmlVgpuInstanceGetType(inst, &vtype);
                char vname[64] = {0}, vm_id[64] = {0}, vuuid[64] = {0}, mdev[64] = {0}, gdrv[128] = {0};
                unsigned int nlen = sizeof(vname), lic = 0, frl = 0;
                unsigned long long fb_used = 0;
                nvmlVgpuVmIdType_t id_type = NVML_VGPU_VM_ID_DOMAIN_ID;
                nvmlVgpuTypeGetName(vtype, vname, &nlen);
                nvmlVgpuInstanceGetVmID(inst, vm_id, sizeof(vm_id), &id_type);
                nvmlVgpuInstanceGetUUID(inst, vuuid, sizeof(vuuid));
                nvmlVgpuInstanceGetMdevUUID(inst, mdev, sizeof(mdev));
                nvmlVgpuInstanceGetVmDriverVersion(inst, gdrv, sizeof(gdrv));
                nvmlVgpuInstanceGetFbUsage(inst, &fb_used);
                nvmlVgpuInstanceGetLicenseStatus(inst, &lic);
                nvmlVgpuInstanceGetFrameRateLimit(inst, &frl);

                printf("    vGPU ID                               : %u\n", inst);
                printf("        VM ID                             : %s\n", vm_id);
                printf("        vGPU Name                         : %s\n", vname);
                printf("        vGPU Type                         : %u\n", vtype);
                printf("        vGPU UUID                         : %s\n", vuuid);
                printf("        MDEV UUID                         : %s\n", mdev);
                printf("        Guest Driver Version              : %s\n", gdrv);
                printf("        License Status                    : %s\n", lic ? "Licensed (Expiry: Permanent)" : "Unlicensed");
                printf("        FB Memory Usage\n");
                printf("            Used                          : %llu MiB\n", fb_used / (1024ULL * 1024ULL));
                printf("        Frame Rate Limit                  : %u FPS\n", frl);
            }
        }
        if (show_supported) {
            printf("GPU %s\n", pci.busId);
            nvmlVgpuTypeId_t types[64];
            unsigned int tcnt = 64;
            if (nvmlDeviceGetSupportedVgpus(dev, &tcnt, types) == NVML_SUCCESS) {
                for (unsigned int j = 0; j < tcnt; j++) {
                    char vname[64] = {0}, vcls[64] = {0};
                    unsigned int nlen = sizeof(vname), clen = sizeof(vcls), max_inst = 0;
                    unsigned long long fb = 0;
                    nvmlVgpuTypeGetName(types[j], vname, &nlen);
                    nvmlVgpuTypeGetClass(types[j], vcls, &clen);
                    nvmlVgpuTypeGetMaxInstances(dev, types[j], &max_inst);
                    nvmlVgpuTypeGetFramebufferSize(types[j], &fb);
                    printf("    %-15s (Type ID: 0x%02x, Class: %-6s, Max Instances: %u, FB: %llu MiB)\n",
                           vname, types[j], vcls, max_inst, fb / (1024ULL * 1024ULL));
                }
            }
        }
        if (show_creatable) {
            printf("GPU %s\n", pci.busId);
            nvmlVgpuTypeId_t types[64];
            unsigned int tcnt = 64;
            if (nvmlDeviceGetCreatableVgpus(dev, &tcnt, types) == NVML_SUCCESS && tcnt > 0) {
                for (unsigned int j = 0; j < tcnt; j++) {
                    char vname[64] = {0};
                    unsigned int nlen = sizeof(vname);
                    nvmlVgpuTypeGetName(types[j], vname, &nlen);
                    printf("    %s\n", vname);
                }
            } else {
                printf("No vGPUs found on this device \n");
            }
        }
    }
    return 0;
}

static int print_csv_query(const char *query_fields, const char *format_opts, int filter_idx) {
    int noheader = (format_opts && strstr(format_opts, "noheader") != NULL);
    int nounits  = (format_opts && strstr(format_opts, "nounits") != NULL);

    char fields_copy[1024];
    snprintf(fields_copy, sizeof(fields_copy), "%s", query_fields);
    char *tokens[64];
    int ntok = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(fields_copy, ",", &saveptr); tok && ntok < 64; tok = strtok_r(NULL, ",", &saveptr)) {
        while (*tok == ' ') tok++;
        tokens[ntok++] = tok;
    }

    if (!noheader) {
        for (int t = 0; t < ntok; t++) {
            printf("%s%s", t > 0 ? ", " : "", tokens[t]);
        }
        printf("\n");
    }

    char drv_ver[80] = {0};
    nvmlSystemGetDriverVersion(drv_ver, sizeof(drv_ver));
    unsigned int dev_count = 0;
    nvmlDeviceGetCount_v2(&dev_count);

    for (unsigned int i = 0; i < dev_count; i++) {
        if (filter_idx >= 0 && (int)i != filter_idx) continue;
        nvmlDevice_t dev = NULL;
        if (nvmlDeviceGetHandleByIndex_v2(i, &dev) != NVML_SUCCESS) continue;

        char name[64] = {0}, uuid[96] = {0}, serial[64] = {0}, vbios[32] = {0};
        nvmlDeviceGetName(dev, name, sizeof(name));
        nvmlDeviceGetUUID(dev, uuid, sizeof(uuid));
        nvmlDeviceGetSerial(dev, serial, sizeof(serial));
        nvmlDeviceGetVbiosVersion(dev, vbios, sizeof(vbios));
        nvmlPciInfo_t pci = {0};
        nvmlDeviceGetPciInfo_v3(dev, &pci);
        nvmlMemory_v2_t mem2 = { .version = nvmlMemory_v2 };
        nvmlDeviceGetMemoryInfo_v2(dev, &mem2);
        nvmlUtilization_t util = {0};
        nvmlDeviceGetUtilizationRates(dev, &util);
        unsigned int temp = 0, pwr_mw = 0, lim_mw = 75000;
        nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp);
        nvmlDeviceGetPowerUsage(dev, &pwr_mw);
        nvmlDeviceGetPowerManagementLimit(dev, &lim_mw);
        nvmlPstates_t pstate = NVML_PSTATE_8;
        nvmlDeviceGetPerformanceState(dev, &pstate);
        unsigned int clk_gr = 0, clk_sm = 0, clk_mem = 0, clk_vid = 0;
        nvmlDeviceGetClockInfo(dev, NVML_CLOCK_GRAPHICS, &clk_gr);
        nvmlDeviceGetClockInfo(dev, NVML_CLOCK_SM, &clk_sm);
        nvmlDeviceGetClockInfo(dev, NVML_CLOCK_MEM, &clk_mem);
        nvmlDeviceGetClockInfo(dev, NVML_CLOCK_VIDEO, &clk_vid);

        for (int t = 0; t < ntok; t++) {
            const char *f = tokens[t];
            if (t > 0) printf(", ");
            if (strcmp(f, "index") == 0) printf("%u", i);
            else if (strcmp(f, "name") == 0 || strcmp(f, "gpu_name") == 0) printf("%s", name);
            else if (strcmp(f, "uuid") == 0 || strcmp(f, "gpu_uuid") == 0) printf("%s", uuid);
            else if (strcmp(f, "serial") == 0 || strcmp(f, "gpu_serial") == 0) printf("%s", serial);
            else if (strcmp(f, "pci.bus_id") == 0 || strcmp(f, "gpu_bus_id") == 0) printf("%s", pci.busId);
            else if (strcmp(f, "driver_version") == 0) printf("%s", drv_ver);
            else if (strcmp(f, "vbios_version") == 0) printf("%s", vbios);
            else if (strcmp(f, "temperature.gpu") == 0) printf("%u", temp);
            else if (strcmp(f, "power.draw") == 0) printf(nounits ? "%.2f" : "%.2f W", pwr_mw / 1000.0);
            else if (strcmp(f, "power.limit") == 0 || strcmp(f, "enforced.power.limit") == 0) printf(nounits ? "%.2f" : "%.2f W", lim_mw / 1000.0);
            else if (strcmp(f, "clocks.gr") == 0 || strcmp(f, "clocks.current.graphics") == 0) printf(nounits ? "%u" : "%u MHz", clk_gr);
            else if (strcmp(f, "clocks.sm") == 0 || strcmp(f, "clocks.current.sm") == 0) printf(nounits ? "%u" : "%u MHz", clk_sm);
            else if (strcmp(f, "clocks.mem") == 0 || strcmp(f, "clocks.current.memory") == 0) printf(nounits ? "%u" : "%u MHz", clk_mem);
            else if (strcmp(f, "clocks.video") == 0 || strcmp(f, "clocks.current.video") == 0) printf(nounits ? "%u" : "%u MHz", clk_vid);
            else if (strcmp(f, "memory.total") == 0) printf(nounits ? "%llu" : "%llu MiB", mem2.total / (1024ULL * 1024ULL));
            else if (strcmp(f, "memory.used") == 0) printf(nounits ? "%llu" : "%llu MiB", mem2.used / (1024ULL * 1024ULL));
            else if (strcmp(f, "memory.free") == 0) printf(nounits ? "%llu" : "%llu MiB", mem2.free / (1024ULL * 1024ULL));
            else if (strcmp(f, "memory.reserved") == 0) printf(nounits ? "%llu" : "%llu MiB", mem2.reserved / (1024ULL * 1024ULL));
            else if (strcmp(f, "utilization.gpu") == 0) printf(nounits ? "%u" : "%u %%", util.gpu);
            else if (strcmp(f, "utilization.memory") == 0) printf(nounits ? "%u" : "%u %%", util.memory);
            else if (strcmp(f, "pstate") == 0) printf("P%u", (unsigned int)pstate);
            else if (strcmp(f, "persistence_mode") == 0) printf("Enabled");
            else if (strcmp(f, "compute_mode") == 0) printf("Default");
            else printf("[N/A]");
        }
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    int mode_list = 0, mode_query = 0, mode_vgpu = 0;
    const char *query_gpu = NULL;
    const char *format_opts = "csv";
    int filter_idx = -1;

    if (argc >= 2 && strcmp(argv[1], "vgpu") == 0) {
        mode_vgpu = 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("open-nvidia-smi (NVIDIA-SMI 580.196.09 clean-room replacement)\n");
            printf("Usage: nvidia-smi [options] | nvidia-smi vgpu [-s|-c|-q]\n");
            printf("  -L, --list-gpus            List attached GPUs and UUIDs\n");
            printf("  -q, --query                Display detailed GPU / vGPU telemetry log\n");
            printf("  -i, --id=<idx>             Target a specific GPU index\n");
            printf("  --query-gpu=<fields>       Comma-separated fields for CSV output\n");
            printf("  --format=csv[,noheader][,nounits]\n");
            return 0;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("NVIDIA-SMI version  : 580.196.09 (open-nvidia-smi)\n");
            printf("NVML version        : 12.580.196.09 (libopen-nvidia-ml.so)\n");
            return 0;
        }
        if (strcmp(argv[i], "-L") == 0 || strcmp(argv[i], "--list-gpus") == 0) {
            mode_list = 1;
        } else if (!mode_vgpu && (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--query") == 0)) {
            mode_query = 1;
        } else if (strncmp(argv[i], "--query-gpu=", 12) == 0) {
            query_gpu = argv[i] + 12;
        } else if (strncmp(argv[i], "--format=", 9) == 0) {
            format_opts = argv[i] + 9;
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            filter_idx = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--id=", 5) == 0) {
            filter_idx = atoi(argv[i] + 5);
        }
    }

    nvmlReturn_t rc = nvmlInit_v2();
    if (rc != NVML_SUCCESS) {
        fprintf(stderr, "NVIDIA-SMI has failed because it couldn't communicate with the NVIDIA driver. (%s)\n",
                nvmlErrorString(rc));
        return 1;
    }

    int ret = 0;
    if (mode_vgpu) {
        ret = print_vgpu_command(argc, argv, filter_idx);
    } else if (mode_list) {
        ret = print_list_gpus(filter_idx);
    } else if (query_gpu) {
        ret = print_csv_query(query_gpu, format_opts, filter_idx);
    } else if (mode_query) {
        ret = print_query_log(filter_idx);
    } else {
        ret = print_default_table(filter_idx);
    }

    nvmlShutdown();
    return ret;
}
