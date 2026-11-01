/* rt.c — the runtime under the translated engine: guest memory, the heap, and the C and C++
 * library calls the original code makes (strings, streams, the red-black tree helpers, libm,
 * random()), plus the handful of engine functions the port replaces outright (configuration
 * values, texture and sound loading). tools/original_oracle.py does the same jobs for the
 * original machine code in an emulator, and the two are kept alike on purpose: with the same
 * inputs, the translated engine and the original leave identical guest memory. */
#include "rt.h"
#include "platform.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *g_mem;
uint32_t R0, R1, R2, R3, SP;
double rt_clock = 1000.0;
int rt_tutorial_enabled;
float rt_display_scale = 1.0f;

static uint8_t *sArena;
static uint32_t sHeapEnd, sBrk, sHeapUsed, sHeapPeak, sStackLow = RT_STACK_TOP;
static int sDepth;

#define ARG(i) ((i) == 0 ? R0 : (i) == 1 ? R1 : (i) == 2 ? R2 : R3)
#define STACK_ARG(i) M32(SP + 4 * (i))
static void die(const char *fmt, ...) {
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    plat_log("engine runtime: %s", text);
    plat_fatal(text);
}

#ifdef RT_CHECK
uint8_t *rt_checked(uint32_t addr, uint32_t size) {
    if (addr < RT_IMAGE_LO || addr + size > sHeapEnd) die("access to %x, outside guest memory", (unsigned)addr);
    return g_mem + addr;
}
#endif

/* ---- heap -------------------------------------------------------------------------------- */
/* Blocks carry a 16-byte header (size and flags, the size of the block before, and the free
 * list links) and are multiples of 16 bytes. Freed blocks merge with free neighbours and are
 * kept in lists by size class; a free block at the top of the heap is given back to it. Memory
 * is zeroed when handed out. The reference harness has the same allocator, so addresses match. */
#define H_SIZE(b) M32(b)
#define H_PREV_SIZE(b) M32((b) + 4)
#define H_PREV_FREE(b) M32((b) + 8)
#define H_NEXT_FREE(b) M32((b) + 12)
#define USED 1u
#define PINNED 2u
#define PINNED_FREED 4u
#define BINS 28
#define MIN_SPLIT 64u
static uint32_t sBins[BINS], sTopSize;      /* sTopSize: the size of the last block below the break */

static uint32_t block_size(uint32_t b) { return H_SIZE(b) & ~15u; }
static unsigned bin_for(uint32_t size) { return 27u - (unsigned)__builtin_clz(size); }
static void insert_free(uint32_t b) {
    unsigned bin = bin_for(block_size(b));
    H_PREV_FREE(b) = 0;
    H_NEXT_FREE(b) = sBins[bin];
    if (sBins[bin]) H_PREV_FREE(sBins[bin]) = b;
    sBins[bin] = b;
}
static void remove_free(uint32_t b) {
    if (H_PREV_FREE(b)) H_NEXT_FREE(H_PREV_FREE(b)) = H_NEXT_FREE(b);
    else sBins[bin_for(block_size(b))] = H_NEXT_FREE(b);
    if (H_NEXT_FREE(b)) H_PREV_FREE(H_NEXT_FREE(b)) = H_PREV_FREE(b);
}
static void set_next_prev(uint32_t b) {
    uint32_t next = b + block_size(b);
    if (next < sBrk) H_PREV_SIZE(next) = block_size(b); else sTopSize = block_size(b);
}
/* b is free and on no list: merge it with free neighbours, then list it or lower the break. */
static void release_block(uint32_t b) {
    H_SIZE(b) = block_size(b);
    uint32_t next = b + block_size(b);
    if (next < sBrk && !(H_SIZE(next) & USED)) {
        remove_free(next);
        H_SIZE(b) += block_size(next);
    }
    if (H_PREV_SIZE(b)) {
        uint32_t prev = b - H_PREV_SIZE(b);
        if (!(H_SIZE(prev) & USED)) {
            remove_free(prev);
            H_SIZE(prev) += block_size(b);
            b = prev;
        }
    }
    if (b + block_size(b) == sBrk) {            /* at the top: give it back */
        sBrk = b;
        sTopSize = H_PREV_SIZE(b);
        return;
    }
    set_next_prev(b);
    insert_free(b);
}

#ifdef RT_TRACE
void rt_trace(uint32_t addr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3);
#endif
uint32_t rt_alloc(uint32_t size) {
    if (SP && SP < sStackLow) sStackLow = SP;
    uint32_t need = (size < 16 ? 16 : (size + 15) & ~15u) + 16, b = 0;
    for (unsigned bin = bin_for(need); bin < BINS && !b; bin++)
        for (uint32_t c = sBins[bin]; c; c = H_NEXT_FREE(c))
            if (block_size(c) >= need) { b = c; break; }
    if (b) {
        remove_free(b);
        uint32_t available = block_size(b);
        if (available - need >= MIN_SPLIT) {
            uint32_t tail = b + need;
            H_SIZE(b) = need | USED;
            H_SIZE(tail) = available - need;
            H_PREV_SIZE(tail) = need;
            set_next_prev(tail);
            insert_free(tail);
        } else {
            H_SIZE(b) = available | USED;
        }
    } else {
        if (sBrk + need > sHeapEnd) die("guest heap exhausted (%u bytes wanted, %u in use)", (unsigned)size, (unsigned)sHeapUsed);
        b = sBrk;
        H_SIZE(b) = need | USED;
        H_PREV_SIZE(b) = sBrk == RT_HEAP ? 0 : sTopSize;
        sBrk += need;
        sTopSize = need;
        if (sBrk - RT_HEAP > sHeapPeak) sHeapPeak = sBrk - RT_HEAP;
    }
    H_PREV_FREE(b) = H_NEXT_FREE(b) = 0;
    memset(g_mem + b + 16, 0, block_size(b) - 16);
#ifdef RT_TRACE
    rt_trace(0xa110c, size, b + 16, 0, 0);
#endif
    sHeapUsed += block_size(b);
    return b + 16;
}

static int is_block(uint32_t addr) { return addr >= RT_HEAP + 16 && addr < sBrk && !(addr & 15) && (H_SIZE(addr - 16) & USED); }

/* A block the port wants to go on using after the engine frees it: the free is remembered
 * and takes effect when the block is unpinned. */
int rt_pin(uint32_t addr, uint32_t size) {
    if (!is_block(addr) || block_size(addr - 16) - 16 < size || (H_SIZE(addr - 16) & (PINNED | PINNED_FREED))) return 0;
    H_SIZE(addr - 16) |= PINNED;
    return 1;
}
void rt_unpin(uint32_t addr) {
    if (!is_block(addr)) return;
    uint32_t flags = H_SIZE(addr - 16);
    H_SIZE(addr - 16) &= ~(PINNED | PINNED_FREED);
    if (flags & PINNED_FREED) rt_free(addr);
}

void rt_free(uint32_t addr) {
    if (!is_block(addr)) return;
    uint32_t b = addr - 16;
    if (H_SIZE(b) & PINNED_FREED) return;
    if (H_SIZE(b) & PINNED) { H_SIZE(b) |= PINNED_FREED; return; }
#ifdef RT_TRACE
    rt_trace(0xf4ee, block_size(b), addr, 0, 0);
#endif
    sHeapUsed -= block_size(b);
    release_block(b);
}

uint32_t rt_heap_used(void) { return sHeapUsed; }
uint32_t rt_heap_peak(void) { return sHeapPeak; }
uint32_t rt_heap_break(void) { return sBrk; }
uint32_t rt_stack_low(void) { return sStackLow; }

/* ---- calls ------------------------------------------------------------------------------- */
void rt_bad_call(uint32_t addr) { die("call to %x, which is not a translated function", (unsigned)addr); }
void rt_bad_jump(uint32_t addr) { die("jump to an unexpected address %x", (unsigned)addr); }
void rt_missing(const char *name) { die("call to %s, which was left out of the translation", name); }

uint32_t rt_lookup(const char *name) {
    int lo = 0, hi = rt_symbol_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(rt_symbols[mid].name, name);
        if (!c) return rt_symbols[mid].addr;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    die("no engine function named %s", name);
    return 0;
}

uint32_t rt_invoke(uint32_t addr, int argc, ...) {
    uint32_t args[12], saved = SP;
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc && i < 12; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    SP = sDepth ? (SP - 0x400) & ~7u : RT_STACK_TOP - 0x1000;
    if (argc > 4) {
        SP -= 4 * (uint32_t)(argc - 4);
        for (int i = 4; i < argc; i++) M32(SP + 4 * (uint32_t)(i - 4)) = args[i];
    }
    uint32_t entry = SP;
    R0 = argc > 0 ? args[0] : 0; R1 = argc > 1 ? args[1] : 0; R2 = argc > 2 ? args[2] : 0; R3 = argc > 3 ? args[3] : 0;
    sDepth++;
    uint64_t result = rt_enter(addr, R0, R1, R2, R3);
    sDepth--;
    R0 = (uint32_t)result; R1 = (uint32_t)(result >> 32);
    if (SP != entry) die("stack pointer %x after a call to %x that began at %x", (unsigned)SP, (unsigned)addr, (unsigned)entry);
    SP = saved;
    return R0;
}

/* ---- std::string: one pointer to the characters; length, capacity, reference count before them */
static char *gstr(uint32_t addr) { return (char *)(g_mem + addr); }
static uint32_t str_len(uint32_t obj) { return M32(M32(obj) - 12); }
static const char *str_data(uint32_t obj) { return gstr(M32(obj)); }

const char *rt_guest_string(uint32_t obj, uint32_t *length) {
    if (length) *length = str_len(obj);
    return str_data(obj);
}

static void set_string(uint32_t obj, const char *data, uint32_t len) {
    uint32_t block = rt_alloc(13 + len);        /* the source may be the old value: copy before linking */
    M32(block) = len; M32(block + 4) = len; M32(block + 8) = 0;
    memmove(g_mem + block + 12, data, len);
    M8(block + 12 + len) = 0;
    M32(obj) = block + 12;
}

/* Replace a constructed string's value, releasing the old characters. */
static void assign_string(uint32_t obj, const char *data, uint32_t len) {
    uint32_t old = M32(obj);
    set_string(obj, data, len);
    rt_free(old - 12);
}

static int compare_bytes(const char *a, uint32_t la, const char *b, uint32_t lb) {
    int c = memcmp(a, b, la < lb ? la : lb);
    if (c) return c < 0 ? -1 : 1;
    return la < lb ? -1 : la > lb;
}

void imp__ZNSsC1EPKcRKSaIcE(void) { const char *s = R1 ? gstr(R1) : ""; set_string(R0, s, (uint32_t)strlen(s)); }
void imp__ZNSsC1ERKSs(void) { set_string(R0, str_data(R1), str_len(R1)); }
void imp__ZNSsC1Ev(void) { set_string(R0, "", 0); }
void imp__ZNSsD1Ev(void) { rt_free(M32(R0) - 12); R0 = 0; }
void imp__ZNSsaSEPKc(void) { const char *s = R1 ? gstr(R1) : ""; assign_string(R0, s, (uint32_t)strlen(s)); }
void imp__ZNSsaSERKSs(void) { if (R0 != R1) assign_string(R0, str_data(R1), str_len(R1)); }
void imp__ZNSs6appendEmc(void) {
    uint32_t obj = R0, n = R1, len = str_len(obj);
    char *t = malloc(len + n + 1);
    memcpy(t, str_data(obj), len);
    memset(t + len, (int)(R2 & 255), n);
    assign_string(obj, t, len + n);
    free(t);
    R0 = obj;
}
void imp__ZNSsixEm(void) { R0 = M32(R0) + R1; }
void imp__ZNKSs2atEm(void) { R0 = M32(R0) + R1; }
void imp__ZNKSs5c_strEv(void) { R0 = M32(R0); }
void imp__ZNKSs4sizeEv(void) { R0 = str_len(R0); }
void imp__ZNKSs6lengthEv(void) { R0 = str_len(R0); }
void imp__ZNKSs5emptyEv(void) { R0 = str_len(R0) == 0; }
void imp__ZNKSs4findEcm(void) {
    uint32_t len = str_len(R0);
    const char *s = str_data(R0);
    for (uint32_t i = R2; i < len; i++) if ((uint8_t)s[i] == (R1 & 255)) { R0 = i; return; }
    R0 = 0xffffffffu;
}
void imp__ZNKSs6substrEmm(void) {
    uint32_t len = str_len(R1), pos = R2, n = R3;
    if (pos > len) pos = len;
    if (n > len - pos) n = len - pos;
    set_string(R0, str_data(R1) + pos, n);
}
void imp__ZNKSs7compareEPKc(void) { R0 = (uint32_t)compare_bytes(str_data(R0), str_len(R0), gstr(R1), (uint32_t)strlen(gstr(R1))); }
void imp__ZNKSs7compareERKSs(void) { R0 = (uint32_t)compare_bytes(str_data(R0), str_len(R0), str_data(R1), str_len(R1)); }
void imp__ZNSaIcEC1Ev(void) { R0 = 0; }
void imp__ZNSaIcED1Ev(void) { R0 = 0; }

/* ---- red-black tree helpers, kept as a plain search tree (lookups only follow the links) ---- */
#define T_COLOR(x) M32(x)
#define T_PARENT(x) M32((x) + 4)
#define T_LEFT(x) M32((x) + 8)
#define T_RIGHT(x) M32((x) + 12)
static uint32_t tree_next(uint32_t x) {
    if (T_RIGHT(x)) {
        x = T_RIGHT(x);
        while (T_LEFT(x)) x = T_LEFT(x);
        return x;
    }
    uint32_t y = T_PARENT(x);
    while (x == T_RIGHT(y)) { x = y; y = T_PARENT(y); }
    return T_RIGHT(x) != y ? y : x;
}
static uint32_t tree_prev(uint32_t x) {
    if (T_COLOR(x) == 0 && T_PARENT(T_PARENT(x)) == x) return T_RIGHT(x);
    if (T_LEFT(x)) {
        uint32_t y = T_LEFT(x);
        while (T_RIGHT(y)) y = T_RIGHT(y);
        return y;
    }
    uint32_t y = T_PARENT(x);
    while (x == T_LEFT(y)) { x = y; y = T_PARENT(y); }
    return y;
}
void imp__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_(void) {
    uint32_t left = R0, x = R1, p = R2, header = R3;
    T_COLOR(x) = 0; T_PARENT(x) = p; T_LEFT(x) = 0; T_RIGHT(x) = 0;
    if (left) {
        T_LEFT(p) = x;
        if (p == header) { T_PARENT(header) = x; T_RIGHT(header) = x; }
        else if (p == T_LEFT(header)) T_LEFT(header) = x;
    } else {
        T_RIGHT(p) = x;
        if (p == T_RIGHT(header)) T_RIGHT(header) = x;
    }
    T_COLOR(T_PARENT(header)) = 1;
}
void imp__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base(void) { R0 = tree_next(R0); }
void imp__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base(void) { R0 = tree_prev(R0); }

/* ---- streams ----------------------------------------------------------------------------- */
typedef struct {
    uint32_t obj;
    uint8_t *data;              /* what is read; for a string stream also what was written */
    uint32_t size, cap, pos;
    int fail, writing, shared, owned;
    char name[96];
} Stream;
#define STREAMS 12
static Stream sStreams[STREAMS];

static Stream *stream_of(uint32_t addr) {
    Stream *best = NULL;
    for (int i = 0; i < STREAMS; i++) {
        Stream *s = &sStreams[i];
        if (!s->obj) continue;
        if (s->obj == addr) return s;
        if (s->obj <= addr && addr < s->obj + 0x400 && (!best || s->obj > best->obj)) best = s;
    }
    return best;
}

static Stream *stream_open(uint32_t obj) {
    for (int i = 0; i < STREAMS; i++)
        if (sStreams[i].obj == obj) { if (sStreams[i].owned) free(sStreams[i].data); sStreams[i].obj = 0; }
    for (int i = 0; i < STREAMS; i++)
        if (!sStreams[i].obj) {
            Stream *s = &sStreams[i];
            memset(s, 0, sizeof *s);
            s->obj = obj;
            /* The engine finds a stream's state through its virtual table; point it at one whose
             * offsets lead back to the object itself. */
            M32(obj) = RT_FAKE_VTABLE;
            M32(RT_FAKE_VTABLE - 12) = 0;
            M32(obj + 8) = RT_FAKE_VTABLE + 16;
            M32(RT_FAKE_VTABLE + 4) = 0xfffffff8u;
            return s;
        }
    die("too many streams open");
    return NULL;
}

static void stream_put(Stream *s, const void *data, uint32_t n) {
    if (s->size + n + 1 > s->cap) {
        s->cap = (s->size + n + 1) * 2 + 64;
        s->data = realloc(s->data, s->cap);
    }
    memcpy(s->data + s->size, data, n);
    s->size += n;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void open_input(uint32_t obj, const char *path) {
    Stream *s = stream_open(obj);
    uint32_t size = 0;
    const char *name = base_name(path);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->data = *name ? plat_read_file(name, &size, 0) : NULL;       /* the game's data, else something it saved */
    if (!s->data && *name) s->data = plat_read_file(name, &size, 1);
    s->owned = 1;
    s->size = size;
    s->fail = s->data == NULL;
}

static void open_output(uint32_t obj, const char *path) {
    Stream *s = stream_open(obj);
    snprintf(s->name, sizeof s->name, "%s", base_name(path));
    s->writing = 1;
    s->owned = 1;
}

static void stream_close(uint32_t obj, int destroy) {
    for (int i = 0; i < STREAMS; i++) {
        Stream *s = &sStreams[i];
        if (s->obj != obj) continue;
        if (s->writing && s->name[0]) { plat_write_file(s->name, s->data, s->size); s->writing = 0; }
        if (destroy) {
            if (s->owned) free(s->data);
            memset(s, 0, sizeof *s);
        }
    }
}

void imp__ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode(void) { open_input(R0, gstr(R1)); }
void imp__ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode(void) { open_output(R0, gstr(R1)); }
void imp__ZNSt13basic_fstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode(void) {
    if (R2 & 0x10) open_output(R0, gstr(R1)); else open_input(R0, gstr(R1));
}
void imp__ZNSt14basic_ifstreamIcSt11char_traitsIcEE5closeEv(void) { stream_close(R0, 0); }
void imp__ZNSt14basic_ofstreamIcSt11char_traitsIcEE5closeEv(void) { stream_close(R0, 0); }
void imp__ZNSt13basic_fstreamIcSt11char_traitsIcEE5closeEv(void) { stream_close(R0, 0); }
void imp__ZNSt14basic_ifstreamIcSt11char_traitsIcEED1Ev(void) { stream_close(R0, 1); }
void imp__ZNSt14basic_ofstreamIcSt11char_traitsIcEED1Ev(void) { stream_close(R0, 1); }
void imp__ZNSt13basic_fstreamIcSt11char_traitsIcEED1Ev(void) { stream_close(R0, 1); }
void imp__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode(void) { Stream *s = stream_open(R0); s->shared = 1; s->owned = 1; }
void imp__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode(void) { Stream *s = stream_open(R0); s->shared = 1; s->owned = 1; }
void imp__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev(void) { stream_close(R0, 1); }
void imp__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev(void) { stream_close(R0, 1); }
void imp__ZNKSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE3strEv(void) {
    Stream *s = stream_of(R1);
    set_string(R0, s && s->data ? (const char *)s->data : "", s ? s->size : 0);
}

void imp__ZNSi4readEPci(void) {
    Stream *s = stream_of(R0);
    uint32_t n = R2, left = s->size - s->pos;
    if ((int32_t)n < 0) n = 0;
    if (n > left) { n = left; s->fail = 1; }
    if (n) memcpy(g_mem + R1, s->data + s->pos, n);
    s->pos += n;
}
void imp__ZNSi4peekEv(void) {
    Stream *s = stream_of(R0);
    R0 = s->pos < s->size && !s->fail ? s->data[s->pos] : 0xffffffffu;
}
void imp__ZNSi7getlineEPci(void) {
    Stream *s = stream_of(R0);
    uint32_t start = s->pos, end = start, max = R2;
    while (end < s->size && s->data[end] != '\n') end++;
    int found = end < s->size;
    uint32_t len = end - start;
    if (!found && !len) s->fail = 1;
    if (len > max - 1) { len = max - 1; s->pos += len; s->fail = 1; }
    else s->pos += len + (found ? 1u : 0u);
    memcpy(g_mem + R1, s->data + start, len);
    M8(R1 + len) = 0;
}
/* Decimal text to a number without the C library's locale machinery; exact (correctly
 * rounded) for up to 15 significant digits and exponents within +-22, which covers the data. */
static double parse_number(const char *t, char **end) {
    static const double pow10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16,
                                    1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    const char *p = t;
    int negative = 0, digits = 0, any = 0, exponent = 0;
    double mantissa = 0.0;
    if (*p == '-' || *p == '+') negative = *p++ == '-';
    for (; *p >= '0' && *p <= '9'; p++, any = 1)
        if (digits < 17) { mantissa = mantissa * 10.0 + (*p - '0'); if (mantissa != 0.0) digits++; } else exponent++;
    if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++, any = 1)
            if (digits < 17) { mantissa = mantissa * 10.0 + (*p - '0'); if (mantissa != 0.0) digits++; exponent--; }
    if (!any) { *end = (char *)t; return 0.0; }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int sign = 1, e = 0, seen = 0;
        if (*q == '-' || *q == '+') sign = *q++ == '-' ? -1 : 1;
        for (; *q >= '0' && *q <= '9'; q++, seen = 1) if (e < 1000) e = e * 10 + (*q - '0');
        if (seen) { exponent += sign * e; p = q; }
    }
    *end = (char *)p;
    while (exponent > 22) { mantissa *= 1e22; exponent -= 22; }
    while (exponent < -22) { mantissa /= 1e22; exponent += 22; }
    mantissa = exponent >= 0 ? mantissa * pow10[exponent] : mantissa / pow10[-exponent];
    return negative ? -mantissa : mantissa;
}

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
static int stream_token(Stream *s, char *out, uint32_t max) {
    if (s->fail) return 0;
    while (s->pos < s->size && is_space(s->data[s->pos])) s->pos++;
    uint32_t start = s->pos;
    while (s->pos < s->size && !is_space(s->data[s->pos])) s->pos++;
    uint32_t len = s->pos - start;
    if (!len) { s->fail = 1; return 0; }
    if (len > max - 1) len = max - 1;
    memcpy(out, s->data + start, len);
    out[len] = 0;
    return 1;
}
void imp__ZNSirsERi(void) {
    Stream *s = stream_of(R0);
    char t[64], *end;
    if (!stream_token(s, t, sizeof t)) return;
    long v = strtol(t, &end, 10);
    if (*end || end == t) s->fail = 1; else M32(R1) = (uint32_t)v;
}
void imp__ZNSirsERf(void) {
    Stream *s = stream_of(R0);
    char t[64], *end;
    if (!stream_token(s, t, sizeof t)) return;
    double v = parse_number(t, &end);
    if (*end || end == t) s->fail = 1; else MF(R1) = (float)v;
}
void imp__ZNSirsERb(void) {
    Stream *s = stream_of(R0);
    char t[64], *end;
    if (!stream_token(s, t, sizeof t)) return;
    long v = strtol(t, &end, 10);
    if (*end || end == t) s->fail = 1; else M8(R1) = v != 0;
}
void imp__ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_E(void) {
    Stream *s = stream_of(R0);
    char t[256];
    if (stream_token(s, t, sizeof t)) set_string(R1, t, (uint32_t)strlen(t));
}
void imp__ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_RS3_(void) {
    Stream *s = stream_of(R0);
    while (s->pos < s->size && is_space(s->data[s->pos])) s->pos++;
    if (s->pos < s->size) M8(R1) = s->data[s->pos++]; else s->fail = 1;
}
void imp__ZNKSt9basic_iosIcSt11char_traitsIcEE4failEv(void) { Stream *s = stream_of(R0); R0 = !s || s->fail; }
void imp__ZNKSt9basic_iosIcSt11char_traitsIcEEntEv(void) { Stream *s = stream_of(R0); R0 = !s || s->fail; }
void imp__ZNKSt9basic_iosIcSt11char_traitsIcEEcvPvEv(void) { Stream *s = stream_of(R0); R0 = !s || s->fail ? 0 : R0; }

static void emit(uint32_t stream, const char *text, uint32_t n) {
    Stream *s = stream_of(stream);
    if (s) stream_put(s, text, n);
#ifdef RT_ENGINE_LOG
    else plat_log("engine: %.*s", (int)n, text);
#endif
}
static void emitf(const char *fmt, ...) {
    char text[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    emit(R0, text, (uint32_t)n);
}
void imp__ZNSolsEi(void) { emitf("%d", (int)R1); }
void imp__ZNSolsEj(void) { emitf("%u", (unsigned)R1); }
void imp__ZNSolsEm(void) { emitf("%u", (unsigned)R1); }
void imp__ZNSolsEb(void) { emitf("%u", (unsigned)(R1 & 1)); }
void imp__ZNSolsEf(void) { emitf("%g", (double)rt_float(R1)); }
void imp__ZNSolsEPFRSoS_E(void) { if (R1 == RT_FAKE_FUNCS) emit(R0, "\n", 1); }
void imp__ZNSo5flushEv(void) { R0 = 0; }
void imp__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(void) { emit(R0, gstr(R1), (uint32_t)strlen(gstr(R1))); }
void imp__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKh(void) { emit(R0, gstr(R1), (uint32_t)strlen(gstr(R1))); }
void imp__ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E(void) { emit(R0, str_data(R1), str_len(R1)); }

/* ---- C library --------------------------------------------------------------------------- */
void imp__Znwm(void) { R0 = rt_alloc(R0); }
void imp__Znam(void) { R0 = rt_alloc(R0); }
void imp__ZdlPv(void) { rt_free(R0); R0 = 0; }
void imp__ZdaPv(void) { rt_free(R0); R0 = 0; }
void imp_memset(void) { memset(g_mem + R0, (int)(R1 & 255), R2); }
void imp_memmove(void) { if (R2) memmove(g_mem + R0, g_mem + R1, R2); }
void imp___cxa_atexit(void) { R0 = 0; }
void imp__Unwind_SjLj_Register(void) { R0 = 0; }
void imp__Unwind_SjLj_Unregister(void) { R0 = 0; }
void imp__Unwind_SjLj_Resume(void) { die("the engine raised a C++ exception"); }
void imp__ZSt17__throw_bad_allocv(void) { die("the engine ran out of memory (bad_alloc)"); }
void imp__ZSt20__throw_length_errorPKc(void) { die("the engine raised length_error: %s", gstr(R0)); }
void imp__ZSt20__throw_out_of_rangePKc(void) { die("the engine raised out_of_range: %s", gstr(R0)); }
void imp__ZNSt8ios_base4InitC1Ev(void) { R0 = 0; }
void imp_objc_msgSend(void) { R0 = 0; }
void imp_NSSearchPathForDirectoriesInDomains(void) { R0 = 0; }
void imp_xmlDocGetRootElement(void) { R0 = 0; }
void imp_xmlFreeDoc(void) { R0 = 0; }
void imp_xmlGetProp(void) { R0 = 0; }
void imp_xmlNodeGetContent(void) { R0 = 0; }
void imp_xmlReadFile(void) { R0 = 0; }

void imp___modsi3(void) { int32_t a = (int32_t)R0, b = (int32_t)R1; R0 = b == 0 || (a == INT32_MIN && b == -1) ? 0 : (uint32_t)(a % b); }
void imp___udivsi3(void) { R0 = R1 ? R0 / R1 : 0; }
void imp___umodsi3(void) { R0 = R1 ? R0 % R1 : 0; }

static double darg(int i) { union { double d; uint32_t w[2]; } x; x.w[0] = ARG(i); x.w[1] = ARG(i + 1); return x.d; }
static void dret(double d) { union { double d; uint32_t w[2]; } x; x.d = d; R0 = x.w[0]; R1 = x.w[1]; }
/* single-precision results are the double result rounded, as in the reference harness */
void imp_sin(void) { dret(sin(darg(0))); }
void imp_cos(void) { dret(cos(darg(0))); }
void imp_tan(void) { dret(tan(darg(0))); }
void imp_floor(void) { dret(floor(darg(0))); }
void imp_atan2(void) { dret(atan2(darg(0), darg(2))); }
void imp_sinf(void) { R0 = rt_fbits((float)sin((double)rt_float(R0))); }
void imp_tanf(void) { R0 = rt_fbits((float)tan((double)rt_float(R0))); }

/* Libc random(): the additive feedback generator of degree 31, as srandom() seeds it. */
static uint32_t sRand[31], sRandCalls;
static int sRandF, sRandR;
static uint32_t random_next(void) {
    sRand[sRandF] += sRand[sRandR];
    uint32_t out = (sRand[sRandF] >> 1) & 0x7fffffffu;
    sRandF = (sRandF + 1) % 31;
    sRandR = (sRandR + 1) % 31;
    sRandCalls++;
    return out;
}
void rt_srandom(uint32_t seed) {
    sRand[0] = seed;
    for (int i = 1; i < 31; i++) {
        int32_t x = (int32_t)sRand[i - 1];
        if (x == 0) x = 123459876;
        int32_t hi = x / 127773, lo = x % 127773;
        x = 16807 * lo - 2836 * hi;
        if (x < 0) x += 0x7fffffff;
        sRand[i] = (uint32_t)x;
    }
    sRandF = 3; sRandR = 0;
    for (int i = 0; i < 310; i++) random_next();
    sRandCalls = 0;
}
uint32_t rt_random_calls(void) { return sRandCalls; }
void imp_random(void) { R0 = random_next(); }

void imp_sprintf(void) {
    const char *fmt = gstr(R1);
    char out[512];
    uint32_t n = 0, word = 0;
#define NEXT_WORD() (word++, word == 1 ? R2 : word == 2 ? R3 : M32(SP + 4 * (word - 3)))
    for (const char *p = fmt; *p && n < sizeof out - 64; p++) {
        if (*p != '%') { out[n++] = *p; continue; }
        char spec[32];
        uint32_t k = 0;
        spec[k++] = *p++;
        while (*p && strchr("-+ 0#.123456789", *p) && k < sizeof spec - 4) spec[k++] = *p++;
        while (*p == 'l' || *p == 'h') p++;
        spec[k++] = *p; spec[k] = 0;
        if (*p == '%') out[n++] = '%';
        else if (*p == 'd' || *p == 'i') n += (uint32_t)snprintf(out + n, sizeof out - n, spec, (int)NEXT_WORD());
        else if (*p == 'u' || *p == 'x' || *p == 'X') n += (uint32_t)snprintf(out + n, sizeof out - n, spec, (unsigned)NEXT_WORD());
        else if (*p == 's') n += (uint32_t)snprintf(out + n, sizeof out - n, spec, gstr(NEXT_WORD()));
        else if (*p == 'f' || *p == 'g' || *p == 'e' || *p == 'G') {
            union { double d; uint32_t w[2]; } x;
            x.w[0] = NEXT_WORD(); x.w[1] = NEXT_WORD();
            n += (uint32_t)snprintf(out + n, sizeof out - n, spec, x.d);
        } else die("sprintf format %s", fmt);
    }
#undef NEXT_WORD
    out[n] = 0;
    memcpy(g_mem + R0, out, n + 1);
    R0 = n;
}

void imp___dynamic_cast(void) {
    uint32_t ptr = R0, target = R2;
    if (!ptr) { R0 = 0; return; }
    uint32_t info = M32(M32(ptr) - 4);
    for (int i = 0; i < 8; i++) {
        if (info == target) { R0 = ptr; return; }
        if (info < RT_IMAGE_LO || info >= RT_IMAGE_HI) break;
        uint32_t base = M32(info + 8);              /* single-inheritance type_info: the base class */
        if (base < 0xd9000u || base >= RT_IMAGE_HI || base == info) break;
        info = base;
    }
    R0 = 0;
}

/* ---- engine functions the port supplies ---------------------------------------------------- */
typedef struct { char key[56]; double value; } ConfigEntry;
static ConfigEntry *sConfig;
static int sConfigCount;

static void load_config(void) {
    uint32_t size = 0;
    char *text = plat_read_file("config.txt", &size, 0);
    if (!text) die("config.txt is missing");
    int lines = 0;
    for (uint32_t i = 0; i < size; i++) if (text[i] == '\n') lines++;
    sConfig = calloc((size_t)lines + 1, sizeof *sConfig);
    char *p = text, *end = text + size;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) nl = end;
        char *space = memchr(p, ' ', (size_t)(nl - p));
        if (space && space - p < (long)sizeof sConfig[0].key) {
            ConfigEntry *e = &sConfig[sConfigCount++];
            memcpy(e->key, p, (size_t)(space - p));
            char number[64];
            size_t n = (size_t)(nl - space - 1);
            if (n > sizeof number - 1) n = sizeof number - 1;
            memcpy(number, space + 1, n);
            number[n] = 0;
            char *stop;
            e->value = parse_number(number, &stop);
        }
        p = nl + 1;
    }
    free(text);
}

static double config_value(uint32_t string_object) {
    uint32_t len = str_len(string_object);
    const char *key = str_data(string_object);
    for (int i = 0; i < sConfigCount; i++)
        if (strlen(sConfig[i].key) == len && !memcmp(sConfig[i].key, key, len)) return sConfig[i].value;
    return 0;
}
void ovr__ZN12ImangiConfig12getBoolValueERKSs(void) { R0 = config_value(R0) != 0; }
void ovr__ZN12ImangiConfig13getFloatValueERKSs(void) { R0 = rt_fbits((float)config_value(R0)); }
void ovr__ZN12ImangiConfig11getIntValueERKSs(void) { R0 = (uint32_t)(int32_t)config_value(R0); }
void ovr__ZN17ImangiPreferences17isTutorialEnabledEv(void) { R0 = (uint32_t)rt_tutorial_enabled; }
void ovr__ZN17ImangiPreferences20setIsTutorialEnabledEb(void) { R0 = 0; }
void ovr__ZN17ImangiPreferences15savePreferencesEv(void) { R0 = 0; }
void ovr__Z24getPathForImangiResourceRKSsb(void) { set_string(R0, str_data(R1), str_len(R1)); }

/* Textures: the engine knows them by name and by the small number handed back here. */
typedef struct { char name[40]; unsigned host; } Texture;
static Texture sTextures[24];
static int sTextureCount;
unsigned rt_texture_host(uint32_t id) { return id >= 1 && id <= (uint32_t)sTextureCount ? sTextures[id - 1].host : 0; }
const char *rt_texture_name(uint32_t id) { return id >= 1 && id <= (uint32_t)sTextureCount ? sTextures[id - 1].name : ""; }
static void load_texture(int repeat) {
    char name[40], file[64];
    snprintf(name, sizeof name, "%.*s", (int)str_len(R1), str_data(R1));
    snprintf(file, sizeof file, "%.*s", (int)str_len(R2), str_data(R2));
    int i;
    for (i = 0; i < sTextureCount; i++) if (!strcmp(sTextures[i].name, name)) break;
    if (i == sTextureCount) {
        if (sTextureCount == 24) die("too many textures");
        snprintf(sTextures[sTextureCount++].name, sizeof sTextures[0].name, "%s", name);
    }
    sTextures[i].host = rt_host_load_texture(name, file, repeat);
    R0 = 1;
}
void ovr__ZN15cTextureManager14loadPVRTextureERKSsS1_b(void) { load_texture((int)(R3 & 1)); }
void ovr__ZN15cTextureManager11loadTextureERKSsS1_bbb(void) { load_texture((int)(R3 & 1)); }
void ovr__ZN15cTextureManager12getTextureIdESs(void) {
    uint32_t len = str_len(R1);
    const char *name = str_data(R1);
    for (int i = 0; i < sTextureCount; i++)
        if (strlen(sTextures[i].name) == len && !memcmp(sTextures[i].name, name, len)) { R0 = (uint32_t)i + 1; return; }
    R0 = 0;
}

/* Sound: the engine's sound manager is replaced whole; plays and stops go to the platform. */
static void sound_name(uint32_t string_object, char *out, size_t max) {
    snprintf(out, max, "%.*s", (int)str_len(string_object), str_data(string_object));
}
void ovr__ZN13cSoundManager11getInstanceEv(void) { R0 = RT_SOUND_MANAGER; }
void ovr__ZN13cSoundManager9playSoundESsbf(void) { char n[40]; sound_name(R1, n, sizeof n); rt_host_sound(n, (int)(R2 & 1), rt_float(R3), 0); R0 = 0; }
void ovr__ZN13cSoundManager16stopPlayingSoundESs(void) { char n[40]; sound_name(R1, n, sizeof n); rt_host_sound(n, 0, 1.0f, 1); R0 = 0; }
void ovr__ZN13cSoundManager9playSoundEmbf(void) { R0 = 0; }
void ovr__ZN13cSoundManager16stopPlayingSoundEm(void) { R0 = 0; }
void ovr__ZN13cSoundManager10getSoundIdESsPm(void) { R0 = 0; }
void ovr__ZN13cSoundManager15isEffectPlayingESs(void) { R0 = 0; }
void ovr__ZN13cSoundManager15isEffectPlayingEm(void) { R0 = 0; }
void ovr__ZN13cSoundManager16changeSoundPitchEmf(void) { R0 = 0; }
void ovr__ZN13cSoundManager19loadBackgroundMusicESsb(void) { R0 = 0; }
void ovr__ZN13cSoundManager19stopBackgroundMusicEv(void) { R0 = 0; }
void ovr__ZN13cSoundManager20pauseBackgroundMusicEv(void) { R0 = 0; }
void ovr__ZN13cSoundManager20startBackgroundMusicEv(void) { R0 = 0; }
void ovr__ZN13cSoundManager21clearSoundPlayedFlagsEv(void) { R0 = 0; }
void ovr__ZN13cSoundManager21unfadeBackgroundMusicEv(void) { R0 = 0; }
void ovr__ZN13cSoundManager21unloadBackgroundMusicEv(void) { R0 = 0; }
void ovr__ZN13cSoundManager22unpauseBackgroundMusicEv(void) { R0 = 0; }
void ovr__ZN13cSoundManager23isBackgroundMusicPausedEv(void) { R0 = 0; }
void ovr__ZN13cSoundManager24isBackgroundMusicPlayingEv(void) { R0 = 0; }
void ovr__ZN13cSoundManager32stopBackgroundMusicAfterNextPlayEv(void) { R0 = 0; }

/* ---- start-up ---------------------------------------------------------------------------- */
static void fill_rune_locale(uint32_t base) {
    enum { A = 0x100, C = 0x200, D = 0x400, G = 0x800, L = 0x1000, P = 0x2000, S = 0x4000, U = 0x8000, X = 0x10000,
           B = 0x20000, R = 0x40000 };
    for (int c = 0; c < 256; c++) {
        uint32_t t = 0, lower = (uint32_t)c, upper = (uint32_t)c;
        if (c < 128) {
            int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (alpha) {
                int low = c >= 'a';
                t |= A | G | R | (low ? L : U);
                if ((c | 32) >= 'a' && (c | 32) <= 'f') t |= X;
                lower = (uint32_t)(c | 32);
                upper = (uint32_t)(c & ~32);
            } else if (c >= '0' && c <= '9') t |= D | G | R | X;
            else if (c == ' ' || (c >= 9 && c <= 13)) t |= S | (c == ' ' ? B | R : 0) | (c == '\t' ? B : 0) | (c != ' ' ? C : 0);
            else if (c < 32 || c == 127) t |= C;
            else t |= P | G | R;
        }
        M32(base + 0x34 + 4 * (uint32_t)c) = t;
        M32(base + 0x434 + 4 * (uint32_t)c) = lower;
        M32(base + 0x834 + 4 * (uint32_t)c) = upper;
    }
}

int rt_init(uint32_t heap_bytes) {
    uint32_t size = 0;
    uint8_t *image = plat_read_file("engine.img", &size, 0);
    if (!image || size != RT_IMAGE_HI - RT_IMAGE_LO) { plat_log("engine.img is missing or the wrong size"); return 0; }
    uint32_t total = RT_HEAP + heap_bytes - RT_IMAGE_LO;
    sArena = calloc(1, total);
    if (!sArena) { plat_log("no memory for the engine (%u bytes)", (unsigned)total); return 0; }
    g_mem = sArena - RT_IMAGE_LO;
    memcpy(sArena, image, size);
    free(image);
    sBrk = RT_HEAP;
    sHeapEnd = RT_HEAP + heap_bytes;
    if (rt_literals_size > RT_STACK_LO - RT_LITERALS) { plat_log("too many code constants"); return 0; }
    memcpy(g_mem + RT_LITERALS, rt_literals, rt_literals_size);

    /* Imported data symbols each get a zeroed block; the few imported functions the engine
     * only passes around (std::endl and friends) get an address of their own. */
    uint32_t next = RT_EXTERN + 0x1000, fake = RT_FAKE_FUNCS;
    for (int i = 0; i < rt_extern_count; i++) {
        const char *name = rt_externs[i].name;
        uint32_t target = 0;
        for (int j = 0; j < i && !target; j++)
            if (!strcmp(rt_externs[j].name, name)) target = M32(rt_externs[j].addr);
        if (!target) {
            if (!strncmp(name, "__ZSt4endl", 10) || !strncmp(name, "__ZSt4ends", 10) || !strncmp(name, "__ZSt5flush", 11)) {
                target = fake;
                fake += 4;
            } else {
                target = next;
                if (!strcmp(name, "__DefaultRuneLocale")) { fill_rune_locale(next); next += 0x1000; }
                else next += 0x100;
            }
        }
        M32(rt_externs[i].addr) = target;
    }
    if (next > RT_SOUND_MANAGER) { plat_log("too many imported data symbols"); return 0; }
    load_config();
    rt_srandom(1);
    for (int i = 0; i < rt_static_init_count; i++) rt_invoke(rt_static_init[i], 0);
    return 1;
}
