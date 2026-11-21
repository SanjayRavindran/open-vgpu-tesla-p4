#define _GNU_SOURCE
#include "open_vgxcfg.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t get_u32(const uint8_t *p, size_t off) {
    uint32_t v = 0;
    memcpy(&v, p + off, sizeof(v));
    return v;
}

static const char *SAMPLE_XML =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<vgxconfig>\n"
    "  <globalSettings>\n"
    "    <homogeneousUsms>FALSE</homogeneousUsms>\n"
    "    <licenseServer>192.168.1.24</licenseServer>\n"
    "  </globalSettings>\n"
    "  <pgpu>\n"
    "    <devId vendorId=\"0x10de\" deviceId=\"0x1bb3\" subsystemVendorId=\"0x10de\" subsystemId=\"0x0\"/>\n"
    "    <homogeneousUsms>TRUE</homogeneousUsms>\n"
    "    <supportedUsm usmId=\"65\">\n"
    "      <devId vendorId=\"0x10de\" deviceId=\"0x1bb3\" subsystemVendorId=\"0x10de\" subsystemId=\"0x1206\"/>\n"
    "      <maxUsms>2</maxUsms>\n"
    "      <digest type=\"signature\">deadbeef</digest>\n"
    "    </supportedUsm>\n"
    "  </pgpu>\n"
    "  <usmType id=\"65\" name=\"GRID P4-4Q\" class=\"Quadro\">\n"
    "    <framebuffer>0x1000</framebuffer>\n"
    "    <license>QVDWS;VWS</license>\n"
    "    <display width=\"3840\" height=\"2160\"/>\n"
    "    <display width=\"2560\" height=\"1440\"/>\n"
    "  </usmType>\n"
    "</vgxconfig>\n";

static void write_tmp(const char *path, const char *content) {
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    assert(fwrite(content, 1, strlen(content), fp) == strlen(content));
    fclose(fp);
}

int main(void) {
    char valid_path[] = "/tmp/open_vgxcfg_unit_XXXXXX";
    int fd1 = mkstemp(valid_path);
    assert(fd1 >= 0);
    close(fd1);
    write_tmp(valid_path, SAMPLE_XML);

    /* 1. Missing digest check -> status 3 */
    char nodigest_path[] = "/tmp/open_vgxcfg_nodigest_XXXXXX";
    int fd2 = mkstemp(nodigest_path);
    assert(fd2 >= 0);
    close(fd2);
    write_tmp(nodigest_path, "<vgxconfig><globalSettings/></vgxconfig>");
    setenv("NV_VGX_CFG_FILE", nodigest_path, 1);
    void *h = NULL;
    assert(vgxConfigOpen(NULL) == VGX_CFG_ERR_NULL_PARAM);
    assert(vgxConfigOpen(&h) == VGX_CFG_ERR_MISSING);
    unlink(nodigest_path);

    /* 2. Open valid fixture */
    setenv("NV_VGX_CFG_FILE", valid_path, 1);
    assert(vgxConfigOpen(&h) == VGX_CFG_OK);
    assert(h != NULL);
    assert(vgxConfigWriteGlobalSettings(NULL) == VGX_CFG_ERR_BAD_HANDLE);
    assert(vgxConfigWriteGlobalSettings(h) == 1);

    /* 3. Size query & global settings read */
    uint32_t sz = 0;
    assert(vgxConfigReadGlobalSettings(h, NULL, &sz) == VGX_CFG_OK);
    assert(sz == 0x210);
    uint8_t gs[0x210];
    assert(vgxConfigReadGlobalSettings(h, gs, &sz) == VGX_CFG_OK);
    assert(get_u32(gs, 0x00) == 1);
    assert(get_u32(gs, 0x04) == 0x210);
    assert(get_u32(gs, 0x08) == 1);
    assert(get_u32(gs, 0x0c) == 0);
    assert(strcmp((char *)(gs + 0x10), "192.168.1.24") == 0);

    /* 4. Small buffer truncation -> status 4 & updated required size */
    uint8_t small[8];
    uint32_t small_sz = 8;
    assert(vgxConfigReadGlobalSettings(h, small, &small_sz) == VGX_CFG_ERR_SMALL_BUF);
    assert(small_sz == 0x210);
    assert(get_u32(small, 0) == 1);

    /* 5. PhysGpu & SupportedUsm */
    const uint16_t p4_dev[4] = {0x10de, 0x1bb3, 0x10de, 0x0};
    const uint16_t bad_dev[4] = {0x10de, 0x9999, 0x10de, 0x0};
    uint8_t pg[0x18];
    sz = sizeof(pg);
    assert(vgxConfigReadPhysGpu(h, bad_dev, pg, &sz) == VGX_CFG_ERR_NOT_FOUND);
    assert(vgxConfigReadPhysGpu(h, p4_dev, pg, &sz) == VGX_CFG_OK);
    assert(get_u32(pg, 0x10) == 1);
    assert(get_u32(pg, 0x14) == 1);

    uint8_t sup[0x18];
    sz = sizeof(sup);
    assert(vgxConfigReadPhysGpuSupportedUsm(h, p4_dev, 1, sup, &sz) == VGX_CFG_ERR_OUT_OF_RANGE);
    assert(vgxConfigReadPhysGpuSupportedUsm(h, p4_dev, 0, sup, &sz) == VGX_CFG_OK);
    assert(get_u32(sup, 0x08) == 65);
    assert(get_u32(sup, 0x14) == 2);

    /* 6. UsmType by index / id / display */
    uint8_t usm[0xe4];
    sz = sizeof(usm);
    assert(vgxConfigReadUsmTypeByIndex(h, 0, usm, &sz) == VGX_CFG_OK);
    assert(get_u32(usm, 0x08) == 65);
    assert(get_u32(usm, 0x0c) == 0x1000);
    assert(get_u32(usm, 0x10) == 2);
    assert(strcmp((char *)(usm + 0x14), "Quadro") == 0);
    assert(strcmp((char *)(usm + 0x3e), "GRID P4-4Q") == 0);
    assert(strcmp((char *)(usm + 0xb8), "QVDWS;VWS") == 0);

    uint8_t usm2[0xe4];
    sz = sizeof(usm2);
    assert(vgxConfigReadUsmTypeById(h, 999, usm2, &sz) == VGX_CFG_ERR_NOT_FOUND);
    assert(vgxConfigReadUsmTypeById(h, 65, usm2, &sz) == VGX_CFG_OK);
    assert(memcmp(usm, usm2, sizeof(usm)) == 0);

    uint8_t disp[0x10];
    sz = sizeof(disp);
    assert(vgxConfigReadUsmTypeDisplay(h, 65, 2, disp, &sz) == VGX_CFG_ERR_OUT_OF_RANGE);
    assert(vgxConfigReadUsmTypeDisplay(h, 65, 1, disp, &sz) == VGX_CFG_OK);
    assert(get_u32(disp, 0x08) == 2560);
    assert(get_u32(disp, 0x0c) == 2560);

    /* 7. Close & invalid handle check */
    assert(vgxConfigClose(h) == VGX_CFG_OK);
    assert(vgxConfigClose(h) == VGX_CFG_ERR_BAD_HANDLE);
    unlink(valid_path);
    printf("test_vgxcfg_unit: ALL PASSED\n");
    return 0;
}
