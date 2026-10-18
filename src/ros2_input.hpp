#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bag2mkv {

struct Ros2InputOptions {
    std::string input_dir;
    std::string output_mkv;
    bool force = false;
    bool color_mjpg = false;
};

struct Ros2InputSummary {
    uint64_t color_frames = 0;
    uint64_t depth_frames = 0;
    uint64_t ir_frames = 0;
    uint64_t mkv_size = 0;
};

using Ros2InputLog = std::function<void(const std::string &)>;

// Converts a ROS2 sqlite3 bag directory (metadata.yaml + *.db3) containing
// sensor_msgs/Image or CompressedImage messages into a K4A-compatible MKV.
Ros2InputSummary convert_ros2_to_mkv(const Ros2InputOptions &options, const Ros2InputLog &log = {});

}  // namespace bag2mkv
