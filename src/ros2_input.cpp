#include "ros2_input.hpp"

#include "jpeg_decoder.hpp"
#include "k4a_writer.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bag2mkv {
namespace {

namespace fs = std::filesystem;

struct ImageFrame {
    std::string kind;
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

class CdrReader {
public:
    explicit CdrReader(const uint8_t *data, size_t size) : data_(data), size_(size) {
        if (size < 4) throw std::runtime_error("CDR message is too small");
        pos_ = 4;  // skip encapsulation header
    }
    uint32_t u32() {
        align(4);
        need(4);
        uint32_t v = uint32_t(data_[pos_]) | (uint32_t(data_[pos_ + 1]) << 8) |
                     (uint32_t(data_[pos_ + 2]) << 16) | (uint32_t(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    uint8_t u8() {
        need(1);
        return data_[pos_++];
    }
    std::string str() {
        uint32_t len = u32();
        need(len);
        std::string value(reinterpret_cast<const char *>(data_ + pos_), len ? len - 1 : 0);
        pos_ += len;
        return value;
    }
    std::vector<uint8_t> bytes() {
        uint32_t len = u32();
        need(len);
        std::vector<uint8_t> out(data_ + pos_, data_ + pos_ + len);
        pos_ += len;
        return out;
    }
    void align(size_t n) {
        while ((pos_ - 4) % n != 0) ++pos_;
    }

private:
    const uint8_t *data_;
    size_t size_;
    size_t pos_ = 0;
    void need(size_t n) {
        if (n > size_ - pos_) throw std::runtime_error("truncated CDR message");
    }
};

void check_sqlite(int rc, sqlite3 *db, const char *what) {
    if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        throw std::runtime_error(std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "sqlite error"));
    }
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string kind_for_topic(const std::string &topic, const std::string &encoding) {
    const std::string t = lower(topic);
    if (t.find("depth") != std::string::npos) return "DEPTH";
    if (t.find("infra") != std::string::npos || t.find("/ir") != std::string::npos) return "IR";
    if (encoding == "16UC1" || encoding == "mono16" || encoding == "y16") return "DEPTH";
    return "COLOR";
}

ImageFrame decode_image(const std::string &topic, const std::string &type, const std::vector<uint8_t> &blob) {
    ImageFrame frame;
    CdrReader cdr(blob.data(), blob.size());
    uint32_t stamp_sec = 0;
    uint32_t stamp_nsec = 0;
    if (type.find("CompressedImage") != std::string::npos) {
        stamp_sec = cdr.u32();
        stamp_nsec = cdr.u32();
        frame.frame_id = cdr.str();
        frame.compressed_format = cdr.str();
        frame.data = cdr.bytes();
        frame.encoding = "jpeg";
        frame.kind = "COLOR";
    } else if (type.find("Image") != std::string::npos) {
        stamp_sec = cdr.u32();
        stamp_nsec = cdr.u32();
        frame.frame_id = cdr.str();
        frame.height = cdr.u32();
        frame.width = cdr.u32();
        frame.encoding = cdr.str();
        frame.bigendian = cdr.u8();
        frame.step = cdr.u32();
        frame.data = cdr.bytes();
        frame.kind = kind_for_topic(topic, frame.encoding);
        if (frame.kind == "COLOR" && frame.encoding == "8UC1") frame.kind = "IR";
    } else {
        throw std::runtime_error("unsupported ROS2 message type: " + type);
    }
    frame.time_usec = uint64_t(stamp_sec) * 1000000ull + stamp_nsec / 1000ull;
    return frame;
}

k4a_color_resolution_t color_resolution_for(uint32_t w, uint32_t h) {
    if (w == 1920 && h == 1080) return K4A_COLOR_RESOLUTION_1080P;
    if (w == 1280 && h == 720) return K4A_COLOR_RESOLUTION_720P;
    if (w == 3840 && h == 2160) return K4A_COLOR_RESOLUTION_2160P;
    if (w == 2560 && h == 1440) return K4A_COLOR_RESOLUTION_1440P;
    if (w == 2048 && h == 1536) return K4A_COLOR_RESOLUTION_1536P;
    if (w == 4096 && h == 3072) return K4A_COLOR_RESOLUTION_3072P;
    throw std::runtime_error("unsupported ROS2 color resolution: " + std::to_string(w) + "x" + std::to_string(h));
}

k4a_depth_mode_t depth_mode_for(uint32_t w, uint32_t h) {
    if (w == 640 && h == 576) return K4A_DEPTH_MODE_NFOV_UNBINNED;
    if (w == 320 && h == 288) return K4A_DEPTH_MODE_NFOV_2X2BINNED;
    if (w == 512 && h == 512) return K4A_DEPTH_MODE_WFOV_2X2BINNED;
    if (w == 1024 && h == 1024) return K4A_DEPTH_MODE_WFOV_UNBINNED;
    throw std::runtime_error("unsupported ROS2 depth resolution: " + std::to_string(w) + "x" + std::to_string(h));
}

std::vector<uint8_t> to_bgra(const ImageFrame &frame, const uint8_t *data, size_t size) {
    if (frame.width == 0 || frame.height == 0) throw std::runtime_error("ROS2 image has zero dimensions");
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
                throw std::runtime_error("unsupported ROS2 color encoding: " + frame.encoding);
            }
            dst[x * 4 + 3] = 255;
        }
    }
    return out;
}

fs::path find_database(const fs::path &dir) {
    std::vector<fs::path> dbs;
    for (const auto &entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".db3") dbs.push_back(entry.path());
    }
    if (dbs.empty()) throw std::runtime_error("ROS2 bag directory has no .db3: " + dir.string());
    if (dbs.size() > 1) throw std::runtime_error("ROS2 bag directory has multiple .db3 files");
    return dbs.front();
}

}  // namespace

Ros2InputSummary convert_ros2_to_mkv(const Ros2InputOptions &options, const Ros2InputLog &log) {
    if (options.input_dir.empty() || options.output_mkv.empty()) {
        throw std::runtime_error("input directory and output MKV must not be empty");
    }
    if (log) log("ROS2 bag: " + options.input_dir);
    if (fs::exists(options.output_mkv) && !options.force) {
        throw std::runtime_error("output already exists (use --force): " + options.output_mkv);
    }
    const fs::path db_path = find_database(options.input_dir);
    sqlite3 *db = nullptr;
    int rc = sqlite3_open_v2(db_path.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = db ? sqlite3_errmsg(db) : "sqlite open failed";
        if (db) sqlite3_close(db);
        throw std::runtime_error("cannot open ROS2 database: " + message);
    }
    std::vector<ImageFrame> frames;
    try {
        sqlite3_stmt *stmt = nullptr;
        check_sqlite(sqlite3_prepare_v2(db,
                                        "SELECT topics.name, topics.type, messages.timestamp, messages.data "
                                        "FROM messages JOIN topics ON messages.topic_id=topics.id "
                                        "ORDER BY messages.timestamp",
                                        -1, &stmt, nullptr),
                     db, "prepare message query");
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const char *topic_text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            const char *type_text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            const uint8_t *blob = static_cast<const uint8_t *>(sqlite3_column_blob(stmt, 3));
            const int blob_size = sqlite3_column_bytes(stmt, 3);
            if (!topic_text || !type_text || !blob || blob_size <= 0) continue;
            std::string type = type_text;
            if (type.find("sensor_msgs") == std::string::npos ||
                (type.find("Image") == std::string::npos && type.find("CompressedImage") == std::string::npos)) {
                continue;
            }
            ImageFrame frame = decode_image(topic_text, type, std::vector<uint8_t>(blob, blob + blob_size));
            const sqlite3_int64 bag_time = sqlite3_column_int64(stmt, 2);
            if (frame.time_usec == 0) frame.time_usec = static_cast<uint64_t>(bag_time) / 1000ull;
            frames.push_back(std::move(frame));
        }
        check_sqlite(rc, db, "read ROS2 messages");
        sqlite3_finalize(stmt);
    } catch (...) {
        sqlite3_close(db);
        throw;
    }
    sqlite3_close(db);
    if (frames.empty()) throw std::runtime_error("ROS2 bag contains no Image/CompressedImage messages");

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
    if (!first_color && !first_depth && !first_ir) throw std::runtime_error("no usable ROS2 image stream");

    JpegDecoder decoder;
    uint32_t color_width = 0;
    uint32_t color_height = 0;
    bool color_is_jpeg = false;
    std::vector<uint8_t> first_bgra;
    if (first_color) {
        color_is_jpeg = first_color->encoding == "jpeg";
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
        throw std::runtime_error("ROS2 depth and IR resolutions differ");
    }
    if (options.color_mjpg && first_color && !color_is_jpeg) {
        throw std::runtime_error("--color mjpg requires CompressedImage input");
    }

    const fs::path output(options.output_mkv);
    const fs::path temp = fs::path(output.string() + ".tmp");
    fs::remove(temp);
    {
        K4aWriter writer(temp.string(), config);
        writer.write_header();
        Ros2InputSummary summary;
        for (const auto &frame : frames) {
            if (frame.kind == "COLOR") {
                if (frame.encoding == "jpeg") {
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
                    throw std::runtime_error("unsupported ROS2 depth encoding: " + frame.encoding);
                }
                if (frame.data.size() < static_cast<size_t>(frame.width) * frame.height * 2) {
                    throw std::runtime_error("ROS2 depth payload is too small");
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
                    throw std::runtime_error("unsupported ROS2 IR encoding: " + frame.encoding);
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
