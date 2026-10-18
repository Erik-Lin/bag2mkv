#include "ros1_input.hpp"

#include "jpeg_decoder.hpp"
#include "k4a_writer.hpp"
#include "lz4_util.hpp"
#include "bz2_util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace bag2mkv {
namespace {

namespace fs = std::filesystem;

uint32_t load_u32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t load_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

struct Fields {
    std::map<std::string, std::vector<uint8_t>> values;
};

uint32_t field_u32(const Fields &fields, const std::string &name) {
    auto it = fields.values.find(name);
    if (it == fields.values.end() || it->second.size() != 4) {
        throw std::runtime_error("ROS1 bag field missing/invalid: " + name);
    }
    return load_u32(it->second.data());
}

uint8_t field_u8(const Fields &fields, const std::string &name) {
    auto it = fields.values.find(name);
    if (it == fields.values.end() || it->second.size() != 1) {
        throw std::runtime_error("ROS1 bag field missing/invalid: " + name);
    }
    return it->second[0];
}

uint64_t field_u64(const Fields &fields, const std::string &name) {
    auto it = fields.values.find(name);
    if (it == fields.values.end() || it->second.size() != 8) {
        throw std::runtime_error("ROS1 bag field missing/invalid: " + name);
    }
    return load_u64(it->second.data());
}

std::string field_string(const Fields &fields, const std::string &name) {
    auto it = fields.values.find(name);
    if (it == fields.values.end()) throw std::runtime_error("ROS1 bag field missing: " + name);
    return std::string(it->second.begin(), it->second.end());
}

struct ConnectionInfo {
    std::string topic;
    std::string type;
};

struct ImageFrame {
    std::string kind;
    std::string topic;
    std::string encoding;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t step = 0;
    uint8_t bigendian = 0;
    uint64_t time_usec = 0;
    uint32_t seq = 0;
    std::string frame_id;
    std::string compressed_format;
    std::vector<uint8_t> data;
};

class Ros1Reader {
public:
    explicit Ros1Reader(const std::string &path) : path_(path), in_(path, std::ios::binary) {
        if (!in_) throw std::runtime_error("cannot open ROS1 bag: " + path);
        in_.seekg(0, std::ios::end);
        size_ = static_cast<uint64_t>(in_.tellg());
        in_.seekg(0);
    }

    void read(std::vector<ImageFrame> &frames, const Ros1InputLog &log) {
        uint8_t magic[13] = {};
        read_exact(magic, sizeof(magic));
        if (std::memcmp(magic, "#ROSBAG V2.0\n", 13) != 0) {
            throw std::runtime_error("not a ROSBAG V2.0 file: " + path_);
        }
        Fields header = read_header();
        if (field_u8(header, "op") != 0x03) throw std::runtime_error("ROS1 bag header has no op=0x03");
        index_pos_ = field_u64(header, "index_pos");
        const uint32_t padding = read_u32();
        if (padding > size_ - tellg()) throw std::runtime_error("ROS1 bag header padding is truncated");
        seek(tellg() + padding);

        while (tellg() < index_pos_) {
            Fields rec = read_header();
            uint8_t op = field_u8(rec, "op");
            if (op == 0x05) {
                uint32_t compressed_size = read_u32();
                std::vector<uint8_t> compressed = read_bytes(compressed_size);
                std::vector<uint8_t> chunk;
                const std::string compression = rec.values.count("compression")
                    ? std::string(rec.values.at("compression").begin(), rec.values.at("compression").end())
                    : "none";
                if (compression == "lz4") {
                    chunk = lz4_decompress(compressed);
                } else if (compression == "bz2") {
                    chunk = bz2_decompress(compressed);
                } else if (compression == "none" || compression.empty()) {
                    chunk = std::move(compressed);
                } else {
                    throw std::runtime_error("unsupported ROS1 chunk compression: " + compression);
                }
                parse_chunk(chunk, frames);
            } else if (op == 0x07) {
                read_connection(rec);
            } else {
                uint32_t data_len = read_u32();
                seek(tellg() + data_len);
            }
        }
        if (log) log("ROS1 bag: " + std::to_string(frames.size()) + " image messages");
    }

private:
    std::string path_;
    std::ifstream in_;
    uint64_t size_ = 0;
    uint64_t index_pos_ = 0;
    std::map<uint32_t, ConnectionInfo> connections_;

    uint64_t tellg() {
        auto p = in_.tellg();
        if (p < 0) throw std::runtime_error("ROS1 bag seek position invalid");
        return static_cast<uint64_t>(p);
    }
    void seek(uint64_t pos) {
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(pos));
        if (!in_) throw std::runtime_error("ROS1 bag seek failed");
    }
    void read_exact(void *dst, size_t size) {
        in_.read(static_cast<char *>(dst), static_cast<std::streamsize>(size));
        if (!in_ || static_cast<size_t>(in_.gcount()) != size) throw std::runtime_error("truncated ROS1 bag");
    }
    uint32_t read_u32() {
        uint8_t b[4];
        read_exact(b, 4);
        return load_u32(b);
    }
    uint64_t read_u64() {
        uint8_t b[8];
        read_exact(b, 8);
        return load_u64(b);
    }
    std::vector<uint8_t> read_bytes(size_t size) {
        std::vector<uint8_t> out(size);
        if (size) read_exact(out.data(), size);
        return out;
    }
    static Fields parse_fields(const std::vector<uint8_t> &buf) {
        Fields fields;
        size_t pos = 0;
        while (pos < buf.size()) {
            if (buf.size() - pos < 4) throw std::runtime_error("truncated ROS1 field length");
            uint32_t len = load_u32(buf.data() + pos);
            pos += 4;
            if (len > buf.size() - pos) throw std::runtime_error("truncated ROS1 field value");
            const uint8_t *field = buf.data() + pos;
            pos += len;
            const uint8_t *eq = static_cast<const uint8_t *>(std::memchr(field, '=', len));
            if (!eq) continue;
            fields.values[std::string(reinterpret_cast<const char *>(field), eq - field)] =
                std::vector<uint8_t>(eq + 1, field + len);
        }
        return fields;
    }
    Fields read_header() {
        const uint32_t len = read_u32();
        if (len > 64 * 1024 * 1024) throw std::runtime_error("ROS1 record header too large");
        return parse_fields(read_bytes(len));
    }
    void read_connection(const Fields &first) {
        Fields second = read_header();
        const uint32_t id = field_u32(first, "conn");
        ConnectionInfo info;
        if (first.values.count("topic")) info.topic = field_string(first, "topic");
        else info.topic = field_string(second, "topic");
        if (second.values.count("type")) info.type = field_string(second, "type");
        connections_[id] = info;
    }
    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }
    static std::string read_string(const std::vector<uint8_t> &buf, size_t &pos) {
        if (buf.size() - pos < 4) throw std::runtime_error("truncated ROS1 string length");
        uint32_t len = load_u32(buf.data() + pos);
        pos += 4;
        if (len > buf.size() - pos) throw std::runtime_error("truncated ROS1 string");
        std::string out(reinterpret_cast<const char *>(buf.data() + pos), len);
        pos += len;
        return out;
    }
    static std::string kind_for(const ConnectionInfo &conn, const std::string &encoding) {
        const std::string topic = lower(conn.topic);
        if (topic.find("depth") != std::string::npos) return "DEPTH";
        if (topic.find("infra") != std::string::npos || topic.find("/ir") != std::string::npos) return "IR";
        if (encoding == "16UC1" || encoding == "mono16" || encoding == "y16") return "DEPTH";
        return "COLOR";
    }
    void parse_chunk(const std::vector<uint8_t> &chunk, std::vector<ImageFrame> &frames) {
        size_t pos = 0;
        while (pos < chunk.size()) {
            if (chunk.size() - pos < 4) throw std::runtime_error("truncated ROS1 record length");
            uint32_t header_len = load_u32(chunk.data() + pos);
            pos += 4;
            if (header_len > chunk.size() - pos) throw std::runtime_error("truncated ROS1 record header");
            Fields rec = parse_fields(std::vector<uint8_t>(chunk.begin() + pos, chunk.begin() + pos + header_len));
            pos += header_len;
            if (!rec.values.count("op")) throw std::runtime_error("ROS1 chunk record missing op");
            uint8_t op = field_u8(rec, "op");
            if (op == 0x07) {
                if (chunk.size() - pos < 4) throw std::runtime_error("truncated ROS1 connection header");
                uint32_t second_len = load_u32(chunk.data() + pos);
                pos += 4;
                if (second_len > chunk.size() - pos) throw std::runtime_error("truncated ROS1 connection header");
                Fields second = parse_fields(std::vector<uint8_t>(chunk.begin() + pos, chunk.begin() + pos + second_len));
                pos += second_len;
                const uint32_t id = field_u32(rec, "conn");
                ConnectionInfo info;
                info.topic = rec.values.count("topic") ? field_string(rec, "topic") : field_string(second, "topic");
                if (second.values.count("type")) info.type = field_string(second, "type");
                connections_[id] = info;
            } else if (op == 0x02) {
                auto time_it = rec.values.find("time");
                if (time_it == rec.values.end() || time_it->second.size() != 8) {
                    throw std::runtime_error("ROS1 message header missing time");
                }
                uint32_t sec = load_u32(time_it->second.data());
                uint32_t nsec = load_u32(time_it->second.data() + 4);
                if (chunk.size() - pos < 4) throw std::runtime_error("truncated ROS1 message payload length");
                uint32_t data_len = load_u32(chunk.data() + pos);
                pos += 4;
                if (data_len > chunk.size() - pos) {
                    throw std::runtime_error("truncated ROS1 message payload at pos " + std::to_string(pos) +
                                             " data_len " + std::to_string(data_len) + " chunk " +
                                             std::to_string(chunk.size()));
                }
                std::vector<uint8_t> payload(chunk.begin() + pos, chunk.begin() + pos + data_len);
                pos += data_len;
                const uint32_t conn_id = field_u32(rec, "conn");
                auto it = connections_.find(conn_id);
                if (it == connections_.end()) continue;
                ImageFrame frame;
                frame.topic = it->second.topic;
                frame.time_usec = uint64_t(sec) * 1000000ull + nsec / 1000ull;
                try {
                    if (it->second.type == "sensor_msgs/Image") {
                        size_t p = 0;
                        frame.seq = load_u32(payload.data() + 0);
                        uint32_t stamp_sec = load_u32(payload.data() + 4);
                        uint32_t stamp_nsec = load_u32(payload.data() + 8);
                        p = 12;
                        frame.frame_id = read_string(payload, p);
                        if (payload.size() - p < 12) throw std::runtime_error("short Image");
                        frame.height = load_u32(payload.data() + p); p += 4;
                        frame.width = load_u32(payload.data() + p); p += 4;
                        frame.encoding = read_string(payload, p);
                        frame.bigendian = payload[p++];
                        frame.step = load_u32(payload.data() + p); p += 4;
                        uint32_t len = load_u32(payload.data() + p); p += 4;
                        if (len > payload.size() - p) throw std::runtime_error("short Image data");
                        frame.data.assign(payload.begin() + p, payload.begin() + p + len);
                        if (stamp_sec || stamp_nsec) frame.time_usec = uint64_t(stamp_sec) * 1000000ull + stamp_nsec / 1000ull;
                        frame.kind = kind_for(it->second, frame.encoding);
                        if (frame.kind == "COLOR" && frame.encoding == "8UC1") frame.kind = "IR";
                        frames.push_back(std::move(frame));
                    } else if (it->second.type == "sensor_msgs/CompressedImage") {
                        size_t p = 0;
                        frame.seq = load_u32(payload.data() + 0);
                        uint32_t stamp_sec = load_u32(payload.data() + 4);
                        uint32_t stamp_nsec = load_u32(payload.data() + 8);
                        p = 12;
                        frame.frame_id = read_string(payload, p);
                        frame.compressed_format = read_string(payload, p);
                        uint32_t len = load_u32(payload.data() + p); p += 4;
                        if (len > payload.size() - p) throw std::runtime_error("short CompressedImage data");
                        frame.data.assign(payload.begin() + p, payload.begin() + p + len);
                        if (stamp_sec || stamp_nsec) frame.time_usec = uint64_t(stamp_sec) * 1000000ull + stamp_nsec / 1000ull;
                        frame.encoding = "jpeg";
                        frame.kind = "COLOR";
                        frames.push_back(std::move(frame));
                    }
                } catch (const std::exception &e) {
                    throw std::runtime_error(std::string("ROS1 message parse failed: ") + e.what());
                }
            } else if (op == 0x04) {
                if (chunk.size() - pos < 4) throw std::runtime_error("truncated ROS1 index data");
                uint32_t data_len = load_u32(chunk.data() + pos);
                pos += 4;
                pos += data_len;
            } else {
                if (chunk.size() - pos < 4) throw std::runtime_error("truncated ROS1 record data");
                uint32_t data_len = load_u32(chunk.data() + pos);
                pos += 4;
                pos += data_len;
            }
        }
    }
};

k4a_color_resolution_t color_resolution_for(uint32_t w, uint32_t h) {
    if (w == 1920 && h == 1080) return K4A_COLOR_RESOLUTION_1080P;
    if (w == 1280 && h == 720) return K4A_COLOR_RESOLUTION_720P;
    if (w == 3840 && h == 2160) return K4A_COLOR_RESOLUTION_2160P;
    if (w == 2560 && h == 1440) return K4A_COLOR_RESOLUTION_1440P;
    if (w == 2048 && h == 1536) return K4A_COLOR_RESOLUTION_1536P;
    if (w == 4096 && h == 3072) return K4A_COLOR_RESOLUTION_3072P;
    throw std::runtime_error("unsupported ROS color resolution: " + std::to_string(w) + "x" + std::to_string(h));
}

k4a_depth_mode_t depth_mode_for(uint32_t w, uint32_t h) {
    if (w == 640 && h == 576) return K4A_DEPTH_MODE_NFOV_UNBINNED;
    if (w == 320 && h == 288) return K4A_DEPTH_MODE_NFOV_2X2BINNED;
    if (w == 512 && h == 512) return K4A_DEPTH_MODE_WFOV_2X2BINNED;
    if (w == 1024 && h == 1024) return K4A_DEPTH_MODE_WFOV_UNBINNED;
    throw std::runtime_error("unsupported ROS depth resolution: " + std::to_string(w) + "x" + std::to_string(h));
}

std::vector<uint8_t> to_bgra(const ImageFrame &frame, const uint8_t *data, size_t size) {
    if (frame.width == 0 || frame.height == 0) throw std::runtime_error("ROS image has zero dimensions");
    std::vector<uint8_t> out(static_cast<size_t>(frame.width) * frame.height * 4);
    if (frame.encoding == "bgra8") {
        if (size < out.size()) throw std::runtime_error("bgra8 frame is too small");
        std::memcpy(out.data(), data, out.size());
        return out;
    }
    const uint32_t step = frame.step ? frame.step : frame.width * 3;
    for (uint32_t y = 0; y < frame.height; ++y) {
        const uint8_t *row = data + static_cast<size_t>(y) * step;
        uint8_t *dst = out.data() + static_cast<size_t>(y) * frame.width * 4;
        for (uint32_t x = 0; x < frame.width; ++x) {
            if (frame.encoding == "rgb8") {
                dst[x * 4 + 0] = row[x * 3 + 2];
                dst[x * 4 + 1] = row[x * 3 + 1];
                dst[x * 4 + 2] = row[x * 3 + 0];
            } else if (frame.encoding == "bgr8") {
                dst[x * 4 + 0] = row[x * 3 + 0];
                dst[x * 4 + 1] = row[x * 3 + 1];
                dst[x * 4 + 2] = row[x * 3 + 2];
            } else if (frame.encoding == "mono8") {
                dst[x * 4 + 0] = row[x];
                dst[x * 4 + 1] = row[x];
                dst[x * 4 + 2] = row[x];
            } else {
                throw std::runtime_error("unsupported ROS color encoding: " + frame.encoding);
            }
            dst[x * 4 + 3] = 255;
        }
    }
    return out;
}

}  // namespace

Ros1InputSummary convert_ros1_to_mkv(const Ros1InputOptions &options, const Ros1InputLog &log) {
    if (options.input_bag.empty() || options.output_mkv.empty()) {
        throw std::runtime_error("input and output paths must not be empty");
    }
    if (fs::exists(options.output_mkv) && !options.force) {
        throw std::runtime_error("output already exists (use --force): " + options.output_mkv);
    }

    std::vector<ImageFrame> frames;
    Ros1Reader reader(options.input_bag);
    reader.read(frames, log);
    if (frames.empty()) throw std::runtime_error("ROS1 bag contains no sensor_msgs/Image or CompressedImage");

    std::stable_sort(frames.begin(), frames.end(),
                     [](const ImageFrame &a, const ImageFrame &b) { return a.time_usec < b.time_usec; });

    const ImageFrame *first_color = nullptr;
    const ImageFrame *first_depth = nullptr;
    const ImageFrame *first_ir = nullptr;
    for (const auto &frame : frames) {
        if (frame.kind == "COLOR" && !first_color) first_color = &frame;
        if (frame.kind == "DEPTH" && !first_depth) first_depth = &frame;
        if (frame.kind == "IR" && !first_ir) first_ir = &frame;
    }
    if (!first_color && !first_depth && !first_ir) throw std::runtime_error("no usable ROS image stream");

    JpegDecoder decoder;
    std::vector<uint8_t> first_bgra;
    uint32_t color_width = 0;
    uint32_t color_height = 0;
    bool color_is_jpeg = false;
    if (first_color) {
        color_is_jpeg = first_color->encoding == "jpeg" || first_color->compressed_format == "jpeg";
        if (color_is_jpeg) {
            decoder.decode(first_color->data.data(), first_color->data.size(), first_bgra, color_width, color_height);
        } else {
            color_width = first_color->width;
            color_height = first_color->height;
        }
    }

    WriterConfig config;
    config.color_enabled = first_color != nullptr;
    config.color_width = color_width;
    config.color_height = color_height;
    config.color_resolution = first_color ? color_resolution_for(color_width, color_height) : K4A_COLOR_RESOLUTION_OFF;
    config.color_format = (first_color && options.color_mjpg) ? K4A_IMAGE_FORMAT_COLOR_MJPG
                                                              : K4A_IMAGE_FORMAT_COLOR_BGRA32;
    config.color_output = options.color_mjpg ? ColorOutput::Mjpg : ColorOutput::Bgra;
    config.depth_enabled = first_depth != nullptr;
    config.depth_width = first_depth ? first_depth->width : 0;
    config.depth_height = first_depth ? first_depth->height : 0;
    config.ir_enabled = first_ir != nullptr;
    config.ir_width = first_ir ? first_ir->width : 0;
    config.ir_height = first_ir ? first_ir->height : 0;
    if (first_depth) config.depth_mode = depth_mode_for(first_depth->width, first_depth->height);
    else if (first_ir) config.depth_mode = depth_mode_for(first_ir->width, first_ir->height);
    if (first_depth && first_ir &&
        (first_depth->width != first_ir->width || first_depth->height != first_ir->height)) {
        throw std::runtime_error("ROS depth and IR resolutions differ");
    }
    if (options.color_mjpg && first_color && !color_is_jpeg) {
        throw std::runtime_error("--color mjpg requires sensor_msgs/CompressedImage input");
    }

    const fs::path output(options.output_mkv);
    const fs::path temp = fs::path(output.string() + ".tmp");
    fs::remove(temp);
    {
        K4aWriter writer(temp.string(), config);
        writer.write_header();
        Ros1InputSummary summary;
        for (const auto &frame : frames) {
            if (frame.kind == "COLOR") {
                if (frame.encoding == "jpeg" || frame.compressed_format == "jpeg") {
                    if (options.color_mjpg) {
                        writer.write_color_mjpg(frame.data.data(), frame.data.size(), frame.time_usec);
                    } else {
                        std::vector<uint8_t> bgra;
                        uint32_t w = 0;
                        uint32_t h = 0;
                        decoder.decode(frame.data.data(), frame.data.size(), bgra, w, h);
                        writer.write_color_bgra(bgra.data(), bgra.size(), frame.time_usec);
                    }
                } else {
                    std::vector<uint8_t> bgra = to_bgra(frame, frame.data.data(), frame.data.size());
                    writer.write_color_bgra(bgra.data(), bgra.size(), frame.time_usec);
                }
                ++summary.color_frames;
            } else if (frame.kind == "DEPTH") {
                if (frame.encoding != "16UC1" && frame.encoding != "mono16") {
                    throw std::runtime_error("unsupported ROS depth encoding: " + frame.encoding);
                }
                if (frame.data.size() < static_cast<size_t>(frame.width) * frame.height * 2) {
                    throw std::runtime_error("ROS depth payload is too small");
                }
                writer.write_depth(frame.data.data(), static_cast<size_t>(frame.width) * frame.height * 2,
                                   frame.time_usec);
                ++summary.depth_frames;
            } else if (frame.kind == "IR") {
                if (frame.encoding == "16UC1" || frame.encoding == "mono16") {
                    writer.write_ir(frame.data.data(), static_cast<size_t>(frame.width) * frame.height * 2,
                                    frame.time_usec);
                } else if (frame.encoding == "8UC1" || frame.encoding == "mono8") {
                    std::vector<uint8_t> expanded(static_cast<size_t>(frame.width) * frame.height * 2);
                    for (size_t i = 0; i < static_cast<size_t>(frame.width) * frame.height; ++i) {
                        expanded[i * 2] = frame.data[i];
                        expanded[i * 2 + 1] = 0;
                    }
                    writer.write_ir(expanded.data(), expanded.size(), frame.time_usec);
                } else {
                    throw std::runtime_error("unsupported ROS IR encoding: " + frame.encoding);
                }
                ++summary.ir_frames;
            }
        }
        writer.close();
        summary.mkv_size = fs::file_size(temp);
        if (fs::exists(output)) fs::remove(output);
        fs::rename(temp, output);
        return summary;
    }
}

}  // namespace bag2mkv
