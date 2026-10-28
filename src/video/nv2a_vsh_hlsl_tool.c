/*
 * nv2a_vsh_hlsl - translate one NV2A vertex program to HLSL, optionally
 * annotated for the Vulkan back end's ahead-of-time corpus.
 *
 * The corpus generator (the game's build, plan-jsrf-gameplay.md V5) extracts a
 * title's static programs and needs the translator's output as a file it can
 * hand to dxc. --vk applies the annotations the corpus pipeline needs and that
 * dxc cannot infer: an explicit [[vk::location(n)]] on every vertex input (dxc
 * otherwise packs ATTR1 and ATTR3 to locations 0 and 1, which is not the vertex
 * input layout the back end builds from the program's mask), a location on
 * every varying so the shader agrees with ps_rc's inputs
 * (shaders/nv2a_vk_rc.hlsl, struct VI), and the two cbuffers' bindings.
 *
 * Every --vk rewrite must find its pattern exactly once. If the translator's
 * output ever changes shape, this fails (exit 3) instead of emitting a shader
 * that binds the wrong registers or leaves an input at a packed location.
 * Without --vk the output is the translator's bytes, unchanged: that is the
 * mode the live-translator comparison uses.
 *
 * Portable C; links nv2a_vsh_hlsl.c and nothing else. No Vulkan, no Windows
 * libraries.
 *
 *   nv2a_vsh_hlsl --bin <file> --mask <hex> [--vk] [--out <file>]
 *
 *   --bin   len*16 bytes, little-endian, one NV2A instruction per 16 bytes,
 *           the last instruction FINAL.
 *   --mask  the program's input mask (0x... or decimal), as
 *           nv2a_vsh_program_info reports it for those words.
 *   --vk    annotate the output for the Vulkan pipeline (see above).
 *   --out   write there (create/truncate); stdout when absent.
 *
 * Exit codes: 0 success; 2 usage or I/O error (including a --bin that is not a
 * whole number of instructions); 3 translation or rewrite failure. Every
 * nonzero exit prints one line to stderr.
 */
#include "nv2a_vsh_hlsl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#define VSH_BUF_BYTES (256u * 1024u)    /* the translator's output starts here */
#define VSH_BUF_RETRY (4u * 1024u * 1024u)

/* A growable byte string; every rewrite goes through it. */
typedef struct {
    char *p;
    size_t n, cap;
} Buf;

static int buf_put(Buf *b, const char *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        char *q;

        while (cap < b->n + n + 1)
            cap *= 2;
        q = (char *)realloc(b->p, cap);
        if (!q)
            return 0;
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = '\0';
    return 1;
}

static size_t count_of(const char *hay, const char *needle)
{
    size_t n = 0, k = strlen(needle);
    const char *p = hay;

    if (!k)
        return 0;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += k;
    }
    return n;
}

/* Replace the single occurrence of `needle` in `in` with `repl`, appending the
 * result to `out`. Returns 0 when `needle` is not there exactly once (or on
 * allocation failure), which the caller reports as a rewrite failure. */
static int replace_once(const char *in, const char *needle, const char *repl, Buf *out)
{
    const char *at = strstr(in, needle);
    size_t k = strlen(needle);

    if (!at || count_of(in, needle) != 1)
        return 0;
    if (!buf_put(out, in, (size_t)(at - in)) || !buf_put(out, repl, strlen(repl))
        || !buf_put(out, at + k, strlen(at + k)))
        return 0;
    return 1;
}

/* The rewrites that do not depend on the mask. Each pattern is the translator's
 * own text (nv2a_vsh_hlsl.c k_preamble and NV2A_VSH_HLSL_VO_FIELDS), including
 * its spelling and spacing. */
static const struct {
    const char *from, *to;
} k_vk_rewrite[] = {
    { "cbuffer VshC : register(b1)",
      "[[vk::binding(9, 0)]] cbuffer VshC : register(b1)" },
    { "cbuffer VshX : register(b2)",
      "[[vk::binding(10, 0)]] cbuffer VshX : register(b2)" },
    /* The varyings, at ps_rc's input locations. */
    { "noperspective float4 d0 : COLOR0;",
      "[[vk::location(0)]] noperspective float4 d0 : COLOR0;" },
    { "noperspective float4 d1 : COLOR1;",
      "[[vk::location(1)]] noperspective float4 d1 : COLOR1;" },
    { "noperspective float fog : TEXCOORD4;",
      "[[vk::location(2)]] noperspective float fog : TEXCOORD4;" },
    { "float4 t0 : TEXCOORD0;", "[[vk::location(3)]] float4 t0 : TEXCOORD0;" },
    { "float4 t1 : TEXCOORD1;", "[[vk::location(4)]] float4 t1 : TEXCOORD1;" },
    { "float4 t2 : TEXCOORD2;", "[[vk::location(5)]] float4 t2 : TEXCOORD2;" },
    { "float4 t3 : TEXCOORD3;", "[[vk::location(6)]] float4 t3 : TEXCOORD3;" },
};

/* Annotate the translator's output. On failure `*why` names the missing
 * pattern; the caller prints it and exits 3. */
static int annotate(const char *in, uint32_t mask, Buf *out, const char **why)
{
    Buf cur, next;
    char pat[64], rep[96];
    size_t i;
    unsigned n;

    memset(&cur, 0, sizeof cur);
    memset(&next, 0, sizeof next);
    if (!buf_put(&cur, in, strlen(in))) {
        *why = "out of memory";
        return 0;
    }
    for (i = 0; i < sizeof k_vk_rewrite / sizeof k_vk_rewrite[0]; i++) {
        if (!replace_once(cur.p, k_vk_rewrite[i].from, k_vk_rewrite[i].to, &next)) {
            static char msg[128];
            snprintf(msg, sizeof msg, "expected %s exactly once, found %zu",
                     k_vk_rewrite[i].from, count_of(cur.p, k_vk_rewrite[i].from));
            *why = msg;
            free(cur.p);
            free(next.p);
            return 0;
        }
        free(cur.p);
        cur = next;
        memset(&next, 0, sizeof next);
    }
    /* One vertex input per set bit of the mask, ascending, as the translator
     * emits them (a leading space, inside struct VIP). */
    for (n = 0; n < 16; n++) {
        if (!((mask >> n) & 1u))
            continue;
        snprintf(pat, sizeof pat, " float4 v%u : ATTR%u;", n, n);
        snprintf(rep, sizeof rep, " [[vk::location(%u)]] float4 v%u : ATTR%u;", n, n, n);
        if (!replace_once(cur.p, pat, rep, &next)) {
            static char msg[128];
            snprintf(msg, sizeof msg, "expected %s exactly once, found %zu",
                     pat, count_of(cur.p, pat));
            *why = msg;
            free(cur.p);
            free(next.p);
            return 0;
        }
        free(cur.p);
        cur = next;
        memset(&next, 0, sizeof next);
    }
    *out = cur;
    return 1;
}

/* The input mask the instructions themselves imply, by the rule every consumer shares
 * (nv2a_vsh_ins_input_mask, nv2a_vsh_hlsl.h): the scan stops at the first FINAL. --mask
 * must agree with this, because it decides both the emitted vertex inputs and
 * the vertex input layout the back end bakes into the pipeline; a mask that
 * disagrees produces a shader no runtime batch can ever match. */
static uint32_t derived_mask(const uint32_t (*w)[4], uint32_t len)
{
    uint32_t mask = 0, n;

    for (n = 0; n < len; n++) {
        mask |= nv2a_vsh_ins_input_mask(w[n]);
        if ((w[n][3] >> 0) & 1u)
            break;
    }
    return mask;
}

static int usage(const char *why)
{
    fprintf(stderr, "nv2a_vsh_hlsl: %s; usage: nv2a_vsh_hlsl --bin <file> "
                    "--mask <hex> [--vk] [--out <file>]\n", why);
    return 2;
}

/* Read a program: len*16 little-endian bytes into len instructions. */
static int read_program(const char *path, uint32_t (**words)[4], uint32_t *len)
{
    FILE *f;
    long size;
    uint8_t *b;
    uint32_t (*w)[4], i, j;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "nv2a_vsh_hlsl: cannot open --bin %s\n", path);
        return 2;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "nv2a_vsh_hlsl: cannot size --bin %s\n", path);
        fclose(f);
        return 2;
    }
    if (size == 0 || size % 16 != 0) {
        fprintf(stderr, "nv2a_vsh_hlsl: --bin %s is %ld bytes, not a whole number of "
                        "16-byte instructions\n", path, size);
        fclose(f);
        return 2;
    }
    b = (uint8_t *)malloc((size_t)size);
    w = (uint32_t (*)[4])malloc((size_t)size);
    if (!b || !w) {
        fprintf(stderr, "nv2a_vsh_hlsl: out of memory reading --bin %s\n", path);
        free(b);
        free(w);
        fclose(f);
        return 2;
    }
    if (fread(b, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "nv2a_vsh_hlsl: cannot read --bin %s\n", path);
        free(b);
        free(w);
        fclose(f);
        return 2;
    }
    fclose(f);
    /* Decode explicitly, so the file's byte order does not depend on the host's. */
    for (i = 0; i < (uint32_t)size / 16; i++)
        for (j = 0; j < 4; j++) {
            const uint8_t *q = b + (size_t)i * 16 + j * 4;
            w[i][j] = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16
                    | (uint32_t)q[3] << 24;
        }
    free(b);
    *words = w;
    *len = (uint32_t)size / 16;
    return 0;
}

static int write_out(const char *path, const char *p, size_t n)
{
    FILE *f = path ? fopen(path, "wb") : stdout;

    if (!f) {
        fprintf(stderr, "nv2a_vsh_hlsl: cannot open --out %s\n", path);
        return 2;
    }
    if (fwrite(p, 1, n, f) != n) {
        fprintf(stderr, "nv2a_vsh_hlsl: cannot write %s\n", path ? path : "stdout");
        if (path)
            fclose(f);
        return 2;
    }
    if (path)
        fclose(f);
    else
        fflush(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *bin = NULL, *out = NULL, *why = NULL;
    uint32_t mask = 0, len = 0;
    uint32_t (*words)[4] = NULL;
    int have_mask = 0, vk = 0, i, rc;
    char *text;
    size_t cap, n;
    Buf ann;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--vk") == 0) {
            vk = 1;
        } else if (i + 1 >= argc) {
            return usage("a missing value");
        } else if (strcmp(a, "--bin") == 0) {
            bin = argv[++i];
        } else if (strcmp(a, "--out") == 0) {
            out = argv[++i];
        } else if (strcmp(a, "--mask") == 0) {
            const char *v = argv[++i];
            char *end;
            mask = (uint32_t)strtoul(v, &end, (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
                                              ? 16 : 10);
            if (end == v || *end != '\0')
                return usage("--mask is not a number");
            have_mask = 1;
        } else {
            return usage("unknown option");
        }
    }
    if (!bin)
        return usage("--bin is required");
    if (!have_mask)
        return usage("--mask is required");
    if ((rc = read_program(bin, &words, &len)) != 0)
        return rc;
    if (derived_mask(words, len) != (mask & 0xFFFFu)) {
        fprintf(stderr, "nv2a_vsh_hlsl: --mask %04X is not the mask these instructions "
                        "imply (%04X)\n", mask & 0xFFFFu, derived_mask(words, len));
        free(words);
        return 3;
    }

    cap = VSH_BUF_BYTES;
    text = (char *)malloc(cap);
    if (!text) {
        fprintf(stderr, "nv2a_vsh_hlsl: out of memory\n");
        free(words);
        return 2;
    }
    n = (size_t)nv2a_vsh_hlsl(words, len, mask, text, cap);
    if (!n) {
        /* Not necessarily too small: no FINAL in the words reads the same way.
         * One generous retry tells the two apart. */
        free(text);
        cap = VSH_BUF_RETRY;
        text = (char *)malloc(cap);
        if (!text) {
            fprintf(stderr, "nv2a_vsh_hlsl: out of memory\n");
            free(words);
            return 2;
        }
        n = (size_t)nv2a_vsh_hlsl(words, len, mask, text, cap);
    }
    free(words);
    if (!n) {
        fprintf(stderr, "nv2a_vsh_hlsl: nv2a_vsh_hlsl translated nothing for %u "
                        "instructions (no FINAL, or output over %u bytes)\n",
                len, (unsigned)cap);
        free(text);
        return 3;
    }

    if (vk) {
        memset(&ann, 0, sizeof ann);
        if (!annotate(text, mask, &ann, &why)) {
            fprintf(stderr, "nv2a_vsh_hlsl: --vk rewrite failed: %s\n", why);
            free(text);
            return 3;
        }
        free(text);
        text = ann.p;
        n = ann.n;
    }

#if defined(_WIN32)
    /* stdout must carry the translator's bytes, not CRLF-translated ones. */
    if (!out)
        _setmode(_fileno(stdout), _O_BINARY);
#endif
    rc = write_out(out, text, n);
    free(text);
    return rc;
}
