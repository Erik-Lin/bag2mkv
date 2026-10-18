#include "scan.hpp"
#include "errors.hpp"
#include "util.hpp"

#include <algorithm>
#include <stdexcept>

namespace bag2mkv {

ScanResult scan_bag(const std::string &path, bool hash_frames, const ScanProgress &progress) {
    BagReader reader(path);
    ScanResult result;
    result.source_path = path;
    result.source_size = reader.file_size();
    result.index_pos = reader.index_pos();
    result.declared_chunk_count = reader.declared_chunk_count();
    result.declared_conn_count = reader.declared_conn_count();

    BagCallbacks cb;
    cb.on_connection = [&](const Connection &conn) { result.connections[conn.id] = conn; };
    cb.on_profile = [&](const StreamProfile &profile) { result.profiles[profile.stream_type] = profile; };
    cb.on_property = [&](const PropertyMessage &property) { result.properties.push_back(property); };
    cb.on_device_info = [&](const DeviceInfo &device) { result.device = device; };
    cb.on_chunk = [&](uint32_t current, uint32_t total) {
        if (progress && !progress(current, total ? total : 1)) {
            throw CancelledError();
        }
    };
    cb.on_image = [&](const ImageMessage &msg, const uint8_t *data, size_t size) {
        FrameMeta frame;
        frame.conn_id = msg.conn_id;
        frame.kind = msg.kind;
        frame.topic = result.connections.count(msg.conn_id) ? result.connections[msg.conn_id].topic : std::string();
        frame.encoding = msg.encoding;
        frame.width = msg.width;
        frame.height = msg.height;
        frame.step = msg.step;
        frame.timestamp_usec = msg.timestamp_usec;
        frame.timestamp_systemusec = msg.timestamp_systemusec;
        frame.timestamp_globalusec = msg.timestamp_globalusec;
        frame.number = msg.number;
        frame.header_seq = msg.header_seq;
        frame.frame_id = msg.frame_id;
        frame.metadatasize = msg.metadatasize;
        frame.pixel_bit_size = msg.pixel_bit_size;
        frame.metadata_hex = to_hex(msg.metadata.data(), msg.metadata.size());
        frame.data_size = size;
        frame.chunk_offset = msg.chunk_offset;
        frame.payload_offset = msg.payload_offset;
        if (hash_frames) {
            frame.sha256 = sha256_hex(data, size);
        }
        if (frame.timestamp_usec == 0) {
            throw std::runtime_error("frame " + std::to_string(result.frames.size()) + " has a zero timestamp");
        }
        if (result.first_timestamp_usec == 0 || frame.timestamp_usec < result.first_timestamp_usec) {
            result.first_timestamp_usec = frame.timestamp_usec;
        }
        if (frame.timestamp_usec > result.last_timestamp_usec) {
            result.last_timestamp_usec = frame.timestamp_usec;
        }
        result.frame_counts[frame.kind]++;
        result.frames.push_back(std::move(frame));
    };

    reader.scan(cb);
    result.chunk_count = reader.last_chunk_count();
    std::stable_sort(result.frames.begin(), result.frames.end(), [](const FrameMeta &a, const FrameMeta &b) {
        if (a.timestamp_usec != b.timestamp_usec) {
            return a.timestamp_usec < b.timestamp_usec;
        }
        return a.kind < b.kind;
    });
    return result;
}

}  // namespace bag2mkv
