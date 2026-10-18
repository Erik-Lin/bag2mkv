#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace bag2mkv {

struct BagConnection {
    uint32_t id = 0;
    std::string topic;
    std::string type;
    std::string md5;
    std::string definition;
};

// Writes a ROSBAG V2.0 stream compatible with the Orbbec rosbag reader.
// Chunks use LZ4 frames; the bag reader also accepts compression=none/bz2.
// Connection records are written in the first chunk and again in the index
// section, matching the original bag.
enum class BagFlavor {
    Orbbec,
    Ros1,
};

class BagWriter {
public:
    explicit BagWriter(const std::string &path, BagFlavor flavor = BagFlavor::Orbbec);
    ~BagWriter();

    BagWriter(const BagWriter &) = delete;
    BagWriter &operator=(const BagWriter &) = delete;

    void add_connection(const BagConnection &connection);
    void add_message(uint32_t conn_id, uint64_t time_ns, const std::vector<uint8_t> &payload);
    void close();

private:
    struct PendingIndex {
        uint32_t conn_id = 0;
        uint64_t time_ns = 0;
        uint32_t offset = 0;
    };

    struct ChunkInfo {
        uint64_t file_offset = 0;
        uint64_t start_time = 0;
        uint64_t end_time = 0;
        std::map<uint32_t, uint32_t> counts;
    };

    std::ofstream out_;
    std::string path_;
    BagFlavor flavor_ = BagFlavor::Orbbec;
    std::vector<BagConnection> connections_;
    std::vector<uint8_t> chunk_;
    std::vector<PendingIndex> pending_;
    std::vector<ChunkInfo> chunks_;
    bool connections_emitted_ = false;
    bool closed_ = false;

    uint64_t header_index_pos_offset_ = 0;
    uint64_t header_conn_count_offset_ = 0;
    uint64_t header_chunk_count_offset_ = 0;

    void write_bytes(const uint8_t *data, size_t size);
    void write_bytes(const std::vector<uint8_t> &data);
    void patch_u32(uint64_t offset, uint32_t value);
    void patch_u64(uint64_t offset, uint64_t value);

    static void append_u32(std::vector<uint8_t> &out, uint32_t value);
    static void append_u64(std::vector<uint8_t> &out, uint64_t value);
    static void append_field(std::vector<uint8_t> &out, const std::string &name,
                             const std::vector<uint8_t> &value);
    static void append_string_field(std::vector<uint8_t> &out, const std::string &name,
                                    const std::string &value);
    static void append_u32_field(std::vector<uint8_t> &out, const std::string &name, uint32_t value);
    static void append_u64_field(std::vector<uint8_t> &out, const std::string &name, uint64_t value);
    static void append_time_field(std::vector<uint8_t> &out, const std::string &name, uint64_t time_ns);
    static void append_u8_field(std::vector<uint8_t> &out, const std::string &name, uint8_t value);

    static void append_record_header(std::vector<uint8_t> &out, const std::vector<uint8_t> &header);
    static void append_record(std::vector<uint8_t> &out, const std::vector<uint8_t> &header,
                              const std::vector<uint8_t> &data);
    static std::vector<uint8_t> connection_header(const BagConnection &connection, bool first);

    void write_bag_header();
    void append_connection_records();
    void flush_chunk();
};

}  // namespace bag2mkv
