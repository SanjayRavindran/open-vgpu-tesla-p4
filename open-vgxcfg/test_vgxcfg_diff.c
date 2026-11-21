#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*fn_open_t)(void **);
typedef int (*fn_close_t)(void *);
typedef int (*fn_read_gs_t)(void *, void *, uint32_t *);
typedef int (*fn_write_gs_t)(void *);
typedef int (*fn_read_pgpu_t)(void *, const uint16_t[4], void *, uint32_t *);
typedef int (*fn_read_pgpu_sup_t)(void *, const uint16_t[4], uint32_t, void *, uint32_t *);
typedef int (*fn_read_usm_idx_t)(void *, uint32_t, void *, uint32_t *);
typedef int (*fn_read_usm_id_t)(void *, uint32_t, void *, uint32_t *);
typedef int (*fn_read_usm_disp_t)(void *, uint32_t, uint32_t, void *, uint32_t *);

typedef struct Api {
    void *dl;
    fn_open_t open;
    fn_close_t close;
    fn_read_gs_t read_gs;
    fn_write_gs_t write_gs;
    fn_read_pgpu_t read_pgpu;
    fn_read_pgpu_sup_t read_pgpu_sup;
    fn_read_usm_idx_t read_usm_idx;
    fn_read_usm_id_t read_usm_id;
    fn_read_usm_disp_t read_usm_disp;
} Api;

static Api load_api(const char *path) {
    Api a;
    memset(&a, 0, sizeof(a));
    a.dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!a.dl) {
        fprintf(stderr, "dlopen(%s): %s\n", path, dlerror());
        exit(2);
    }
    a.open = (fn_open_t)dlsym(a.dl, "vgxConfigOpen");
    a.close = (fn_close_t)dlsym(a.dl, "vgxConfigClose");
    a.read_gs = (fn_read_gs_t)dlsym(a.dl, "vgxConfigReadGlobalSettings");
    a.write_gs = (fn_write_gs_t)dlsym(a.dl, "vgxConfigWriteGlobalSettings");
    a.read_pgpu = (fn_read_pgpu_t)dlsym(a.dl, "vgxConfigReadPhysGpu");
    a.read_pgpu_sup = (fn_read_pgpu_sup_t)dlsym(a.dl, "vgxConfigReadPhysGpuSupportedUsm");
    a.read_usm_idx = (fn_read_usm_idx_t)dlsym(a.dl, "vgxConfigReadUsmTypeByIndex");
    a.read_usm_id = (fn_read_usm_id_t)dlsym(a.dl, "vgxConfigReadUsmTypeById");
    a.read_usm_disp = (fn_read_usm_disp_t)dlsym(a.dl, "vgxConfigReadUsmTypeDisplay");
    assert(a.open && a.close && a.read_gs && a.write_gs && a.read_pgpu &&
           a.read_pgpu_sup && a.read_usm_idx && a.read_usm_id && a.read_usm_disp);
    return a;
}

static void check_eq_buf(const char *label, int rc_a, int rc_b, uint32_t sz_a, uint32_t sz_b,
                         const uint8_t *ba, const uint8_t *bb, size_t n) {
    if (rc_a != rc_b || sz_a != sz_b || (n > 0 && memcmp(ba, bb, n) != 0)) {
        fprintf(stderr, "DIFF at %s: rc=(%d vs %d) sz=(0x%x vs 0x%x)\n",
                label, rc_a, rc_b, sz_a, sz_b);
        for (size_t i = 0; i < n; i++) {
            if (ba[i] != bb[i]) {
                fprintf(stderr, "  first diff @ +0x%zx: orig=0x%02x open=0x%02x\n",
                        i, ba[i], bb[i]);
                break;
            }
        }
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <orig_so> <open_so> <nvVgxConfig.xml>\n", argv[0]);
        return 2;
    }
    Api orig = load_api(argv[1]);
    Api open = load_api(argv[2]);
    setenv("NV_VGX_CFG_FILE", argv[3], 1);

    void *ha = NULL, *hb = NULL;
    int rca = orig.open(&ha);
    int rcb = open.open(&hb);
    assert(rca == rcb);
    assert(orig.write_gs(NULL) == open.write_gs(NULL));
    assert(orig.write_gs(ha) == open.write_gs(hb));

    /* GlobalSettings: query, full, small */
    uint32_t sza = 0, szb = 0;
    rca = orig.read_gs(ha, NULL, &sza);
    rcb = open.read_gs(hb, NULL, &szb);
    check_eq_buf("gs_query", rca, rcb, sza, szb, NULL, NULL, 0);

    uint8_t ba[0x220], bb[0x220];
    memset(ba, 0xaa, sizeof(ba));
    memset(bb, 0xaa, sizeof(bb));
    rca = orig.read_gs(ha, ba, &sza);
    rcb = open.read_gs(hb, bb, &szb);
    check_eq_buf("gs_full", rca, rcb, sza, szb, ba, bb, sza);

    memset(ba, 0xaa, sizeof(ba));
    memset(bb, 0xaa, sizeof(bb));
    sza = 12; szb = 12;
    rca = orig.read_gs(ha, ba, &sza);
    rcb = open.read_gs(hb, bb, &szb);
    check_eq_buf("gs_small", rca, rcb, sza, szb, ba, bb, 12);

    /* PhysGpu + SupportedUsm */
    const uint16_t p4_dev[4] = {0x10de, 0x1bb3, 0x10de, 0x0};
    const uint16_t bad_dev[4] = {0x10de, 0x9999, 0x10de, 0x0};
    memset(ba, 0x55, 0x20); memset(bb, 0x55, 0x20);
    sza = 0x18; szb = 0x18;
    rca = orig.read_pgpu(ha, p4_dev, ba, &sza);
    rcb = open.read_pgpu(hb, p4_dev, bb, &szb);
    check_eq_buf("pgpu_p4", rca, rcb, sza, szb, ba, bb, 0x18);

    rca = orig.read_pgpu(ha, bad_dev, ba, &sza);
    rcb = open.read_pgpu(hb, bad_dev, bb, &szb);
    check_eq_buf("pgpu_missing", rca, rcb, sza, szb, NULL, NULL, 0);

    for (uint32_t idx = 0; idx < 4; idx++) {
        memset(ba, 0x55, 0x20); memset(bb, 0x55, 0x20);
        sza = 0x18; szb = 0x18;
        rca = orig.read_pgpu_sup(ha, p4_dev, idx, ba, &sza);
        rcb = open.read_pgpu_sup(hb, p4_dev, idx, bb, &szb);
        check_eq_buf("pgpu_sup", rca, rcb, sza, szb, ba, bb, (rca == 0) ? 0x18 : 0);
    }

    /* UsmType by index / id / display */
    for (uint32_t idx = 0; idx < 4; idx++) {
        memset(ba, 0x55, 0x100); memset(bb, 0x55, 0x100);
        sza = 0xe4; szb = 0xe4;
        rca = orig.read_usm_idx(ha, idx, ba, &sza);
        rcb = open.read_usm_idx(hb, idx, bb, &szb);
        check_eq_buf("usm_idx", rca, rcb, sza, szb, ba, bb, (rca == 0) ? 0xe4 : 0);
    }
    for (uint32_t id = 64; id <= 67; id++) {
        memset(ba, 0x55, 0x100); memset(bb, 0x55, 0x100);
        sza = 0xe4; szb = 0xe4;
        rca = orig.read_usm_id(ha, id, ba, &sza);
        rcb = open.read_usm_id(hb, id, bb, &szb);
        check_eq_buf("usm_id", rca, rcb, sza, szb, ba, bb, (rca == 0) ? 0xe4 : 0);
    }
    for (uint32_t d = 0; d < 3; d++) {
        memset(ba, 0x55, 0x20); memset(bb, 0x55, 0x20);
        sza = 0x10; szb = 0x10;
        rca = orig.read_usm_disp(ha, 65, d, ba, &sza);
        rcb = open.read_usm_disp(hb, 65, d, bb, &szb);
        check_eq_buf("usm_disp", rca, rcb, sza, szb, ba, bb, (rca == 0) ? 0x10 : 0);
    }

    assert(orig.close(NULL) == open.close(NULL));
    assert(orig.close((void *)0x1234) == open.close((void *)0x1234));
    assert(orig.close(ha) == open.close(hb));
    dlclose(orig.dl);
    dlclose(open.dl);
    printf("test_vgxcfg_diff: 0 DIFFERENCES ACROSS ALL 9 EXPORTS!\n");
    return 0;
}
