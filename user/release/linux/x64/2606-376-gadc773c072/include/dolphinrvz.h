#pragma once

#include <stddef.h>

#if defined(_WIN32)
#  ifdef DOLPHINRVZ_BUILDING_DLL
#    define DOLPHINRVZ_API __declspec(dllexport)
#  else
#    define DOLPHINRVZ_API __declspec(dllimport)
#  endif
#else
#  define DOLPHINRVZ_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Output container format -- matches DiscIO::BlobType's PLAIN/GCZ/WIA/RVZ. */
typedef enum DolphinRvzFormat
{
    DOLPHINRVZ_FORMAT_ISO = 0,
    DOLPHINRVZ_FORMAT_GCZ = 1,
    DOLPHINRVZ_FORMAT_WIA = 2,
    DOLPHINRVZ_FORMAT_RVZ = 3,
} DolphinRvzFormat;

/* Compression method for WIA/RVZ output -- matches DiscIO::WIARVZCompressionType.
 * WIA does not support Zstd; RVZ does not support Purge (same restriction as the
 * Dolphin GUI/CLI). */
typedef enum DolphinRvzCompression
{
    DOLPHINRVZ_COMPRESSION_NONE = 0,
    DOLPHINRVZ_COMPRESSION_PURGE = 1,
    DOLPHINRVZ_COMPRESSION_BZIP2 = 2,
    DOLPHINRVZ_COMPRESSION_LZMA = 3,
    DOLPHINRVZ_COMPRESSION_LZMA2 = 4,
    DOLPHINRVZ_COMPRESSION_ZSTD = 5,
} DolphinRvzCompression;

/* Called periodically during conversion with a status message and progress in [0,100].
 * Return 0 to cancel the conversion, nonzero to continue. May be NULL. */
typedef int (*DolphinRvzProgressCb)(const char* text, float percent, void* user_data);

typedef struct DolphinRvzConvertOptions
{
    /* Required. Path to the source disc image (ISO/GCZ/WIA/RVZ/CISO/NFS/etc -- anything
     * DiscIO::CreateBlobReader recognizes). */
    const char* input_path;

    /* Required. Path to write the converted file to. */
    const char* output_path;

    /* Required. Output container format. */
    DolphinRvzFormat format;

    /* Optional. Dolphin user directory for temporary processing files, created if it
     * doesn't exist. Pass NULL or "" to use Dolphin's normal per-platform default user
     * directory. */
    const char* user_dir;

    /* Optional (default: false/0). Scrub junk data during conversion. Only valid for
     * GameCube/Wii disc images; ignored (with a warning in the log) otherwise. */
    int scrub;

    /* Required for GCZ/WIA/RVZ, ignored for ISO. Block size in bytes -- see
     * DOLPHINRVZ_PREFERRED_MIN_BLOCK_SIZE / DOLPHINRVZ_PREFERRED_MAX_BLOCK_SIZE below.
     * Suggested value for RVZ: 131072 (128 KiB). */
    int block_size;

    /* Required for WIA/RVZ, ignored otherwise. */
    DolphinRvzCompression compression;

    /* Required for WIA/RVZ when compression is not NONE, ignored otherwise. Valid range
     * depends on the compression method -- see dolphinrvz_get_allowed_compression_levels. */
    int compression_level;

    /* Optional. May be NULL. */
    DolphinRvzProgressCb on_progress;
    void* user_data;
} DolphinRvzConvertOptions;

enum
{
    DOLPHINRVZ_PREFERRED_MIN_BLOCK_SIZE = 0x8000,
    DOLPHINRVZ_PREFERRED_MAX_BLOCK_SIZE = 0x200000,
};

/* Runs the conversion described by *options, synchronously on the calling thread.
 * Returns 0 on success, negative on failure -- see dolphinrvz_get_last_error() for a
 * human-readable message (including validation failures: missing required fields,
 * invalid block size/compression level for the chosen format, etc -- the same checks
 * Dolphin's own convert CLI performs). A return of exactly -6 specifically means
 * on_progress returned 0 (the caller cancelled) -- check for that value if you need to
 * distinguish cancellation from every other failure.
 *
 * Not safe to call concurrently with itself on multiple threads at once (shares
 * process-wide DiscIO/Common state). Serialize calls, e.g. one thread per conversion.
 */
DOLPHINRVZ_API int dolphinrvz_convert(const DolphinRvzConvertOptions* options);

/* Message for the most recent failed dolphinrvz_convert call on this thread. */
DOLPHINRVZ_API const char* dolphinrvz_get_last_error(void);

/* Returns the [min, max] compression_level range accepted by dolphinrvz_convert for the
 * given compression method. */
DOLPHINRVZ_API void dolphinrvz_get_allowed_compression_levels(DolphinRvzCompression compression,
                                                                int* out_min, int* out_max);

#ifdef __cplusplus
}
#endif
