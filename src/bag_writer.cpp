#include "bag_writer.hpp"
#include "lz4_util.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace bag2mkv {
namespace {

constexpr size_t kChunkLimit = 8 * 1024 * 1024;

void append_u32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(uint8_t(value & 0xFF));
    out.push_back(uint8_t((value >> 8) & 0xFF));
    out.push_back(uint8_t((value >> 16) & 0xFF));
    out.push_back(uint8_t((value >> 24) & 0xFF));
}

void append_u64(std::vector<uint8_t> &out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(uint8_t((value >> (i * 8)) & 0xFF));
    }
}

void append_field(std::vector<uint8_t> &out, const std::string &name,
                  const std::vector<uint8_t> &value) {
    append_u32(out, static_cast<uint32_t>(name.size() + 1 + value.size()));
    out.insert(out.end(), name.begin(), name.end());
    out.push_back('=');
    out.insert(out.end(), value.begin(), value.end());
}

std::vector<uint8_t> bytes_u8(uint8_t value) { return {value}; }
std::vector<uint8_t> bytes_u32(uint32_t value) {
    std::vector<uint8_t> out;
    append_u32(out, value);
    return out;
}
std::vector<uint8_t> bytes_u64(uint64_t value) {
    std::vector<uint8_t> out;
    append_u64(out, value);
    return out;
}
std::vector<uint8_t> bytes_string(const std::string &value) {
    return std::vector<uint8_t>(value.begin(), value.end());
}

size_t find_field_value_offset(const std::vector<uint8_t> &header, const std::string &name) {
    size_t pos = 0;
    while (pos + 4 <= header.size()) {
        uint32_t len = uint32_t(header[pos]) | (uint32_t(header[pos + 1]) << 8) |
                       (uint32_t(header[pos + 2]) << 16) | (uint32_t(header[pos + 3]) << 24);
        pos += 4;
        if (len > header.size() - pos) break;
        const uint8_t *field = header.data() + pos;
        size_t name_len = name.size();
        if (len > name_len && field[name_len] == '=' &&
            std::memcmp(field, name.data(), name_len) == 0) {
            return pos + name_len + 1;
        }
        pos += len;
    }
    throw std::runtime_error("bag header field not found: " + name);
}

}  // namespace

BagWriter::BagWriter(const std::string &path) : path_(path) {
    out_.open(path_, std::ios::binary | std::ios::trunc);
    if (!out_) {
        throw std::runtime_error("cannot create bag: " + path_);
    }
    static const char kMagic[] = "#ROSBAG V2.0\n";
    write_bytes(reinterpret_cast<const uint8_t *>(kMagic), 13);
    write_bag_header();
}

BagWriter::~BagWriter() {
    if (!closed_) {
        try {
            close();
        } catch (...) {
        }
    }
}

void BagWriter::write_bytes(const uint8_t *data, size_t size) {
    if (size == 0) return;
    out_.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    if (!out_) {
        throw std::runtime_error("failed writing bag: " + path_);
    }
}

void BagWriter::write_bytes(const std::vector<uint8_t> &data) {
    write_bytes(data.data(), data.size());
}

void BagWriter::patch_u32(uint64_t offset, uint32_t value) {
    out_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    std::vector<uint8_t> data;
    append_u32(data, value);
    write_bytes(data);
}

void BagWriter::patch_u64(uint64_t offset, uint64_t value) {
    out_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    std::vector<uint8_t> data;
    append_u64(data, value);
    write_bytes(data);
}

void BagWriter::append_u32(std::vector<uint8_t> &out, uint32_t value) {
    ::bag2mkv::append_u32(out, value);
}

void BagWriter::append_u64(std::vector<uint8_t> &out, uint64_t value) {
    ::bag2mkv::append_u64(out, value);
}

void BagWriter::append_field(std::vector<uint8_t> &out, const std::string &name,
                             const std::vector<uint8_t> &value) {
    ::bag2mkv::append_field(out, name, value);
}

void BagWriter::append_string_field(std::vector<uint8_t> &out, const std::string &name,
                                    const std::string &value) {
    append_field(out, name, bytes_string(value));
}

void BagWriter::append_u32_field(std::vector<uint8_t> &out, const std::string &name, uint32_t value) {
    append_field(out, name, bytes_u32(value));
}

void BagWriter::append_u64_field(std::vector<uint8_t> &out, const std::string &name, uint64_t value) {
    append_field(out, name, bytes_u64(value));
}

void BagWriter::append_u8_field(std::vector<uint8_t> &out, const std::string &name, uint8_t value) {
    append_field(out, name, bytes_u8(value));
}

void BagWriter::append_record_header(std::vector<uint8_t> &out, const std::vector<uint8_t> &header) {
    append_u32(out, static_cast<uint32_t>(header.size()));
    out.insert(out.end(), header.begin(), header.end());
}

void BagWriter::append_record(std::vector<uint8_t> &out, const std::vector<uint8_t> &header,
                              const std::vector<uint8_t> &data) {
    append_record_header(out, header);
    append_u32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
}

std::vector<uint8_t> BagWriter::connection_header(const BagConnection &connection, bool first) {
    std::vector<uint8_t> header;
    if (first) {
        append_u8_field(header, "op", 0x07);
        append_u32_field(header, "conn", connection.id);
        append_string_field(header, "topic", connection.topic);
    } else {
        append_string_field(header, "md5sum", connection.md5);
        append_string_field(header, "message_definition", connection.definition);
        append_string_field(header, "type", connection.type);
    }
    return header;
}

void BagWriter::write_bag_header() {
    std::vector<uint8_t> header;
    append_u8_field(header, "op", 0x03);
    append_u64_field(header, "index_pos", 0);
    append_u32_field(header, "conn_count", 0);
    append_u32_field(header, "chunk_count", 0);

    const uint64_t header_start = static_cast<uint64_t>(out_.tellp());
    std::vector<uint8_t> encoded_header;
    append_record(encoded_header, header, std::vector<uint8_t>{});
    write_bytes(encoded_header);

    // header_start points at the 4-byte header length; field values start after
    // the 4-byte length, the 4-byte field length, and "name=".
    header_index_pos_offset_ = header_start + 4 + find_field_value_offset(header, "index_pos");
    header_conn_count_offset_ = header_start + 4 + find_field_value_offset(header, "conn_count");
    header_chunk_count_offset_ = header_start + 4 + find_field_value_offset(header, "chunk_count");
}

void BagWriter::add_connection(const BagConnection &connection) {
    if (connections_emitted_ || !pending_.empty() || !chunk_.empty()) {
        throw std::runtime_error("bag connections must be added before messages");
    }
    for (const auto &existing : connections_) {
        if (existing.id == connection.id) {
            throw std::runtime_error("duplicate bag connection id");
        }
    }
    connections_.push_back(connection);
}

void BagWriter::append_connection_records() {
    for (const auto &connection : connections_) {
        std::vector<uint8_t> first = connection_header(connection, true);
        std::vector<uint8_t> second = connection_header(connection, false);
        append_record_header(chunk_, first);
        append_record_header(chunk_, second);
    }
    connections_emitted_ = true;
}

void BagWriter::add_message(uint32_t conn_id, uint64_t time_ns, const std::vector<uint8_t> &payload) {
    bool known = false;
    for (const auto &connection : connections_) {
        if (connection.id == conn_id) {
            known = true;
            break;
        }
    }
    if (!known) {
        throw std::runtime_error("bag message references unknown connection " + std::to_string(conn_id));
    }
    if (!connections_emitted_) {
        append_connection_records();
    }

    std::vector<uint8_t> header;
    append_u8_field(header, "op", 0x02);
    append_u32_field(header, "conn", conn_id);
    append_u64_field(header, "time", time_ns);

    std::vector<uint8_t> record;
    append_record(record, header, payload);
    if (!pending_.empty() && chunk_.size() + record.size() > kChunkLimit) {
        flush_chunk();
    }

    PendingIndex entry;
    entry.conn_id = conn_id;
    entry.time_ns = time_ns;
    entry.offset = static_cast<uint32_t>(chunk_.size());
    chunk_.insert(chunk_.end(), record.begin(), record.end());
    pending_.push_back(entry);
}

void BagWriter::flush_chunk() {
    if (chunk_.empty()) return;
    if (chunk_.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("bag chunk is too large");
    }

    ChunkInfo info;
    info.file_offset = static_cast<uint64_t>(out_.tellp());
    info.start_time = std::numeric_limits<uint64_t>::max();
    info.end_time = 0;
    for (const auto &entry : pending_) {
        info.start_time = std::min(info.start_time, entry.time_ns);
        info.end_time = std::max(info.end_time, entry.time_ns);
        info.counts[entry.conn_id]++;
    }
    if (pending_.empty()) {
        info.start_time = 0;
        info.end_time = 0;
    }

    std::vector<uint8_t> compressed = lz4_compress(chunk_);
    std::vector<uint8_t> header;
    append_u8_field(header, "op", 0x05);
    append_string_field(header, "compression", "lz4");
    append_u32_field(header, "size", static_cast<uint32_t>(chunk_.size()));
    // A chunk record has no outer data-length field: its payload is the
    // 4-byte compressed size followed by the compressed bytes.
    std::vector<uint8_t> encoded;
    append_record_header(encoded, header);
    append_u32(encoded, static_cast<uint32_t>(compressed.size()));
    encoded.insert(encoded.end(), compressed.begin(), compressed.end());
    write_bytes(encoded);

    chunks_.push_back(std::move(info));
    chunk_.clear();
    pending_.clear();
}

void BagWriter::close() {
    if (closed_) return;
    closed_ = true;

    if (!connections_emitted_) {
        append_connection_records();
    }
    flush_chunk();

    const uint64_t index_pos = static_cast<uint64_t>(out_.tellp());
    for (const auto &connection : connections_) {
        std::vector<uint8_t> first = connection_header(connection, true);
        std::vector<uint8_t> second = connection_header(connection, false);
        std::vector<uint8_t> encoded;
        append_record_header(encoded, first);
        append_record_header(encoded, second);
        write_bytes(encoded);
    }
    for (const auto &chunk : chunks_) {
        std::vector<uint8_t> header;
        append_u8_field(header, "op", 0x06);
        append_u32_field(header, "ver", 1);
        append_u64_field(header, "chunk_pos", chunk.file_offset);
        append_u64_field(header, "start_time", chunk.start_time);
        append_u64_field(header, "end_time", chunk.end_time);
        append_u32_field(header, "count", static_cast<uint32_t>(chunk.counts.size()));
        std::vector<uint8_t> data;
        for (const auto &kv : chunk.counts) {
            append_u32(data, kv.first);
            append_u32(data, kv.second);
        }
        std::vector<uint8_t> encoded;
        append_record(encoded, header, data);
        write_bytes(encoded);
    }
    out_.flush();
    patch_u64(header_index_pos_offset_, index_pos);
    patch_u32(header_conn_count_offset_, static_cast<uint32_t>(connections_.size()));
    patch_u32(header_chunk_count_offset_, static_cast<uint32_t>(chunks_.size()));
    out_.seekp(0, std::ios::end);
    if (!out_) {
        throw std::runtime_error("failed finalizing bag: " + path_);
    }
    out_.close();
}

}  // namespace bag2mkv
