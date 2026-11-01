/* rt.h — what the translated engine code (tools/recomp.py) runs on.
 *
 * The original engine is ARM code that expects a 32-bit address space laid out as in the
 * iOS app. The translation keeps that: "guest" addresses index one block of memory holding
 * the executable's data sections at their original addresses, a stack, and a heap. Each
 * translated function keeps the ARM registers in local variables; arguments, results and
 * the stack pointer pass between functions through the five globals below, following the
 * ARM calling convention the original code was compiled for. */
#ifndef TR_RT_H
#define TR_RT_H
#include <stdint.h>
#include <math.h>

typedef union { double d; uint64_t q; float f[2]; uint32_t u[2]; } vreg;

extern uint8_t *g_mem;                  /* guest address a is at g_mem + a */
extern uint32_t R0, R1, R2, R3, SP;     /* arguments and results of runtime calls; the guest stack pointer */

/* Translated functions take the four ARM argument registers as C arguments and return r0 and
 * r1 as one 64-bit value, so on the iPod a translated call is an ordinary ARM call. The guest
 * stack pointer and the memory base are shared by all of them: on ARM they live in two
 * reserved registers, elsewhere in ordinary globals. */
#if defined(RT_GENERATED) && defined(__arm__) && !defined(RT_NO_RESERVED_REGISTERS)
register uint8_t *rt_mem __asm__("r10");
register uint32_t rt_sp __asm__("r11");
#define RT_ENTER_SAVE() uint8_t *saved_mem = rt_mem; uint32_t saved_sp = rt_sp; rt_mem = g_mem; rt_sp = SP
#define RT_ENTER_RESTORE() SP = rt_sp; rt_mem = saved_mem; rt_sp = saved_sp
#define RT_SP_OUT() (SP = rt_sp)
#else
#define rt_mem g_mem
#define rt_sp SP
#define RT_ENTER_SAVE() ((void)0)
#define RT_ENTER_RESTORE() ((void)0)
#define RT_SP_OUT() ((void)0)
#endif

#ifdef RT_CHECK                         /* test builds: stop on an access outside guest memory */
uint8_t *rt_checked(uint32_t addr, uint32_t size);
#define RT_AT(a, n) rt_checked((uint32_t)(a), n)
#elif defined(RT_GENERATED)
#define RT_AT(a, n) (rt_mem + (uint32_t)(a))
#else
#define RT_AT(a, n) (g_mem + (uint32_t)(a))
#endif
#define M8(a) (*(uint8_t *)RT_AT(a, 1))
#define M16(a) (*(uint16_t *)RT_AT(a, 2))
#define M32(a) (*(uint32_t *)RT_AT(a, 4))
#define M64(a) (*(uint64_t *)RT_AT(a, 8))
#define MF(a) (*(float *)RT_AT(a, 4))

#ifdef RT_GENERATED
/* inline square root and absolute value even where the build turns built-ins off */
#define sqrtf(x) __builtin_sqrtf(x)
#define sqrt(x) __builtin_sqrt(x)
#define fabsf(x) __builtin_fabsf(x)
#define fabs(x) __builtin_fabs(x)
#define sp rt_sp
#define CALL(x) do { uint64_t ret_ = (x); r0 = (uint32_t)ret_; r1 = (uint32_t)(ret_ >> 32); } while (0)
/* runtime functions take and return through R0-R3 */
#define CALL_IMP(f) do { R0 = r0; R1 = r1; R2 = r2; R3 = r3; RT_SP_OUT(); f(); r0 = R0; r1 = R1; } while (0)
#define RETURN() return (uint64_t)r1 << 32 | r0
#ifdef RT_TRACE
void rt_trace(uint32_t addr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3);
#define ENTER(a) rt_trace(a, r0, r1, r2, r3)
#else
#define ENTER(a) ((void)0)
#endif
#endif

static inline uint32_t lsl_reg(uint32_t x, uint32_t n) { n &= 255; return n < 32 ? x << n : 0; }
static inline uint32_t lsr_reg(uint32_t x, uint32_t n) { n &= 255; return n < 32 ? x >> n : 0; }
static inline uint32_t asr_reg(uint32_t x, uint32_t n) { n &= 255; return (uint32_t)((int32_t)x >> (n < 32 ? n : 31)); }
static inline uint32_t lsl_reg_c(uint32_t x, uint32_t n, uint32_t c) { n &= 255; return n == 0 ? c : n <= 32 ? (x >> (32 - n)) & 1 : 0; }
static inline uint32_t lsr_reg_c(uint32_t x, uint32_t n, uint32_t c) { n &= 255; return n == 0 ? c : n <= 32 ? (x >> (n - 1)) & 1 : 0; }
static inline uint32_t asr_reg_c(uint32_t x, uint32_t n, uint32_t c) { n &= 255; return n == 0 ? c : n <= 32 ? (x >> (n - 1)) & 1 : x >> 31; }
static inline uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
/* float to integer as the ARM instructions do it: toward zero, saturating, NaN to 0 */
static inline int32_t f2i(double x) { return x != x ? 0 : x >= 2147483647.0 ? 2147483647 : x <= -2147483648.0 ? (int32_t)0x80000000 : (int32_t)x; }
static inline uint32_t f2u(double x) { return x != x || x <= 0.0 ? 0 : x >= 4294967295.0 ? 0xffffffffu : (uint32_t)x; }
static inline float neon_min(float a, float b) { return a < b ? a : b; }
static inline float neon_max(float a, float b) { return a > b ? a : b; }

typedef struct { uint32_t addr; uint64_t (*fn)(uint32_t, uint32_t, uint32_t, uint32_t); } rt_func;
typedef struct { const char *name; uint32_t addr; } rt_symbol;
typedef struct { uint32_t addr; const char *name; } rt_extern;
extern const rt_func rt_functions[];
extern const int rt_function_count;
extern const uint32_t rt_static_init[];
extern const int rt_static_init_count;
extern const rt_symbol rt_symbols[];
extern const int rt_symbol_count;
extern const uint8_t rt_literals[];
extern const uint32_t rt_literals_size;
extern const rt_extern rt_externs[];
extern const int rt_extern_count;

uint64_t rt_call(uint32_t addr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3);
uint64_t rt_enter(uint32_t addr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3);
void rt_bad_call(uint32_t addr);
void rt_note_mesh(uint32_t mesh);       /* cMesh3D::createVertexBufferObjects is starting for this mesh */
void rt_bad_jump(uint32_t addr);
void rt_missing(const char *name);

/* ---- the layout of guest memory (the reference harness uses the same) -------------------- */
#define RT_IMAGE_LO 0x000ba000u         /* string and constant sections, then the data segment */
#define RT_IMAGE_HI 0x000ea000u
#define RT_EXTERN 0x000ea000u           /* stand-ins for imported data symbols */
#define RT_FAKE_VTABLE 0x000f3e20u
#define RT_SOUND_MANAGER 0x000f3c00u
#define RT_FAKE_FUNCS 0x000f3f00u       /* addresses for imported functions that are only passed around */
#define RT_LITERALS 0x000f4000u         /* constants that sat among the original code */
#define RT_STACK_LO 0x000f5000u
#define RT_STACK_TOP 0x00100000u        /* 44 KB; the game uses about 11 */
#define RT_HEAP 0x00100000u

/* ---- for the port's own code --------------------------------------------------------------- */
/* Set up guest memory from the engine image file and run the static initialisers.
 * heap_bytes is the size of the guest heap. Returns 0 on failure. */
int rt_init(uint32_t heap_bytes);
uint32_t rt_lookup(const char *name);
/* Call an engine function with up to four word arguments (floats as their bits), then any
 * further ones on the stack. */
uint32_t rt_invoke(uint32_t addr, int argc, ...);
uint32_t rt_alloc(uint32_t size);
void rt_free(uint32_t addr);
int rt_pin(uint32_t addr, uint32_t size);
void rt_unpin(uint32_t addr);
uint32_t rt_heap_used(void);
uint32_t rt_heap_peak(void);                /* the most the heap has spanned, in bytes */
uint32_t rt_heap_break(void);               /* the guest address just past the last block */
uint32_t rt_stack_low(void);
void rt_srandom(uint32_t seed);
uint32_t rt_random_calls(void);
const char *rt_guest_string(uint32_t string_object, uint32_t *length);
static inline uint32_t rt_fbits(float f) { union { float f; uint32_t u; } x; x.f = f; return x.u; }
static inline float rt_float(uint32_t u) { union { float f; uint32_t u; } x; x.u = u; return x.f; }
extern double rt_clock;                 /* what the engine reads as the time, in seconds */
extern int rt_tutorial_enabled;
extern float rt_display_scale;          /* the panel's width over 320: see DISPLAY_OFFSETS in tools/recomp.py */

/* Hooks the platform layer supplies. */
unsigned rt_host_load_texture(const char *name, const char *file, int repeat);
void rt_host_sound(const char *name, int loop, float pitch, int stop);
#endif
