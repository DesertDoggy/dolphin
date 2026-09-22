#pragma once

// Shared between dolphinrvz.cpp and dolphinrvz_stream.cpp (streaming build only). Not part
// of the public C API.

#include <functional>
#include <memory>
#include <string>

#include "dolphinrvz.h"

namespace DiscIO
{
class BlobReader;
}

namespace dolphinrvz_internal
{
// Once per process: user directory, config system, logger (see dolphinrvz.cpp).
void EnsureMinimalInit(const char* user_dir);

void SetLastError(std::string message);

// Optionally replaces the input reader right before conversion starts (after scrubbing).
using WrapReader =
    std::function<std::unique_ptr<DiscIO::BlobReader>(std::unique_ptr<DiscIO::BlobReader>)>;

// dolphinrvz_convert's implementation. With wrap == nullptr it is dolphinrvz_convert.
// external_cancel, if non-null, is polled at each progress callback; true cancels (-6).
int Convert(const DolphinRvzConvertOptions* options, const WrapReader& wrap,
            const bool* external_cancel);
}  // namespace dolphinrvz_internal
