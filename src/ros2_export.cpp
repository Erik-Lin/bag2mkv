#include "ros2_export.hpp"

#include "bag.hpp"
#include "scan.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace bag2mkv {
namespace {

namespace fs = std::filesystem;

class CdrWriter {
public:
    CdrWriter() : data_{0x00, 0x01, 0x00, 0x00} {}

    void header(uint32_t sec, uint32_t nsec, const std::string &frame_id) {
        i32(static_cast<int32_t>(sec));
        u32(nsec);
        str(frame_id);
    }
    void image(uint32_t sec, uint32_t nsec, const std::string &frame_id, uint32_t height, uint32_t width,
               const std::string &encoding, uint8_t bigendian, uint32_t step, const uint8_t *bytes, size_t size) {
        header(sec, nsec, frame_id);
        u32(height);
        u32(width);
        str(encoding);
        u8(bigendian);
        align(4);
        u32(step);
        u32(static_cast<uint32_t>(size));
        data_.insert(data_.end(), bytes, bytes + size);
    }
    void compressed(uint32_t sec, uint32_t nsec, const std::string &frame_id, const std::string &format,
                    const uint8_t *bytes, size_t size) {
        header(sec, nsec, frame_id);
        str(format);
        u32(static_cast<uint32_t>(size));
        data_.insert(data_.end(), bytes, bytes + size);
    }
    const std::vector<uint8_t> &data() const { return data_; }

private:
    std::vector<uint8_t> data_;

    void align(size_t n) {
        while ((data_.size() - 4) % n != 0) data_.push_back(0);
    }
    void u8(uint8_t value) { data_.push_back(value); }
    void u32(uint32_t value) {
        align(4);
        data_.push_back(static_cast<uint8_t>(value & 0xFF));
        data_.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        data_.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        data_.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    }
    void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }
    void str(const std::string &value) {
        u32(static_cast<uint32_t>(value.size() + 1));
        data_.insert(data_.end(), value.begin(), value.end());
        data_.push_back(0);
    }
};

struct Topic {
    uint32_t id = 0;
    std::string name;
    std::string type;
    bool compressed = false;
    uint64_t count = 0;
};

void check_sqlite(int rc, sqlite3 *db, const char *what) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        std::string message = std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "sqlite error");
        throw std::runtime_error(message);
    }
}

void exec(sqlite3 *db, const char *sql) {
    char *error = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string message = error ? error : "sqlite exec failed";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

void write_metadata(const fs::path &path, const std::vector<Topic> &topics, uint64_t start_ns, uint64_t end_ns,
                    uint64_t messages, const std::string &database_name) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write ROS2 metadata.yaml");
    out << "rosbag2_bagfile_information:\n"
        << "  version: 5\n"
        << "  storage_identifier: sqlite3\n"
        << "  relative_file_paths:\n"
        << "    - " << database_name << "\n"
        << "  duration:\n"
        << "    nanoseconds: " << (end_ns >= start_ns ? end_ns - start_ns : 0) << "\n"
        << "  starting_time:\n"
        << "    nanoseconds_since_epoch: " << start_ns << "\n"
        << "  message_count: " << messages << "\n"
        << "  topics_with_message_count:\n";
    for (const auto &topic : topics) {
        out << "    - topic_metadata:\n"
            << "        name: " << topic.name << "\n"
            << "        type: " << topic.type << "\n"
            << "        serialization_format: cdr\n"
            << "        offered_qos_profiles: \"\"\n"
            << "      message_count: " << topic.count << "\n";
    }
    out << "  compression_format: \"\"\n"
        << "  compression_mode: \"\"\n"
        << "  custom_data: {}\n";
}

}  // namespace

Ros2ExportSummary export_ob_bag_to_ros2(const Ros2ExportOptions &options, const Ros2ExportLog &log) {
    if (options.input_bag.empty() || options.output_dir.empty()) {
        throw std::runtime_error("input bag and output directory must not be empty");
    }
    ScanResult scan = scan_bag(options.input_bag, false);
    const FrameMeta *color = nullptr;
    const FrameMeta *depth = nullptr;
    const FrameMeta *ir = nullptr;
    for (const auto &frame : scan.frames) {
        if (frame.kind == "COLOR" && !color) color = &frame;
        if (frame.kind == "DEPTH" && !depth) depth = &frame;
        if (frame.kind == "IR" && !ir) ir = &frame;
    }
    if (!color && !depth && !ir) throw std::runtime_error("source bag contains no image frames");

    const bool color_compressed =
        color && (lower(color->encoding) == "mjpg" || lower(color->encoding) == "jpeg");
    std::vector<Topic> topics;
    std::map<std::string, uint32_t> topic_index;
    auto add_topic = [&](const std::string &kind, const std::string &name, const std::string &type,
                         bool compressed) {
        Topic topic;
        topic.id = static_cast<uint32_t>(topics.size());
        topic.name = name;
        topic.type = type;
        topic.compressed = compressed;
        topic_index[kind] = topic.id;
        topics.push_back(topic);
    };
    if (color) {
        add_topic("COLOR", color_compressed ? "/camera/color/image_raw/compressed" : "/camera/color/image_raw",
                  color_compressed ? "sensor_msgs/msg/CompressedImage" : "sensor_msgs/msg/Image",
                  color_compressed);
    }
    if (depth) add_topic("DEPTH", "/camera/depth/image_raw", "sensor_msgs/msg/Image", false);
    if (ir) add_topic("IR", "/camera/ir/image_raw", "sensor_msgs/msg/Image", false);

    const fs::path output_dir(options.output_dir);
    fs::create_directories(output_dir);
    const std::string database_name = "bag_0.db3";
    const fs::path database_path = output_dir / database_name;
    const fs::path metadata_path = output_dir / "metadata.yaml";
    if (!options.force && (fs::exists(database_path) || fs::exists(metadata_path))) {
        throw std::runtime_error("output ROS2 bag already exists (use --force): " + options.output_dir);
    }
    fs::remove(database_path);

    sqlite3 *db = nullptr;
    int rc = sqlite3_open(database_path.string().c_str(), &db);
    if (rc != SQLITE_OK) {
        std::string message = db ? sqlite3_errmsg(db) : "sqlite open failed";
        if (db) sqlite3_close(db);
        throw std::runtime_error("cannot create ROS2 database: " + message);
    }
    try {
        exec(db, "PRAGMA journal_mode=OFF");
        exec(db, "PRAGMA synchronous=OFF");
        exec(db, "CREATE TABLE schema(schema_version INTEGER)");
        exec(db, "INSERT INTO schema VALUES(2)");
        exec(db, "CREATE TABLE topics(id INTEGER PRIMARY KEY, name TEXT NOT NULL, type TEXT NOT NULL, "
                 "serialization_format TEXT NOT NULL, offered_qos_profiles TEXT NOT NULL)");
        exec(db, "CREATE TABLE messages(id INTEGER PRIMARY KEY, topic_id INTEGER NOT NULL, timestamp INTEGER NOT NULL, "
                 "data BLOB NOT NULL)");
        exec(db, "CREATE INDEX timestamp_idx ON messages(timestamp)");

        sqlite3_stmt *insert_topic = nullptr;
        check_sqlite(sqlite3_prepare_v2(db,
                                        "INSERT INTO topics(id,name,type,serialization_format,offered_qos_profiles) "
                                        "VALUES(?,?,?,?,?)",
                                        -1, &insert_topic, nullptr),
                     db, "prepare topic");
        for (const auto &topic : topics) {
            sqlite3_bind_int(insert_topic, 1, static_cast<int>(topic.id));
            sqlite3_bind_text(insert_topic, 2, topic.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_topic, 3, topic.type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_topic, 4, "cdr", -1, SQLITE_STATIC);
            sqlite3_bind_text(insert_topic, 5, "", -1, SQLITE_STATIC);
            check_sqlite(sqlite3_step(insert_topic), db, "insert topic");
            sqlite3_reset(insert_topic);
        }
        sqlite3_finalize(insert_topic);

        sqlite3_stmt *insert_message = nullptr;
        check_sqlite(sqlite3_prepare_v2(db, "INSERT INTO messages(topic_id,timestamp,data) VALUES(?,?,?)", -1,
                                        &insert_message, nullptr),
                     db, "prepare message");
        uint64_t start_ns = 0;
        uint64_t end_ns = 0;
        bool first_time = true;
        Ros2ExportSummary summary;

        BagReader reader(options.input_bag);
        reader.scan(BagCallbacks{
            {},
            [&](const ImageMessage &image, const uint8_t *data, size_t size) {
                auto it = topic_index.find(image.kind);
                if (it == topic_index.end()) return;
                Topic &topic = topics[it->second];
                const uint64_t time_ns = image.timestamp_usec * 1000ull;
                if (first_time) {
                    start_ns = time_ns;
                    first_time = false;
                }
                end_ns = time_ns;

                CdrWriter cdr;
                if (image.kind == "COLOR" && topic.compressed) {
                    cdr.compressed(image.stamp_sec, image.stamp_nsec, image.frame_id, "jpeg", data, size);
                } else {
                    std::string encoding = image.encoding;
                    if (encoding == "mono16" || encoding == "y16" || encoding == "16uc1") encoding = "16UC1";
                    cdr.image(image.stamp_sec, image.stamp_nsec, image.frame_id, image.height, image.width, encoding,
                              0, image.step ? image.step : image.width * 4, data, size);
                }
                sqlite3_bind_int(insert_message, 1, static_cast<int>(topic.id));
                sqlite3_bind_int64(insert_message, 2, static_cast<sqlite3_int64>(time_ns));
                sqlite3_bind_blob(insert_message, 3, cdr.data().data(), static_cast<int>(cdr.data().size()),
                                  SQLITE_TRANSIENT);
                check_sqlite(sqlite3_step(insert_message), db, "insert message");
                sqlite3_reset(insert_message);
                ++topic.count;
                ++summary.message_count;
                if (image.kind == "COLOR") ++summary.color_frames;
                else if (image.kind == "DEPTH") ++summary.depth_frames;
                else if (image.kind == "IR") ++summary.ir_frames;
            },
            {},
            {},
            {},
            {}});
        sqlite3_finalize(insert_message);
        sqlite3_close(db);
        db = nullptr;

        write_metadata(metadata_path, topics, start_ns, end_ns, summary.message_count, database_name);
        summary.database_path = database_path.string();
        if (log) {
            log("exporting ROS2 bag: " + options.output_dir);
            log("messages: " + std::to_string(summary.message_count));
        }
        return summary;
    } catch (...) {
        if (db) sqlite3_close(db);
        throw;
    }
}

}  // namespace bag2mkv
