#define _GNU_SOURCE
#include "open_vgxcfg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_VGX_CFG_PATH "/usr/share/nvidia/vgx/nvVgxConfig.xml"

#define GLOBAL_SETTINGS_SIZE 0x210U
#define PHYS_GPU_SIZE        0x018U
#define SUPPORTED_USM_SIZE   0x018U
#define USM_TYPE_SIZE        0x0e4U
#define USM_DISPLAY_SIZE     0x010U

typedef struct XmlAttr {
    char *name;
    char *value;
} XmlAttr;

typedef struct XmlNode {
    char *tag;
    char *text;
    XmlAttr *attrs;
    size_t n_attrs;
    struct XmlNode *parent;
    struct XmlNode *first_child;
    struct XmlNode *last_child;
    struct XmlNode *next_sibling;
    struct XmlNode *next_preorder;
} XmlNode;

typedef struct VgxConfigHandle {
    XmlNode *root;
    XmlNode *head;
} VgxConfigHandle;

static void *g_active_handle = NULL;

static char *dup_range(const char *s, size_t len) {
    char *p = (char *)malloc(len + 1);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static void xml_free_all(XmlNode *head) {
    while (head) {
        XmlNode *next = head->next_preorder;
        free(head->tag);
        free(head->text);
        for (size_t i = 0; i < head->n_attrs; i++) {
            free(head->attrs[i].name);
            free(head->attrs[i].value);
        }
        free(head->attrs);
        free(head);
        head = next;
    }
}

static int parse_xml_document(const char *buf, size_t len, XmlNode **out_root, XmlNode **out_head) {
    const char *p = buf;
    const char *end = buf + len;
    XmlNode *root = NULL;
    XmlNode *head = NULL;
    XmlNode *tail = NULL;
    XmlNode *cur = NULL;
    int seen_root = 0;

    while (p < end && isspace((unsigned char)*p)) {
        p++;
    }
    if (p >= end || *p != '<') {
        return 1;
    }

    while (p < end) {
        const char *lt = (const char *)memchr(p, '<', (size_t)(end - p));
        if (!lt) {
            break;
        }
        if (cur && lt > p) {
            const char *ts = p;
            const char *te = lt;
            while (ts < te && isspace((unsigned char)*ts)) ts++;
            while (te > ts && isspace((unsigned char)te[-1])) te--;
            if (te > ts && !cur->text) {
                cur->text = dup_range(ts, (size_t)(te - ts));
                if (!cur->text) {
                    xml_free_all(head);
                    return 1;
                }
            }
        }
        p = lt + 1;
        if (p >= end) {
            xml_free_all(head);
            return 1;
        }
        if (p + 2 < end && memcmp(p, "!--", 3) == 0) {
            p += 3;
            while (p + 2 < end && memcmp(p, "-->", 3) != 0) p++;
            if (p + 2 >= end) {
                xml_free_all(head);
                return 1;
            }
            p += 3;
            continue;
        }
        if (*p == '?') {
            p++;
            while (p + 1 < end && !(p[0] == '?' && p[1] == '>')) p++;
            if (p + 1 >= end) {
                xml_free_all(head);
                return 1;
            }
            p += 2;
            continue;
        }
        if (*p == '!') {
            while (p < end && *p != '>') p++;
            if (p >= end) {
                xml_free_all(head);
                return 1;
            }
            p++;
            continue;
        }
        if (*p == '/') {
            p++;
            const char *ns = p;
            while (p < end && *p != '>' && !isspace((unsigned char)*p)) p++;
            size_t nlen = (size_t)(p - ns);
            while (p < end && *p != '>') p++;
            if (p >= end || !cur || strlen(cur->tag) != nlen || memcmp(cur->tag, ns, nlen) != 0) {
                xml_free_all(head);
                return 1;
            }
            p++;
            cur = cur->parent;
            continue;
        }

        const char *ns = p;
        while (p < end && !isspace((unsigned char)*p) && *p != '>' && *p != '/') p++;
        if (p == ns || p >= end) {
            xml_free_all(head);
            return 1;
        }
        if (!cur && seen_root) {
            xml_free_all(head);
            return 1;
        }

        XmlNode *node = (XmlNode *)calloc(1, sizeof(XmlNode));
        if (!node) {
            xml_free_all(head);
            return 1;
        }
        node->tag = dup_range(ns, (size_t)(p - ns));
        if (!node->tag) {
            free(node);
            xml_free_all(head);
            return 1;
        }
        node->parent = cur;
        if (!head) {
            head = node;
            tail = node;
        } else {
            tail->next_preorder = node;
            tail = node;
        }
        if (!cur) {
            root = node;
            seen_root = 1;
        } else {
            if (!cur->first_child) {
                cur->first_child = node;
            } else {
                cur->last_child->next_sibling = node;
            }
            cur->last_child = node;
        }

        int self_closing = 0;
        while (p < end) {
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p >= end) {
                xml_free_all(head);
                return 1;
            }
            if (*p == '/') {
                self_closing = 1;
                p++;
                while (p < end && isspace((unsigned char)*p)) p++;
                if (p >= end || *p != '>') {
                    xml_free_all(head);
                    return 1;
                }
                p++;
                break;
            }
            if (*p == '>') {
                p++;
                break;
            }
            const char *as = p;
            while (p < end && *p != '=' && !isspace((unsigned char)*p) && *p != '>' && *p != '/') p++;
            size_t alen = (size_t)(p - as);
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p >= end || *p != '=' || alen == 0) {
                xml_free_all(head);
                return 1;
            }
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p >= end || (*p != '"' && *p != '\'')) {
                xml_free_all(head);
                return 1;
            }
            char q = *p++;
            const char *vs = p;
            while (p < end && *p != q) p++;
            if (p >= end) {
                xml_free_all(head);
                return 1;
            }
            size_t vlen = (size_t)(p - vs);
            p++;

            XmlAttr *na = (XmlAttr *)realloc(node->attrs, (node->n_attrs + 1) * sizeof(XmlAttr));
            if (!na) {
                xml_free_all(head);
                return 1;
            }
            node->attrs = na;
            node->attrs[node->n_attrs].name = dup_range(as, alen);
            node->attrs[node->n_attrs].value = dup_range(vs, vlen);
            if (!node->attrs[node->n_attrs].name || !node->attrs[node->n_attrs].value) {
                xml_free_all(head);
                return 1;
            }
            node->n_attrs++;
        }

        if (!self_closing) {
            cur = node;
        }
    }

    if (cur != NULL || !root) {
        xml_free_all(head);
        return 1;
    }
    *out_root = root;
    *out_head = head;
    return 0;
}

static int valid_handle(void *handle) {
    return handle != NULL && handle == g_active_handle;
}

static const char *node_attr(const XmlNode *n, const char *name) {
    if (!n) return NULL;
    for (size_t i = 0; i < n->n_attrs; i++) {
        if (strcmp(n->attrs[i].name, name) == 0) {
            return n->attrs[i].value;
        }
    }
    return NULL;
}

static XmlNode *find_descendant_after(XmlNode *after, XmlNode *top, const char *tag) {
    XmlNode *n = (after && after != top) ? after->next_preorder : (top ? top->next_preorder : NULL);
    if (!top) return NULL;
    if (after == NULL) n = top;
    else if (after == top) n = top->first_child;
    else {
        /* Walk strictly inside `top`'s subtree after `after`. */
        XmlNode *walk = after->next_preorder;
        while (walk) {
            XmlNode *anc = walk->parent;
            int inside = (walk == top);
            while (anc && !inside) {
                if (anc == top) inside = 1;
                anc = anc->parent;
            }
            if (!inside) return NULL;
            if (strcmp(walk->tag, tag) == 0) return walk;
            walk = walk->next_preorder;
        }
        return NULL;
    }
    for (XmlNode *walk = n; walk; walk = walk->next_preorder) {
        if (walk != top) {
            XmlNode *anc = walk->parent;
            int inside = 0;
            while (anc) {
                if (anc == top) { inside = 1; break; }
                anc = anc->parent;
            }
            if (!inside) return NULL;
        }
        if (strcmp(walk->tag, tag) == 0) return walk;
    }
    return NULL;
}

static int read_str_field(const XmlNode *node, const char *attr, char *dst, uint32_t cap) {
    const char *src = attr ? node_attr(node, attr) : (node ? node->text : NULL);
    if (!src) return 1;
    strncpy(dst, src, cap);
    return 0;
}

static int read_u16_field(const XmlNode *node, const char *attr, uint16_t *out) {
    const char *s = attr ? node_attr(node, attr) : (node ? node->text : NULL);
    if (!s) return 1;
    *out = (uint16_t)strtoul(s, NULL, 0);
    return 0;
}

static int read_u32_field(const XmlNode *node, const char *attr, uint32_t *out) {
    const char *s = attr ? node_attr(node, attr) : (node ? node->text : NULL);
    if (!s) return 1;
    *out = (uint32_t)strtoul(s, NULL, 0);
    return 0;
}

static int read_bool_field(const XmlNode *node, uint32_t *out) {
    if (!node || !node->text) return 1;
    if (strcmp(node->text, "TRUE") == 0 || strcmp(node->text, "true") == 0) {
        *out = 1;
        return 0;
    }
    if (strcmp(node->text, "FALSE") == 0 || strcmp(node->text, "false") == 0) {
        *out = 0;
        return 0;
    }
    return 1;
}

static int read_dev_id(const XmlNode *node, uint16_t dev_id[4]) {
    if (read_u16_field(node, "vendorId", &dev_id[0]) != 0) return 1;
    if (read_u16_field(node, "deviceId", &dev_id[1]) != 0) return 1;
    if (read_u16_field(node, "subsystemVendorId", &dev_id[2]) != 0) return 1;
    if (read_u16_field(node, "subsystemId", &dev_id[3]) != 0) return 1;
    return 0;
}

static void put_u32_le(uint8_t *buf, size_t off, uint32_t v) {
    memcpy(buf + off, &v, sizeof(v));
}

static int copy_sized(void *dst, const uint8_t *src, uint32_t required, uint32_t *size_io) {
    int rc = VGX_CFG_OK;
    uint32_t copy_n = required;
    if (*size_io < required) {
        rc = VGX_CFG_ERR_SMALL_BUF;
        copy_n = *size_io;
    }
    memcpy(dst, src, copy_n);
    return rc;
}

int vgxConfigOpen(void **handle) {
    if (!handle) {
        return VGX_CFG_ERR_NULL_PARAM;
    }
    const char *path = getenv("NV_VGX_CFG_FILE");
    if (!path || !*path) {
        path = DEFAULT_VGX_CFG_PATH;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return VGX_CFG_ERR_XML;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return VGX_CFG_ERR_XML;
    }
    long fsize = ftell(fp);
    if (fsize <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return VGX_CFG_ERR_XML;
    }
    char *raw = (char *)malloc((size_t)fsize + 1);
    if (!raw) {
        fclose(fp);
        return VGX_CFG_ERR_XML;
    }
    size_t nread = fread(raw, 1, (size_t)fsize, fp);
    fclose(fp);
    if (nread != (size_t)fsize) {
        free(raw);
        return VGX_CFG_ERR_XML;
    }
    raw[nread] = '\0';

    XmlNode *root = NULL;
    XmlNode *head = NULL;
    int parse_rc = parse_xml_document(raw, nread, &root, &head);
    free(raw);
    if (parse_rc != 0) {
        return VGX_CFG_ERR_XML;
    }

    VgxConfigHandle *cfg = (VgxConfigHandle *)calloc(1, sizeof(VgxConfigHandle));
    if (!cfg) {
        xml_free_all(head);
        return VGX_CFG_ERR_XML;
    }
    cfg->root = root;
    cfg->head = head;

    XmlNode *digest = find_descendant_after(NULL, root, "digest");
    if (!digest || !node_attr(digest, "type")) {
        xml_free_all(head);
        free(cfg);
        return VGX_CFG_ERR_MISSING;
    }

    g_active_handle = cfg;
    *handle = cfg;
    return VGX_CFG_OK;
}

int vgxConfigClose(void *handle) {
    if (!valid_handle(handle)) {
        return VGX_CFG_ERR_BAD_HANDLE;
    }
    VgxConfigHandle *cfg = (VgxConfigHandle *)handle;
    xml_free_all(cfg->head);
    free(cfg);
    g_active_handle = NULL;
    return VGX_CFG_OK;
}

int vgxConfigWriteGlobalSettings(void *handle) {
    if (!handle) {
        return VGX_CFG_ERR_BAD_HANDLE;
    }
    return (g_active_handle != handle) ? 2 : 1;
}

int vgxConfigReadGlobalSettings(void *handle, void *buf, uint32_t *size) {
    if (!valid_handle(handle)) {
        return VGX_CFG_ERR_BAD_HANDLE;
    }
    if (!size) {
        return VGX_CFG_ERR_NULL_PARAM;
    }
    int rc = VGX_CFG_OK;
    if (buf) {
        VgxConfigHandle *cfg = (VgxConfigHandle *)handle;
        uint8_t tmp[GLOBAL_SETTINGS_SIZE];
        memset(tmp, 0, sizeof(tmp));
        put_u32_le(tmp, 0x00, 1);
        put_u32_le(tmp, 0x04, GLOBAL_SETTINGS_SIZE);

        XmlNode *gs = find_descendant_after(NULL, cfg->root, "globalSettings");
        if (!gs) {
            *size = GLOBAL_SETTINGS_SIZE;
            return VGX_CFG_ERR_MISSING;
        }
        XmlNode *homo = find_descendant_after(gs, gs, "homogeneousUsms");
        uint32_t homo_val = 1;
        if (homo) {
            uint32_t parsed = 1;
            if (read_bool_field(homo, &parsed) == 0) {
                homo_val = parsed;
            }
        }
        put_u32_le(tmp, 0x0c, homo_val);

        XmlNode *ls = find_descendant_after(gs, gs, "licenseServer");
        if (ls) {
            read_str_field(ls, NULL, (char *)(tmp + 0x10), 0x100);
        }

        uint32_t n_usm = 0;
        XmlNode *u = NULL;
        while ((u = find_descendant_after(u, cfg->root, "usmType")) != NULL) {
            n_usm++;
        }
        put_u32_le(tmp, 0x08, n_usm);
        rc = copy_sized(buf, tmp, GLOBAL_SETTINGS_SIZE, size);
    }
    *size = GLOBAL_SETTINGS_SIZE;
    return rc;
}

static XmlNode *find_pgpu_node(XmlNode *root, const uint16_t dev_id[4]) {
    XmlNode *pgpu = NULL;
    while ((pgpu = find_descendant_after(pgpu, root, "pgpu")) != NULL) {
        XmlNode *dn = find_descendant_after(pgpu, pgpu, "devId");
        uint16_t cand[4];
        if (dn && read_dev_id(dn, cand) == 0 && memcmp(cand, dev_id, sizeof(cand)) == 0) {
            return pgpu;
        }
    }
    return NULL;
}

static XmlNode *find_usm_by_id(XmlNode *root, uint32_t usm_id) {
    XmlNode *u = NULL;
    while ((u = find_descendant_after(u, root, "usmType")) != NULL) {
        uint32_t id = 0;
        if (read_u32_field(u, "id", &id) == 0 && id == usm_id) {
            return u;
        }
    }
    return NULL;
}

static int pack_usm_type(const XmlNode *u, uint8_t tmp[USM_TYPE_SIZE]) {
    memset(tmp, 0, USM_TYPE_SIZE);
    put_u32_le(tmp, 0x00, 1);
    put_u32_le(tmp, 0x04, USM_TYPE_SIZE);
    if (read_str_field(u, "class", (char *)(tmp + 0x14), 0x14) != 0) return VGX_CFG_ERR_XML;
    if (read_str_field(u, "name", (char *)(tmp + 0x3e), 0x3c) != 0) return VGX_CFG_ERR_XML;
    uint32_t id = 0;
    if (read_u32_field(u, "id", &id) != 0) return VGX_CFG_ERR_XML;
    put_u32_le(tmp, 0x08, id);

    XmlNode *fb = find_descendant_after((XmlNode *)u, (XmlNode *)u, "framebuffer");
    uint32_t fb_val = 0;
    if (!fb || read_u32_field(fb, NULL, &fb_val) != 0) return VGX_CFG_ERR_XML;
    put_u32_le(tmp, 0x0c, fb_val);

    XmlNode *lic = find_descendant_after((XmlNode *)u, (XmlNode *)u, "license");
    if (!lic || read_str_field(lic, NULL, (char *)(tmp + 0xb8), 0x14) != 0) return VGX_CFG_ERR_XML;

    uint32_t n_disp = 0;
    XmlNode *d = (XmlNode *)u;
    while ((d = find_descendant_after(d, (XmlNode *)u, "display")) != NULL) {
        n_disp++;
    }
    put_u32_le(tmp, 0x10, n_disp);
    return VGX_CFG_OK;
}

int vgxConfigReadPhysGpu(void *handle, const uint16_t dev_id[4], void *buf, uint32_t *size) {
    if (!valid_handle(handle)) {
        return VGX_CFG_ERR_BAD_HANDLE;
    }
    if (!dev_id || !size) {
        return VGX_CFG_ERR_NULL_PARAM;
    }
    int rc = VGX_CFG_OK;
    if (buf) {
        VgxConfigHandle *cfg = (VgxConfigHandle *)handle;
        XmlNode *pgpu = find_pgpu_node(cfg->root, dev_id);
        if (!pgpu) {
            *size = PHYS_GPU_SIZE;
            return VGX_CFG_ERR_NOT_FOUND;
        }
        uint8_t tmp[PHYS_GPU_SIZE];
        memset(tmp, 0, sizeof(tmp));
        put_u32_le(tmp, 0x00, 1);
        put_u32_le(tmp, 0x04, PHYS_GPU_SIZE);
        memcpy(tmp + 0x08, dev_id, 8);

        uint32_t homo_val = 1;
        XmlNode *homo = find_descendant_after(pgpu, pgpu, "homogeneousUsms");
        if (homo) {
            uint32_t parsed = 1;
            if (read_bool_field(homo, &parsed) == 0) {
                homo_val = parsed;
            }
        }
        put_u32_le(tmp, 0x14, homo_val);

        uint32_t n_sup = 0;
        XmlNode *s = pgpu;
        while ((s = find_descendant_after(s, pgpu, "supportedUsm")) != NULL) {
            n_sup++;
        }
        put_u32_le(tmp, 0x10, n_sup);
        rc = copy_sized(buf, tmp, PHYS_GPU_SIZE, size);
    }
    *size = PHYS_GPU_SIZE;
    return rc;
}

int vgxConfigReadPhysGpuSupportedUsm(
    void *handle, const uint16_t dev_id[4], uint32_t index, void *buf, uint32_t *size) {
    if (!valid_handle(handle)) {
        return VGX_CFG_ERR_BAD_HANDLE;
    }
    if (!dev_id || !size) {
        return VGX_CFG_ERR_NULL_PARAM;
    }
    int rc = VGX_CFG_OK;
    if (buf) {
        VgxConfigHandle *cfg = (VgxConfigHandle *)handle;
        XmlNode *pgpu = find_pgpu_node(cfg->root, dev_id);
        if (!pgpu) {
            *size = SUPPORTED_USM_SIZE;
            return VGX_CFG_ERR_NOT_FOUND;
        }
        XmlNode *s = pgpu;
        uint32_t cur = 0;
        while ((s = find_descendant_after(s, pgpu, "supportedUsm")) != NULL) {
            if (cur == index) break;
            cur++;
        }
        if (!s) {
            *size = SUPPORTED_USM_SIZE;
            return VGX_CFG_ERR_OUT_OF_RANGE;
        }

        uint8_t tmp[SUPPORTED_USM_SIZE];
        memset(tmp, 0, sizeof(tmp));
        put_u32_le(tmp, 0x00, 1);
        put_u32_le(tmp, 0x04, SUPPORTED_USM_SIZE);

        uint32_t usm_id = 0, max_usms = 0;
        uint16_t udev[4];
        XmlNode *max_node = find_descendant_after(s, s, "maxUsms");
        XmlNode *dev_node = find_descendant_after(s, s, "devId");
        if (read_u32_field(s, "usmId", &usm_id) != 0 || !max_node ||
            read_u32_field(max_node, NULL, &max_usms) != 0 || !dev_node ||
            read_dev_id(dev_node, udev) != 0) {
            *size = SUPPORTED_USM_SIZE;
            return VGX_CFG_ERR_XML;
        }
        put_u32_le(tmp, 0x08, usm_id);
        memcpy(tmp + 0x0c, udev, 8);
        put_u32_le(tmp, 0x14, max_usms);
        rc = copy_sized(buf, tmp, SUPPORTED_USM_SIZE, size);
    }
    *size = SUPPORTED_USM_SIZE;
    return rc;
}

int vgxConfigReadUsmTypeByIndex(void *handle, uint32_t index, void *buf, uint32_t *size) {
    if (!valid_handle(handle)) {
        return VGX_CFG_ERR_BAD_HANDLE;
    }
    if (!size) {
        return VGX_CFG_ERR_NULL_PARAM;
    }
    int rc = VGX_CFG_OK;
    if (buf) {
        VgxConfigHandle *cfg = (VgxConfigHandle *)handle;
        XmlNode *u = NULL;
        uint32_t cur = 0;
        while ((u = find_descendant_after(u, cfg->root, "usmType")) != NULL) {
            if (cur == index) break;
            cur++;
        }
        if (!u) {
            *size = USM_TYPE_SIZE;
            return VGX_CFG_ERR_OUT_OF_RANGE;
        }
        uint8_t tmp[USM_TYPE_SIZE];
        rc = pack_usm_type(u, tmp);
        if (rc == VGX_CFG_OK) {
            rc = copy_sized(buf, tmp, USM_TYPE_SIZE, size);
        }
    }
    *size = USM_TYPE_SIZE;
    return rc;
}

int vgxConfigReadUsmTypeById(void *handle, uint32_t usm_id, void *buf, uint32_t *size) {
    if (!valid_handle(handle)) {
        return VGX_CFG_ERR_BAD_HANDLE;
    }
    if (!size) {
        return VGX_CFG_ERR_NULL_PARAM;
    }
    int rc = VGX_CFG_OK;
    if (buf) {
        VgxConfigHandle *cfg = (VgxConfigHandle *)handle;
        XmlNode *u = find_usm_by_id(cfg->root, usm_id);
        if (!u) {
            *size = USM_TYPE_SIZE;
            return VGX_CFG_ERR_NOT_FOUND;
        }
        uint8_t tmp[USM_TYPE_SIZE];
        rc = pack_usm_type(u, tmp);
        if (rc == VGX_CFG_OK) {
            rc = copy_sized(buf, tmp, USM_TYPE_SIZE, size);
        }
    }
    *size = USM_TYPE_SIZE;
    return rc;
}

int vgxConfigReadUsmTypeDisplay(
    void *handle, uint32_t usm_id, uint32_t display_index, void *buf, uint32_t *size) {
    if (!valid_handle(handle)) {
        return VGX_CFG_ERR_BAD_HANDLE;
    }
    if (!size) {
        return VGX_CFG_ERR_NULL_PARAM;
    }
    int rc = VGX_CFG_OK;
    if (buf) {
        VgxConfigHandle *cfg = (VgxConfigHandle *)handle;
        XmlNode *u = find_usm_by_id(cfg->root, usm_id);
        if (!u) {
            *size = USM_DISPLAY_SIZE;
            return VGX_CFG_ERR_NOT_FOUND;
        }
        XmlNode *d = u;
        uint32_t cur = 0;
        while ((d = find_descendant_after(d, u, "display")) != NULL) {
            if (cur == display_index) break;
            cur++;
        }
        if (!d) {
            *size = USM_DISPLAY_SIZE;
            return VGX_CFG_ERR_OUT_OF_RANGE;
        }
        uint8_t tmp[USM_DISPLAY_SIZE];
        memset(tmp, 0, sizeof(tmp));
        put_u32_le(tmp, 0x00, 1);
        put_u32_le(tmp, 0x04, USM_DISPLAY_SIZE);

        uint32_t width = 0;
        if (read_u32_field(d, "width", &width) != 0) {
            *size = USM_DISPLAY_SIZE;
            return VGX_CFG_ERR_XML;
        }
        /* Match libnvidia-vgxcfg.so 0x103430 reading width for both +0x08 and +0x0c. */
        put_u32_le(tmp, 0x08, width);
        put_u32_le(tmp, 0x0c, width);
        rc = copy_sized(buf, tmp, USM_DISPLAY_SIZE, size);
    }
    *size = USM_DISPLAY_SIZE;
    return rc;
}
