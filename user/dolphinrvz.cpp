#include "dolphinrvz.h"

#include <atomic>
#include <limits>
#include <mutex>
#include <string>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/LogManager.h"

#include "Core/ConfigLoaders/BaseConfigLoader.h"
#include "Core/ConfigManager.h"

#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/ScrubbedBlob.h"
#include "DiscIO/Volume.h"
#include "DiscIO/WIABlob.h"

#include "UICommon/UICommon.h"

namespace
{
thread_local std::string g_last_error;

// Deliberately NOT UICommon::Init(): that also calls VideoBackendBase::ActivateBackend,
// Discord::Init, and sets up controller subsystems -- none of which disc conversion
// needs, and all of which pull in extra dependencies (a video backend, Discord RPC,
// SDL2/hidapi) for no benefit here. This does just enough of what UICommon::Init does
// for DiscIO/Common to work correctly: establishes the user directory (DiscIO's WIA/RVZ
// writer uses it for temporary files, same as the official convert CLI's --user option),
// initializes the config system many util/DiscIO code paths read defaults from, and
// starts the logger (INFO_LOG_FMT/ERROR_LOG_FMT etc calls throughout DiscIO/Common
// otherwise have nowhere to go).
std::once_flag g_init_once;

void EnsureMinimalInit(const char* user_dir)
{
    std::call_once(g_init_once, [&] {
        UICommon::SetUserDirectory(user_dir && *user_dir ? std::string(user_dir) : std::string());
        Config::Init();
        Config::AddLayer(ConfigLoaders::GenerateBaseConfigLoader());
        SConfig::Init();
        Common::Log::LogManager::Init();
    });
}

DiscIO::BlobType ToBlobType(DolphinRvzFormat format)
{
    switch (format)
    {
    case DOLPHINRVZ_FORMAT_ISO:
        return DiscIO::BlobType::PLAIN;
    case DOLPHINRVZ_FORMAT_GCZ:
        return DiscIO::BlobType::GCZ;
    case DOLPHINRVZ_FORMAT_WIA:
        return DiscIO::BlobType::WIA;
    case DOLPHINRVZ_FORMAT_RVZ:
    default:
        return DiscIO::BlobType::RVZ;
    }
}

DiscIO::WIARVZCompressionType ToCompressionType(DolphinRvzCompression compression)
{
    switch (compression)
    {
    case DOLPHINRVZ_COMPRESSION_NONE:
        return DiscIO::WIARVZCompressionType::None;
    case DOLPHINRVZ_COMPRESSION_PURGE:
        return DiscIO::WIARVZCompressionType::Purge;
    case DOLPHINRVZ_COMPRESSION_BZIP2:
        return DiscIO::WIARVZCompressionType::Bzip2;
    case DOLPHINRVZ_COMPRESSION_LZMA:
        return DiscIO::WIARVZCompressionType::LZMA;
    case DOLPHINRVZ_COMPRESSION_LZMA2:
        return DiscIO::WIARVZCompressionType::LZMA2;
    case DOLPHINRVZ_COMPRESSION_ZSTD:
    default:
        return DiscIO::WIARVZCompressionType::Zstd;
    }
}

}  // namespace

void dolphinrvz_get_allowed_compression_levels(DolphinRvzCompression compression, int* out_min,
                                                int* out_max)
{
    const std::pair<int, int> range =
        DiscIO::GetAllowedCompressionLevels(ToCompressionType(compression), false);
    if (out_min)
        *out_min = range.first;
    if (out_max)
        *out_max = range.second;
}

const char* dolphinrvz_get_last_error(void)
{
    return g_last_error.c_str();
}

int dolphinrvz_convert(const DolphinRvzConvertOptions* options)
{
    if (!options || !options->input_path || !*options->input_path)
    {
        g_last_error = "input_path is required";
        return -1;
    }
    if (!options->output_path || !*options->output_path)
    {
        g_last_error = "output_path is required";
        return -1;
    }

    EnsureMinimalInit(options->user_dir);

    const std::string input_path = options->input_path;
    const std::string output_path = options->output_path;
    const DiscIO::BlobType format = ToBlobType(options->format);

    std::unique_ptr<DiscIO::BlobReader> blob_reader = DiscIO::CreateBlobReader(input_path);
    if (!blob_reader)
    {
        g_last_error = "The input file could not be opened";
        return -2;
    }

    const bool scrub = options->scrub != 0;

    const std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(input_path);
    if (!volume)
    {
        if (scrub)
        {
            g_last_error = "Scrubbing is only supported for GC/Wii disc images";
            return -3;
        }
        // Not a GC/Wii disc image (e.g. a Wii WAD or raw file) -- DiscIO::ConvertTo*
        // below still handles this fine via the blob_reader alone, same as the CLI.
    }

    if (scrub)
    {
        if (volume->IsDatelDisc())
        {
            g_last_error = "Scrubbing a Datel disc is not supported";
            return -3;
        }

        blob_reader = DiscIO::ScrubbedBlob::Create(input_path);
        if (!blob_reader)
        {
            g_last_error = "Unable to process disc image for scrubbing";
            return -3;
        }
    }

    const bool needs_block_size =
        format == DiscIO::BlobType::GCZ || format == DiscIO::BlobType::WIA || format == DiscIO::BlobType::RVZ;
    if (needs_block_size)
    {
        if (!DiscIO::IsDiscImageBlockSizeValid(options->block_size, format))
        {
            g_last_error = "block_size is not valid for this format";
            return -4;
        }
    }

    DiscIO::WIARVZCompressionType compression_type = DiscIO::WIARVZCompressionType::None;
    int compression_level = 0;
    const bool needs_compression = format == DiscIO::BlobType::WIA || format == DiscIO::BlobType::RVZ;
    if (needs_compression)
    {
        compression_type = ToCompressionType(options->compression);

        if ((format == DiscIO::BlobType::WIA && compression_type == DiscIO::WIARVZCompressionType::Zstd) ||
            (format == DiscIO::BlobType::RVZ && compression_type == DiscIO::WIARVZCompressionType::Purge))
        {
            g_last_error = "compression type is not supported for this container format";
            return -4;
        }

        if (compression_type == DiscIO::WIARVZCompressionType::None)
        {
            compression_level = 0;
        }
        else
        {
            compression_level = options->compression_level;
            const std::pair<int, int> range = DiscIO::GetAllowedCompressionLevels(compression_type, false);
            if (compression_level < range.first || compression_level > range.second)
            {
                g_last_error = "compression_level is not in the acceptable range for this compression type";
                return -4;
            }
        }
    }

    DolphinRvzProgressCb progress_cb = options->on_progress;
    void* user_data = options->user_data;
    const DiscIO::CompressCB callback = [progress_cb, user_data](const std::string& text, float percent) {
        if (!progress_cb)
            return true;
        return progress_cb(text.c_str(), percent * 100.0f, user_data) != 0;
    };

    bool success = false;
    switch (format)
    {
    case DiscIO::BlobType::PLAIN:
        success = DiscIO::ConvertToPlain(blob_reader.get(), input_path, output_path, callback);
        break;

    case DiscIO::BlobType::GCZ:
    {
        u32 sub_type = std::numeric_limits<u32>::max();
        if (volume)
        {
            if (volume->GetVolumeType() == DiscIO::Platform::GameCubeDisc)
                sub_type = 0;
            else if (volume->GetVolumeType() == DiscIO::Platform::WiiDisc)
                sub_type = 1;
        }
        success = DiscIO::ConvertToGCZ(blob_reader.get(), input_path, output_path, sub_type,
                                        options->block_size, callback);
        break;
    }

    case DiscIO::BlobType::WIA:
    case DiscIO::BlobType::RVZ:
        success = DiscIO::ConvertToWIAOrRVZ(blob_reader.get(), input_path, output_path,
                                             format == DiscIO::BlobType::RVZ, compression_type,
                                             compression_level, options->block_size, callback);
        break;

    default:
        g_last_error = "unsupported format";
        return -1;
    }

    if (!success)
    {
        if (g_last_error.empty())
            g_last_error = "conversion failed";
        return -5;
    }

    g_last_error.clear();
    return 0;
}
