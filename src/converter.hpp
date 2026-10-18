#pragma once

#include "k4a_writer.hpp"
#include "scan.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace bag2mkv {

struct ConvertOptions {
    std::string input_bag;
    std::string output_mkv;
    bool force = false;
    bool no_calibration = false;
    bool verify_after = false;
    bool preserve_source_color = true;
    ColorOutput color_output = ColorOutput::Bgra;
    std::string calibration_json_path;
};

enum class Stage {
    Scanning,
    Preparing,
    Writing,
    Finalizing,
    Verifying,
    Done,
};

struct ProgressInfo {
    Stage stage = Stage::Scanning;
    uint64_t current = 0;
    uint64_t total = 0;
    std::string message;
};

struct ConvertSummary {
    std::string output_path;
    uint64_t output_size = 0;
    uint64_t total_frames = 0;
    uint64_t color_frames = 0;
    uint64_t depth_frames = 0;
    uint64_t ir_frames = 0;
    uint64_t first_timestamp_usec = 0;
    uint64_t last_timestamp_usec = 0;
    uint32_t color_width = 0;
    uint32_t color_height = 0;
    bool bgra = true;
    bool source_track = false;
    std::string calibration_source;
    bool verified = false;
};

struct VerifySummary {
    bool ok = false;
    uint64_t color_frames = 0;
    uint64_t depth_frames = 0;
    uint64_t ir_frames = 0;
    uint64_t source_color_frames = 0;
    uint64_t recording_length_usec = 0;
    bool has_source_track = false;
};

using ProgressCallback = std::function<bool(const ProgressInfo &)>;
using LogCallback = std::function<void(const std::string &)>;

// Installs a K4A log handler that suppresses the noisy "disk write speed" warning.
void install_k4a_log_filter();

// Scans a bag and returns its metadata/frame index. `hash_frames` fills SHA-256 for
// every frame, which is required by verify.
ScanResult inspect_bag(const std::string &bag_path, bool hash_frames = false);

ConvertSummary convert_bag(const ConvertOptions &options, const ProgressCallback &progress = {},
                           const LogCallback &log = {});

VerifySummary verify_bag(const std::string &bag_path, const std::string &mkv_path,
                         const ProgressCallback &progress = {}, const LogCallback &log = {});

// Fast self-test for SHA-256/base64/JSON/byte-swap primitives.
bool self_test();

std::string version();

}  // namespace bag2mkv
