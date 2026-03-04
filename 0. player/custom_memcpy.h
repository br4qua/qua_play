#include <immintrin.h> // Provides INTRINSICS
// Custom Implementation of memcpy - AVX2 intrinsics, 4Kb aligned
// UNSAFE - use _mm_sfense prevents race condition.
// ----------------------------------------------------------------------
// --- Preprocessor Calculations (Used for the OFFSET_8_BYTES constant) ---
#define SAMPLE_SIZE (sizeof(sample_t))
#define OFFSET_8_BYTES (8 * FACTOR_MULTIPLE * SAMPLE_SIZE) // 32 bytes
#define LOOP_STEP_SAMPLES (32 * FACTOR_MULTIPLE)           // Total samples per iteration
#define LOOP_STEP_BYTES_V2 (256)
#define LOOP_512 (512)



#if TARGET_SAMPLE_RATE >= 88000
    #if TARGET_BITDEPTH == 32
        #define LOOP_INIT_ASM_INSTRUCTION "movq $1024, %%rdx\n\t"
    #else
        #define LOOP_INIT_ASM_INSTRUCTION "movq $512, %%rdx\n\t"
    #endif
#else
    #if TARGET_BITDEPTH == 32
        #define LOOP_INIT_ASM_INSTRUCTION "movq $512, %%rdx\n\t"
    #else
        #define LOOP_INIT_ASM_INSTRUCTION "movq $256, %%rdx\n\t"
    #endif
#endif

#if TARGET_SAMPLE_RATE >= 88000
    #if TARGET_BITDEPTH == 32
        #define LOOP_COUNT 1024
    #else
        #define LOOP_COUNT 512
    #endif
#else
    #if TARGET_BITDEPTH == 32
        #define LOOP_COUNT 512
    #else
        #define LOOP_COUNT 256
    #endif
#endif

// currently will cause 1 register spill from gcc because it
// "discovers" is the post-memcpy src reuse, which backfires by creating the 9th value.
__attribute__((always_inline))
static inline void avx2_stream_copy_intrinsic(
    void * restrict dst,
    const void * restrict src)
{
    __m256i *d = (__m256i *)__builtin_assume_aligned(dst, 32);
    const __m256i *s = (const __m256i *)__builtin_assume_aligned(src, 32);

    for (size_t i = 0; i < LOOP_COUNT * 2; i++) {
        size_t off = i * 8;

        __m256i y0 = _mm256_stream_load_si256(s + off);
        __m256i y1 = _mm256_stream_load_si256(s + off + 1);
        __m256i y2 = _mm256_stream_load_si256(s + off + 2);
        __m256i y3 = _mm256_stream_load_si256(s + off + 3);
        __m256i y4 = _mm256_stream_load_si256(s + off + 4);
        __m256i y5 = _mm256_stream_load_si256(s + off + 5);
        __m256i y6 = _mm256_stream_load_si256(s + off + 6);
        __m256i y7 = _mm256_stream_load_si256(s + off + 7);

        _mm256_stream_si256(d + off, y0);
        _mm256_stream_si256(d + off + 1, y1);
        _mm256_stream_si256(d + off + 2, y2);
        _mm256_stream_si256(d + off + 3, y3);
        _mm256_stream_si256(d + off + 4, y4);
        _mm256_stream_si256(d + off + 5, y5);
        _mm256_stream_si256(d + off + 6, y6);
        _mm256_stream_si256(d + off + 7, y7);
    }

    _mm_sfence();
}

__attribute__((always_inline, optimize("-fno-rename-registers")))
static inline void avx2_stream_copy_zero_x86_x8(
    void *dst,
    const void *src)
{
    uint8_t *d = (uint8_t *)__builtin_assume_aligned(dst, ALIGN_4K);
    const uint8_t *s = (const uint8_t *)__builtin_assume_aligned(src, ALIGN_4K);

    __asm__ __volatile__("" ::: "memory");

    __asm__ __volatile__(
        LOOP_INIT_ASM_INSTRUCTION
        "addq %%rdx, %%rdx\n\t"
        ".align 32\n\t"
        "1:\n\t"
        "vmovntdqa   (%%rsi), %%ymm0\n\t"
        "vmovntdqa   32(%%rsi), %%ymm1\n\t"
        "vmovntdqa   64(%%rsi), %%ymm2\n\t"
        "vmovntdqa   96(%%rsi), %%ymm3\n\t"
        "vmovntdqa   128(%%rsi), %%ymm4\n\t"
        "vmovntdqa   160(%%rsi), %%ymm5\n\t"
        "vmovntdqa   192(%%rsi), %%ymm6\n\t"
        "vmovntdqa   224(%%rsi), %%ymm7\n\t"
        "vmovntdq    %%ymm0, (%%rdi)\n\t"
        "vmovntdq    %%ymm1, 32(%%rdi)\n\t"
        "vmovntdq    %%ymm2, 64(%%rdi)\n\t"
        "vmovntdq    %%ymm3, 96(%%rdi)\n\t"
        "vmovntdq    %%ymm4, 128(%%rdi)\n\t"
        "vmovntdq    %%ymm5, 160(%%rdi)\n\t"
        "vmovntdq    %%ymm6, 192(%%rdi)\n\t"
        "vmovntdq    %%ymm7, 224(%%rdi)\n\t"
        "addq        $256, %%rsi\n\t"
        "addq        $256, %%rdi\n\t"
        "decq        %%rdx\n\t"
        "jnz         1b\n\t"


        : "+D"(d), "+S"(s)
        :
        : "rdx", "ymm0", "ymm1", "ymm2", "ymm3",
          "ymm4", "ymm5", "ymm6", "ymm7",
          "memory", "cc");

    __asm__ __volatile__("" ::: "memory");
}

__attribute__((always_inline))
static inline void avx512_stream_copy_intrinsic(
    void * restrict dst,
    const void * restrict src)
{
    __m512i *d = (__m512i *)__builtin_assume_aligned(dst, 64);
    const __m512i *s = (const __m512i *)__builtin_assume_aligned(src, 64);

    for (size_t i = 0; i < LOOP_COUNT; i++) {
        size_t off = i * 8;

        __m512i z0 = _mm512_stream_load_si512(s + off);
        __m512i z1 = _mm512_stream_load_si512(s + off + 1);
        __m512i z2 = _mm512_stream_load_si512(s + off + 2);
        __m512i z3 = _mm512_stream_load_si512(s + off + 3);
        __m512i z4 = _mm512_stream_load_si512(s + off + 4);
        __m512i z5 = _mm512_stream_load_si512(s + off + 5);
        __m512i z6 = _mm512_stream_load_si512(s + off + 6);
        __m512i z7 = _mm512_stream_load_si512(s + off + 7);

        _mm512_stream_si512(d + off, z0);
        _mm512_stream_si512(d + off + 1, z1);
        _mm512_stream_si512(d + off + 2, z2);
        _mm512_stream_si512(d + off + 3, z3);
        _mm512_stream_si512(d + off + 4, z4);
        _mm512_stream_si512(d + off + 5, z5);
        _mm512_stream_si512(d + off + 6, z6);
        _mm512_stream_si512(d + off + 7, z7);
    }

    _mm_sfence();
}

__attribute__((always_inline, optimize("-fno-rename-registers")))
static inline void avx512_stream_copy_zero_x86(
    void * restrict dst,
    const void * restrict src)
{
    uint8_t *d = (uint8_t *)__builtin_assume_aligned(dst, ALIGN_4K);
    const uint8_t *s = (const uint8_t *)__builtin_assume_aligned(src, ALIGN_4K);
    
    // Compiler barrier to prevent reordering before asm block
    __asm__ __volatile__("" ::: "memory");
    
    __asm__ __volatile__(
        LOOP_INIT_ASM_INSTRUCTION
        ".align 64\n\t"
        "1:\n\t"
        "vmovntdqa   (%%rsi), %%zmm0\n\t"
        "vmovntdqa   64(%%rsi), %%zmm1\n\t"
        "vmovntdqa   128(%%rsi), %%zmm2\n\t"
        "vmovntdqa   192(%%rsi), %%zmm3\n\t"
        "vmovntdqa   256(%%rsi), %%zmm4\n\t"
        "vmovntdqa   320(%%rsi), %%zmm5\n\t"
        "vmovntdqa   384(%%rsi), %%zmm6\n\t"
        "vmovntdqa   448(%%rsi), %%zmm7\n\t"
        "vmovntdq    %%zmm0, (%%rdi)\n\t"
        "vmovntdq    %%zmm1, 64(%%rdi)\n\t"
        "vmovntdq    %%zmm2, 128(%%rdi)\n\t"
        "vmovntdq    %%zmm3, 192(%%rdi)\n\t"
        "vmovntdq    %%zmm4, 256(%%rdi)\n\t"
        "vmovntdq    %%zmm5, 320(%%rdi)\n\t"
        "vmovntdq    %%zmm6, 384(%%rdi)\n\t"
        "vmovntdq    %%zmm7, 448(%%rdi)\n\t"
        "addq        $512, %%rsi\n\t"
        "addq        $512, %%rdi\n\t"
        "decq        %%rdx\n\t"
        "jnz         1b\n\t"
        
        : "+D"(d), "+S"(s)
        :
        : "rdx", "zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7",
          "memory", "cc");
    
    // Compiler barrier after asm block
    __asm__ __volatile__("" ::: "memory");
}

__attribute__((always_inline, optimize("-fno-rename-registers")))
static inline void avx2_stream_copy_zero_x86_x16(
    void *dst,
    const void *src)
{
    uint8_t *d = (uint8_t *)__builtin_assume_aligned(dst, ALIGN_4K);
    const uint8_t *s = (const uint8_t *)__builtin_assume_aligned(src, ALIGN_4K);
    
    // Option B: Compiler barrier to prevent reordering before asm block
    __asm__ __volatile__("" ::: "memory");
    
    // Option A: Explicit register constraints with +D and +S
    __asm__ __volatile__(
        LOOP_INIT_ASM_INSTRUCTION
        ".align 32\n\t"
        "1:\n\t"
        "vmovntdqa   (%%rsi), %%ymm0\n\t"
        "vmovntdqa   32(%%rsi), %%ymm1\n\t"
        "vmovntdqa   64(%%rsi), %%ymm2\n\t"
        "vmovntdqa   96(%%rsi), %%ymm3\n\t"
        "vmovntdqa   128(%%rsi), %%ymm4\n\t"
        "vmovntdqa   160(%%rsi), %%ymm5\n\t"
        "vmovntdqa   192(%%rsi), %%ymm6\n\t"
        "vmovntdqa   224(%%rsi), %%ymm7\n\t"
        "vmovntdqa   256(%%rsi), %%ymm8\n\t"
        "vmovntdqa   288(%%rsi), %%ymm9\n\t"
        "vmovntdqa   320(%%rsi), %%ymm10\n\t"
        "vmovntdqa   352(%%rsi), %%ymm11\n\t"
        "vmovntdqa   384(%%rsi), %%ymm12\n\t"
        "vmovntdqa   416(%%rsi), %%ymm13\n\t"
        "vmovntdqa   448(%%rsi), %%ymm14\n\t"
        "vmovntdqa   480(%%rsi), %%ymm15\n\t"
        "vmovntdq    %%ymm0, (%%rdi)\n\t"
        "vmovntdq    %%ymm1, 32(%%rdi)\n\t"
        "vmovntdq    %%ymm2, 64(%%rdi)\n\t"
        "vmovntdq    %%ymm3, 96(%%rdi)\n\t"
        "vmovntdq    %%ymm4, 128(%%rdi)\n\t"
        "vmovntdq    %%ymm5, 160(%%rdi)\n\t"
        "vmovntdq    %%ymm6, 192(%%rdi)\n\t"
        "vmovntdq    %%ymm7, 224(%%rdi)\n\t"
        "vmovntdq    %%ymm8, 256(%%rdi)\n\t"
        "vmovntdq    %%ymm9, 288(%%rdi)\n\t"
        "vmovntdq    %%ymm10, 320(%%rdi)\n\t"
        "vmovntdq    %%ymm11, 352(%%rdi)\n\t"
        "vmovntdq    %%ymm12, 384(%%rdi)\n\t"
        "vmovntdq    %%ymm13, 416(%%rdi)\n\t"
        "vmovntdq    %%ymm14, 448(%%rdi)\n\t"
        "vmovntdq    %%ymm15, 480(%%rdi)\n\t"
        "addq        $512, %%rsi\n\t"
        "addq        $512, %%rdi\n\t"
        "decq        %%rdx\n\t"
        "jnz         1b\n\t"

        
        : "+D"(d), "+S"(s)  // Option A: Explicit RDI/RSI constraints (input/output)
        :
        : "rdx", "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7",
          "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15", 
          "memory", "cc");
    
    // Option B: Another compiler barrier after asm block (optional but safe)
    __asm__ __volatile__("" ::: "memory");
}

static __attribute__((always_inline)) inline void avx2_intrin_stream_store_x86_4x(sample_t *dst, const sample_t *src)
{
  dst = (sample_t *)__builtin_assume_aligned(dst, ALIGN_4K);
  src = (const sample_t *)__builtin_assume_aligned(src, ALIGN_4K);
  for (int i = 0; i < (int)(SAMPLES_PER_FRAME * FRAMES_PER_PERIOD); i += 32 * FACTOR_MULTIPLE)
  {
    const __m256i data0 = _mm256_load_si256((const __m256i *)(src + i));
    const __m256i data1 = _mm256_load_si256((const __m256i *)(src + i + 8 * FACTOR_MULTIPLE));
    const __m256i data2 = _mm256_load_si256((const __m256i *)(src + i + 16 * FACTOR_MULTIPLE));
    const __m256i data3 = _mm256_load_si256((const __m256i *)(src + i + 24 * FACTOR_MULTIPLE));
    _mm256_stream_si256((__m256i *)(dst + i), data0);
    _mm256_stream_si256((__m256i *)(dst + i + 8 * FACTOR_MULTIPLE), data1);
    _mm256_stream_si256((__m256i *)(dst + i + 16 * FACTOR_MULTIPLE), data2);
    _mm256_stream_si256((__m256i *)(dst + i + 24 * FACTOR_MULTIPLE), data3);
  }
}
