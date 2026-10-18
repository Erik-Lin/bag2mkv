#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bag2mkv {

struct Ros1ExportOptions {
    std::string input_bag;
    std::string output_bag;
    bool force = false;
};

struct Ros1ExportSummary {
    uint64_t color_frames = 0;
    uint64_t depth_frames = 0;
    uint64_t ir_frames = 0;
    uint64_t bag_size = 0;
};

using Ros1ExportLog = std::function<void(const std::string &)>;

// Exports an Orbbec ROSBAG V2.0 recording to a standard ROS1 bag.
// Color MJPEG is written as sensor_msgs/CompressedImage; raw color, depth and
// IR are written as sensor_msgs/Image. Depth is little-endian 16UC1.
Ros1ExportSummary export_ob_bag_to_ros1(const Ros1ExportOptions &options, const Ros1ExportLog &log = {});

}  // namespace bag2mkv
