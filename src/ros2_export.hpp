#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bag2mkv {

struct Ros2ExportOptions {
    std::string input_bag;
    std::string output_dir;
    bool force = false;
};

struct Ros2ExportSummary {
    uint64_t color_frames = 0;
    uint64_t depth_frames = 0;
    uint64_t ir_frames = 0;
    uint64_t message_count = 0;
    std::string database_path;
};

using Ros2ExportLog = std::function<void(const std::string &)>;

// Exports an Orbbec bag to a ROS2 sqlite3 bag directory
// (metadata.yaml + bag_0.db3), using CDR-serialized sensor_msgs.
Ros2ExportSummary export_ob_bag_to_ros2(const Ros2ExportOptions &options, const Ros2ExportLog &log = {});

}  // namespace bag2mkv
