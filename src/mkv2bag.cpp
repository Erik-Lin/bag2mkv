#include "mkv2bag.hpp"

#include "bag.hpp"
#include "bag_writer.hpp"
#include "json.hpp"
#include "util.hpp"

#include <k4a/k4a.h>
#include <k4arecord/playback.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace bag2mkv {
namespace fs = std::filesystem;

namespace {

const char *kImageDefinition =
    "uint32 seq\n"
    "time stamp\n"
    "string frame_id\n"
    "uint32 height\n"
    "uint32 width\n"
    "string encoding\n"
    "uint8 is_bigendian\n"
    "uint32 step\n"
    "uint8[] data\n"
    "float32 depth_units\n"
    "uint64 number\n"
    "uint64 timestamp_usec\n"
    "uint64 timestamp_systemusec\n"
    "uint64 timestamp_globalusec\n"
    "uint8[] metadata\n"
    "uint32 metadatasize\n"
    "uint8 pixel_bit_size\n";

const char *kPropertyDefinition =
    "uint32 seq\n"
    "time stamp\n"
    "string frame_id\n"
    "uint32 propertyId\n"
    "uint8[] data\n"
    "uint32 datasize\n";

const char *kDeviceDefinition =
    "uint32 seq\n"
    "time stamp\n"
    "string frame_id\n"
    "string name\n"
    "string fullName\n"
    "string asicName\n"
    "uint16 vid\n"
    "uint16 pid\n"
    "string uid\n"
    "string sn\n"
    "string fwVersion\n"
    "string hwVersion\n"
    "string supportedSdkVersion\n"
    "string connectionType\n"
    "uint16 type\n"
    "uint8 backendType\n"
    "string ipAddress\n"
    "string localMac\n";

const char *kProfileDefinition =
    "uint32 seq\n"
    "time stamp\n"
    "string frame_id\n"
    "uint8 streamType\n"
    "uint8 format\n"
    "float32 rotationMatrix[9]\n"
    "float32 translationMatrix[3]\n"
    "uint16 width\n"
    "uint16 height\n"
    "uint16 fps\n"
    "float32 cameraIntrinsic[4]\n"
    "float32 cameraDistortion[8]\n"
    "uint8 distortionModel\n";

void log_line(const Mkv2BagLog &log, const std::string &message) {
    if (log) log(message);
}

void append_u8(std::vector<uint8_t> &out, uint8_t value) { out.push_back(value); }

void append_u16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(uint8_t(value & 0xFF));
    out.push_back(uint8_t((value >> 8) & 0xFF));
}

void append_u32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(uint8_t(value & 0xFF));
    out.push_back(uint8_t((value >> 8) & 0xFF));
    out.push_back(uint8_t((value >> 16) & 0xFF));
    out.push_back(uint8_t((value >> 24) & 0xFF));
}

void append_u64(std::vector<uint8_t> &out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t((value >> (i * 8)) & 0xFF));
}

void append_f32(std::vector<uint8_t> &out, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(out, bits);
}

void append_string(std::vector<uint8_t> &out, const std::string &value) {
    append_u32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void append_bytes(std::vector<uint8_t> &out, const uint8_t *data, size_t size) {
    append_u32(out, static_cast<uint32_t>(size));
    if (size) out.insert(out.end(), data, data + size);
}

std::vector<uint8_t> image_header(uint32_t seq, uint64_t timestamp_usec, const std::string &frame_id) {
    std::vector<uint8_t> out;
    append_u32(out, seq);
    append_u32(out, static_cast<uint32_t>(timestamp_usec / 1000000ULL));
    append_u32(out, static_cast<uint32_t>((timestamp_usec % 1000000ULL) * 1000ULL));
    append_string(out, frame_id);
    return out;
}

std::vector<uint8_t> image_payload(const FrameMeta *meta, uint64_t timestamp_usec, uint32_t width,
                                   uint32_t height, const std::string &encoding, uint32_t step,
                                   const std::vector<uint8_t> &data, uint8_t pixel_bit_size,
                                   float depth_units) {
    const uint32_t seq = meta ? meta->header_seq : 0;
    const std::string frame_id = meta ? meta->frame_id : std::string();
    std::vector<uint8_t> out = image_header(seq, timestamp_usec, frame_id);
    append_u32(out, height);
    append_u32(out, width);
    append_string(out, encoding);
    append_u8(out, 0);
    append_u32(out, step);
    append_bytes(out, data.data(), data.size());
    append_f32(out, depth_units);

    const uint64_t number = meta ? meta->number : 0;
    const uint64_t system = meta ? meta->timestamp_systemusec : timestamp_usec;
    const uint64_t global = meta ? meta->timestamp_globalusec : timestamp_usec;
    append_u64(out, number);
    append_u64(out, timestamp_usec);
    append_u64(out, system);
    append_u64(out, global);

    std::vector<uint8_t> metadata;
    if (meta && !meta->metadata_hex.empty()) {
        if (meta->metadata_hex.size() % 2 == 0) {
            metadata.reserve(meta->metadata_hex.size() / 2);
            for (size_t i = 0; i < meta->metadata_hex.size(); i += 2) {
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int hi = hex(meta->metadata_hex[i]);
                int lo = hex(meta->metadata_hex[i + 1]);
                if (hi < 0 || lo < 0) {
                    metadata.clear();
                    break;
                }
                metadata.push_back(static_cast<uint8_t>((hi << 4) | lo));
            }
        }
    }
    append_bytes(out, metadata.data(), metadata.size());
    append_u32(out, static_cast<uint32_t>(metadata.size()));
    append_u8(out, pixel_bit_size);
    return out;
}

std::vector<uint8_t> property_payload(const PropertyMessage &property) {
    std::vector<uint8_t> out = image_header(0, property.timestamp_usec, {});
    append_u32(out, property.id);
    append_bytes(out, property.data.data(), property.data.size());
    append_u32(out, property.datasize ? property.datasize : static_cast<uint32_t>(property.data.size()));
    return out;
}

std::vector<uint8_t> device_payload(const DeviceInfo &device) {
    std::vector<uint8_t> out = image_header(0, device.timestamp_usec, {});
    append_string(out, device.name);
    append_string(out, device.full_name);
    append_string(out, device.asic_name);
    append_u16(out, device.vid);
    append_u16(out, device.pid);
    append_string(out, device.uid);
    append_string(out, device.sn);
    append_string(out, device.fw_version);
    append_string(out, device.hw_version);
    append_string(out, device.supported_sdk_version);
    append_string(out, device.connection_type);
    append_u16(out, device.type);
    append_u8(out, device.backend_type);
    append_string(out, device.ip_address);
    append_string(out, device.local_mac);
    return out;
}

std::vector<uint8_t> profile_payload(const StreamProfile &profile) {
    std::vector<uint8_t> out = image_header(0, profile.timestamp_usec, {});
    append_u8(out, profile.stream_type);
    append_u8(out, profile.format);
    for (float value : profile.rotation) append_f32(out, value);
    for (float value : profile.translation) append_f32(out, value);
    append_u16(out, profile.width);
    append_u16(out, profile.height);
    append_u16(out, profile.fps);
    for (float value : profile.intrinsic) append_f32(out, value);
    for (float value : profile.distortion) append_f32(out, value);
    append_u8(out, profile.distortion_model);
    return out;
}

std::string str_field(const Json *object, const char *key) {
    if (!object) return {};
    const Json *value = object->find(key);
    return value ? value->as_string() : std::string();
}

uint64_t u64_field(const Json *object, const char *key, uint64_t fallback = 0) {
    if (!object) return fallback;
    const Json *value = object->find(key);
    return value ? value->as_u64(fallback) : fallback;
}

double number_field(const Json *object, const char *key, double fallback = 0.0) {
    if (!object) return fallback;
    const Json *value = object->find(key);
    return value ? value->as_double(fallback) : fallback;
}

bool read_attachment(k4a_playback_t playback, const char *name, std::vector<uint8_t> &out) {
    size_t size = 0;
    k4a_buffer_result_t result = k4a_playback_get_attachment(playback, name, nullptr, &size);
    if (result != K4A_BUFFER_RESULT_TOO_SMALL || size == 0) return false;
    out.resize(size);
    result = k4a_playback_get_attachment(playback, name, out.data(), &size);
    if (result != K4A_BUFFER_RESULT_SUCCEEDED) {
        out.clear();
        return false;
    }
    out.resize(size);
    return true;
}

struct Manifest {
    bool used = false;
    DeviceInfo device;
    std::vector<FrameMeta> frames;
    std::vector<PropertyMessage> properties;
    std::map<uint8_t, StreamProfile> profiles;
};

FrameMeta frame_from_json(const Json &value) {
    FrameMeta frame;
    frame.conn_id = static_cast<uint32_t>(u64_field(&value, "conn"));
    frame.kind = str_field(&value, "kind");
    frame.topic = str_field(&value, "topic");
    frame.encoding = str_field(&value, "encoding");
    frame.width = static_cast<uint32_t>(u64_field(&value, "width"));
    frame.height = static_cast<uint32_t>(u64_field(&value, "height"));
    frame.step = static_cast<uint32_t>(u64_field(&value, "step"));
    frame.timestamp_usec = u64_field(&value, "timestamp_usec");
    frame.timestamp_systemusec = u64_field(&value, "timestamp_systemusec");
    frame.timestamp_globalusec = u64_field(&value, "timestamp_globalusec");
    frame.number = u64_field(&value, "number");
    frame.header_seq = static_cast<uint32_t>(u64_field(&value, "header_seq"));
    frame.frame_id = str_field(&value, "frame_id");
    frame.metadatasize = static_cast<uint32_t>(u64_field(&value, "metadatasize"));
    frame.pixel_bit_size = static_cast<uint8_t>(u64_field(&value, "pixel_bit_size"));
    frame.metadata_hex = str_field(&value, "metadata_hex");
    frame.data_size = u64_field(&value, "data_size");
    frame.sha256 = str_field(&value, "sha256");
    return frame;
}

bool load_manifest(k4a_playback_t playback, Manifest &manifest, const Mkv2BagLog &log) {
    std::vector<uint8_t> manifest_bytes;
    std::vector<uint8_t> frames_bytes;
    if (!read_attachment(playback, "orbbec_manifest.json", manifest_bytes)) {
        log_line(log, "warning: orbbec_manifest.json not found; using MKV tags/defaults");
        return false;
    }
    if (!read_attachment(playback, "orbbec_frames.jsonl", frames_bytes)) {
        log_line(log, "warning: orbbec_frames.jsonl not found; frames use defaults");
    }

    const std::string text(manifest_bytes.begin(), manifest_bytes.end());
    Json root = parse_json(text);
    const Json *device = root.find("device");
    if (device && device->is_object()) {
        manifest.device.present = true;
        manifest.device.name = str_field(device, "name");
        manifest.device.full_name = str_field(device, "full_name");
        manifest.device.asic_name = str_field(device, "asic_name");
        manifest.device.vid = static_cast<uint16_t>(u64_field(device, "vid"));
        manifest.device.pid = static_cast<uint16_t>(u64_field(device, "pid"));
        manifest.device.uid = str_field(device, "uid");
        manifest.device.sn = str_field(device, "sn");
        manifest.device.fw_version = str_field(device, "fw_version");
        manifest.device.hw_version = str_field(device, "hw_version");
        manifest.device.supported_sdk_version = str_field(device, "supported_sdk_version");
        manifest.device.connection_type = str_field(device, "connection_type");
        manifest.device.type = static_cast<uint16_t>(u64_field(device, "type"));
        manifest.device.backend_type = static_cast<uint8_t>(u64_field(device, "backend_type"));
        manifest.device.ip_address = str_field(device, "ip_address");
        manifest.device.local_mac = str_field(device, "local_mac");
        manifest.device.timestamp_usec = u64_field(device, "timestamp_usec");
    }

    if (const Json *streams = root.find("streams")) {
        if (streams->is_array()) {
            for (const Json &stream : streams->array) {
                StreamProfile profile;
                profile.stream_type = static_cast<uint8_t>(u64_field(&stream, "stream_type"));
                profile.format = static_cast<uint8_t>(u64_field(&stream, "format"));
                profile.width = static_cast<uint16_t>(u64_field(&stream, "width"));
                profile.height = static_cast<uint16_t>(u64_field(&stream, "height"));
                profile.fps = static_cast<uint16_t>(u64_field(&stream, "fps"));
                profile.distortion_model = static_cast<uint8_t>(u64_field(&stream, "distortion_model"));
                profile.timestamp_usec = u64_field(&stream, "timestamp_usec");
                if (const Json *intrinsic = stream.find("intrinsic")) {
                    profile.intrinsic[0] = static_cast<float>(number_field(intrinsic, "fx"));
                    profile.intrinsic[1] = static_cast<float>(number_field(intrinsic, "fy"));
                    profile.intrinsic[2] = static_cast<float>(number_field(intrinsic, "cx"));
                    profile.intrinsic[3] = static_cast<float>(number_field(intrinsic, "cy"));
                }
                if (const Json *distortion = stream.find("distortion")) {
                    for (size_t i = 0; i < 8 && i < distortion->array.size(); ++i) {
                        profile.distortion[i] = static_cast<float>(distortion->array[i].as_double());
                    }
                }
                if (const Json *rotation = stream.find("rotation")) {
                    for (size_t i = 0; i < 9 && i < rotation->array.size(); ++i) {
                        profile.rotation[i] = static_cast<float>(rotation->array[i].as_double());
                    }
                }
                if (const Json *translation = stream.find("translation")) {
                    for (size_t i = 0; i < 3 && i < translation->array.size(); ++i) {
                        profile.translation[i] = static_cast<float>(translation->array[i].as_double());
                    }
                }
                manifest.profiles[profile.stream_type] = profile;
            }
        }
    }

    if (const Json *properties = root.find("properties")) {
        if (properties->is_array()) {
            for (const Json &property : properties->array) {
                PropertyMessage message;
                message.id = static_cast<uint32_t>(u64_field(&property, "id"));
                message.datasize = static_cast<uint32_t>(u64_field(&property, "datasize"));
                message.timestamp_usec = u64_field(&property, "timestamp_usec");
                try {
                    message.data = base64_decode(str_field(&property, "base64"));
                } catch (const std::exception &) {
                    log_line(log, "warning: skipped invalid base64 property " + std::to_string(message.id));
                    continue;
                }
                if (message.datasize == 0) message.datasize = static_cast<uint32_t>(message.data.size());
                manifest.properties.push_back(std::move(message));
            }
        }
    }

    if (!frames_bytes.empty()) {
        std::istringstream input(std::string(frames_bytes.begin(), frames_bytes.end()));
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            try {
                manifest.frames.push_back(frame_from_json(parse_json(line)));
            } catch (const std::exception &e) {
                log_line(log, std::string("warning: skipped frame JSONL line: ") + e.what());
            }
        }
    }
    manifest.used = true;
    return true;
}

std::string get_tag(k4a_playback_t playback, const char *name) {
    size_t size = 0;
    k4a_buffer_result_t result = k4a_playback_get_tag(playback, name, nullptr, &size);
    if (result != K4A_BUFFER_RESULT_TOO_SMALL || size == 0) return {};
    std::vector<char> buffer(size + 1, 0);
    result = k4a_playback_get_tag(playback, name, buffer.data(), &size);
    if (result != K4A_BUFFER_RESULT_SUCCEEDED) return {};
    return std::string(buffer.data());
}

bool has_track(k4a_playback_t playback, const char *name) {
    const size_t count = k4a_playback_get_track_count(playback);
    for (size_t i = 0; i < count; ++i) {
        size_t size = 0;
        k4a_buffer_result_t result = k4a_playback_get_track_name(playback, i, nullptr, &size);
        if (result != K4A_BUFFER_RESULT_TOO_SMALL || size == 0) continue;
        std::vector<char> buffer(size + 1, 0);
        result = k4a_playback_get_track_name(playback, i, buffer.data(), &size);
        if (result == K4A_BUFFER_RESULT_SUCCEEDED && std::string(buffer.data()) == name) {
            return true;
        }
    }
    return false;
}

const FrameMeta *find_frame(const std::map<uint64_t, const FrameMeta *> &frames, uint64_t timestamp) {
    auto exact = frames.find(timestamp);
    if (exact != frames.end()) return exact->second;
    // K4A timestamps are normally exact; tolerate a few microseconds for a
    // hand-made MKV by selecting the nearest preceding frame.
    auto it = frames.lower_bound(timestamp);
    if (it != frames.end() && it->first - timestamp <= 1000) return it->second;
    if (it != frames.begin()) {
        --it;
        if (timestamp - it->first <= 1000) return it->second;
    }
    return nullptr;
}

std::vector<uint8_t> copy_image_buffer(k4a_image_t image, uint32_t *width, uint32_t *height,
                                       uint32_t *stride, k4a_image_format_t *format) {
    *width = static_cast<uint32_t>(k4a_image_get_width_pixels(image));
    *height = static_cast<uint32_t>(k4a_image_get_height_pixels(image));
    *stride = static_cast<uint32_t>(k4a_image_get_stride_bytes(image));
    *format = k4a_image_get_format(image);
    const uint8_t *buffer = k4a_image_get_buffer(image);
    const size_t size = k4a_image_get_size(image);
    if (!buffer || size == 0) return {};
    return std::vector<uint8_t>(buffer, buffer + size);
}

void add_connection_if_missing(BagWriter &writer, std::set<uint32_t> &ids, uint32_t id,
                               const std::string &topic, const std::string &type,
                               const std::string &definition, const std::string &md5 = {}) {
    if (!ids.insert(id).second) return;
    BagConnection connection;
    connection.id = id;
    connection.topic = topic;
    connection.type = type;
    connection.md5 = md5;
    connection.definition = definition;
    writer.add_connection(connection);
}

std::string image_topic_for_conn(uint32_t conn, const std::map<uint32_t, std::string> &topics) {
    auto it = topics.find(conn);
    if (it != topics.end()) return it->second;
    switch (conn) {
    case 1: return "/cam/sensor_3/frameType_3";
    case 2: return "/cam/sensor_2/frameType_2";
    case 6: return "/cam/sensor_1/frameType_1";
    default: return "/cam/sensor_" + std::to_string(conn) + "/frameType_" + std::to_string(conn);
    }
}

}  // namespace

Mkv2BagSummary mkv_to_bag(const Mkv2BagOptions &options, const Mkv2BagLog &log) {
    if (options.input_mkv.empty()) throw std::runtime_error("mkv2bag: input path is empty");
    if (options.output_bag.empty()) throw std::runtime_error("mkv2bag: output path is empty");

    std::error_code ec;
    fs::path input = fs::weakly_canonical(options.input_mkv, ec);
    fs::path output = fs::weakly_canonical(options.output_bag, ec);
    if (!ec && input == output) {
        throw std::runtime_error("input and output are the same file");
    }
    if (fs::exists(options.output_bag) && !options.force) {
        throw std::runtime_error("output already exists (use --force): " + options.output_bag);
    }
    if (fs::path(options.output_bag).has_parent_path()) {
        fs::create_directories(fs::path(options.output_bag).parent_path(), ec);
        if (ec) throw std::runtime_error("cannot create output directory: " + ec.message());
    }

    k4a_playback_t playback = nullptr;
    if (k4a_playback_open(options.input_mkv.c_str(), &playback) != K4A_RESULT_SUCCEEDED) {
        throw std::runtime_error("cannot open MKV: " + options.input_mkv);
    }
    struct PlaybackGuard {
        k4a_playback_t handle;
        ~PlaybackGuard() { if (handle) k4a_playback_close(handle); }
    } guard{playback};

    Manifest manifest;
    load_manifest(playback, manifest, log);
    if (!manifest.device.present) {
        std::string serial = get_tag(playback, "K4A_DEVICE_SERIAL_NUMBER");
        if (!serial.empty()) {
            manifest.device.present = true;
            manifest.device.sn = serial;
            manifest.device.name = "Orbbec";
            manifest.device.full_name = "Orbbec";
        }
    }

    std::map<uint64_t, const FrameMeta *> color_frames;
    std::map<uint64_t, const FrameMeta *> depth_frames;
    std::map<uint64_t, const FrameMeta *> ir_frames;
    std::map<uint32_t, std::string> image_topics;
    for (const auto &frame : manifest.frames) {
        image_topics[frame.conn_id] = frame.topic;
        if (frame.kind == "COLOR") color_frames[frame.timestamp_usec] = &frame;
        else if (frame.kind == "DEPTH") depth_frames[frame.timestamp_usec] = &frame;
        else if (frame.kind == "IR" || frame.kind == "IR_LEFT" || frame.kind == "IR_RIGHT") {
            ir_frames[frame.timestamp_usec] = &frame;
        }
    }

    const fs::path temp_path = options.output_bag + ".tmp";
    fs::remove(temp_path, ec);

    Mkv2BagSummary summary;
    summary.manifest_used = manifest.used;
    summary.properties = manifest.properties.size();
    summary.profiles = manifest.profiles.size();
    try {
        BagWriter writer(temp_path.string());
        std::set<uint32_t> conn_ids;

        add_connection_if_missing(writer, conn_ids, 0, "/cam/property", "custom_msg/OBProperty",
                                  kPropertyDefinition);
        add_connection_if_missing(writer, conn_ids, 3, "/cam/deviceInfo", "custom_msg/OBDeviceInfo",
                                  kDeviceDefinition);
        if (manifest.profiles.count(2)) {
            add_connection_if_missing(writer, conn_ids, 4, "/cam/streamProfileType_2",
                                      "custom_msg/OBStreamProfileInfo", kProfileDefinition);
        }
        if (manifest.profiles.count(3)) {
            add_connection_if_missing(writer, conn_ids, 5, "/cam/streamProfileType_3",
                                      "custom_msg/OBStreamProfileInfo", kProfileDefinition);
        }

        std::set<uint32_t> frame_conns;
        if (color_frames.empty()) frame_conns.insert(2);
        if (depth_frames.empty()) frame_conns.insert(1);
        // Keep an IR connection available even when the manifest is absent; the
        // K4A playback may expose an IR image while the manifest has no frame row.
        if (ir_frames.empty()) frame_conns.insert(6);
        for (const auto &kv : color_frames) frame_conns.insert(kv.second->conn_id);
        for (const auto &kv : depth_frames) frame_conns.insert(kv.second->conn_id);
        for (const auto &kv : ir_frames) frame_conns.insert(kv.second->conn_id);
        for (uint32_t conn : frame_conns) {
            add_connection_if_missing(writer, conn_ids, conn, image_topic_for_conn(conn, image_topics),
                                      "sensor_msgs/Image", kImageDefinition,
                                      "060021388200f6f0f447d0fcd9c64743");
        }

        for (const auto &property : manifest.properties) {
            writer.add_message(0, property.timestamp_usec * 1000ULL, property_payload(property));
        }
        if (manifest.device.present) {
            writer.add_message(3, manifest.device.timestamp_usec * 1000ULL, device_payload(manifest.device));
        }
        for (const auto &kv : manifest.profiles) {
            const uint32_t conn = kv.first == 2 ? 4 : (kv.first == 3 ? 5 : 0);
            if (conn != 0) writer.add_message(conn, kv.second.timestamp_usec * 1000ULL,
                                              profile_payload(kv.second));
        }

        // The default bag2mkv output stores the original JPEG stream on this
        // custom track. Use it instead of re-encoding the BGRA COLOR track.
        std::map<uint64_t, std::vector<uint8_t>> source_jpegs;
        if (has_track(playback, "ORIGINAL_COLOR_MJPG")) {
            k4a_playback_data_block_t block = nullptr;
            while (k4a_playback_get_next_data_block(playback, "ORIGINAL_COLOR_MJPG", &block) ==
                   K4A_STREAM_RESULT_SUCCEEDED) {
                if (!block) break;
                const uint64_t ts = k4a_playback_data_block_get_device_timestamp_usec(block);
                const size_t size = k4a_playback_data_block_get_buffer_size(block);
                const uint8_t *data = k4a_playback_data_block_get_buffer(block);
                if (ts && size && data) source_jpegs[ts] = std::vector<uint8_t>(data, data + size);
                k4a_playback_data_block_release(block);
                block = nullptr;
            }
        }
        if (!source_jpegs.empty()) {
            log_line(log, "using ORIGINAL_COLOR_MJPG for " + std::to_string(source_jpegs.size()) +
                              " color frames");
        }

        uint64_t capture_count = 0;
        k4a_capture_t capture = nullptr;
        while (k4a_playback_get_next_capture(playback, &capture) == K4A_STREAM_RESULT_SUCCEEDED) {
            if (!capture) break;
            ++capture_count;

            k4a_image_t color = k4a_capture_get_color_image(capture);
            if (color) {
                const uint64_t ts = k4a_image_get_device_timestamp_usec(color);
                const FrameMeta *meta = find_frame(color_frames, ts);
                const uint32_t conn = meta && meta->conn_id ? meta->conn_id : 2;
                std::vector<uint8_t> payload;
                auto jpeg_it = source_jpegs.find(ts);
                if (jpeg_it != source_jpegs.end()) {
                    payload = image_payload(meta, ts, static_cast<uint32_t>(k4a_image_get_width_pixels(color)),
                                            static_cast<uint32_t>(k4a_image_get_height_pixels(color)), "mjpg",
                                            meta ? meta->step : static_cast<uint32_t>(k4a_image_get_width_pixels(color)),
                                            jpeg_it->second, 0, 0.0f);
                } else {
                    uint32_t width = 0, height = 0, stride = 0;
                    k4a_image_format_t format = K4A_IMAGE_FORMAT_CUSTOM;
                    std::vector<uint8_t> buffer = copy_image_buffer(color, &width, &height, &stride, &format);
                    if (!buffer.empty()) {
                        if (format == K4A_IMAGE_FORMAT_COLOR_MJPG) {
                            payload = image_payload(meta, ts, width, height, "mjpg",
                                                    meta ? meta->step : width, buffer, 0, 0.0f);
                        } else if (format == K4A_IMAGE_FORMAT_COLOR_BGRA32) {
                            const uint32_t row = width * 4;
                            std::vector<uint8_t> packed;
                            packed.resize(static_cast<size_t>(row) * height);
                            for (uint32_t y = 0; y < height; ++y) {
                                std::memcpy(packed.data() + static_cast<size_t>(y) * row,
                                            buffer.data() + static_cast<size_t>(y) * stride, row);
                            }
                            payload = image_payload(meta, ts, width, height, "bgra8", row, packed, 0, 0.0f);
                        }
                    }
                }
                if (!payload.empty()) {
                    writer.add_message(conn, ts * 1000ULL, payload);
                    ++summary.color_frames;
                }
                k4a_image_release(color);
            }

            k4a_image_t depth = k4a_capture_get_depth_image(capture);
            if (depth) {
                const uint64_t ts = k4a_image_get_device_timestamp_usec(depth);
                const uint32_t width = static_cast<uint32_t>(k4a_image_get_width_pixels(depth));
                const uint32_t height = static_cast<uint32_t>(k4a_image_get_height_pixels(depth));
                const uint32_t stride = static_cast<uint32_t>(k4a_image_get_stride_bytes(depth));
                const uint8_t *data = k4a_image_get_buffer(depth);
                if (data && stride >= width * 2) {
                    std::vector<uint8_t> packed(static_cast<size_t>(width) * 2 * height);
                    for (uint32_t y = 0; y < height; ++y) {
                        std::memcpy(packed.data() + static_cast<size_t>(y) * width * 2,
                                    data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 2);
                    }
                    const FrameMeta *meta = find_frame(depth_frames, ts);
                    const uint32_t conn = meta && meta->conn_id ? meta->conn_id : 1;
                    writer.add_message(conn, ts * 1000ULL,
                                       image_payload(meta, ts, width, height, "mono16", width * 2, packed, 16,
                                                     0.0f));
                    ++summary.depth_frames;
                }
                k4a_image_release(depth);
            }

            k4a_image_t ir = k4a_capture_get_ir_image(capture);
            if (ir) {
                const uint64_t ts = k4a_image_get_device_timestamp_usec(ir);
                const uint32_t width = static_cast<uint32_t>(k4a_image_get_width_pixels(ir));
                const uint32_t height = static_cast<uint32_t>(k4a_image_get_height_pixels(ir));
                const uint32_t stride = static_cast<uint32_t>(k4a_image_get_stride_bytes(ir));
                const uint8_t *data = k4a_image_get_buffer(ir);
                if (data && stride >= width * 2) {
                    std::vector<uint8_t> packed(static_cast<size_t>(width) * 2 * height);
                    for (uint32_t y = 0; y < height; ++y) {
                        std::memcpy(packed.data() + static_cast<size_t>(y) * width * 2,
                                    data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 2);
                    }
                    const FrameMeta *meta = find_frame(ir_frames, ts);
                    const uint32_t conn = meta && meta->conn_id ? meta->conn_id : 6;
                    writer.add_message(conn, ts * 1000ULL,
                                       image_payload(meta, ts, width, height, "mono16", width * 2, packed, 16,
                                                     0.0f));
                    ++summary.ir_frames;
                }
                k4a_image_release(ir);
            }

            k4a_capture_release(capture);
            capture = nullptr;
            if ((capture_count % 100) == 0) {
                log_line(log, "converted " + std::to_string(capture_count) + " captures");
            }
        }
        if (capture) k4a_capture_release(capture);

        writer.close();
        if (fs::exists(options.output_bag)) fs::remove(options.output_bag);
        fs::rename(temp_path, options.output_bag);
        summary.bag_size = fs::file_size(options.output_bag);
        log_line(log, "wrote: " + options.output_bag + " (" + std::to_string(summary.color_frames) +
                          " color, " + std::to_string(summary.depth_frames) + " depth, " +
                          std::to_string(summary.ir_frames) + " IR)");
        return summary;
    } catch (...) {
        std::error_code cleanup_ec;
        fs::remove(temp_path, cleanup_ec);
        throw;
    }
}

}  // namespace bag2mkv
