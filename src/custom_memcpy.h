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


__attribute__((always_inline, optimize("-fno-rename-registers")))
static inline void avx512_stream_copy_zero(
    void *dst,
    const void *src)
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
static inline void avx2_stream_copy_zero(
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

static __attribute__((always_inline)) inline void avx2_stream_copy_zero_unsafe(
    void *dst,
    const void *src)
{
    register uint8_t *d __asm__("rdi") = (uint8_t *)__builtin_assume_aligned(dst, ALIGN_4K);
    register const uint8_t *s __asm__("rsi") = (const uint8_t *)__builtin_assume_aligned(src, ALIGN_4K);
    
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
        
        :  // No outputs
        : "r"(d), "r"(s)
        : "rdx", "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7",
          "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15", "memory", "cc");
          // NOTE: NO "rdi", "rsi" in clobber list!
}

// static __attribute__((always_inline)) inline void avx2_stream_copy_outputs(
//     void *dst,
//     const void *src)
// {
//     uint8_t *d = (uint8_t *)__builtin_assume_aligned(dst, ALIGN_4K);
//     const uint8_t *s = (const uint8_t *)__builtin_assume_aligned(src, ALIGN_4K);
//     
//     __asm__ __volatile__(
//         LOOP_INIT_ASM_INSTRUCTION
//         ".align 32\n\t"
//         "1:\n\t"
//         "vmovntdqa   (%%rsi), %%ymm0\n\t"
//         "vmovntdqa   32(%%rsi), %%ymm1\n\t"
//         "vmovntdqa   64(%%rsi), %%ymm2\n\t"
//         "vmovntdqa   96(%%rsi), %%ymm3\n\t"
//         "vmovntdqa   128(%%rsi), %%ymm4\n\t"
//         "vmovntdqa   160(%%rsi), %%ymm5\n\t"
//         "vmovntdqa   192(%%rsi), %%ymm6\n\t"
//         "vmovntdqa   224(%%rsi), %%ymm7\n\t"
//         "vmovntdqa   256(%%rsi), %%ymm8\n\t"
//         "vmovntdqa   288(%%rsi), %%ymm9\n\t"
//         "vmovntdqa   320(%%rsi), %%ymm10\n\t"
//         "vmovntdqa   352(%%rsi), %%ymm11\n\t"
//         "vmovntdqa   384(%%rsi), %%ymm12\n\t"
//         "vmovntdqa   416(%%rsi), %%ymm13\n\t"
//         "vmovntdqa   448(%%rsi), %%ymm14\n\t"
//         "vmovntdqa   480(%%rsi), %%ymm15\n\t"
//         "vmovntdq    %%ymm0, (%%rdi)\n\t"
//         "vmovntdq    %%ymm1, 32(%%rdi)\n\t"
//         "vmovntdq    %%ymm2, 64(%%rdi)\n\t"
//         "vmovntdq    %%ymm3, 96(%%rdi)\n\t"
//         "vmovntdq    %%ymm4, 128(%%rdi)\n\t"
//         "vmovntdq    %%ymm5, 160(%%rdi)\n\t"
//         "vmovntdq    %%ymm6, 192(%%rdi)\n\t"
//         "vmovntdq    %%ymm7, 224(%%rdi)\n\t"
//         "vmovntdq    %%ymm8, 256(%%rdi)\n\t"
//         "vmovntdq    %%ymm9, 288(%%rdi)\n\t"
//         "vmovntdq    %%ymm10, 320(%%rdi)\n\t"
//         "vmovntdq    %%ymm11, 352(%%rdi)\n\t"
//         "vmovntdq    %%ymm12, 384(%%rdi)\n\t"
//         "vmovntdq    %%ymm13, 416(%%rdi)\n\t"
//         "vmovntdq    %%ymm14, 448(%%rdi)\n\t"
//         "vmovntdq    %%ymm15, 480(%%rdi)\n\t"
//         "addq        $512, %%rsi\n\t"
//         "addq        $512, %%rdi\n\t"
//         "decq        %%rdx\n\t"
//         "jnz         1b\n\t"
//            : [dst_out] "=&D"(d),
//           [src_out] "=&S"(s)
//         // Inputs  
//         : [dst_in] "0"(d),
//           [src_in] "1"(s)
//         : "rdx", "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7",
//           "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15", "memory", "cc");
// }
// 
// static __attribute__((always_inline)) inline void avx2_asm_stream_both_16ymm(sample_t *dst, const sample_t *src)
// {
//   const size_t total_bytes = SAMPLES_PER_FRAME * FRAMES_PER_PERIOD * sizeof(sample_t);
//   // Calculate number of 512-byte blocks
//   size_t num_blocks = total_bytes / LOOP_512;
// 
//   dst = (sample_t *)__builtin_assume_aligned(dst, ALIGN_4K);
//   src = (const sample_t *)__builtin_assume_aligned(src, ALIGN_4K);
// 
//   __asm__ __volatile__(
//       // RDX = total number of 512-byte blocks
//       "movq %[num_blocks], %%rdx\n\t"
//       ".align 32\n\t"
//       "1:\n\t"
// 
//       // === Load 16 x 256-bit (512 bytes) from SRC with streaming loads ===
//       // First 8 (0-255 bytes)
//       "vmovntdqa   (%%rsi), %%ymm0\n\t"
//       "vmovntdqa   32(%%rsi), %%ymm1\n\t"
//       "vmovntdqa   64(%%rsi), %%ymm2\n\t"
//       "vmovntdqa   96(%%rsi), %%ymm3\n\t"
//       "vmovntdqa   128(%%rsi), %%ymm4\n\t"
//       "vmovntdqa   160(%%rsi), %%ymm5\n\t"
//       "vmovntdqa   192(%%rsi), %%ymm6\n\t"
//       "vmovntdqa   224(%%rsi), %%ymm7\n\t"
// 
//       // Second 8 (256-511 bytes)
//       "vmovntdqa   256(%%rsi), %%ymm8\n\t"
//       "vmovntdqa   288(%%rsi), %%ymm9\n\t"
//       "vmovntdqa   320(%%rsi), %%ymm10\n\t"
//       "vmovntdqa   352(%%rsi), %%ymm11\n\t"
//       "vmovntdqa   384(%%rsi), %%ymm12\n\t"
//       "vmovntdqa   416(%%rsi), %%ymm13\n\t"
//       "vmovntdqa   448(%%rsi), %%ymm14\n\t"
//       "vmovntdqa   480(%%rsi), %%ymm15\n\t"
// 
//       // === Store 16 x 256-bit (512 bytes) to DST (VMOVNTDQ) ===
//       // First 8 (0-255 bytes)
//       "vmovntdq    %%ymm0, (%%rdi)\n\t"
//       "vmovntdq    %%ymm1, 32(%%rdi)\n\t"
//       "vmovntdq    %%ymm2, 64(%%rdi)\n\t"
//       "vmovntdq    %%ymm3, 96(%%rdi)\n\t"
//       "vmovntdq    %%ymm4, 128(%%rdi)\n\t"
//       "vmovntdq    %%ymm5, 160(%%rdi)\n\t"
//       "vmovntdq    %%ymm6, 192(%%rdi)\n\t"
//       "vmovntdq    %%ymm7, 224(%%rdi)\n\t"
// 
//       // Second 8 (256-511 bytes)
//       "vmovntdq    %%ymm8, 256(%%rdi)\n\t"
//       "vmovntdq    %%ymm9, 288(%%rdi)\n\t"
//       "vmovntdq    %%ymm10, 320(%%rdi)\n\t"
//       "vmovntdq    %%ymm11, 352(%%rdi)\n\t"
//       "vmovntdq    %%ymm12, 384(%%rdi)\n\t"
//       "vmovntdq    %%ymm13, 416(%%rdi)\n\t"
//       "vmovntdq    %%ymm14, 448(%%rdi)\n\t"
//       "vmovntdq    %%ymm15, 480(%%rdi)\n\t"
// 
//       // === Update Pointers and Counter (Advance by 512 bytes) ===
//       "addq        $512, %%rsi\n\t"
//       "addq        $512, %%rdi\n\t"
//       "decq        %%rdx\n\t"
//       "jnz         1b\n\t"
// 
//       // Outputs
//       : [dst_out] "=&D"(dst),
//         [src_out] "=&S"(src)
//       // Inputs
//       : [dst_in] "0"(dst),
//         [src_in] "1"(src),
//         [num_blocks] "r"(num_blocks)
//       // Clobbers: Now using ymm0-ymm15
//       : "rdx", "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7",
//         "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15", "cc");
// }
// static __attribute__((always_inline)) inline void avx2_asm_stream_store_16ymm(sample_t *dst, const sample_t *src)
// {
//   const size_t total_bytes = SAMPLES_PER_FRAME * FRAMES_PER_PERIOD * sizeof(sample_t);
//   // Calculate number of 512-byte blocks
//   size_t num_blocks = total_bytes / LOOP_512;
// 
//   dst = (sample_t *)__builtin_assume_aligned(dst, ALIGN_4K);
//   src = (const sample_t *)__builtin_assume_aligned(src, ALIGN_4K);
// 
//   __asm__ __volatile__(
//       // RDX = total number of 512-byte blocks
//       "movq %[num_blocks], %%rdx\n\t"
//       ".align 32\n\t" // <-- ADD THIS LINE
//       "1:\n\t"
// 
//       // === Load 16 x 256-bit (512 bytes) from SRC ===
//       // First 8 (0-255 bytes)
//       "vmovdqa     (%%rsi), %%ymm0\n\t"
//       "vmovdqa     32(%%rsi), %%ymm1\n\t"
//       "vmovdqa     64(%%rsi), %%ymm2\n\t"
//       "vmovdqa     96(%%rsi), %%ymm3\n\t"
//       "vmovdqa     128(%%rsi), %%ymm4\n\t"
//       "vmovdqa     160(%%rsi), %%ymm5\n\t"
//       "vmovdqa     192(%%rsi), %%ymm6\n\t"
//       "vmovdqa     224(%%rsi), %%ymm7\n\t"
// 
//       // Second 8 (256-511 bytes)
//       "vmovdqa     256(%%rsi), %%ymm8\n\t"
//       "vmovdqa     288(%%rsi), %%ymm9\n\t"
//       "vmovdqa     320(%%rsi), %%ymm10\n\t"
//       "vmovdqa     352(%%rsi), %%ymm11\n\t"
//       "vmovdqa     384(%%rsi), %%ymm12\n\t"
//       "vmovdqa     416(%%rsi), %%ymm13\n\t"
//       "vmovdqa     448(%%rsi), %%ymm14\n\t"
//       "vmovdqa     480(%%rsi), %%ymm15\n\t"
// 
//       // === Store 16 x 256-bit (512 bytes) to DST (VMOVNTDQ) ===
//       // First 8 (0-255 bytes)
//       "vmovntdq    %%ymm0, (%%rdi)\n\t"
//       "vmovntdq    %%ymm1, 32(%%rdi)\n\t"
//       "vmovntdq    %%ymm2, 64(%%rdi)\n\t"
//       "vmovntdq    %%ymm3, 96(%%rdi)\n\t"
//       "vmovntdq    %%ymm4, 128(%%rdi)\n\t"
//       "vmovntdq    %%ymm5, 160(%%rdi)\n\t"
//       "vmovntdq    %%ymm6, 192(%%rdi)\n\t"
//       "vmovntdq    %%ymm7, 224(%%rdi)\n\t"
// 
//       // Second 8 (256-511 bytes)
//       "vmovntdq    %%ymm8, 256(%%rdi)\n\t"
//       "vmovntdq    %%ymm9, 288(%%rdi)\n\t"
//       "vmovntdq    %%ymm10, 320(%%rdi)\n\t"
//       "vmovntdq    %%ymm11, 352(%%rdi)\n\t"
//       "vmovntdq    %%ymm12, 384(%%rdi)\n\t"
//       "vmovntdq    %%ymm13, 416(%%rdi)\n\t"
//       "vmovntdq    %%ymm14, 448(%%rdi)\n\t"
//       "vmovntdq    %%ymm15, 480(%%rdi)\n\t"
// 
//       // === Update Pointers and Counter (Advance by 512 bytes) ===
//       "addq        $512, %%rsi\n\t"
//       "addq        $512, %%rdi\n\t"
// 
//       "decq        %%rdx\n\t"
//       "jnz         1b\n\t"
// 
//       // Outputs
//       : [dst_out] "=&D"(dst),
//         [src_out] "=&S"(src)
// 
//       // Inputs
//       : [dst_in] "0"(dst),
//         [src_in] "1"(src),
//         [num_blocks] "r"(num_blocks)
// 
//       // Clobbers: Now using ymm0-ymm15
//       : "rdx", "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7",
//         "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15", "cc");
// }
// 
// static __attribute__((always_inline)) inline void avx2_asm_stream_store_8ymm(sample_t *dst, const sample_t *src)
// {
//   const size_t total_bytes = SAMPLES_PER_FRAME * FRAMES_PER_PERIOD * sizeof(sample_t);
//   size_t num_blocks = total_bytes / LOOP_STEP_BYTES_V2;
// 
//   dst = (sample_t *)__builtin_assume_aligned(dst, ALIGN_4K);
//   src = (const sample_t *)__builtin_assume_aligned(src, ALIGN_4K);
// 
//   __asm__ __volatile__(
//       // RDX = total number of 256-byte blocks
//       "movq %[num_blocks], %%rdx\n\t"
// 
//       "1:\n\t"
// 
//       // === Load 8 x 256-bit (256 bytes) from SRC ===
//       "vmovdqa     (%%rsi), %%ymm0\n\t"
//       "vmovdqa     32(%%rsi), %%ymm1\n\t"
//       "vmovdqa     64(%%rsi), %%ymm2\n\t"
//       "vmovdqa     96(%%rsi), %%ymm3\n\t"
//       "vmovdqa     128(%%rsi), %%ymm4\n\t"
//       "vmovdqa     160(%%rsi), %%ymm5\n\t"
//       "vmovdqa     192(%%rsi), %%ymm6\n\t"
//       "vmovdqa     224(%%rsi), %%ymm7\n\t"
// 
//       // === Store 8 x 256-bit (256 bytes) to DST (VMOVNTDQ) ===
//       "vmovntdq    %%ymm0, (%%rdi)\n\t"
//       "vmovntdq    %%ymm1, 32(%%rdi)\n\t"
//       "vmovntdq    %%ymm2, 64(%%rdi)\n\t"
//       "vmovntdq    %%ymm3, 96(%%rdi)\n\t"
//       "vmovntdq    %%ymm4, 128(%%rdi)\n\t"
//       "vmovntdq    %%ymm5, 160(%%rdi)\n\t"
//       "vmovntdq    %%ymm6, 192(%%rdi)\n\t"
//       "vmovntdq    %%ymm7, 224(%%rdi)\n\t"
// 
//       // === Update Pointers and Counter ===
//       "addq        $256, %%rsi\n\t"
//       "addq        $256, %%rdi\n\t"
// 
//       "decq        %%rdx\n\t"
//       "jnz         1b\n\t"
// 
//       // Outputs
//       : [dst_out] "=&D"(dst),
//         [src_out] "=&S"(src)
// 
//       // Inputs
//       : [dst_in] "0"(dst),
//         [src_in] "1"(src),
//         [num_blocks] "r"(num_blocks)
// 
//       // Clobbers
//       : "rdx", "ymm0", "ymm1", "ymm2", "ymm3",
//         "ymm4", "ymm5", "ymm6", "ymm7", "cc");
// }
// static __attribute__((always_inline)) inline void avx2_asm_stream_store_4ymm(sample_t *dst, const sample_t *src)
// {
//   const size_t total_samples = SAMPLES_PER_FRAME * FRAMES_PER_PERIOD;
//   size_t num_blocks = total_samples / LOOP_STEP_SAMPLES;
// 
//   dst = (sample_t *)__builtin_assume_aligned(dst, ALIGN_4K);
//   src = (const sample_t *)__builtin_assume_aligned(src, ALIGN_4K);
// 
//   __asm__ __volatile__(
//       // RDX = total number of blocks
//       "movq %[num_blocks], %%rdx\n\t"
// 
//       // R8 gets the base offset (OFFSET_8_BYTES, e.g., 32)
//       "movq %[offset8_val], %%r8\n\t"
// 
//       // R9 gets the 16-sample offset (R8 * 2)
//       "leaq (%%r8, %%r8, 1), %%r9\n\t" // R9 = R8 + R8*1 = R8 * 2
// 
//       // R10 gets the 24-sample offset (R8 * 3 = R9 + R8)
//       "addq %%r8, %%r9\n\t"  // R9 = R9 + R8 = R8 * 3 (Use R9 as temporary for R10)
//       "movq %%r9, %%r10\n\t" // R10 = R8 * 3
//       "subq %%r8, %%r9\n\t"  // R9 = R10 - R8 = R8 * 2 (Restore R9)
// 
//       "1:\n\t"
// 
//       // === Load 4 x 256-bit (32-byte) blocks from SRC ===
//       "vmovdqa     (%%rsi), %%ymm0\n\t"
//       "vmovdqa     (%%rsi, %%r8, 1), %%ymm1\n\t"  // Offset 8: Base + R8*1 (Valid scale 1)
//       "vmovdqa     (%%rsi, %%r9, 1), %%ymm2\n\t"  // Offset 16: Base + R9*1 (R9=R8*2)
//       "vmovdqa     (%%rsi, %%r10, 1), %%ymm3\n\t" // Offset 24: Base + R10*1 (R10=R8*3)
// 
//       // === Store 4 x 256-bit (32-byte) blocks to DST (VMOVNTDQ) ===
//       "vmovntdq    %%ymm0, (%%rdi)\n\t"
//       "vmovntdq    %%ymm1, (%%rdi, %%r8, 1)\n\t"
//       "vmovntdq    %%ymm2, (%%rdi, %%r9, 1)\n\t"
//       "vmovntdq    %%ymm3, (%%rdi, %%r10, 1)\n\t"
// 
//       // === Update Pointers and Counter ===
//       // Advance RSI/RDI by 4 * R8. Use LEA for speed.
//       "lea     (%%rsi, %%r8, 4), %%rsi\n\t"
//       "lea     (%%rdi, %%r8, 4), %%rdi\n\t"
// 
//       "decq    %%rdx\n\t"
//       "jnz     1b\n\t"
// 
//       // Outputs (using legacy aliasing fix)
//       : [dst_out] "=&D"(dst),
//         [src_out] "=&S"(src)
// 
//       // Inputs
//       : [dst_in] "0"(dst),
//         [src_in] "1"(src),
//         [num_blocks] "r"(num_blocks),
//         // R8 gets the base 8-sample offset (32 bytes)
//         [offset8_val] "r"(OFFSET_8_BYTES)
// 
//       // Clobbers: R8, R9, R10 are used as constant registers
//       : "rdx", "r8", "r9", "r10", "ymm0", "ymm1", "ymm2", "ymm3", "cc");
// }
// 
// static __attribute__((always_inline)) inline void avx2_intrin_stream_both_16x(sample_t *dst, const sample_t *src)
// {
//   dst = (sample_t *)__builtin_assume_aligned(dst, ALIGN_4K);
//   src = (const sample_t *)__builtin_assume_aligned(src, ALIGN_4K);
// 
//   for (int i = 0; i < (int)(SAMPLES_PER_FRAME * FRAMES_PER_PERIOD); i += 128 * FACTOR_MULTIPLE)
//   {
//     // Load 16 registers (512 bytes)
//     const __m256i d0 = _mm256_stream_load_si256((const __m256i *)(src + i + 0 * FACTOR_MULTIPLE));
//     const __m256i d1 = _mm256_stream_load_si256((const __m256i *)(src + i + 8 * FACTOR_MULTIPLE));
//     const __m256i d2 = _mm256_stream_load_si256((const __m256i *)(src + i + 16 * FACTOR_MULTIPLE));
//     const __m256i d3 = _mm256_stream_load_si256((const __m256i *)(src + i + 24 * FACTOR_MULTIPLE));
//     const __m256i d4 = _mm256_stream_load_si256((const __m256i *)(src + i + 32 * FACTOR_MULTIPLE));
//     const __m256i d5 = _mm256_stream_load_si256((const __m256i *)(src + i + 40 * FACTOR_MULTIPLE));
//     const __m256i d6 = _mm256_stream_load_si256((const __m256i *)(src + i + 48 * FACTOR_MULTIPLE));
//     const __m256i d7 = _mm256_stream_load_si256((const __m256i *)(src + i + 56 * FACTOR_MULTIPLE));
//     const __m256i d8 = _mm256_stream_load_si256((const __m256i *)(src + i + 64 * FACTOR_MULTIPLE));
//     const __m256i d9 = _mm256_stream_load_si256((const __m256i *)(src + i + 72 * FACTOR_MULTIPLE));
//     const __m256i d10 = _mm256_stream_load_si256((const __m256i *)(src + i + 80 * FACTOR_MULTIPLE));
//     const __m256i d11 = _mm256_stream_load_si256((const __m256i *)(src + i + 88 * FACTOR_MULTIPLE));
//     const __m256i d12 = _mm256_stream_load_si256((const __m256i *)(src + i + 96 * FACTOR_MULTIPLE));
//     const __m256i d13 = _mm256_stream_load_si256((const __m256i *)(src + i + 104 * FACTOR_MULTIPLE));
//     const __m256i d14 = _mm256_stream_load_si256((const __m256i *)(src + i + 112 * FACTOR_MULTIPLE));
//     const __m256i d15 = _mm256_stream_load_si256((const __m256i *)(src + i + 120 * FACTOR_MULTIPLE));
// 
//     // Store 16 registers
//     _mm256_stream_si256((__m256i *)(dst + i + 0 * FACTOR_MULTIPLE), d0);
//     _mm256_stream_si256((__m256i *)(dst + i + 8 * FACTOR_MULTIPLE), d1);
//     _mm256_stream_si256((__m256i *)(dst + i + 16 * FACTOR_MULTIPLE), d2);
//     _mm256_stream_si256((__m256i *)(dst + i + 24 * FACTOR_MULTIPLE), d3);
//     _mm256_stream_si256((__m256i *)(dst + i + 32 * FACTOR_MULTIPLE), d4);
//     _mm256_stream_si256((__m256i *)(dst + i + 40 * FACTOR_MULTIPLE), d5);
//     _mm256_stream_si256((__m256i *)(dst + i + 48 * FACTOR_MULTIPLE), d6);
//     _mm256_stream_si256((__m256i *)(dst + i + 56 * FACTOR_MULTIPLE), d7);
//     _mm256_stream_si256((__m256i *)(dst + i + 64 * FACTOR_MULTIPLE), d8);
//     _mm256_stream_si256((__m256i *)(dst + i + 72 * FACTOR_MULTIPLE), d9);
//     _mm256_stream_si256((__m256i *)(dst + i + 80 * FACTOR_MULTIPLE), d10);
//     _mm256_stream_si256((__m256i *)(dst + i + 88 * FACTOR_MULTIPLE), d11);
//     _mm256_stream_si256((__m256i *)(dst + i + 96 * FACTOR_MULTIPLE), d12);
//     _mm256_stream_si256((__m256i *)(dst + i + 104 * FACTOR_MULTIPLE), d13);
//     _mm256_stream_si256((__m256i *)(dst + i + 112 * FACTOR_MULTIPLE), d14);
//     _mm256_stream_si256((__m256i *)(dst + i + 120 * FACTOR_MULTIPLE), d15);
//   }
// }
// 
// static __attribute__((always_inline)) inline void avx2_intrin_stream_both_4x(sample_t *dst, const sample_t *src)
// {
//   dst = (sample_t *)__builtin_assume_aligned(dst, ALIGN_4K);
//   src = (const sample_t *)__builtin_assume_aligned(src, ALIGN_4K);
// 
//   for (int i = 0; i < (int)(SAMPLES_PER_FRAME * FRAMES_PER_PERIOD); i += 32 * FACTOR_MULTIPLE)
//   {
//     // Use _mm256_stream_load_si256 for non-temporal loads
//     const __m256i data0 = _mm256_stream_load_si256((const __m256i *)(src + i));
//     const __m256i data1 = _mm256_stream_load_si256((const __m256i *)(src + i + 8 * FACTOR_MULTIPLE));
//     const __m256i data2 = _mm256_stream_load_si256((const __m256i *)(src + i + 16 * FACTOR_MULTIPLE));
//     const __m256i data3 = _mm256_stream_load_si256((const __m256i *)(src + i + 24 * FACTOR_MULTIPLE));
// 
//     // Non-temporal stores (you already have these)
//     _mm256_stream_si256((__m256i *)(dst + i), data0);
//     _mm256_stream_si256((__m256i *)(dst + i + 8 * FACTOR_MULTIPLE), data1);
//     _mm256_stream_si256((__m256i *)(dst + i + 16 * FACTOR_MULTIPLE), data2);
//     _mm256_stream_si256((__m256i *)(dst + i + 24 * FACTOR_MULTIPLE), data3);
//   }
// }
// 
static __attribute__((always_inline)) inline void avx2_intrin_stream_store_4x(sample_t *dst, const sample_t *src)
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
