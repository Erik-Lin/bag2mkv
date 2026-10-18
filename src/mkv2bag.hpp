#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bag2mkv {

struct Mkv2BagOptions {
    std::string input_mkv;
    std::string output_bag;
    bool force = false;
};

struct Mkv2BagSummary {
    uint64_t color_frames = 0;
    uint64_t depth_frames = 0;
    uint64_t ir_frames = 0;
    uint64_t properties = 0;
    uint64_t profiles = 0;
    uint64_t bag_size = 0;
    bool manifest_used = false;
};

using Mkv2BagLog = std::function<void(const std::string &)>;
// current is the number of K4A captures processed; total is 0 because K4A
// playback does not expose a reliable frame count before decoding.
using Mkv2BagProgress = std::function<bool(uint64_t current, uint64_t total)>;

Mkv2BagSummary mkv_to_bag(const Mkv2BagOptions &options, const Mkv2BagLog &log = {},
                          const Mkv2BagProgress &progress = {});

}  // namespace bag2mkv
