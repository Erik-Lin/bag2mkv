#include "bag.hpp"
#include "lz4_util.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace bag2mkv {
namespace {

uint32_t load_u32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t load_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

uint32_t parse_digits(const std::string &s, const std::string &needle, uint32_t fallback) {
    size_t pos = s.find(needle);
    if (pos == std::string::npos) {
        return fallback;
    }
    pos += needle.size();
    uint32_t value = 0;
    bool any = false;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        any = true;
        value = value * 10 + uint32_t(s[pos] - '0');
        ++pos;
    }
    return any ? value : fallback;
}

class ByteReader {
public:
    ByteReader(const uint8_t *data, size_t size) : data_(data), size_(size) {}

    size_t remaining() const { return size_ - pos_; }
    bool at_end() const { return pos_ == size_; }
    size_t position() const { return pos_; }

    uint8_t u8() {
        need(1);
        return data_[pos_++];
    }
    uint16_t u16() {
        need(2);
        uint16_t v = uint16_t(data_[pos_]) | (uint16_t(data_[pos_ + 1]) << 8);
        pos_ += 2;
        return v;
    }
    uint32_t u32() {
        need(4);
        uint32_t v = load_u32(data_ + pos_);
        pos_ += 4;
        return v;
    }
    uint64_t u64() {
        need(8);
        uint64_t v = load_u64(data_ + pos_);
        pos_ += 8;
        return v;
    }
    float f32() {
        uint32_t bits = u32();
        float v = 0.0f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    std::string str() {
        uint32_t n = u32();
        need(n);
        std::string s(reinterpret_cast<const char *>(data_ + pos_), n);
        pos_ += n;
        return s;
    }
    const uint8_t *take(size_t n) {
        need(n);
        const uint8_t *p = data_ + pos_;
        pos_ += n;
        return p;
    }

private:
    void need(size_t n) const {
        if (n > size_ - pos_) {
            throw std::runtime_error("truncated bag message payload");
        }
    }

    const uint8_t *data_;
    size_t size_;
    size_t pos_ = 0;
};

}  // namespace

std::string frame_kind(uint32_t frame_type) {
    switch (frame_type) {
    case 1: return "IR";
    case 2: return "COLOR";
    case 3: return "DEPTH";
    case 4: return "ACCEL";
    case 5: return "SET";
    case 6: return "POINTS";
    case 7: return "GYRO";
    case 8: return "IR_LEFT";
    case 9: return "IR_RIGHT";
    case 10: return "RAW_PHASE";
    case 11: return "CONFIDENCE";
    case 12: return "LIDAR";
    case 13: return "COLOR_LEFT";
    case 14: return "COLOR_RIGHT";
    default: return "UNKNOWN_" + std::to_string(frame_type);
    }
}

std::string stream_kind(uint32_t stream_type) {
    switch (stream_type) {
    case 1: return "IR";
    case 2: return "COLOR";
    case 3: return "DEPTH";
    case 4: return "ACCEL";
    case 5: return "GYRO";
    case 6: return "IR_LEFT";
    case 7: return "IR_RIGHT";
    case 10: return "LIDAR";
    default: return "UNKNOWN_" + std::to_string(stream_type);
    }
}

BagReader::BagReader(const std::string &path) : path_(path) {
    file_.open(path_, std::ios::binary);
    if (!file_) {
        throw std::runtime_error("cannot open bag: " + path_);
    }
    file_.seekg(0, std::ios::end);
    std::streamoff end = file_.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot determine bag size: " + path_);
    }
    file_size_ = static_cast<uint64_t>(end);
    read_bag_header();
}

BagReader::~BagReader() = default;

void BagReader::read_exact(void *dst, size_t size) {
    if (size == 0) {
        return;
    }
    file_.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(size));
    if (!file_ || static_cast<size_t>(file_.gcount()) != size) {
        throw std::runtime_error("unexpected end of bag file at offset " + std::to_string(tellg()));
    }
}

uint32_t BagReader::read_u32() {
    uint8_t b[4];
    read_exact(b, sizeof(b));
    return load_u32(b);
}

uint64_t BagReader::read_u64() {
    uint8_t b[8];
    read_exact(b, sizeof(b));
    return load_u64(b);
}

uint64_t BagReader::tellg() {
    std::streampos p = file_.tellg();
    if (p < 0) {
        throw std::runtime_error("bag seek position is invalid");
    }
    return static_cast<uint64_t>(p);
}

void BagReader::seekg(uint64_t pos) {
    if (pos > file_size_) {
        throw std::runtime_error("bag seek beyond end of file");
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
    if (!file_) {
        throw std::runtime_error("bag seek failed at offset " + std::to_string(pos));
    }
}

std::vector<uint8_t> BagReader::read_bytes(size_t size) {
    std::vector<uint8_t> data(size);
    read_exact(data.data(), data.size());
    return data;
}

BagReader::Fields BagReader::read_header_fields() {
    uint64_t offset = tellg();
    uint32_t len = read_u32();
    if (len > 64 * 1024 * 1024) {
        throw std::runtime_error("bag record header is unreasonably large: " + std::to_string(len) + " at offset " +
                                 std::to_string(offset));
    }
    std::vector<uint8_t> buf = read_bytes(len);
    size_t pos = 0;
    return parse_fields(buf.data(), buf.size(), pos);
}

BagReader::Fields BagReader::parse_fields(const uint8_t *data, size_t size, size_t &pos) {
    Fields fields;
    while (pos < size) {
        if (size - pos < 4) {
            throw std::runtime_error("truncated bag field length");
        }
        uint32_t len = load_u32(data + pos);
        pos += 4;
        if (len > size - pos) {
            throw std::runtime_error("truncated bag field value");
        }
        const uint8_t *field = data + pos;
        pos += len;
        const uint8_t *eq = static_cast<const uint8_t *>(std::memchr(field, '=', len));
        if (eq == nullptr) {
            // Orbbec's writer sometimes emits a padding header with only spaces.
            continue;
        }
        size_t name_len = static_cast<size_t>(eq - field);
        std::string name(reinterpret_cast<const char *>(field), name_len);
        fields.values[name] = std::vector<uint8_t>(eq + 1, field + len);
    }
    return fields;
}

uint32_t BagReader::field_u32(const Fields &fields, const std::string &name) {
    auto it = fields.values.find(name);
    if (it == fields.values.end() || it->second.size() != 4) {
        throw std::runtime_error("bag field missing or invalid: " + name);
    }
    return load_u32(it->second.data());
}

uint64_t BagReader::field_u64(const Fields &fields, const std::string &name) {
    auto it = fields.values.find(name);
    if (it == fields.values.end() || it->second.size() != 8) {
        throw std::runtime_error("bag field missing or invalid: " + name);
    }
    return load_u64(it->second.data());
}

std::string BagReader::field_string(const Fields &fields, const std::string &name) {
    auto it = fields.values.find(name);
    if (it == fields.values.end()) {
        throw std::runtime_error("bag field missing: " + name);
    }
    return std::string(it->second.begin(), it->second.end());
}

void BagReader::read_bag_header() {
    seekg(0);
    uint8_t magic[13];
    read_exact(magic, sizeof(magic));
    static const char kMagic[] = "#ROSBAG V2.0\n";
    if (std::memcmp(magic, kMagic, 13) != 0) {
        throw std::runtime_error("not a ROSBAG V2.0 file: " + path_);
    }
    Fields header = read_header_fields();
    auto op_it = header.values.find("op");
    if (op_it == header.values.end() || op_it->second.size() != 1 || op_it->second[0] != 0x03) {
        throw std::runtime_error("bag header record is missing op=0x03");
    }
    index_pos_ = field_u64(header, "index_pos");
    declared_chunk_count_ = field_u32(header, "chunk_count");
    declared_conn_count_ = field_u32(header, "conn_count");
    if (index_pos_ > file_size_) {
        throw std::runtime_error("bag index position is beyond end of file");
    }
}

void BagReader::scan(const BagCallbacks &callbacks) {
    seekg(0);
    uint8_t magic[13];
    read_exact(magic, sizeof(magic));
    Fields header = read_header_fields();
    if (field_u64(header, "index_pos") != index_pos_) {
        throw std::runtime_error("bag header changed between scans");
    }
    uint32_t header_data_len = read_u32();
    if (header_data_len > file_size_ - tellg()) {
        throw std::runtime_error("bag header padding is truncated");
    }
    seekg(tellg() + header_data_len);

    connections_.clear();
    uint32_t chunk_count = 0;

    while (tellg() < index_pos_) {
        uint64_t record_pos = tellg();
        Fields rec = read_header_fields();
        auto op_it = rec.values.find("op");
        if (op_it == rec.values.end() || op_it->second.size() != 1) {
            throw std::runtime_error("bag record missing op at offset " + std::to_string(record_pos));
        }
        uint8_t op = op_it->second[0];
        if (op == 0x05) {
            std::string compression = "none";
            auto comp_it = rec.values.find("compression");
            if (comp_it != rec.values.end()) {
                compression.assign(comp_it->second.begin(), comp_it->second.end());
            }
            uint32_t compressed_size = read_u32();
            if (compressed_size > file_size_ - tellg()) {
                throw std::runtime_error("bag chunk is truncated at offset " + std::to_string(record_pos));
            }
            std::vector<uint8_t> compressed = read_bytes(compressed_size);
            std::vector<uint8_t> chunk;
            if (compression == "lz4") {
                chunk = lz4_decompress(compressed);
            } else if (compression == "none" || compression.empty()) {
                chunk = std::move(compressed);
            } else if (compression == "bz2") {
                throw std::runtime_error("bz2-compressed bag chunks are not supported in this version");
            } else {
                throw std::runtime_error("unsupported bag chunk compression: " + compression);
            }
            parse_chunk(chunk, record_pos, callbacks);
            ++chunk_count;
            if (callbacks.on_chunk) {
                callbacks.on_chunk(chunk_count, declared_chunk_count_);
            }
        } else if (op == 0x07) {
            Fields second = read_header_fields();
            handle_connection(rec, second, callbacks);
        } else {
            uint32_t data_len = read_u32();
            if (data_len > file_size_ - tellg()) {
                throw std::runtime_error("bag record data is truncated at offset " + std::to_string(record_pos));
            }
            seekg(tellg() + data_len);
        }
    }

    if (tellg() != index_pos_) {
        throw std::runtime_error("bag scan ended at unexpected offset " + std::to_string(tellg()));
    }
    last_chunk_count_ = chunk_count;
    if (declared_chunk_count_ != 0 && chunk_count != declared_chunk_count_) {
        // Some writers are sloppy; this is a warning-level condition, not fatal.
    }
}

std::vector<uint8_t> BagReader::read_chunk(uint64_t chunk_offset) {
    seekg(chunk_offset);
    Fields rec = read_header_fields();
    auto op_it = rec.values.find("op");
    if (op_it == rec.values.end() || op_it->second.size() != 1 || op_it->second[0] != 0x05) {
        throw std::runtime_error("offset does not point to a chunk: " + std::to_string(chunk_offset));
    }
    std::string compression = "none";
    auto comp_it = rec.values.find("compression");
    if (comp_it != rec.values.end()) {
        compression.assign(comp_it->second.begin(), comp_it->second.end());
    }
    uint32_t compressed_size = read_u32();
    if (compressed_size > file_size_ - tellg()) {
        throw std::runtime_error("chunk is truncated at offset " + std::to_string(chunk_offset));
    }
    std::vector<uint8_t> compressed = read_bytes(compressed_size);
    if (compression == "lz4") {
        return lz4_decompress(compressed);
    }
    if (compression == "none" || compression.empty()) {
        return compressed;
    }
    throw std::runtime_error("unsupported chunk compression: " + compression);
}

void BagReader::parse_chunk(const std::vector<uint8_t> &chunk, uint64_t chunk_offset, const BagCallbacks &callbacks) {
    size_t pos = 0;
    auto read_chunk_header = [&]() -> Fields {
        if (chunk.size() - pos < 4) {
            throw std::runtime_error("truncated chunk record length");
        }
        uint32_t len = load_u32(chunk.data() + pos);
        pos += 4;
        if (len > chunk.size() - pos) {
            throw std::runtime_error("truncated chunk record header");
        }
        size_t end = pos + len;
        Fields fields = parse_fields(chunk.data(), end, pos);
        if (pos != end) {
            throw std::runtime_error("chunk record header length mismatch");
        }
        return fields;
    };

    while (pos < chunk.size()) {
        size_t record_pos = pos;
        Fields rec = read_chunk_header();
        auto op_it = rec.values.find("op");
        if (op_it == rec.values.end() || op_it->second.size() != 1) {
            throw std::runtime_error("chunk record missing op at chunk offset " + std::to_string(record_pos) +
                                     " (chunk size " + std::to_string(chunk.size()) + ")");
        }
        uint8_t op = op_it->second[0];
        if (op == 0x07) {
            Fields second = read_chunk_header();
            handle_connection(rec, second, callbacks);
        } else if (op == 0x02) {
            uint32_t conn_id = field_u32(rec, "conn");
            uint64_t time = field_u64(rec, "time");
            if (chunk.size() - pos < 4) {
                throw std::runtime_error("truncated message payload length");
            }
            uint32_t payload_size = load_u32(chunk.data() + pos);
            pos += 4;
            if (payload_size > chunk.size() - pos) {
                throw std::runtime_error("truncated message payload");
            }
            uint32_t payload_offset = static_cast<uint32_t>(pos);
            handle_message(conn_id, time / 1000000000ULL, time % 1000000000ULL, chunk.data() + pos, payload_size,
                           chunk_offset, payload_offset, callbacks);
            pos += payload_size;
        } else {
            throw std::runtime_error("unexpected record op inside chunk: " + std::to_string(op));
        }
    }
}

void BagReader::handle_connection(const Fields &first, const Fields &second, const BagCallbacks &callbacks) {
    Connection conn;
    conn.id = field_u32(first, "conn");
    auto topic_it = first.values.find("topic");
    if (topic_it != first.values.end()) {
        conn.topic.assign(topic_it->second.begin(), topic_it->second.end());
    }
    auto type_it = second.values.find("type");
    if (type_it != second.values.end()) {
        conn.type.assign(type_it->second.begin(), type_it->second.end());
    }
    auto md5_it = second.values.find("md5sum");
    if (md5_it != second.values.end()) {
        conn.md5.assign(md5_it->second.begin(), md5_it->second.end());
    }
    auto def_it = second.values.find("message_definition");
    if (def_it != second.values.end()) {
        conn.definition.assign(def_it->second.begin(), def_it->second.end());
    }
    auto existing = connections_.find(conn.id);
    bool changed = existing == connections_.end() || existing->second.type != conn.type ||
                   existing->second.topic != conn.topic;
    connections_[conn.id] = conn;
    if (changed && callbacks.on_connection) {
        callbacks.on_connection(conn);
    }
}

void BagReader::handle_message(uint32_t conn_id, uint64_t time_sec, uint64_t time_nsec, const uint8_t *payload,
                               size_t size, uint64_t chunk_offset, uint32_t payload_offset,
                               const BagCallbacks &callbacks) {
    (void)time_sec;
    (void)time_nsec;
    auto it = connections_.find(conn_id);
    if (it == connections_.end()) {
        throw std::runtime_error("message references unknown connection " + std::to_string(conn_id));
    }
    const Connection &conn = it->second;
    const std::string &type = conn.type;

    if (type == "sensor_msgs/Image" || type == "sensor_msgs/msg/Image") {
        uint32_t sensor_type = parse_digits(conn.topic, "/sensor_", 0);
        uint32_t frame_type = parse_digits(conn.topic, "/frameType_", 0);
        parse_image(conn_id, sensor_type, frame_type, frame_kind(frame_type), payload, size, chunk_offset,
                    payload_offset, callbacks);
    } else if (type == "custom_msg/OBStreamProfileInfo") {
        parse_profile(payload, size, callbacks);
    } else if (type == "custom_msg/OBProperty") {
        parse_property(payload, size, callbacks);
    } else if (type == "custom_msg/OBDeviceInfo") {
        parse_device_info(payload, size, callbacks);
    } else if (type == "sensor_msgs/Imu" || type == "sensor_msgs/PointCloud2") {
        throw std::runtime_error("unsupported message type in this version: " + type + " (" + conn.topic + ")");
    } else {
        throw std::runtime_error("unsupported bag topic type: " + type + " (" + conn.topic + ")");
    }
}

void BagReader::parse_image(uint32_t conn_id, uint32_t sensor_type, uint32_t frame_type, const std::string &kind,
                            const uint8_t *payload, size_t size, uint64_t chunk_offset, uint32_t payload_offset,
                            const BagCallbacks &callbacks) {
    ByteReader r(payload, size);
    ImageMessage msg;
    msg.conn_id = conn_id;
    msg.sensor_type = sensor_type;
    msg.frame_type = frame_type;
    msg.kind = kind;
    msg.chunk_offset = chunk_offset;
    msg.payload_offset = payload_offset;
    msg.header_seq = r.u32();
    msg.stamp_sec = r.u32();
    msg.stamp_nsec = r.u32();
    msg.frame_id = r.str();
    msg.height = r.u32();
    msg.width = r.u32();
    msg.encoding = r.str();
    msg.is_bigendian = r.u8();
    msg.step = r.u32();
    uint32_t data_len = r.u32();
    const uint8_t *data = r.take(data_len);
    msg.data_size = data_len;
    msg.payload_offset = payload_offset + uint32_t(r.position() - data_len);
    msg.depth_units = r.f32();
    msg.number = r.u64();
    msg.timestamp_usec = r.u64();
    msg.timestamp_systemusec = r.u64();
    msg.timestamp_globalusec = r.u64();
    uint32_t meta_len = r.u32();
    const uint8_t *meta = r.take(meta_len);
    msg.metadata.assign(meta, meta + meta_len);
    msg.metadatasize = r.u32();
    msg.pixel_bit_size = r.u8();
    if (!r.at_end()) {
        throw std::runtime_error("image message has trailing bytes");
    }
    if (msg.timestamp_usec == 0) {
        msg.timestamp_usec = uint64_t(msg.stamp_sec) * 1000000ULL + msg.stamp_nsec / 1000ULL;
    }
    if (callbacks.on_image) {
        callbacks.on_image(msg, data, data_len);
    }
}

void BagReader::parse_profile(const uint8_t *payload, size_t size, const BagCallbacks &callbacks) {
    ByteReader r(payload, size);
    StreamProfile p;
    r.u32();  // seq
    uint32_t sec = r.u32();
    uint32_t nsec = r.u32();
    r.str();  // frame_id
    p.stream_type = r.u8();
    p.format = r.u8();
    for (float &v : p.rotation) {
        v = r.f32();
    }
    for (float &v : p.translation) {
        v = r.f32();
    }
    p.width = r.u16();
    p.height = r.u16();
    p.fps = r.u16();
    for (float &v : p.intrinsic) {
        v = r.f32();
    }
    for (float &v : p.distortion) {
        v = r.f32();
    }
    p.distortion_model = r.u8();
    if (!r.at_end()) {
        throw std::runtime_error("stream profile message has trailing bytes");
    }
    p.timestamp_usec = uint64_t(sec) * 1000000ULL + nsec / 1000ULL;
    if (callbacks.on_profile) {
        callbacks.on_profile(p);
    }
}

void BagReader::parse_property(const uint8_t *payload, size_t size, const BagCallbacks &callbacks) {
    ByteReader r(payload, size);
    PropertyMessage p;
    r.u32();  // seq
    uint32_t sec = r.u32();
    uint32_t nsec = r.u32();
    r.str();  // frame_id
    p.id = r.u32();
    uint32_t data_len = r.u32();
    const uint8_t *data = r.take(data_len);
    p.data.assign(data, data + data_len);
    p.datasize = r.u32();
    if (!r.at_end()) {
        throw std::runtime_error("property message has trailing bytes");
    }
    p.timestamp_usec = uint64_t(sec) * 1000000ULL + nsec / 1000ULL;
    if (callbacks.on_property) {
        callbacks.on_property(p);
    }
}

void BagReader::parse_device_info(const uint8_t *payload, size_t size, const BagCallbacks &callbacks) {
    ByteReader r(payload, size);
    DeviceInfo d;
    r.u32();  // seq
    uint32_t sec = r.u32();
    uint32_t nsec = r.u32();
    r.str();  // frame_id
    d.name = r.str();
    d.full_name = r.str();
    d.asic_name = r.str();
    d.vid = r.u16();
    d.pid = r.u16();
    d.uid = r.str();
    d.sn = r.str();
    d.fw_version = r.str();
    d.hw_version = r.str();
    d.supported_sdk_version = r.str();
    d.connection_type = r.str();
    d.type = r.u16();
    d.backend_type = r.u8();
    d.ip_address = r.str();
    d.local_mac = r.str();
    if (!r.at_end()) {
        throw std::runtime_error("device info message has trailing bytes");
    }
    d.present = true;
    d.timestamp_usec = uint64_t(sec) * 1000000ULL + nsec / 1000ULL;
    if (callbacks.on_device_info) {
        callbacks.on_device_info(d);
    }
}

}  // namespace bag2mkv
