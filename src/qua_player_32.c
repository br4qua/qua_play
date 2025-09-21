/**
 * ALSA Player with + Mmap + Pre-Calculation Optimization
 */
#define CACHE_LINE_SIZE 64
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <alsa/asoundlib.h>
#include <stdint.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/resource.h>


// Debug configuration
#ifdef DEBUG
#define DEBUG_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...) \
    do                        \
    {                         \
    } while (0)
#endif

#define RING_BUFFER_SIZE_VAL 65536
#define PERIOD_SIZE_VAL (4096 * 4)
#define CHUNK_SIZE_VAL (4096 * 4)


static const snd_pcm_uframes_t RING_BUFFER_SIZE = RING_BUFFER_SIZE_VAL; // 16384*4
static const snd_pcm_uframes_t PERIOD_SIZE = PERIOD_SIZE_VAL;           // 4096*4
static const snd_pcm_uframes_t CHUNK_SIZE = CHUNK_SIZE_VAL;             // 4096*4
#define COPY_SIZE_BYTES ((size_t)(CHUNK_SIZE_VAL * 2 * sizeof(int32_t)))


// Branch prediction hints
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)


typedef struct
{
    char riff_header[4];
    uint32_t wav_size;
    char wave_header[4];
    char fmt_header[4];
    uint32_t fmt_chunk_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t sample_alignment;
    uint16_t bit_depth;
    char data_header[4];
    uint32_t data_bytes;
} WavHeader;



_Alignas(CACHE_LINE_SIZE) static struct
{
    int32_t **current_src_ptr;
    int32_t **current_dst_ptr;
    int32_t **end_src_ptr;

    snd_pcm_t *pcm_handle;
    snd_pcm_uframes_t commit_offset;
    snd_pcm_uframes_t commit_frames;

    char padding[16]; 

} hot_audio_state;
#pragma message "hot_audio_state size check passed"
_Static_assert(sizeof(hot_audio_state) == 64, "hot_audio_state must be exactly 64 bytes");

// COLD PATH: Setup data (rarely accessed during playback)
static struct
{
    int32_t *audio_data;
    
    snd_async_handler_t *async_handler;
    uint16_t num_channels;
    volatile sig_atomic_t total_frames;

    // Pre-calculated arrays (used during setup, not in hot path)
    int32_t **src_ptrs;
    int32_t **dst_ptrs;
    size_t total_copies;
} cold_audio_state;

// Function declarations
int setup_alsa(snd_pcm_t **handle, char *device, int channels, int rate);
void xrun_recovery(snd_pcm_t *handle, int err);
off_t read_wav_header(int fd, WavHeader *header);
void audio_callback(snd_async_handler_t *handler);
void cleanup_and_exit(int sig);
void setup_precalculated_sources(void);
void setup_precalculated_destinations(void);

char *mmap_audio_base;

int setup_alsa(snd_pcm_t **handle, char *device, int channels, int rate)
{
    int err;
    snd_pcm_hw_params_t *hw_params;

    if ((err = snd_pcm_open(handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
    {
        fprintf(stderr, "Cannot open audio device %s: %s\n", device, snd_strerror(err));
        return -1;
    }

    if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0)
    {
        fprintf(stderr, "Cannot allocate hardware parameters: %s\n", snd_strerror(err));
        return -1;
    }

    if ((err = snd_pcm_hw_params_any(*handle, hw_params)) < 0)
    {
        fprintf(stderr, "Cannot initialize hardware parameters: %s\n", snd_strerror(err));
        snd_pcm_hw_params_free(hw_params);
        return -1;
    }

    // Set mmap access mode
    if ((err = snd_pcm_hw_params_set_access(*handle, hw_params, SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0)
    {
        fprintf(stderr, "Cannot set interleaved mmap access type: %s\n", snd_strerror(err));
        snd_pcm_hw_params_free(hw_params);
        return -1;
    }

    if ((err = snd_pcm_hw_params_set_format(*handle, hw_params, SND_PCM_FORMAT_S32_LE)) < 0)
    {
        fprintf(stderr, "Cannot set sample format: %s\n", snd_strerror(err));
        snd_pcm_hw_params_free(hw_params);
        return -1;
    }

    if ((err = snd_pcm_hw_params_set_rate(*handle, hw_params, rate, 0)) < 0)
    {
        fprintf(stderr, "Cannot set sample rate: %s\n", snd_strerror(err));
        snd_pcm_hw_params_free(hw_params);
        return -1;
    }

    if ((err = snd_pcm_hw_params_set_channels(*handle, hw_params, channels)) < 0)
    {
        fprintf(stderr, "Cannot set channel count: %s\n", snd_strerror(err));
        snd_pcm_hw_params_free(hw_params);
        return -1;
    }

    // Set buffer and period sizes
    snd_pcm_uframes_t ring_buffer_size = RING_BUFFER_SIZE;
    if ((err = snd_pcm_hw_params_set_buffer_size_near(*handle, hw_params, &ring_buffer_size)) < 0)
    {
        fprintf(stderr, "Cannot set buffer size: %s\n", snd_strerror(err));
        snd_pcm_hw_params_free(hw_params);
        return -1;
    }

    snd_pcm_uframes_t period_size = PERIOD_SIZE;
    if ((err = snd_pcm_hw_params_set_period_size_near(*handle, hw_params, &period_size, 0)) < 0)
    {
        fprintf(stderr, "Cannot set period size: %s\n", snd_strerror(err));
        snd_pcm_hw_params_free(hw_params);
        return -1;
    }

    if ((err = snd_pcm_hw_params(*handle, hw_params)) < 0)
    {
        fprintf(stderr, "Cannot set hardware parameters: %s\n", snd_strerror(err));
        snd_pcm_hw_params_free(hw_params);
        return -1;
    }

    // Check available space before we start
    snd_pcm_sframes_t initial_avail = snd_pcm_avail_update(*handle);
    DEBUG_PRINT("Initial available space: %ld frames\n", initial_avail);

    snd_pcm_hw_params_free(hw_params);

    return snd_pcm_prepare(*handle);
}

void xrun_recovery(snd_pcm_t *handle, int err)
{
    DEBUG_PRINT("XRUN recovery, error: %s\n", snd_strerror(err));
    if (err == -EPIPE)
    {
        err = snd_pcm_prepare(handle);
        if (err < 0)
        {
            DEBUG_PRINT("Can't recover from underrun, prepare failed: %s\n", snd_strerror(err));
        }
    }
    else if (err == -ESTRPIPE)
    {
        while ((err = snd_pcm_resume(handle)) == -EAGAIN)
            sleep(1);
        if (err < 0)
        {
            err = snd_pcm_prepare(handle);
            if (err < 0)
            {
                DEBUG_PRINT("Can't recover from suspend, prepare failed: %s\n", snd_strerror(err));
            }
        }
    }
}

off_t read_wav_header(int fd, WavHeader *header)
{
    uint8_t basic_header[12];
    if (read(fd, basic_header, 12) != 12)
    {
        fprintf(stderr, "Failed to read WAV header\n");
        return -1;
    }

    if (strncmp((char *)basic_header, "RIFF", 4) != 0 ||
        strncmp((char *)basic_header + 8, "WAVE", 4) != 0)
    {
        fprintf(stderr, "Invalid WAV file format\n");
        return -1;
    }

    memcpy(header->riff_header, basic_header, 4);
    memcpy(&header->wav_size, basic_header + 4, 4);
    memcpy(header->wave_header, basic_header + 8, 4);

    int fmt_chunk_found = 0;
    int data_chunk_found = 0;
    off_t data_offset = 0;

    while (!data_chunk_found)
    {
        char chunk_id[4];
        uint32_t chunk_size;

        if (read(fd, chunk_id, 4) != 4 || read(fd, &chunk_size, 4) != 4)
        {
            fprintf(stderr, "Couldn't find required chunks\n");
            return -1;
        }

        if (strncmp(chunk_id, "fmt ", 4) == 0)
        {
            memcpy(header->fmt_header, chunk_id, 4);
            header->fmt_chunk_size = chunk_size;

            if (chunk_size < 16)
            {
                fprintf(stderr, "Format chunk too small\n");
                return -1;
            }

            uint8_t fmt_data[16];
            if (read(fd, fmt_data, 16) != 16)
            {
                fprintf(stderr, "Failed to read format chunk data\n");
                return -1;
            }

            memcpy(&header->audio_format, fmt_data, 2);
            memcpy(&header->num_channels, fmt_data + 2, 2);
            memcpy(&header->sample_rate, fmt_data + 4, 4);
            memcpy(&header->byte_rate, fmt_data + 8, 4);
            memcpy(&header->sample_alignment, fmt_data + 12, 2);
            memcpy(&header->bit_depth, fmt_data + 14, 2);

            if (chunk_size > 16)
            {
                lseek(fd, chunk_size - 16, SEEK_CUR);
            }

            if (header->audio_format != 1 && header->audio_format != 65534)
            {
                fprintf(stderr, "Only PCM WAV files are supported\n");
                return -1;
            }

            if (header->bit_depth != 32)
            {
                fprintf(stderr, "Only 32-bit audio is supported\n");
                return -1;
            }

            fmt_chunk_found = 1;
        }
        else if (strncmp(chunk_id, "data", 4) == 0)
        {
            memcpy(header->data_header, chunk_id, 4);
            header->data_bytes = chunk_size;
            data_chunk_found = 1;
            data_offset = lseek(fd, 0, SEEK_CUR);
            break;
        }
        else
        {
            lseek(fd, chunk_size, SEEK_CUR);
            if (chunk_size % 2 != 0)
            {
                lseek(fd, 1, SEEK_CUR);
            }
        }
    }

    if (!fmt_chunk_found)
    {
        fprintf(stderr, "No format chunk found\n");
        return -1;
    }

    return data_offset;
}

void setup_precalculated_sources(void)
{
    // Calculate total number of copy operations needed
    cold_audio_state.total_copies = cold_audio_state.total_frames / CHUNK_SIZE;
    if (cold_audio_state.total_frames % CHUNK_SIZE != 0)
    {
        cold_audio_state.total_copies++;
    }

    // Pre-calculate constants
    // hot_audio_state.copy_size_bytes = CHUNK_SIZE * cold_audio_state.num_channels * sizeof(int32_t);

    // Allocate arrays
    cold_audio_state.src_ptrs = malloc(cold_audio_state.total_copies * sizeof(int32_t *));
    cold_audio_state.dst_ptrs = malloc(cold_audio_state.total_copies * sizeof(int32_t *));

    if (!cold_audio_state.src_ptrs || !cold_audio_state.dst_ptrs)
    {
        fprintf(stderr, "Failed to allocate precalc arrays\n");
        cleanup_and_exit(1);
    }

    // Pre-calculate ALL source pointers
    for (size_t i = 0; i < cold_audio_state.total_copies; i++)
    {
        cold_audio_state.src_ptrs[i] = cold_audio_state.audio_data + (i * CHUNK_SIZE * cold_audio_state.num_channels);
    }

    // Set current and total:
    hot_audio_state.current_src_ptr = cold_audio_state.src_ptrs;
    hot_audio_state.end_src_ptr = cold_audio_state.src_ptrs + cold_audio_state.total_copies;

    DEBUG_PRINT("Phase 1: Pre-calculated %zu source pointers\n", cold_audio_state.total_copies);
}

void setup_precalculated_destinations(void)
{
    for (size_t i = 0; i < cold_audio_state.total_copies; i++)
    {
        unsigned long raw_calc = i * CHUNK_SIZE;
        unsigned long frame_offset = raw_calc % RING_BUFFER_SIZE;
        unsigned long byte_offset = frame_offset * 8; // 8 bytes per frame (2ch * 4 bytes)

        cold_audio_state.dst_ptrs[i] = (int32_t *)(mmap_audio_base + byte_offset);

        if (i < 8)
        {
            DEBUG_PRINT("i=%zu: raw=%lu, frame_offset=%lu, byte_offset=%lu, ptr=%p\n",
                        i, raw_calc, frame_offset, byte_offset, cold_audio_state.dst_ptrs[i]);
        }
    }
    hot_audio_state.current_dst_ptr = cold_audio_state.dst_ptrs;
}

void cleanup_and_exit(int sig)
{


    if (hot_audio_state.pcm_handle)
    {
        snd_pcm_close(hot_audio_state.pcm_handle);
        hot_audio_state.pcm_handle = NULL;
    }

    if (cold_audio_state.audio_data)
    {
        free(cold_audio_state.audio_data);
        cold_audio_state.audio_data = NULL;
    }

    // Cleanup pre-calculated data
    if (cold_audio_state.src_ptrs)
    {
        free(cold_audio_state.src_ptrs);
        cold_audio_state.src_ptrs = NULL;
    }

    if (cold_audio_state.dst_ptrs)
    {
        free(cold_audio_state.dst_ptrs);
        cold_audio_state.dst_ptrs = NULL;
    }

    exit(0);
}

int main(int argc, char *argv[])
{
#ifndef DEBUG
    extern char **environ;
    environ = NULL;
   
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    for (int fd = 0; fd < rl.rlim_cur; fd++)
    {
        close(fd);
    }
    umask(0);
    chdir("/"); 
#endif

    if (argc < 2)
    {
        printf("Usage: %s <wav_file> [device_name]\n", argv[0]);
        return 1;
    }

    // Set up signal handlers for clean exit
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);

    char *filename = argv[1];
    char *device_name = (argc > 2) ? argv[2] : "hw:0,0";

    DEBUG_PRINT("ALSA Optimized Mmap Player\n");
    DEBUG_PRINT("File: %s, Device: %s\n", filename, device_name);

    WavHeader header;
    int fd = open(filename, O_RDONLY);
    // TOTAL HACK FOR STUTTER AT THE END
    if (fd >= 0)
    {
        // Seek to end and add silence
        lseek(fd, 0, SEEK_END);
        size_t silence_bytes = CHUNK_SIZE * 4 * 2 * sizeof(int32_t);
        char *silence = calloc(1, silence_bytes);
        write(fd, silence, silence_bytes);
        free(silence);
        lseek(fd, 0, SEEK_SET); // Reset to beginning for header reading
    }
    if (fd < 0)
    {
        perror("Error opening WAV file");
        return 1;
    }

    off_t data_offset = read_wav_header(fd, &header);
    if (data_offset < 0)
    {
        close(fd);
        return 1;
    }

    snd_pcm_t *pcm_handle = NULL;

    if (setup_alsa(&pcm_handle, device_name, 2, header.sample_rate) < 0)
    {
        close(fd);
        return 1;
    }

    hot_audio_state.pcm_handle = pcm_handle;
    cold_audio_state.num_channels = 2;

    // Load entire audio data into memory
    int32_t *audio_data;
    size_t padding_bytes2 = CHUNK_SIZE * 2 * sizeof(int32_t);

    // Try huge pages first
    size_t total_size = header.data_bytes + padding_bytes2;
    audio_data = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (audio_data == MAP_FAILED)
    {
        // Fallback to regular pages
        if (posix_memalign((void **)&audio_data, 4096, header.data_bytes + padding_bytes2) != 0)
        {
            fprintf(stderr, "Failed to allocate page-aligned memory\n");
            snd_pcm_close(pcm_handle);
            close(fd);
            return 1;
        }
        DEBUG_PRINT("Using regular 4KB pages\n");
    }
    else
    {
        DEBUG_PRINT("Using 2MB huge pages\n");
    }

    cold_audio_state.audio_data = audio_data;

    // Read audio data
    if (lseek(fd, data_offset, SEEK_SET) != data_offset)
    {
        fprintf(stderr, "Failed to seek to audio data\n");
        free(audio_data);
        close(fd);
        return 1;
    }

    ssize_t total_read = 0;
    while (total_read < header.data_bytes)
    {
        ssize_t bytes_read = read(fd, (char *)audio_data + total_read, header.data_bytes - total_read);
        if (bytes_read <= 0)
        {
            fprintf(stderr, "Failed to read audio data\n");
            free(audio_data);
            snd_pcm_close(pcm_handle);
            close(fd);
            return 1;
        }
        total_read += bytes_read;
    }
    close(fd);
    size_t padding_bytes = CHUNK_SIZE * 2 * sizeof(int32_t);
    memset((char *)audio_data + header.data_bytes, 0, padding_bytes);
    //    chdir("/");

    // Lock memory for real-time performance
    int result = mlockall(MCL_CURRENT | MCL_FUTURE);
    if (result == -1)
    {
        perror("mlockall failed");
    }

    // Initialize global state
    cold_audio_state.total_frames = header.data_bytes / (2 * sizeof(int32_t));

    // PHASE 1: Pre-calculate sources (hot_audio_state.mmap_audio_base not available yet)
    setup_precalculated_sources();

    // Pre-fill buffer to prime the async system
    DEBUG_PRINT("Pre-filling buffer to trigger async callbacks...\n");

    // Check how much space is available for pre-filling
    snd_pcm_sframes_t prefill_avail = snd_pcm_avail_update(pcm_handle);
    DEBUG_PRINT("Available space for pre-fill: %ld frames\n", prefill_avail);

    int prefill_periods = 4; // Fill 3 periods to get async system started
    for (int i = 0; i < prefill_periods && hot_audio_state.current_src_ptr < hot_audio_state.end_src_ptr; i++)
    {
        const snd_pcm_channel_area_t *areas;

        // Check available space before each period
        snd_pcm_avail_update(hot_audio_state.pcm_handle);

        hot_audio_state.commit_frames = CHUNK_SIZE;
        int err = snd_pcm_mmap_begin(hot_audio_state.pcm_handle,
                                     &areas,
                                     &hot_audio_state.commit_offset,
                                     &hot_audio_state.commit_frames);

        if (err >= 0 && hot_audio_state.commit_frames > 0)
        {
            // Set mmap base on first access
            if (!mmap_audio_base)
            {
                mmap_audio_base = (char *)areas[0].addr + (areas[0].first >> 3);
                DEBUG_PRINT("Set mmap base address: %p\n", mmap_audio_base);

                // PHASE 2: Pre-calculate destinations (now mmap_audio_base is available)
                setup_precalculated_destinations();
            }

            memcpy(*hot_audio_state.current_dst_ptr++, *hot_audio_state.current_src_ptr++, COPY_SIZE_BYTES);

            snd_pcm_mmap_commit(hot_audio_state.pcm_handle,
                                hot_audio_state.commit_offset,
                                hot_audio_state.commit_frames);

            DEBUG_PRINT("Pre-filled period %d using precalc arrays\n", i + 1);
        }
        else
        {
            DEBUG_PRINT("Failed to get buffer area for period %d\n", i + 1);
            break;
        }
    }

    // Start playback - this will trigger async callbacks
    int err = snd_pcm_start(pcm_handle);
    if (err < 0)
    {
        DEBUG_PRINT("Failed to start playback: %s\n", snd_strerror(err));
        return 1;
    }
    DEBUG_PRINT("Playback started");
	// Or add this in main() for runtime check:
	       
    // Main Loop
    const snd_pcm_channel_area_t *areas;
    while (1)
    {
		snd_pcm_wait(pcm_handle, -1); 
        //snd_pcm_sframes_t avail = snd_pcm_avail_update(hot_audio_state.pcm_handle);

        // Fast path: not enough space - just continue waiting
        // If NOT blocking
 		// if (likely(avail < CHUNK_SIZE)) {
  		//	 continue;
		// }
		
		// 
		// if (unlikely(avail < CHUNK_SIZE)){
		// printf("DEBUG: Insufficient space, avail=%ld, waiting...\n", avail);
  //           continue;
		// }
		// printf("DEBUG: Space available, avail=%ld, processing chunk\n", avail);
		// snd_pcm_sw_params_t *sw_params;
		// snd_pcm_uframes_t avail_min;
		// snd_pcm_sw_params_malloc(&sw_params);
		// snd_pcm_sw_params_current(pcm_handle, sw_params);
		// snd_pcm_sw_params_get_avail_min(sw_params, &avail_min);
		// printf("avail_min=%lu, CHUNK_SIZE=%d\n", avail_min, CHUNK_SIZE_VAL);
		// snd_pcm_sw_params_free(sw_params);
		// printf("updating buffer\n");
        // Process current audio chunk
        // const snd_pcm_channel_area_t *areas;
        snd_pcm_mmap_begin(hot_audio_state.pcm_handle,
                           &areas,
                           &hot_audio_state.commit_offset,
                           &hot_audio_state.commit_frames);

        memcpy(*hot_audio_state.current_dst_ptr++,
               *hot_audio_state.current_src_ptr++,
               COPY_SIZE_BYTES);

        snd_pcm_mmap_commit(hot_audio_state.pcm_handle,
                            hot_audio_state.commit_offset,
                            hot_audio_state.commit_frames);

        // Increase pointer (Todo: Move to after copy)
        // if (likely(hot_audio_state.current_src_ptr < hot_audio_state.end_src_ptr))
        // {
        //     // Prefetch next audio chunk into L2/L3 cache (370ms ahead)
        //     __builtin_prefetch(*(hot_audio_state.current_src_ptr), 0, 2);
        // }

        int at_end = (hot_audio_state.current_src_ptr >= hot_audio_state.end_src_ptr);
        if (unlikely(at_end))
        {
            snd_pcm_drain(hot_audio_state.pcm_handle);
            DEBUG_PRINT("\nPlayback completed!\n");
            #ifdef PROFILE_GENERATE
                __llvm_profile_write_file();
            #endif
            return 0;
        }
    }
}
