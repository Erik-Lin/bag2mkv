#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace bag2mkv::gui {

enum class InputKind {
    None,
    OrbbecBag,
    Ros1Bag,
    Ros2Bag,
    K4aMkv,
};

enum class OutputKind {
    K4aMkv,
    Ros1Bag,
    Ros2Bag,
    OrbbecBag,
};

struct SourceItem {
    std::filesystem::path path;
    InputKind kind = InputKind::None;
};

struct ConversionJob {
    std::filesystem::path input;
    std::filesystem::path output;
    InputKind input_kind = InputKind::None;
    OutputKind output_kind = OutputKind::K4aMkv;
    bool force = false;
    bool verify = false;
    bool no_calibration = false;
    bool preserve_source_color = true;
    bool color_mjpg = false;
    std::filesystem::path calibration_json;
};

struct JobResult {
    bool ok = false;
    bool cancelled = false;
    uint64_t output_size = 0;
    std::wstring message;
};

using JobLog = std::function<void(const std::wstring &)>;
using JobProgress = std::function<bool(int percent, const std::wstring &stage)>;

std::wstring input_kind_label(InputKind kind);
std::wstring output_kind_label(OutputKind kind);
std::wstring input_kind_hint(InputKind kind);
std::wstring output_kind_hint(OutputKind kind);

bool output_supported(InputKind input, OutputKind output);
std::vector<OutputKind> supported_outputs(InputKind input);
OutputKind default_output(InputKind input);

InputKind detect_input(const std::filesystem::path &path, std::wstring &error);
bool is_ros2_bag(const std::filesystem::path &path);

std::filesystem::path suggested_output(const std::filesystem::path &input, InputKind input_kind,
                                       OutputKind output_kind);
std::vector<SourceItem> scan_batch(const std::filesystem::path &root, std::wstring &error);
std::vector<ConversionJob> build_batch_jobs(const std::vector<SourceItem> &items,
                                            const std::filesystem::path &input_root,
                                            const std::filesystem::path &output_root,
                                            OutputKind output_kind, bool force, bool verify,
                                            bool no_calibration, bool preserve_source_color,
                                            bool color_mjpg, const std::filesystem::path &calibration_json,
                                            std::wstring &error);

JobResult run_job(const ConversionJob &job, const JobLog &log, const JobProgress &progress);

}  // namespace bag2mkv::gui
