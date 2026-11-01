/*
 * nv2a_present_chain.c - see nv2a_present_chain.h.
 */
#include "nv2a_present_chain.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "librashader.h"
#include "platform/recomp_exe_dir.h"

#define LOGP "[SHADER] "

const char *const nv2a_present_chain_names[] = { NV2A_PRESENT_CHAIN_NAMES, NULL };

/* The request string is short and changes only from the config at start
 * and the menu; a spin lock keeps a reader from copying half a write. */
static atomic_flag s_lock = ATOMIC_FLAG_INIT;
static char s_req[512] = "none";
static char s_lib_path[512];
static atomic_uint s_gen;
static atomic_int s_wanted;
static atomic_int s_lines_mode;     /* present.shader_lines, NV2A_PRESENT_LINES_* */
/* A profile request: its slug ("" for a present.shader request) and the
 * stages' parameter names publish tags rows by. Under s_lock. */
static char s_req_slug[64];
static struct nv2a_present_stage s_stages[NV2A_PRESENT_STAGES_MAX];
static int s_nstages;
static char s_profile_err[NV2A_PRESENT_PROFILE_ERR_MAX];
/* A start request that composes later (request_pending): run once, by the
 * first nv2a_present_chain_gen() after it. s_pending is the cheap test the
 * backends' per-present gen() read pays; s_prepare is under s_lock. */
static atomic_int s_pending;
static void (*s_prepare)(void);

static struct nv2a_lrs s_lrs;
static int s_lib_state;             /* 0 not tried, 1 loaded, -1 failed */
static char s_lib_why[640];         /* why it failed, for the menu */
/* The load runs once: the present thread (a chain to build) and the menu
 * (is the Shaders page shown) may both ask first. Its own flag, since the
 * request lock is held only for copies. */
static atomic_flag s_load_lock = ATOMIC_FLAG_INIT;
static char s_host[16];             /* the chain presenter, "" none */
#ifdef _WIN32
static HMODULE s_mod;
#else
static void *s_mod;
#endif

static void lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire))
        ;
}

static void unlock(void)
{
    atomic_flag_clear_explicit(&s_lock, memory_order_release);
}

static int is_none(const char *s)
{
    return !s || !*s || !strcmp(s, "none");
}

static int path_is_absolute(const char *p)
{
    return p[0] == '/' || p[0] == '\\'
        || (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':');
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

void nv2a_present_chain_request(const char *shader)
{
    lock();
    snprintf(s_req, sizeof s_req, "%s", is_none(shader) ? "none" : shader);
    s_req_slug[0] = 0;
    s_nstages = 0;
    s_profile_err[0] = 0;
    s_prepare = NULL;
    atomic_store(&s_pending, 0);
    atomic_store(&s_wanted, !is_none(shader));
    atomic_fetch_add(&s_gen, 1);
    unlock();
}

/* The profile request under the lock; with `at`, only while the generation
 * is still *at (no request came since): 1 stored, 0 dropped. */
static int store_profile(const unsigned *at, const char *slug, const char *path,
                         const struct nv2a_present_stage *stages, int nstages)
{
    int i;
    if (nstages > NV2A_PRESENT_STAGES_MAX) {
        fprintf(stderr, LOGP "profile %s: %d stages, the first %d kept\n", slug, nstages,
                NV2A_PRESENT_STAGES_MAX);
        nstages = NV2A_PRESENT_STAGES_MAX;
    }
    lock();
    if (at && atomic_load(&s_gen) != *at) {
        unlock();
        return 0;
    }
    snprintf(s_req, sizeof s_req, "%s", path);
    snprintf(s_req_slug, sizeof s_req_slug, "%s", slug);
    s_nstages = 0;
    for (i = 0; stages && i < nstages; i++) {
        s_stages[i] = stages[i];
        if (s_stages[i].nnames > NV2A_PRESENT_PARAMS_MAX)
            s_stages[i].nnames = NV2A_PRESENT_PARAMS_MAX;
        if (s_stages[i].nnames < 0)
            s_stages[i].nnames = 0;
        s_nstages++;
    }
    s_profile_err[0] = 0;
    s_prepare = NULL;
    atomic_store(&s_pending, 0);
    atomic_store(&s_wanted, 1);
    atomic_fetch_add(&s_gen, 1);
    unlock();
    return 1;
}

void nv2a_present_chain_request_profile(const char *slug, const char *path,
                                        const struct nv2a_present_stage *stages, int nstages)
{
    if (is_none(path) || !slug || !*slug) {
        nv2a_present_chain_request("none");
        return;
    }
    store_profile(NULL, slug, path, stages, nstages);
}

unsigned nv2a_present_chain_request_gen(void)
{
    return atomic_load(&s_gen);
}

int nv2a_present_chain_request_profile_at(unsigned gen, const char *slug, const char *path,
                                          const struct nv2a_present_stage *stages, int nstages,
                                          const char *err)
{
    if (!slug || !*slug)
        return 0;
    if (!is_none(path))
        return store_profile(&gen, slug, path, stages, nstages);
    /* Stock with the error, as a failed request_profile leaves it: one
     * lock, so a menu request cannot land between the two. */
    lock();
    if (atomic_load(&s_gen) != gen) {
        unlock();
        return 0;
    }
    snprintf(s_req, sizeof s_req, "none");
    s_req_slug[0] = 0;
    s_nstages = 0;
    snprintf(s_profile_err, sizeof s_profile_err, "%s", err ? err : "");
    s_prepare = NULL;
    atomic_store(&s_pending, 0);
    atomic_store(&s_wanted, 0);
    atomic_fetch_add(&s_gen, 1);
    unlock();
    return 1;
}

void nv2a_present_chain_request_pending(const char *slug, void (*prepare)(void))
{
    if (!prepare || !slug || !*slug) {
        nv2a_present_chain_request("none");
        return;
    }
    lock();
    /* No path yet: a preset read now finds none and builds nothing. */
    snprintf(s_req, sizeof s_req, "none");
    snprintf(s_req_slug, sizeof s_req_slug, "%s", slug);
    s_nstages = 0;
    s_profile_err[0] = 0;
    s_prepare = prepare;
    atomic_store(&s_pending, 1);
    atomic_store(&s_wanted, 1);
    atomic_fetch_add(&s_gen, 1);
    unlock();
}

int nv2a_present_chain_request_rearm(unsigned gen, void (*prepare)(void))
{
    int ok;
    lock();
    /* Only the pending request's own empty "none": rearming a real
     * profile request would set s_pending, and failed() would then ignore
     * that generation's genuine build failure. */
    ok = prepare && atomic_load(&s_gen) == gen && s_req_slug[0] && is_none(s_req);
    if (ok) {
        s_prepare = prepare;
        atomic_store(&s_pending, 1);
    }
    unlock();
    return ok;
}

/* Take the pending prepare and run it, outside the lock: it composes and
 * makes the real request, which takes the lock itself. */
static void run_pending(void)
{
    void (*fn)(void);
    lock();
    fn = s_prepare;
    s_prepare = NULL;
    atomic_store(&s_pending, 0);
    unlock();
    if (fn)
        fn();
}

int nv2a_present_chain_requested_profile(char *out, size_t cap)
{
    int on;
    lock();
    on = s_req_slug[0] != 0;
    if (cap)
        snprintf(out, cap, "%s", s_req_slug);
    unlock();
    return on;
}

void nv2a_present_chain_profile_error_set(const char *msg)
{
    lock();
    snprintf(s_profile_err, sizeof s_profile_err, "%s", msg ? msg : "");
    unlock();
}

int nv2a_present_chain_profile_error_get(char *out, size_t cap)
{
    int on;
    lock();
    on = s_profile_err[0] != 0;
    if (cap)
        snprintf(out, cap, "%s", s_profile_err);
    unlock();
    return on;
}

void nv2a_present_chain_set_lib(const char *path)
{
    lock();
    snprintf(s_lib_path, sizeof s_lib_path, "%s", path ? path : "");
    unlock();
}

void nv2a_present_chain_requested(char *out, size_t cap)
{
    lock();
    snprintf(out, cap, "%s", s_req);
    unlock();
}

unsigned nv2a_present_chain_gen(void)
{
    if (atomic_load_explicit(&s_pending, memory_order_relaxed))
        run_pending();
    return atomic_load(&s_gen);
}

int nv2a_present_chain_wanted(void)
{
    return atomic_load_explicit(&s_wanted, memory_order_relaxed);
}

float nv2a_present_chain_rate(uint64_t now_ns)
{
    static uint64_t last;
    static double avg_ns;           /* 0: no interval yet */
    double d;

    if (!last || now_ns <= last || now_ns - last > 1000000000ull) {
        last = now_ns;
        avg_ns = 0;
        return 60.0f;
    }
    d = (double)(now_ns - last);
    last = now_ns;
    /* An eighth per frame: steady within a few frames, deaf to one late
     * present. */
    avg_ns = avg_ns > 0 ? avg_ns + (d - avg_ns) / 8.0 : d;
    return (float)(1e9 / avg_ns);
}

void nv2a_present_chain_failed(unsigned gen)
{
    lock();
    /* Only the generation that failed: a request that arrived meanwhile
     * gets its own try. A start still waiting to compose (rearmed) has no
     * preset to build yet, which is not a failure: the backend keeps
     * asking while it is wanted, and that read is what runs the wait. */
    if (gen == atomic_load(&s_gen) && !atomic_load(&s_pending)) {
        atomic_store(&s_wanted, 0);
        /* The backend said why in the log; librashader seldom names the
         * pass, so the menu's line points there and at the composed file. */
        if (s_req_slug[0]) {
            const char *f = s_req, *c;
            for (c = s_req; *c; c++)
                if (*c == '/' || *c == '\\')
                    f = c + 1;
            /* The file's name: the log has the full path, and the menu's
             * row has no room for it. */
            snprintf(s_profile_err, sizeof s_profile_err,
                     "the chain did not build: see the log (%s)", f);
        }
    }
    unlock();
}

static void *lrs_sym(const char *name)
{
    if (!s_mod)
        return NULL;
#ifdef _WIN32
    return (void *)GetProcAddress(s_mod, name);
#else
    return dlsym(s_mod, name);
#endif
}

/* Default locations: beside the executable, and in a macOS app bundle's
 * Frameworks directory. */
static int lib_candidates(char paths[][1024], int max)
{
    const char *dir = recomp_exe_dir();
    int n = 0;
    (void)max;                          /* one candidate off macOS */
#if defined(_WIN32)
    snprintf(paths[n++], 1024, "%s/librashader.dll", dir);
#elif defined(__APPLE__)
    snprintf(paths[n++], 1024, "%s/librashader.dylib", dir);
    if (n < max)
        snprintf(paths[n++], 1024, "%s/../Frameworks/librashader.dylib", dir);
#else
    snprintf(paths[n++], 1024, "%s/librashader.so", dir);
#endif
    return n;
}

#ifdef _WIN32
/* The first dll that <w>'s import table names and the loader cannot find,
 * into <name>, or 0. Error 126 alone does not say which import is missing;
 * the hint names the package that brings it. Each import is looked for
 * beside <w> first, as LOAD_WITH_ALTERED_SEARCH_PATH does when open_lib
 * loads it, then through the standard search. Only direct imports: a
 * missing dll further down the chain leaves the plain error. */
static int missing_import(const wchar_t *w, char *name, size_t cap)
{
    HMODULE img = LoadLibraryExW(w, NULL, DONT_RESOLVE_DLL_REFERENCES);
    const unsigned char *base = (const unsigned char *)img;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_DATA_DIRECTORY *dir;
    const IMAGE_IMPORT_DESCRIPTOR *imp;
    wchar_t beside[1024];
    size_t dirlen = 0, i;
    DWORD size;
    int found = 0;

    if (!img)
        return 0;
    for (i = 0; w[i]; i++)
        if (w[i] == L'\\' || w[i] == L'/')
            dirlen = i + 1;
    /* A file that is not a PE32+ image (the loader maps what it can) has
     * no import table to trust: leave the plain error. */
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        goto done;
    nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        goto done;
    size = nt->OptionalHeader.SizeOfImage;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
        goto done;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress || dir->VirtualAddress >= size)
        goto done;
    for (imp = (const IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);
         (const unsigned char *)(imp + 1) <= base + size && imp->Name && !found; imp++) {
        const char *dll;
        HMODULE h;
        if (imp->Name >= size || !memchr(base + imp->Name, 0, size - imp->Name))
            break;
        dll = (const char *)(base + imp->Name);
        if (dirlen && dirlen + strlen(dll) < 1024) {
            memcpy(beside, w, dirlen * sizeof *w);
            if (MultiByteToWideChar(CP_ACP, 0, dll, -1, beside + dirlen,
                                    (int)(1024 - dirlen)) &&
                GetFileAttributesW(beside) != INVALID_FILE_ATTRIBUTES)
                continue;
        }
        h = LoadLibraryA(dll);
        if (h) {
            FreeLibrary(h);
        } else {
            snprintf(name, cap, "%s", dll);
            found = 1;
        }
    }
done:
    FreeLibrary(img);
    return found;
}

static const char *import_hint(const char *dll)
{
    if (!_strnicmp(dll, "d3dx9_", 6))
        return ": it comes with the DirectX End-User Runtime,"
               " https://www.microsoft.com/download/details.aspx?id=35";
    if (!_strnicmp(dll, "msvcp140", 8) || !_strnicmp(dll, "vcruntime140", 12))
        return ": it comes with the Microsoft Visual C++ Redistributable (x64),"
               " https://aka.ms/vs/17/release/vc_redist.x64.exe";
    return "";
}
#endif

static int open_lib(const char *path, char *why, size_t cap)
{
#ifdef _WIN32
    wchar_t w[1024];
    char dll[128];
    DWORD err;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) {
        snprintf(why, cap, "bad path");
        return 0;
    }
    /* The dll's own directory first for its imports (d3dcompiler_47 can
     * sit beside it). */
    s_mod = LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (s_mod)
        return 1;
    err = GetLastError();
    if (!file_exists(path)) {
        snprintf(why, cap, "not found");
    } else if (missing_import(w, dll, sizeof dll)) {
        /* The toolkit's own x86-64 build imports only system dlls; an
         * upstream or user-supplied build may need D3DX9 or the VC++
         * runtime, which a stock Windows lacks. */
        snprintf(why, cap, "needs %s, which is not installed%s (LoadLibrary error %lu)",
                 dll, import_hint(dll), (unsigned long)err);
    } else if (err == ERROR_MOD_NOT_FOUND) {
        snprintf(why, cap, "needs a DLL that is missing (LoadLibrary error %lu)",
                 (unsigned long)err);
    } else {
        snprintf(why, cap, "failed to load (LoadLibrary error %lu)", (unsigned long)err);
    }
    return 0;
#else
    const char *e;
    s_mod = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (s_mod)
        return 1;
    e = dlerror();
    if (!file_exists(path))
        snprintf(why, cap, "not found");
    else
        snprintf(why, cap, "failed to load: %s", e ? e : "dlopen failed");
    return 0;
#endif
}

static void close_lib(void)
{
    if (!s_mod)
        return;
#ifdef _WIN32
    FreeLibrary(s_mod);
#else
    dlclose(s_mod);
#endif
    s_mod = NULL;
}

static const struct nv2a_lrs *lib_load(void);

const struct nv2a_lrs *nv2a_present_chain_lib(void)
{
    const struct nv2a_lrs *l;
    while (atomic_flag_test_and_set_explicit(&s_load_lock, memory_order_acquire))
        ;
    l = s_lib_state ? (s_lib_state > 0 ? &s_lrs : NULL) : lib_load();
    atomic_flag_clear_explicit(&s_load_lock, memory_order_release);
    return l;
}

void nv2a_present_chain_host(const char *name)
{
    lock();
    snprintf(s_host, sizeof s_host, "%s", name && *name ? name : "?");
    unlock();
}

int nv2a_present_chain_supported(char *why, size_t cap)
{
    char host[sizeof s_host];
    lock();
    memcpy(host, s_host, sizeof host);
    unlock();
    if (!host[0]) {
        snprintf(why, cap, "this presenter runs no shader chain (Metal and D3D11 do)");
        return 0;
    }
    if (!nv2a_present_chain_lib()) {
        snprintf(why, cap, "%s", s_lib_why);
        return 0;
    }
    if (cap)
        why[0] = 0;
    return 1;
}

void nv2a_present_chain_lib_reset_for_test(void)
{
    close_lib();
    s_lib_state = 0;
    s_lib_why[0] = 0;
    memset(&s_lrs, 0, sizeof s_lrs);
    lock();
    s_host[0] = 0;
    unlock();
}

static const struct nv2a_lrs *lib_load(void)
{
    char paths[2][1024], why[512] = "", over[512];
    PFN_libra_instance_abi_version abi;
    PFN_libra_instance_api_version api;
    int n, i;

    s_lib_state = -1;
    lock();
    snprintf(over, sizeof over, "%s", s_lib_path);
    unlock();
    if (over[0]) {
        if (path_is_absolute(over))
            snprintf(paths[0], sizeof paths[0], "%s", over);
        else
            snprintf(paths[0], sizeof paths[0], "%s/%s", recomp_exe_dir(), over);
        n = 1;
    } else {
        n = lib_candidates(paths, 2);
    }
    for (i = 0; i < n; i++)
        if (open_lib(paths[i], why, sizeof why))
            break;
    if (i == n) {
        fprintf(stderr, LOGP "present.shader off: librashader %s (%s)\n", why, paths[n - 1]);
        snprintf(s_lib_why, sizeof s_lib_why, "librashader %s", why);
        return NULL;
    }
    abi = (PFN_libra_instance_abi_version)lrs_sym("libra_instance_abi_version");
    api = (PFN_libra_instance_api_version)lrs_sym("libra_instance_api_version");
    if (!abi || !api) {
        fprintf(stderr, LOGP "present.shader off: %s has no librashader version"
                " exports\n", paths[i]);
        snprintf(s_lib_why, sizeof s_lib_why, "librashader has no version exports");
        close_lib();
        return NULL;
    }
    s_lrs.abi = (unsigned)abi();
    s_lrs.api = (unsigned)api();
    /* ABI changes break the structs we pass; the API only grows, so an
     * older one lacks calls or fields we use. */
    if (s_lrs.abi != LIBRASHADER_CURRENT_ABI || s_lrs.api < LIBRASHADER_CURRENT_VERSION) {
        fprintf(stderr, LOGP "present.shader off: %s is ABI %u API %u; this build needs"
                " ABI %d API %d or later (librashader 0.12)\n", paths[i], s_lrs.abi,
                s_lrs.api, LIBRASHADER_CURRENT_ABI, LIBRASHADER_CURRENT_VERSION);
        snprintf(s_lib_why, sizeof s_lib_why, "librashader is ABI %u API %u, not 0.12",
                 s_lrs.abi, s_lrs.api);
        close_lib();
        return NULL;
    }
    s_lrs.sym = lrs_sym;
    s_lib_state = 1;
    fprintf(stderr, LOGP "librashader ABI %u API %u: %s\n", s_lrs.abi, s_lrs.api, paths[i]);
    return &s_lrs;
}

static int has_path_chars(const char *s)
{
    size_t n = strlen(s);
    return strchr(s, '/') || strchr(s, '\\') || (n > 7 && !strcmp(s + n - 7, ".slangp"));
}

/* The preset file for request req into out: 1 found, -1 not (said when
 * say is set, with what was tried). */
static int preset_path(const char *req, char *out, size_t cap, int say)
{
    const char *dir = recomp_exe_dir();

    if (has_path_chars(req)) {
        if (path_is_absolute(req))
            snprintf(out, cap, "%s", req);
        else
            snprintf(out, cap, "%s/%s", dir, req);
        if (file_exists(out))
            return 1;
        if (say)
            fprintf(stderr, LOGP "present.shader=%s: no such file (%s)\n", req, out);
        return -1;
    }
    snprintf(out, cap, "%s/shaders/%s.slangp", dir, req);
    if (file_exists(out))
        return 1;
#ifdef __APPLE__
    snprintf(out, cap, "%s/../Resources/shaders/%s.slangp", dir, req);
    if (file_exists(out))
        return 1;
#endif
    if (say)
        fprintf(stderr, LOGP "present.shader=%s: no shaders/%s.slangp beside the executable"
                " (%s)\n", req, req, dir);
    return -1;
}

static int lines_locked(void);

int nv2a_present_chain_preset_lines(char *out, size_t cap, unsigned *gen_out, int *lines_out)
{
    char req[512];
    unsigned gen;
    int on;

    lock();
    snprintf(req, sizeof req, "%s", s_req);
    gen = atomic_load(&s_gen);
    on = lines_locked();
    unlock();
    if (gen_out)
        *gen_out = gen;
    if (lines_out)
        *lines_out = on;
    if (is_none(req))
        return 0;
    return preset_path(req, out, cap, 1);
}

int nv2a_present_chain_preset(char *out, size_t cap, unsigned *gen_out)
{
    return nv2a_present_chain_preset_lines(out, cap, gen_out, NULL);
}

/* ---- Which pass file declares a parameter ----
 *
 * librashader lists a preset's parameters with no file, but the menu keys
 * its parameter kinds on the shader that declares each one (two presets
 * can both name a MASK). So the preset is read here as far as that needs:
 * shaderN through #reference (each path relative to the file that set it),
 * then each pass's source and its #include files for #pragma parameter.
 * Every file is read at most once per call: guarded headers that include
 * each other would otherwise be read again at every level. */
#define DECL_PASSES_MAX 64
#define DECL_DEPTH_MAX 8
#define DECL_SEEN_MAX 256
#define DECL_PATH 1024
#define DECL_TEXT_MAX (4 << 20)

/* One call's state, on the heap: the pass table and the files read are
 * too big for a present thread's stack, and a call shares nothing, so the
 * menu's describe and a backend's publish never wait on each other. */
struct decl_ctx {
    char pass[DECL_PASSES_MAX][DECL_PATH];
    int npass;
    char seen[DECL_SEEN_MAX][DECL_PATH];
    int nseen;
};

/* The scratch paths of one level of the walk. */
struct decl_paths {
    char dir[DECL_PATH], rel[DECL_PATH], full[DECL_PATH];
};

/* 1 the first time path comes up in this call (and remembered), 0 after,
 * or when the list is full. */
static int decl_first(struct decl_ctx *cx, const char *path)
{
    for (int i = 0; i < cx->nseen; i++)
        if (!strcmp(cx->seen[i], path))
            return 0;
    if (cx->nseen >= DECL_SEEN_MAX)
        return 0;
    snprintf(cx->seen[cx->nseen++], DECL_PATH, "%s", path);
    return 1;
}

static char *decl_read(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long n;
    if (!f)
        return NULL;
    if (!fseek(f, 0, SEEK_END) && (n = ftell(f)) >= 0 && n <= DECL_TEXT_MAX
        && !fseek(f, 0, SEEK_SET) && (buf = malloc((size_t)n + 1))) {
        if (fread(buf, 1, (size_t)n, f) == (size_t)n) {
            buf[n] = 0;
        } else {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    return buf;
}

static void decl_dir(const char *path, char *out, size_t cap)
{
    const char *s, *cut = NULL;
    for (s = path; *s; s++)
        if (*s == '/' || *s == '\\')
            cut = s;
    if (cut)
        snprintf(out, cap, "%.*s", (int)(cut - path), path);
    else
        snprintf(out, cap, ".");
}

/* rel against dir. A Windows preset's "inc\x.h" names the same file on a
 * POSIX host, where a backslash is no separator. */
static void decl_join(const char *dir, const char *rel, char *out, size_t cap)
{
    if (path_is_absolute(rel))
        snprintf(out, cap, "%s", rel);
    else
        snprintf(out, cap, "%s/%s", dir, rel);
#ifndef _WIN32
    for (char *c = out; *c; c++)
        if (*c == '\\')
            *c = '/';
#endif
}

static int decl_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* A line's value after '=' or a directive: "quoted" whole, else to a
 * comment, trimmed. */
static void decl_value(const char *v, const char *end, char *out, size_t cap)
{
    const char *e;
    while (v < end && decl_space(*v))
        v++;
    if (v < end && *v == '"') {
        const char *q = memchr(v + 1, '"', (size_t)(end - v - 1));
        e = q ? q : end;
        v++;
    } else {
        for (e = v; e < end && *e != '#' && !(e[0] == '/' && e + 1 < end && e[1] == '/'); e++)
            ;
        while (e > v && decl_space(e[-1]))
            e--;
    }
    snprintf(out, cap, "%.*s", (int)(e - v), v);
}

/* shaderN = path of path's preset and its #reference files, into the pass
 * table (absolute or relative to the working directory). The referenced
 * file first, so the preset's own lines override it, as librashader reads
 * it. */
static void decl_preset(struct decl_ctx *cx, const char *path, int depth)
{
    struct decl_paths *ps;
    char *text;
    const char *l;
    int round;

    if (depth > DECL_DEPTH_MAX || !decl_first(cx, path))
        return;
    if (!(ps = malloc(sizeof *ps)))
        return;
    if (!(text = decl_read(path))) {
        free(ps);
        return;
    }
    decl_dir(path, ps->dir, sizeof ps->dir);
    for (round = 0; round < 2; round++) {
        for (l = text; *l;) {
            const char *e = strchr(l, '\n'), *b = l;
            if (!e)
                e = l + strlen(l);
            while (b < e && decl_space(*b))
                b++;
            if (round == 0 && !strncmp(b, "#reference", 10)) {
                decl_value(b + 10, e, ps->rel, sizeof ps->rel);
                decl_join(ps->dir, ps->rel, ps->full, sizeof ps->full);
                decl_preset(cx, ps->full, depth + 1);
            } else if (round == 1 && !strncmp(b, "shader", 6) && b[6] >= '0' && b[6] <= '9') {
                const char *d = b + 6;
                int idx = 0;
                while (d < e && *d >= '0' && *d <= '9' && idx < 1000)
                    idx = idx * 10 + (*d++ - '0');
                while (d < e && decl_space(*d))
                    d++;
                if (d < e && *d == '=' && idx < DECL_PASSES_MAX) {
                    decl_value(d + 1, e, ps->rel, sizeof ps->rel);
                    if (ps->rel[0]) {
                        decl_join(ps->dir, ps->rel, cx->pass[idx], DECL_PATH);
                        if (idx + 1 > cx->npass)
                            cx->npass = idx + 1;
                    }
                }
            }
            l = *e ? e + 1 : e;
        }
    }
    free(text);
    free(ps);
}

/* The rows path's source (and its #include files) declares get file. */
static void decl_source(struct decl_ctx *cx, const char *path, const char *file, int depth,
                        struct nv2a_present_param *t, int n)
{
    struct decl_paths *ps;
    char *text;
    const char *l;

    if (depth > DECL_DEPTH_MAX || !decl_first(cx, path))
        return;
    if (!(ps = malloc(sizeof *ps)))
        return;
    if (!(text = decl_read(path))) {
        free(ps);
        return;
    }
    decl_dir(path, ps->dir, sizeof ps->dir);
    for (l = text; *l;) {
        const char *e = strchr(l, '\n'), *b = l;
        if (!e)
            e = l + strlen(l);
        while (b < e && decl_space(*b))
            b++;
        if (!strncmp(b, "#pragma", 7) && decl_space(b[7])) {
            const char *w = b + 7, *nb;
            while (w < e && decl_space(*w))
                w++;
            if (!strncmp(w, "parameter", 9) && decl_space(w[9])) {
                w += 9;
                while (w < e && decl_space(*w))
                    w++;
                nb = w;
                while (w < e && !decl_space(*w))
                    w++;
                for (int i = 0; i < n; i++)
                    if (!t[i].file[0] && strlen(t[i].name) == (size_t)(w - nb)
                        && !memcmp(t[i].name, nb, (size_t)(w - nb)))
                        snprintf(t[i].file, sizeof t[i].file, "%s", file);
            }
        } else if (!strncmp(b, "#include", 8)) {
            const char *q = memchr(b, '"', (size_t)(e - b)), *q2;
            if (q && (q2 = memchr(q + 1, '"', (size_t)(e - q - 1)))) {
                snprintf(ps->rel, sizeof ps->rel, "%.*s", (int)(q2 - q - 1), q + 1);
                decl_join(ps->dir, ps->rel, ps->full, sizeof ps->full);
                decl_source(cx, ps->full, file, depth + 1, t, n);
            }
        }
        l = *e ? e + 1 : e;
    }
    free(text);
    free(ps);
}

/* Set each row's file from the preset at path: passes in order, so a name
 * two passes declare takes the first one's. Rows no pass declares keep "". */
static void tag_files(const char *path, struct nv2a_present_param *t, int n)
{
    struct decl_ctx *cx;
    int i;

    for (i = 0; i < n; i++)
        t[i].file[0] = 0;
    if (!path || !*path || n <= 0 || !(cx = calloc(1, sizeof *cx)))
        return;
    decl_preset(cx, path, 0);
    /* The preset files read so far are no sources; a pass may still be
     * one of them only by mistake, and then it declares nothing. */
    for (i = 0; i < cx->npass; i++) {
        const char *b = cx->pass[i], *s;
        if (!cx->pass[i][0])
            continue;
        for (s = cx->pass[i]; *s; s++)
            if (*s == '/' || *s == '\\')
                b = s + 1;
        decl_source(cx, cx->pass[i], b, 0, t, n);
    }
    free(cx);
}

int nv2a_present_chain_describe(const char *request, struct nv2a_present_param *out, int cap)
{
    PFN_libra_preset_create pc;
    PFN_libra_preset_free pf;
    libra_shader_preset_t preset = NULL;
    libra_error_t e;
    char path[1024], why[256];
    int n;

    /* The file first: a dump with no preset on disk loads no library. */
    if (is_none(request) || preset_path(request, path, sizeof path, 0) < 0
        || !nv2a_present_chain_lib())
        return -1;
    pc = (PFN_libra_preset_create)lrs_sym("libra_preset_create");
    pf = (PFN_libra_preset_free)lrs_sym("libra_preset_free");
    if (!pc || !pf)
        return -1;
    e = pc(path, &preset);
    if (e) {
        nv2a_present_chain_error(e, why, sizeof why);
        fprintf(stderr, LOGP "%s: %s\n", path, why);
        return -1;
    }
    n = nv2a_present_chain_params_read(&preset, out, cap);
    pf(&preset);
    tag_files(path, out, n);
    return n;
}

void nv2a_present_chain_error(void *err, char *out, size_t cap)
{
    PFN_libra_error_write wr = (PFN_libra_error_write)lrs_sym("libra_error_write");
    PFN_libra_error_free_string fs = (PFN_libra_error_free_string)lrs_sym("libra_error_free_string");
    PFN_libra_error_free fe = (PFN_libra_error_free)lrs_sym("libra_error_free");
    libra_error_t e = (libra_error_t)err;
    char *m = NULL;

    snprintf(out, cap, "unknown error");
    if (!e)
        return;
    if (wr && wr(e, &m) == 0 && m) {
        snprintf(out, cap, "%s", m);
        if (fs)
            fs(&m);
    }
    if (fe)
        fe(&e);
}

/* ---- Runtime parameters ---- */

/* Overrides of every preset seen this session: the config's, then the
 * menu's. A row is kept after its override is removed, so Save can delete
 * the key it once wrote. */
#define OVR_MAX NV2A_PRESENT_OVERRIDES_MAX
#define SCOPE_PRESET 0   /* keyed by preset key, saved to the config */
#define SCOPE_PROFILE 1  /* keyed by profile slug, saved to its file */
struct ovr {
    char key[64], name[64];
    float v, saved_v;
    unsigned char has, saved_has, scope;
};

static struct nv2a_present_param s_params[NV2A_PRESENT_PARAMS_MAX];
static int s_nparams;
static char s_params_key[64];
static int s_params_scope;
static atomic_uint s_pgen;
static struct ovr s_ovr[OVR_MAX];
static int s_novr;

void nv2a_present_chain_preset_key(const char *request, char *out, size_t cap)
{
    const char *b, *s;
    size_t n, i, o = 0;

    if (!cap)
        return;
    out[0] = 0;
    if (is_none(request))
        return;
    b = request;
    if (has_path_chars(request)) {
        for (s = request; *s; s++)
            if (*s == '/' || *s == '\\')
                b = s + 1;
    }
    n = strlen(b);
    if (n > 7 && !strcmp(b + n - 7, ".slangp"))
        n -= 7;
    /* A config section name: keep it to what a bare TOML key allows. */
    for (i = 0; i < n && o + 1 < cap; i++) {
        char c = b[i];
        out[o++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || c == '-' || c == '_') ? c : '_';
    }
    out[o] = 0;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

/* 1 when s[0..n) starts with the lower-case word w. */
static int starts(const char *s, size_t n, const char *w)
{
    size_t i;
    for (i = 0; w[i]; i++)
        if (i >= n || lower((unsigned char)s[i]) != w[i])
            return 0;
    return 1;
}

int nv2a_present_chain_key_lines(const char *key)
{
    const char *t;

    if (!key)
        return 0;
    /* RetroArch names its CRT presets crt-*, crt_* and crtsomething; the
     * rest put crt or scanline(s) somewhere in the stem. */
    if (starts(key, strlen(key), "crt"))
        return 1;
    for (t = key; *t;) {
        size_t n = strcspn(t, "-_.");
        if ((n == 3 && starts(t, n, "crt")) || starts(t, n, "scanline"))
            return 1;
        t += n;
        if (*t)
            t++;
    }
    return 0;
}

int nv2a_present_chain_ref_lines(const char *ref)
{
    char key[64];
    const char *end, *dir;

    if (is_none(ref))
        return 0;
    nv2a_present_chain_preset_key(ref, key, sizeof key);
    if (nv2a_present_chain_key_lines(key))
        return 1;
    /* The parent directory: the last component before the file name. */
    end = ref + strlen(ref);
    while (end > ref && end[-1] != '/' && end[-1] != '\\')
        end--;
    if (end == ref)
        return 0;
    end--;
    dir = end;
    while (dir > ref && dir[-1] != '/' && dir[-1] != '\\')
        dir--;
    return end - dir == 3 && starts(dir, 3, "crt");
}

void nv2a_present_chain_set_lines(int mode)
{
    atomic_store(&s_lines_mode, mode == NV2A_PRESENT_LINES_GUEST || mode == NV2A_PRESENT_LINES_HOST
                                    ? mode : NV2A_PRESENT_LINES_AUTO);
}

/* The line choice for the request as it stands; under s_lock. */
static int lines_locked(void)
{
    int mode = atomic_load(&s_lines_mode);

    if (is_none(s_req))
        return 0;
    if (mode != NV2A_PRESENT_LINES_AUTO)
        return mode == NV2A_PRESENT_LINES_GUEST;
    if (s_nstages > 0) {
        if (s_stages[0].lines != NV2A_PRESENT_LINES_AUTO)
            return s_stages[0].lines == NV2A_PRESENT_LINES_GUEST;
        return nv2a_present_chain_key_lines(s_stages[0].key);
    }
    return nv2a_present_chain_ref_lines(s_req);
}

int nv2a_present_chain_lines(void)
{
    int on;

    lock();
    on = lines_locked();
    unlock();
    return on;
}

int nv2a_present_chain_params_read(void *preset, struct nv2a_present_param *out, int cap)
{
    PFN_libra_preset_get_runtime_params get =
        (PFN_libra_preset_get_runtime_params)lrs_sym("libra_preset_get_runtime_params");
    PFN_libra_preset_free_runtime_params fr =
        (PFN_libra_preset_free_runtime_params)lrs_sym("libra_preset_free_runtime_params");
    libra_preset_param_list_t list;
    libra_error_t e;
    int n = 0;
    uint64_t i;

    if (!get || !fr || !preset)
        return 0;
    memset(&list, 0, sizeof list);
    e = get((const libra_shader_preset_t *)preset, &list);
    if (e) {
        char why[256];
        nv2a_present_chain_error(e, why, sizeof why);
        fprintf(stderr, LOGP "preset parameters unreadable: %s\n", why);
        return 0;
    }
    if (list.length > (uint64_t)cap)
        fprintf(stderr, LOGP "preset declares %llu parameters, the first %d kept\n",
                (unsigned long long)list.length, cap);
    for (i = 0; i < list.length && n < cap; i++) {
        const libra_preset_param_t *p = &list.parameters[i];
        struct nv2a_present_param *d = &out[n];
        if (!p->name || !p->name[0])
            continue;
        snprintf(d->name, sizeof d->name, "%s", p->name);
        snprintf(d->desc, sizeof d->desc, "%s", p->description ? p->description : "");
        d->initial = p->initial;
        d->min = p->minimum;
        d->max = p->maximum > p->minimum ? p->maximum : p->minimum;
        d->step = p->step > 0 ? p->step : 0;
        d->stage = 0;
        d->shared = 0;
        d->file[0] = 0;
        n++;
    }
    fr(list);
    return n;
}

static struct ovr *ovr_find(int scope, const char *key, const char *name)
{
    int i;
    for (i = 0; i < s_novr; i++)
        if (s_ovr[i].scope == scope && !strcmp(s_ovr[i].key, key)
            && !strcmp(s_ovr[i].name, name))
            return &s_ovr[i];
    return NULL;
}

static struct ovr *ovr_add(int scope, const char *key, const char *name)
{
    static int s_full_said;
    struct ovr *o = ovr_find(scope, key, name);
    int i;
    if (o)
        return o;
    if (s_novr < OVR_MAX) {
        o = &s_ovr[s_novr++];
    } else {
        /* A row with no value now and none on disk has nothing left for
         * Save to delete: reuse it, so a long session of slider moves
         * across presets does not fill the table. */
        for (i = 0; i < s_novr && !o; i++)
            if (!s_ovr[i].has && !s_ovr[i].saved_has)
                o = &s_ovr[i];
        if (!o) {
            if (!s_full_said) {
                s_full_said = 1;
                fprintf(stderr, LOGP "shader_params: %d overrides held, %s.%s not kept\n",
                        OVR_MAX, key, name);
            }
            return NULL;
        }
    }
    memset(o, 0, sizeof *o);
    o->scope = (unsigned char)scope;
    snprintf(o->key, sizeof o->key, "%s", key);
    snprintf(o->name, sizeof o->name, "%s", name);
    return o;
}

static const struct nv2a_present_param *active_find(const char *name)
{
    int i;
    for (i = 0; i < s_nparams; i++)
        if (!strcmp(s_params[i].name, name))
            return &s_params[i];
    return NULL;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* The value a row runs at: its override, clamped to the preset's range (a
 * hand-edited config can hold anything), or the preset's own. */
static float active_value(const struct nv2a_present_param *p)
{
    const struct ovr *o = ovr_find(s_params_scope, s_params_key, p->name);
    return o && o->has ? clampf(o->v, p->min, p->max) : p->initial;
}

static int stage_declares(const struct nv2a_present_stage *st, const char *name)
{
    int i;
    for (i = 0; i < st->nnames; i++)
        if (!strcmp(st->name[i], name))
            return 1;
    return 0;
}

/* A profile's rows by name: the first stage whose list holds it owns it;
 * shared when a later one holds it too. Under s_lock. */
static void tag_rows(struct nv2a_present_param *t, int n)
{
    int i, s, owner;
    for (i = 0; i < n; i++) {
        owner = -1;
        /* -1: no stage lists it (a stage changed on disk since the
         * compose); the chain owns it, not stage 1. */
        t[i].stage = -1;
        t[i].shared = 0;
        for (s = 0; s < s_nstages; s++) {
            if (!stage_declares(&s_stages[s], t[i].name))
                continue;
            if (owner < 0) {
                owner = s;
                t[i].stage = s;
            } else {
                t[i].shared = 1;
                break;
            }
        }
    }
}

void nv2a_present_chain_params_publish(void *preset, unsigned gen)
{
    struct nv2a_present_param t[NV2A_PRESENT_PARAMS_MAX];
    char req[512], key[64], path[1024];
    int n = nv2a_present_chain_params_read(preset, t, NV2A_PRESENT_PARAMS_MAX);

    /* The files before the lock: reading them must not hold the menu. A
     * request that moved meanwhile drops the table below anyway, so its
     * files are not read at all. */
    lock();
    snprintf(req, sizeof req, "%s", s_req);
    unlock();
    if (gen == atomic_load(&s_gen) && !is_none(req)
        && preset_path(req, path, sizeof path, 0) > 0)
        tag_files(path, t, n);

    lock();
    if (gen == atomic_load(&s_gen)) {
        if (s_req_slug[0]) {
            snprintf(key, sizeof key, "%s", s_req_slug);
            tag_rows(t, n);
            s_params_scope = SCOPE_PROFILE;
        } else {
            snprintf(req, sizeof req, "%s", s_req);
            nv2a_present_chain_preset_key(req, key, sizeof key);
            s_params_scope = SCOPE_PRESET;
        }
        memcpy(s_params, t, (size_t)n * sizeof t[0]);
        s_nparams = n;
        snprintf(s_params_key, sizeof s_params_key, "%s", key);
        atomic_fetch_add(&s_pgen, 1);
    }
    unlock();
}

void nv2a_present_chain_params_clear(void)
{
    lock();
    if (s_nparams || s_params_key[0]) {
        s_nparams = 0;
        s_params_key[0] = 0;
        s_params_scope = SCOPE_PRESET;
        atomic_fetch_add(&s_pgen, 1);
    }
    unlock();
}

unsigned nv2a_present_chain_params_gen(void)
{
    return atomic_load(&s_pgen);
}

int nv2a_present_chain_params(struct nv2a_present_param *out, int cap, char *key, size_t keycap)
{
    int n;
    lock();
    n = s_nparams < cap ? s_nparams : cap;
    if (n > 0)
        memcpy(out, s_params, (size_t)n * sizeof s_params[0]);
    if (key && keycap)
        snprintf(key, keycap, "%s", s_params_key);
    unlock();
    return n;
}

int nv2a_present_chain_params_profile(void)
{
    int on;
    lock();
    on = s_params_key[0] && s_params_scope == SCOPE_PROFILE;
    unlock();
    return on;
}

int nv2a_present_chain_param_get(const char *name, float *out)
{
    const struct nv2a_present_param *p;
    lock();
    p = active_find(name);
    if (p && out)
        *out = active_value(p);
    unlock();
    return p != NULL;
}

int nv2a_present_chain_param_set(const char *name, float v)
{
    const struct nv2a_present_param *p;
    struct ovr *o;
    float eps;

    lock();
    p = active_find(name);
    if (!p) {
        unlock();
        return 0;
    }
    v = clampf(v, p->min, p->max);
    /* Within a thousandth of a step of the preset's value is the preset's
     * value: menu steps add up in floats. */
    eps = p->step > 0 ? p->step * 0.001f : 1e-6f;
    o = ovr_find(s_params_scope, s_params_key, name);
    if (v > p->initial - eps && v < p->initial + eps) {
        if (o)
            o->has = 0;
    } else if ((o = ovr_add(s_params_scope, s_params_key, name)) != NULL) {
        o->v = v;
        o->has = 1;
    }
    atomic_fetch_add(&s_pgen, 1);
    unlock();
    return 1;
}

void nv2a_present_chain_params_reset(const char *key)
{
    int i;
    lock();
    for (i = 0; i < s_novr; i++)
        if (s_ovr[i].scope == SCOPE_PRESET && (!key || !strcmp(s_ovr[i].key, key)))
            s_ovr[i].has = 0;
    atomic_fetch_add(&s_pgen, 1);
    unlock();
}

void nv2a_present_chain_params_apply(unsigned *applied, nv2a_present_param_set_fn set, void *ctx)
{
    struct { char name[64]; float v; } t[NV2A_PRESENT_PARAMS_MAX];
    unsigned g = atomic_load(&s_pgen);
    int n, i;

    if (g == *applied)
        return;
    lock();
    g = atomic_load(&s_pgen);
    n = s_nparams;
    for (i = 0; i < n; i++) {
        memcpy(t[i].name, s_params[i].name, sizeof t[i].name);
        t[i].v = active_value(&s_params[i]);
    }
    unlock();
    /* Every row, not only the one that moved: a reset or a new preset
     * changes many, and a set_param is a store into the chain's table. */
    for (i = 0; i < n; i++)
        set(ctx, t[i].name, t[i].v);
    *applied = g;
}

void nv2a_present_chain_override_load(const char *key, const char *name, float v)
{
    struct ovr *o;
    lock();
    if ((o = ovr_add(SCOPE_PRESET, key, name)) != NULL) {
        o->v = o->saved_v = v;
        o->has = o->saved_has = 1;
        atomic_fetch_add(&s_pgen, 1);
    }
    unlock();
}

static int ovr_dirty(const struct ovr *o)
{
    return o->has != o->saved_has || (o->has && o->v != o->saved_v);
}

int nv2a_present_chain_override_at(int i, char *key, size_t keycap, char *name,
                                   size_t namecap, float *v, int *has, int *dirty)
{
    int ok = 0, j;
    const struct ovr *o = NULL;
    lock();
    /* The i-th preset-scope row: profile rows go to their own files. */
    for (j = 0; j < s_novr && i >= 0; j++)
        if (s_ovr[j].scope == SCOPE_PRESET && i-- == 0)
            o = &s_ovr[j];
    ok = o != NULL;
    if (ok) {
        if (key && keycap)
            snprintf(key, keycap, "%s", o->key);
        if (name && namecap)
            snprintf(name, namecap, "%s", o->name);
        if (v)
            *v = o->v;
        if (has)
            *has = o->has;
        if (dirty)
            *dirty = ovr_dirty(o);
    }
    unlock();
    return ok;
}

void nv2a_present_chain_overrides_saved(void)
{
    int i;
    lock();
    for (i = 0; i < s_novr; i++) {
        if (s_ovr[i].scope != SCOPE_PRESET)
            continue;
        s_ovr[i].saved_v = s_ovr[i].v;
        s_ovr[i].saved_has = s_ovr[i].has;
    }
    unlock();
}

int nv2a_present_chain_overrides_dirty(void)
{
    int i, d = 0;
    lock();
    for (i = 0; i < s_novr; i++)
        if (s_ovr[i].scope == SCOPE_PRESET)
            d += ovr_dirty(&s_ovr[i]);
    unlock();
    return d;
}

int nv2a_present_chain_override_get(const char *key, const char *name, float *v)
{
    const struct ovr *o;
    int has;
    lock();
    o = ovr_find(SCOPE_PRESET, key, name);
    has = o && o->has;
    if (has && v)
        *v = o->v;
    unlock();
    return has;
}

/* ---- The profile scope ---- */

/* No value now and none on disk: ovr_add may reuse the row, and its empty
 * key matches no slug. */
static void ovr_free(struct ovr *o)
{
    o->has = o->saved_has = 0;
    o->key[0] = 0;
}

void nv2a_present_chain_profile_value_load(const char *slug, const char *name, float v)
{
    struct ovr *o;
    lock();
    if ((o = ovr_add(SCOPE_PROFILE, slug, name)) != NULL) {
        o->v = o->saved_v = v;
        o->has = o->saved_has = 1;
        atomic_fetch_add(&s_pgen, 1);
    }
    unlock();
}

int nv2a_present_chain_profile_value_get(const char *slug, const char *name, float *v)
{
    const struct ovr *o;
    int has;
    lock();
    o = ovr_find(SCOPE_PROFILE, slug, name);
    has = o && o->has;
    if (has && v)
        *v = o->v;
    unlock();
    return has;
}

int nv2a_present_chain_profile_values(const char *slug, char (*name)[64], float *v, int cap)
{
    int i, n = 0;
    lock();
    for (i = 0; i < s_novr && n < cap; i++) {
        const struct ovr *o = &s_ovr[i];
        if (o->scope != SCOPE_PROFILE || !o->has || strcmp(o->key, slug))
            continue;
        memcpy(name[n], o->name, sizeof name[n]);
        v[n++] = o->v;
    }
    unlock();
    return n;
}

void nv2a_present_chain_profile_reset(const char *slug, const char *name)
{
    int i;
    lock();
    for (i = 0; i < s_novr; i++) {
        struct ovr *o = &s_ovr[i];
        if (o->scope == SCOPE_PROFILE && !strcmp(o->key, slug)
            && (!name || !strcmp(o->name, name)))
            o->has = 0;
    }
    atomic_fetch_add(&s_pgen, 1);
    unlock();
}

int nv2a_present_chain_profile_dirty(const char *slug)
{
    int i, d = 0;
    lock();
    for (i = 0; i < s_novr; i++)
        if (s_ovr[i].scope == SCOPE_PROFILE && (!slug || !strcmp(s_ovr[i].key, slug)))
            d += ovr_dirty(&s_ovr[i]);
    unlock();
    return d;
}

void nv2a_present_chain_profile_saved(const char *slug)
{
    int i;
    lock();
    for (i = 0; i < s_novr; i++) {
        struct ovr *o = &s_ovr[i];
        if (o->scope != SCOPE_PROFILE || strcmp(o->key, slug))
            continue;
        o->saved_v = o->v;
        o->saved_has = o->has;
    }
    unlock();
}

void nv2a_present_chain_profile_rekey(const char *from, const char *to)
{
    int i;
    lock();
    /* A row of to left from an older profile of that slug would shadow
     * the renamed one's: forget it first. */
    for (i = 0; i < s_novr; i++)
        if (s_ovr[i].scope == SCOPE_PROFILE && !strcmp(s_ovr[i].key, to))
            ovr_free(&s_ovr[i]);
    for (i = 0; i < s_novr; i++)
        if (s_ovr[i].scope == SCOPE_PROFILE && !strcmp(s_ovr[i].key, from))
            snprintf(s_ovr[i].key, sizeof s_ovr[i].key, "%s", to);
    if (s_params_scope == SCOPE_PROFILE && !strcmp(s_params_key, from))
        snprintf(s_params_key, sizeof s_params_key, "%s", to);
    if (!strcmp(s_req_slug, from))
        snprintf(s_req_slug, sizeof s_req_slug, "%s", to);
    atomic_fetch_add(&s_pgen, 1);
    unlock();
}

void nv2a_present_chain_profile_forget(const char *slug)
{
    int i;
    lock();
    for (i = 0; i < s_novr; i++)
        if (s_ovr[i].scope == SCOPE_PROFILE && !strcmp(s_ovr[i].key, slug))
            ovr_free(&s_ovr[i]);
    atomic_fetch_add(&s_pgen, 1);
    unlock();
}

void nv2a_present_chain_params_reset_for_test(void)
{
    lock();
    s_nparams = 0;
    s_params_key[0] = 0;
    s_params_scope = SCOPE_PRESET;
    s_novr = 0;
    s_req_slug[0] = 0;
    s_nstages = 0;
    s_profile_err[0] = 0;
    s_prepare = NULL;
    atomic_store(&s_pending, 0);
    atomic_fetch_add(&s_pgen, 1);
    unlock();
}

static void ovr_revert(int scope, const char *key)
{
    int i;
    lock();
    for (i = 0; i < s_novr; i++) {
        struct ovr *o = &s_ovr[i];
        if (o->scope != scope || (key && strcmp(o->key, key)))
            continue;
        o->has = o->saved_has;
        o->v = o->saved_v;
    }
    atomic_fetch_add(&s_pgen, 1);
    unlock();
}

void nv2a_present_chain_override_revert(void)
{
    ovr_revert(SCOPE_PRESET, NULL);
}

void nv2a_present_chain_profile_revert(const char *slug)
{
    ovr_revert(SCOPE_PROFILE, slug);
}

int nv2a_present_chain_param_dirty(const char *name)
{
    const struct ovr *o;
    int d;
    if (!name)
        return 0;
    lock();
    o = ovr_find(s_params_scope, s_params_key, name);
    d = o && ovr_dirty(o);
    unlock();
    return d;
}
