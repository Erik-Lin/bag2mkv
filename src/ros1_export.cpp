#include "ros1_export.hpp"

#include "bag.hpp"
#include "bag_writer.hpp"
#include "scan.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace bag2mkv {
namespace {

namespace fs = std::filesystem;

const char *kImageDefinition =
    "# This message contains an uncompressed image\n"
    "Header header\n"
    "uint32 height\n"
    "uint32 width\n"
    "string encoding\n"
    "uint8 is_bigendian\n"
    "uint32 step\n"
    "uint8[] data\n";

const char *kCompressedImageDefinition =
    "# This message contains a compressed image\n"
    "Header header\n"
    "string format\n"
    "uint8[] data\n";

const char *kImageMd5 = "060021388200f6f0f447d0fcd9c64743";
const char *kCompressedImageMd5 = "8f7a12909da2c9d3332d540a0977563f";

void append_u32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void append_string(std::vector<uint8_t> &out, const std::string &value) {
    append_u32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void append_image_header(std::vector<uint8_t> &out, const ImageMessage &image) {
    append_u32(out, image.header_seq);
    append_u32(out, image.stamp_sec ? image.stamp_sec : static_cast<uint32_t>(image.timestamp_usec / 1000000ull));
    append_u32(out, image.stamp_nsec ? image.stamp_nsec
                                     : static_cast<uint32_t>((image.timestamp_usec % 1000000ull) * 1000ull));
    append_string(out, image.frame_id);
}

std::vector<uint8_t> image_payload(const ImageMessage &image, const uint8_t *data, size_t size) {
    std::vector<uint8_t> out;
    append_image_header(out, image);
    append_u32(out, image.height);
    append_u32(out, image.width);
    std::string encoding = image.encoding;
    if (encoding == "mono16" || encoding == "y16" || encoding == "16uc1") {
        encoding = "16UC1";
    } else if (encoding == "bgra8") {
        encoding = "bgra8";
    } else if (encoding == "rgb8") {
        encoding = "rgb8";
    } else if (encoding == "bgr8") {
        encoding = "bgr8";
    }
    append_string(out, encoding);
    out.push_back(0);  // is_bigendian
    append_u32(out, image.step ? image.step : (encoding == "16UC1" ? image.width * 2 : image.width * 4));
    append_u32(out, static_cast<uint32_t>(size));
    out.insert(out.end(), data, data + size);
    return out;
}

std::vector<uint8_t> compressed_payload(const ImageMessage &image, const uint8_t *data, size_t size) {
    std::vector<uint8_t> out;
    append_image_header(out, image);
    append_string(out, "jpeg");
    append_u32(out, static_cast<uint32_t>(size));
    out.insert(out.end(), data, data + size);
    return out;
}

BagConnection make_connection(uint32_t id, const std::string &topic, const std::string &type,
                              const std::string &definition, const std::string &md5) {
    BagConnection connection;
    connection.id = id;
    connection.topic = topic;
    connection.type = type;
    connection.definition = definition;
    connection.md5 = md5;
    return connection;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

struct TempFileGuard {
    fs::path path;
    bool keep = false;
    ~TempFileGuard() {
        if (!keep) {
            std::error_code error;
            fs::remove(path, error);
        }
    }
};

}  // namespace

Ros1ExportSummary export_ob_bag_to_ros1(const Ros1ExportOptions &options, const Ros1ExportLog &log) {
    if (options.input_bag.empty() || options.output_bag.empty()) {
        throw std::runtime_error("input and output bag paths must not be empty");
    }
    const fs::path output(options.output_bag);
    if (fs::exists(output) && !options.force) {
        throw std::runtime_error("output bag already exists (use --force): " + options.output_bag);
    }

    ScanResult scan = scan_bag(options.input_bag, false);
    const FrameMeta *color = nullptr;
    const FrameMeta *depth = nullptr;
    const FrameMeta *ir = nullptr;
    for (const auto &frame : scan.frames) {
        if (frame.kind == "COLOR" && color == nullptr) color = &frame;
        if (frame.kind == "DEPTH" && depth == nullptr) depth = &frame;
        if (frame.kind == "IR" && ir == nullptr) ir = &frame;
    }
    if (color == nullptr && depth == nullptr && ir == nullptr) {
        throw std::runtime_error("source bag contains no color/depth/IR image frames");
    }

    const bool color_compressed =
        color != nullptr && (lower(color->encoding) == "mjpg" || lower(color->encoding) == "jpeg");
    std::map<std::string, uint32_t> conn_ids;
    uint32_t next_id = 0;
    const fs::path temp = fs::path(output.string() + ".tmp");
    fs::remove(temp);
    TempFileGuard temp_guard{temp};
    BagWriter writer(temp.string(), BagFlavor::Ros1);
    if (color != nullptr) {
        conn_ids["COLOR"] = next_id++;
        if (color_compressed) {
            writer.add_connection(make_connection(conn_ids["COLOR"], "/camera/color/image_raw/compressed",
                                                  "sensor_msgs/CompressedImage", kCompressedImageDefinition,
                                                  kCompressedImageMd5));
        } else {
            writer.add_connection(make_connection(conn_ids["COLOR"], "/camera/color/image_raw",
                                                  "sensor_msgs/Image", kImageDefinition, kImageMd5));
        }
    }
    if (depth != nullptr) {
        conn_ids["DEPTH"] = next_id++;
        writer.add_connection(make_connection(conn_ids["DEPTH"], "/camera/depth/image_raw", "sensor_msgs/Image",
                                              kImageDefinition, kImageMd5));
    }
    if (ir != nullptr) {
        conn_ids["IR"] = next_id++;
        writer.add_connection(make_connection(conn_ids["IR"], "/camera/ir/image_raw", "sensor_msgs/Image",
                                              kImageDefinition, kImageMd5));
    }

    if (log) {
        log("exporting ROS1 bag: " + options.output_bag);
        if (color) log(std::string("color: ") + (color_compressed ? "CompressedImage/jpeg" : "Image/raw"));
        if (depth) log("depth: Image/16UC1");
        if (ir) log("IR: Image/16UC1");
    }

    Ros1ExportSummary summary;
    BagReader reader(options.input_bag);
    reader.scan(BagCallbacks{
        {},
        [&](const ImageMessage &image, const uint8_t *data, size_t size) {
            auto it = conn_ids.find(image.kind);
            if (it == conn_ids.end()) return;
            std::vector<uint8_t> payload;
            if (image.kind == "COLOR" && color_compressed) {
                payload = compressed_payload(image, data, size);
                ++summary.color_frames;
            } else {
                payload = image_payload(image, data, size);
                if (image.kind == "COLOR") ++summary.color_frames;
                if (image.kind == "DEPTH") ++summary.depth_frames;
                if (image.kind == "IR") ++summary.ir_frames;
            }
            const uint64_t time_ns = image.timestamp_usec * 1000ull;
            writer.add_message(it->second, time_ns, payload);
        },
        {},
        {},
        {},
        {}});
    writer.close();
    if (fs::exists(output)) fs::remove(output);
    fs::rename(temp, output);
    temp_guard.keep = true;
    summary.bag_size = fs::file_size(output);
    return summary;
}

}  // namespace bag2mkv
