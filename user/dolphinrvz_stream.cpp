// Streaming build only (DOLPHINRVZ_WITH_STREAMING, target dolphinrvz_streaming in
// dolphinrvz_target.cmake): dolphinrvz_extract_stream, dolphinrvz_convert_stream and the
// dolphinrvz_reader_* API declared in dolphinrvz.h. Everything here goes through DiscIO's
// public BlobReader interface -- no Dolphin source is modified.

#include "dolphinrvz.h"
#include "dolphinrvz_internal.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"

#include "DiscIO/Blob.h"

namespace
{
// Same chunking as DiscIO::ConvertToPlain (FileBlob.cpp): the container's block size,
// doubled until it reaches 512 KiB, or 512 KiB for formats without blocks.
u64 PlainBufferSize(const DiscIO::BlobReader& blob)
{
    constexpr u64 DESIRED_BUFFER_SIZE = 0x80000;
    u64 buffer_size = blob.GetBlockSize();
    if (buffer_size == 0)
        return DESIRED_BUFFER_SIZE;
    while (buffer_size < DESIRED_BUFFER_SIZE)
        buffer_size *= 2;
    return buffer_size;
}

// Turns whatever reads a converter makes into one complete, in-order stream of the input's
// bytes: re-reads of already-delivered ranges are dropped, and a forward jump first reads
// and delivers the skipped range from the same reader.
class InputTap
{
public:
    InputTap(std::string name, u64 data_size, DolphinRvzDataCb cb, void* user_data)
        : m_name(std::move(name)), m_data_size(data_size), m_cb(cb), m_user_data(user_data)
    {
    }

    void SetSource(DiscIO::BlobReader* source) { m_source = source; }

    // Called after every successful read the converter makes.
    void Observe(u64 offset, u64 size, const u8* data)
    {
        if (m_stopped || m_failed)
            return;
        const u64 end = std::min(offset + size, m_data_size);
        if (end <= m_cursor)
            return;
        if (offset > m_cursor && !Fill(offset))
            return;
        const u64 skip = m_cursor - offset;
        Deliver(data + skip, end - m_cursor);
    }

    // Delivers everything the converter never read.
    bool Finish() { return Fill(m_data_size) && !m_stopped; }

private:
    bool Fill(u64 until)
    {
        if (m_buffer.empty())
            m_buffer.resize(0x80000);
        while (m_cursor < until && !m_stopped)
        {
            const u64 n = std::min<u64>(m_buffer.size(), until - m_cursor);
            if (!m_source->Read(m_cursor, n, m_buffer.data()))
            {
                m_failed = true;
                return false;
            }
            Deliver(m_buffer.data(), n);
        }
        return !m_failed;
    }

    void Deliver(const u8* data, u64 size)
    {
        while (size && !m_stopped)
        {
            const u32 n = static_cast<u32>(std::min<u64>(size, 0x40000000));
            if (m_cb && !m_cb(0, m_name.c_str(), m_cursor, data, n, m_user_data))
                m_stopped = true;
            m_cursor += n;
            data += n;
            size -= n;
        }
    }

    std::string m_name;
    u64 m_data_size;
    DolphinRvzDataCb m_cb;
    void* m_user_data;
    DiscIO::BlobReader* m_source = nullptr;
    u64 m_cursor = 0;
    bool m_stopped = false;
    bool m_failed = false;
    std::vector<u8> m_buffer;
};

// Forwards everything to the wrapped reader, reporting each Read to an InputTap.
// (Pattern: DiscIO::ScrubbedBlob.)
class ObservingBlob final : public DiscIO::BlobReader
{
public:
    ObservingBlob(std::unique_ptr<DiscIO::BlobReader> inner, InputTap* tap)
        : m_inner(std::move(inner)), m_tap(tap)
    {
        m_tap->SetSource(m_inner.get());
    }

    DiscIO::BlobType GetBlobType() const override { return m_inner->GetBlobType(); }
    // Copies read unobserved; InputTap fills in whatever they covered.
    std::unique_ptr<DiscIO::BlobReader> CopyReader() const override { return m_inner->CopyReader(); }

    u64 GetRawSize() const override { return m_inner->GetRawSize(); }
    u64 GetDataSize() const override { return m_inner->GetDataSize(); }
    DiscIO::DataSizeType GetDataSizeType() const override { return m_inner->GetDataSizeType(); }

    u64 GetBlockSize() const override { return m_inner->GetBlockSize(); }
    bool HasFastRandomAccessInBlock() const override { return m_inner->HasFastRandomAccessInBlock(); }
    std::string GetCompressionMethod() const override { return m_inner->GetCompressionMethod(); }
    std::optional<int> GetCompressionLevel() const override { return m_inner->GetCompressionLevel(); }

    bool Read(u64 offset, u64 size, u8* out_ptr) override
    {
        if (!m_inner->Read(offset, size, out_ptr))
            return false;
        m_tap->Observe(offset, size, out_ptr);
        return true;
    }

    bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const override
    {
        return m_inner->SupportsReadWiiDecrypted(offset, size, partition_data_offset);
    }
    bool ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset) override
    {
        return m_inner->ReadWiiDecrypted(offset, size, out_ptr, partition_data_offset);
    }
    bool IsCached() const override { return m_inner->IsCached(); }

private:
    std::unique_ptr<DiscIO::BlobReader> m_inner;
    InputTap* m_tap;
};
}  // namespace

int dolphinrvz_extract_stream(const char* input_path, const char* output_path, const char* user_dir,
                              DolphinRvzDataCb on_data, DolphinRvzProgressCb on_progress,
                              void* user_data)
{
    using dolphinrvz_internal::SetLastError;
    if (!input_path || !*input_path)
    {
        SetLastError("input_path is required");
        return -1;
    }
    dolphinrvz_internal::EnsureMinimalInit(user_dir);

    std::unique_ptr<DiscIO::BlobReader> blob = DiscIO::CreateBlobReader(input_path);
    if (!blob)
    {
        SetLastError("The input file could not be opened");
        return -2;
    }
    if (blob->GetDataSizeType() != DiscIO::DataSizeType::Accurate)
    {
        SetLastError("The input's plain size is not known exactly; it can't be unpacked");
        return -2;
    }

    const std::string out_name = output_path ? output_path : "";
    std::optional<File::IOFile> out;
    if (output_path && *output_path)
    {
        out.emplace(out_name, "wb");
        if (!out->IsOpen())
        {
            SetLastError("Failed to open the output file");
            return -5;
        }
    }

    const u64 data_size = blob->GetDataSize();
    std::vector<u8> buffer(PlainBufferSize(*blob));
    const u64 num_buffers = (data_size + buffer.size() - 1) / buffer.size();
    const u64 progress_monitor = std::max<u64>(1, num_buffers / 100);

    int rc = 0;
    for (u64 i = 0; i < num_buffers; i++)
    {
        if (i % progress_monitor == 0 && on_progress &&
            !on_progress("Unpacking", 100.0f * static_cast<float>(i) / static_cast<float>(num_buffers),
                         user_data))
        {
            SetLastError("cancelled by caller");
            rc = -6;
            break;
        }
        const u64 pos = i * buffer.size();
        const u64 n = std::min<u64>(buffer.size(), data_size - pos);
        if (!blob->Read(pos, n, buffer.data()))
        {
            SetLastError("Failed to read from the input file");
            rc = -5;
            break;
        }
        if (out && !out->WriteBytes(buffer.data(), n))
        {
            SetLastError("Failed to write the output file");
            rc = -5;
            break;
        }
        if (on_data && !on_data(0, out_name.c_str(), pos, buffer.data(), static_cast<u32>(n), user_data))
        {
            SetLastError("stopped by data callback");
            rc = -6;
            break;
        }
    }

    if (out)
    {
        out->Close();
        if (rc != 0)
            File::Delete(out_name);
    }
    if (rc == 0)
        SetLastError("");
    return rc;
}

int dolphinrvz_convert_stream(const DolphinRvzConvertOptions* options, DolphinRvzDataCb on_input_data)
{
    if (!options || !options->input_path)
        return dolphinrvz_internal::Convert(options, nullptr, nullptr);  // reports the error

    // The relay records a stop request in `stopped`, which Convert polls at each progress
    // point (it can't be signalled from inside a read without faking a read error).
    bool stopped = false;
    struct Relay
    {
        DolphinRvzDataCb cb;
        void* user;
        bool* stopped;
    } relay{on_input_data, options->user_data, &stopped};
    const DolphinRvzDataCb relay_cb = [](uint32_t index, const char* name, uint64_t offset,
                                         const void* data, uint32_t size, void* user) -> int {
        auto* r = static_cast<Relay*>(user);
        if (r->cb && !r->cb(index, name, offset, data, size, r->user))
        {
            *r->stopped = true;
            return 0;
        }
        return 1;
    };

    std::unique_ptr<InputTap> tap;
    const auto wrap = [&](std::unique_ptr<DiscIO::BlobReader> inner) -> std::unique_ptr<DiscIO::BlobReader> {
        tap = std::make_unique<InputTap>(options->input_path, inner->GetDataSize(), relay_cb, &relay);
        return std::make_unique<ObservingBlob>(std::move(inner), tap.get());
    };

    const int rc = dolphinrvz_internal::Convert(options, wrap, &stopped);
    if (rc != 0 || !tap)
        return rc;

    // The observing reader died with Convert; deliver whatever the converter never read
    // from a fresh reader.
    std::unique_ptr<DiscIO::BlobReader> tail = DiscIO::CreateBlobReader(options->input_path);
    if (!tail)
    {
        dolphinrvz_internal::SetLastError("The input file could not be reopened");
        return -5;
    }
    tap->SetSource(tail.get());
    if (!tap->Finish())
    {
        dolphinrvz_internal::SetLastError(stopped ? "stopped by data callback" : "Failed to read from the input file");
        return stopped ? -6 : -5;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Random access reader
// ---------------------------------------------------------------------------

struct DolphinRvzReader
{
    std::unique_ptr<DiscIO::BlobReader> blob;
};

DolphinRvzReader* dolphinrvz_reader_open(const char* path, const char* user_dir)
{
    if (!path || !*path)
    {
        dolphinrvz_internal::SetLastError("path is required");
        return nullptr;
    }
    dolphinrvz_internal::EnsureMinimalInit(user_dir);
    std::unique_ptr<DiscIO::BlobReader> blob = DiscIO::CreateBlobReader(path);
    if (!blob)
    {
        dolphinrvz_internal::SetLastError("The input file could not be opened");
        return nullptr;
    }
    return new DolphinRvzReader{std::move(blob)};
}

void dolphinrvz_reader_close(DolphinRvzReader* reader)
{
    delete reader;
}

int dolphinrvz_reader_get_info(DolphinRvzReader* reader, DolphinRvzReaderInfo* out)
{
    if (!reader || !out)
    {
        dolphinrvz_internal::SetLastError("NULL argument");
        return -1;
    }
    out->data_size = reader->blob->GetDataSize();
    out->raw_size = reader->blob->GetRawSize();
    out->block_size = reader->blob->GetBlockSize();
    out->blob_type = static_cast<int32_t>(reader->blob->GetBlobType());
    out->data_size_accurate = reader->blob->GetDataSizeType() == DiscIO::DataSizeType::Accurate;
    return 0;
}

int dolphinrvz_reader_read(DolphinRvzReader* reader, uint64_t offset, void* buffer, uint64_t size)
{
    if (!reader || (!buffer && size))
    {
        dolphinrvz_internal::SetLastError("NULL argument");
        return -1;
    }
    const u64 data_size = reader->blob->GetDataSize();
    if (offset > data_size || size > data_size - offset)
    {
        dolphinrvz_internal::SetLastError("read past end of disc image");
        return -1;
    }
    if (size && !reader->blob->Read(offset, size, static_cast<u8*>(buffer)))
    {
        dolphinrvz_internal::SetLastError("read failed");
        return -5;
    }
    return 0;
}
