#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bag2mkv {

struct Ros1InputOptions {
    std::string input_bag;
    std::string output_mkv;
    bool force = false;
    bool color_mjpg = false;
};

struct Ros1InputSummary {
    uint64_t color_frames = 0;
    uint64_t depth_frames = 0;
    uint64_t ir_frames = 0;
    uint64_t mkv_size = 0;
};

using Ros1InputLog = std::function<void(const std::string &)>;

// Converts a standard ROS1 bag containing sensor_msgs/Image and/or
// sensor_msgs/CompressedImage into a K4A-compatible MKV.
Ros1InputSummary convert_ros1_to_mkv(const Ros1InputOptions &options, const Ros1InputLog &log = {});

}  // namespace bag2mkv
