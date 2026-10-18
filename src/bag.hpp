#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace bag2mkv {

struct Connection {
    uint32_t id = 0;
    std::string topic;
    std::string type;
    std::string md5;
    std::string definition;
};

struct DeviceInfo {
    bool present = false;
    std::string name;
    std::string full_name;
    std::string asic_name;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string uid;
    std::string sn;
    std::string fw_version;
    std::string hw_version;
    std::string supported_sdk_version;
    std::string connection_type;
    uint16_t type = 0;
    uint8_t backend_type = 0;
    std::string ip_address;
    std::string local_mac;
    uint64_t timestamp_usec = 0;
};

struct StreamProfile {
    uint64_t timestamp_usec = 0;
    uint8_t stream_type = 0;
    uint8_t format = 0;
    float rotation[9] = {};
    float translation[3] = {};
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t fps = 0;
    float intrinsic[4] = {};   // fx, fy, cx, cy
    float distortion[8] = {};  // k1..k6, p1, p2
    uint8_t distortion_model = 0;
};

struct PropertyMessage {
    uint64_t timestamp_usec = 0;
    uint32_t id = 0;
    uint32_t datasize = 0;
    std::vector<uint8_t> data;
};

struct ImageMessage {
    uint32_t conn_id = 0;
    uint32_t sensor_type = 0;
    uint32_t frame_type = 0;
    std::string kind;  // COLOR / DEPTH / IR / ...
    uint32_t header_seq = 0;
    uint32_t stamp_sec = 0;
    uint32_t stamp_nsec = 0;
    std::string frame_id;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string encoding;
    uint8_t is_bigendian = 0;
    uint32_t step = 0;
    float depth_units = 0.0f;
    uint64_t number = 0;
    uint64_t timestamp_usec = 0;
    uint64_t timestamp_systemusec = 0;
    uint64_t timestamp_globalusec = 0;
    std::vector<uint8_t> metadata;
    uint32_t metadatasize = 0;
    uint8_t pixel_bit_size = 0;
    uint32_t data_size = 0;
    uint64_t chunk_offset = 0;
    uint32_t payload_offset = 0;
};

struct FrameMeta {
    uint32_t conn_id = 0;
    std::string kind;
    std::string topic;
    std::string encoding;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t step = 0;
    uint64_t timestamp_usec = 0;
    uint64_t timestamp_systemusec = 0;
    uint64_t timestamp_globalusec = 0;
    uint64_t number = 0;
    uint32_t header_seq = 0;
    std::string frame_id;
    uint32_t metadatasize = 0;
    uint8_t pixel_bit_size = 0;
    std::string metadata_hex;
    uint64_t data_size = 0;
    std::string sha256;
    uint64_t chunk_offset = 0;
    uint32_t payload_offset = 0;
};

struct ScanResult {
    std::string source_path;
    uint64_t source_size = 0;
    uint64_t index_pos = 0;
    uint32_t declared_chunk_count = 0;
    uint32_t declared_conn_count = 0;
    uint32_t chunk_count = 0;
    std::map<uint32_t, Connection> connections;
    std::map<uint8_t, StreamProfile> profiles;
    std::vector<PropertyMessage> properties;
    DeviceInfo device;
    std::vector<FrameMeta> frames;
    std::map<std::string, uint64_t> frame_counts;
    uint64_t first_timestamp_usec = 0;
    uint64_t last_timestamp_usec = 0;
};

struct BagCallbacks {
    std::function<void(const Connection &)> on_connection;
    std::function<void(const ImageMessage &, const uint8_t *data, size_t size)> on_image;
    std::function<void(const StreamProfile &)> on_profile;
    std::function<void(const PropertyMessage &)> on_property;
    std::function<void(const DeviceInfo &)> on_device_info;
    std::function<void(uint32_t current, uint32_t total)> on_chunk;
};

class BagReader {
public:
    explicit BagReader(const std::string &path);
    ~BagReader();

    BagReader(const BagReader &) = delete;
    BagReader &operator=(const BagReader &) = delete;

    const std::string &path() const { return path_; }
    uint64_t file_size() const { return file_size_; }
    uint64_t index_pos() const { return index_pos_; }
    uint32_t declared_chunk_count() const { return declared_chunk_count_; }
    uint32_t declared_conn_count() const { return declared_conn_count_; }
    uint32_t last_chunk_count() const { return last_chunk_count_; }

    // Re-reads the whole bag. Callbacks may be empty; this is used for pass 1 and pass 2.
    void scan(const BagCallbacks &callbacks);

    // Reads and decompresses one top-level chunk at the given file offset.
    std::vector<uint8_t> read_chunk(uint64_t chunk_offset);

private:
    struct Fields {
        std::map<std::string, std::vector<uint8_t>> values;
    };

    std::string path_;
    std::ifstream file_;
    uint64_t file_size_ = 0;
    uint64_t index_pos_ = 0;
    uint32_t declared_chunk_count_ = 0;
    uint32_t declared_conn_count_ = 0;
    uint32_t last_chunk_count_ = 0;
    std::map<uint32_t, Connection> connections_;

    void read_exact(void *dst, size_t size);
    uint32_t read_u32();
    uint64_t read_u64();
    uint64_t tellg();
    void seekg(uint64_t pos);
    std::vector<uint8_t> read_bytes(size_t size);

    Fields read_header_fields();
    Fields parse_fields(const uint8_t *data, size_t size, size_t &pos);
    static uint32_t field_u32(const Fields &fields, const std::string &name);
    static uint64_t field_u64(const Fields &fields, const std::string &name);
    static std::string field_string(const Fields &fields, const std::string &name);

    void read_bag_header();
    void parse_chunk(const std::vector<uint8_t> &chunk, uint64_t chunk_offset, const BagCallbacks &callbacks);
    void handle_connection(const Fields &first, const Fields &second, const BagCallbacks &callbacks);
    void handle_message(uint32_t conn_id, uint64_t time_sec, uint64_t time_nsec, const uint8_t *payload,
                        size_t size, uint64_t chunk_offset, uint32_t payload_offset, const BagCallbacks &callbacks);

    void parse_image(uint32_t conn_id, uint32_t sensor_type, uint32_t frame_type, const std::string &kind,
                     const uint8_t *payload, size_t size, uint64_t chunk_offset, uint32_t payload_offset,
                     const BagCallbacks &callbacks);
    void parse_profile(const uint8_t *payload, size_t size, const BagCallbacks &callbacks);
    void parse_property(const uint8_t *payload, size_t size, const BagCallbacks &callbacks);
    void parse_device_info(const uint8_t *payload, size_t size, const BagCallbacks &callbacks);
};

std::string frame_kind(uint32_t frame_type);
std::string stream_kind(uint32_t stream_type);

}  // namespace bag2mkv
