// The Maxwell shader translator (runtime/nvn/maxwell/translate.c) over the game's own shaders: every vertex, fragment
// and compute stage of the BNSH containers in the RomFS's shader archives (*.bfsha), effects (*.ptcl) and UI archives
// (*.arc) - the files runtime/nvn/shader_prep.c indexes - translated to GLSL with plain keys (float attributes and
// targets, every varying) and written to <out>/<code hash>.<vert|frag|comp> for tools/test_translate_corpus.sh to
// compile and validate. Each stage once (by its code and constants).
// usage: test_translate_corpus <extracted/romfs folder> <out folder>
// Switches from the environment: TT_GMEM_MIRROR=1 (no heap import: the copies' table, exec.c gmir_), TT_GMEM_SSBO=1
// (storage slots as their own buffers, shader.h maxwell_gmem_ssbo), TT_STRUCTURED=1 (static stacks + structured
// branches: the Mali / Adreno default), TT_PUSH=1 (c1 inlined + push constants), plus the translator's LARECOMP_VK_*.
// Prints per stage: shaders, translation failures, those reading storage slots through their own bindings, and those
// of them left with no heap.
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../runtime/nvn/maxwell/shader.h"

static uint32_t rd32(const uint8_t *d, size_t o) { uint32_t v; memcpy(&v, d + o, 4); return v; }
static uint64_t rd64(const uint8_t *d, size_t o) { uint64_t v; memcpy(&v, d + o, 8); return v; }
static uint64_t fnv(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

static uint64_t *seen;
static const size_t seen_cap = 1u << 20;
static bool seen_add(uint64_t h) {
    if (!seen) seen = calloc(seen_cap, 8);
    h |= 1;
    for (size_t j = h & (seen_cap - 1);; j = (j + 1) & (seen_cap - 1)) {
        if (seen[j] == h) return false;
        if (!seen[j]) { seen[j] = h; return true; }
    }
}

static const char *out_dir;
static int n[3], failed[3], gss[3], gss_only[3];
static const char *const ext[3] = {"vert", "frag", "comp"};

static void one(const uint8_t *code, uint32_t len, uint32_t coff, int k) {
    uint64_t h = fnv(code, len) ^ (uint64_t)k;
    if (!seen_add(h)) return;
    if ((k == 2) == (len >= 4 && rd32(code, 0) == 0x12345678)) return;  // (graphics stages carry NVN's header)
    static const int stages[3] = {STAGE_VERTEX, STAGE_FRAGMENT, STAGE_COMPUTE};
    ShaderKey key;
    memset(&key, 0, sizeof key);
    key.glsl = 1;
    if (k == 1) key.vs_out_mask = 0xFFFFFFFFu;
    uint8_t *buf = calloc(1, (size_t)len + 0x1000);  // (zero padding past the constants: jump tables read up to 1 KB)
    memcpy(buf, code, len);
    maxwell_const_data = k != 2 && coff < len ? buf + coff : NULL;  // (compute: none, as exec.c)
    maxwell_const_size = k != 2 && coff < len ? len - coff : 0;
    ShaderInfo *si = maxwell_translate(buf, coff, stages[k], &key);
    n[k]++;
    if (!si->msl) {
        if (failed[k]++ < 5) fprintf(stderr, "%s %016llx: %s\n", ext[k], (unsigned long long)h, si->error);
    } else {
        if (si->gss_mask) { gss[k]++; if (!si->uses_gmem) gss_only[k]++; }
        char path[1024];
        snprintf(path, sizeof path, "%s/%016llx.%s", out_dir, (unsigned long long)h, ext[k]);
        FILE *f = fopen(path, "w");
        if (!f) { perror(path); exit(1); }
        fputs(si->msl, f);
        fclose(f);
    }
    maxwell_free(si);
    free(buf);
}

// A BNSH container (nn::gfx shader file) at d[b..]: its programs' vertex, fragment and compute codes (the walk of
// runtime/nvn/shader_prep.c idx_bnsh)
static void bnsh(const uint8_t *d, size_t sz, size_t b) {
    if (b + 0x20 > sz || memcmp(d + b + 0xC, "\xff\xfe", 2)) return;
    size_t fsz = rd32(d, b + 0x1C);
    if (fsz < 0x40 || b + fsz > sz) return;
    size_t first = (size_t)d[b + 0x16] | (size_t)d[b + 0x17] << 8;
    if (first + 0x28 > fsz || memcmp(d + b + first, "grsc", 4)) return;
    uint32_t nvar = rd32(d, b + first + 0x1C);
    uint64_t pvar = rd64(d, b + first + 0x20);
    if (pvar >= fsz || nvar > (fsz - pvar) / 0x40) return;
    for (uint32_t v = 0; v < nvar; v++) {
        uint64_t pbin = rd64(d, b + pvar + (uint64_t)v * 0x40 + 0x10);
        if (!pbin || pbin + 0x38 > fsz) continue;
        for (int s = 0; s < 6; s++) {
            if (s != 0 && s != 4 && s != 5) continue;  // (vertex, fragment, compute)
            uint64_t pc = rd64(d, b + pbin + 8 + 8 * (uint64_t)s);
            if (!pc || pc + 0x20 > fsz) continue;
            uint64_t pctl = rd64(d, b + pc + 8), pcode = rd64(d, b + pc + 0x10);
            uint32_t csz = rd32(d, b + pc + 0x18);
            if (pctl + 0x750 > fsz || pcode >= fsz || csz > fsz - pcode) continue;
            uint32_t coff = rd32(d, b + pctl + 0x700), cs = rd32(d, b + pctl + 0x6FC);
            if (!coff || coff > csz || cs > csz - coff) continue;
            one(d + b + pcode, coff + cs, coff, s == 0 ? 0 : s == 4 ? 1 : 2);
        }
    }
}

static void file(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = sz > 0 && sz < 0x7FFFFFFF ? malloc((size_t)sz) : NULL;
    if (d && fread(d, 1, (size_t)sz, f) == (size_t)sz)
        for (size_t i = 0; i + 8 <= (size_t)sz; i += 8)  // (containers are aligned)
            if (!memcmp(d + i, "BNSH\0\0\0\0", 8)) bnsh(d, (size_t)sz, i);
    free(d);
    fclose(f);
}

static int cmp_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void dir(const char *p, int depth) {  // (sorted: the same order every run)
    DIR *dd = depth < 8 ? opendir(p) : NULL;
    if (!dd) return;
    char **names = NULL;
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        names = realloc(names, sizeof *names * (size_t)(nn + 1));
        names[nn++] = strdup(e->d_name);
    }
    closedir(dd);
    qsort(names, (size_t)nn, sizeof *names, cmp_name);
    for (int i = 0; i < nn; i++) {
        char q[1024];
        snprintf(q, sizeof q, "%s/%s", p, names[i]);
        size_t l = strlen(names[i]);
        const char *x = l > 5 ? names[i] + l - 5 : "";
        struct stat sb;
        if (!strcmp(x, "bfsha") || !strcmp(x, ".ptcl") || (l > 4 && !strcmp(names[i] + l - 4, ".arc"))) file(q);
        else if (stat(q, &sb) == 0 && S_ISDIR(sb.st_mode)) dir(q, depth + 1);
        free(names[i]);
    }
    free(names);
}

static bool env_on(const char *name) { return getenv(name) && atoi(getenv(name)) != 0; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <romfs folder> <out folder>\n", argv[0]); return 2; }
    out_dir = argv[2];
    maxwell_gmem_base = 0x1000000000ull;  // (any heap: global memory addresses are relative to it)
    maxwell_gmem_size = 0xC0000000ull;
    maxwell_gmem_mirror = env_on("TT_GMEM_MIRROR");
    maxwell_gmem_ssbo = env_on("TT_GMEM_SSBO");
    maxwell_glsl_structured = env_on("TT_STRUCTURED");
    maxwell_glsl_push = env_on("TT_PUSH");
    maxwell_glsl_stack_vars = false;  // (arrays: the runtime's default off Intel)
    maxwell_push_cap = 64;            // (a 256-byte push range: PanVK, Turnip)
    dir(argv[1], 0);
    for (int k = 0; k < 3; k++)
        printf("%s: %d shaders, %d failed to translate, %d read storage slots as their own buffers, %d of them with no heap\n",
               ext[k], n[k], failed[k], gss[k], gss_only[k]);
    return 0;
}
