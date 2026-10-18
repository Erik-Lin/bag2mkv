#include "gui_backend.hpp"

#include "converter.hpp"
#include "errors.hpp"
#include "mkv2bag.hpp"
#include "ros1_export.hpp"
#include "ros1_input.hpp"
#include "ros2_export.hpp"
#include "ros2_input.hpp"
#include "scan.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

namespace bag2mkv::gui {
namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path &path) {
    return path.u8string();
}

std::wstring widen(const std::string &value) {
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring &value) {
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr,
                                nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool is_regular_file(const fs::path &path, const wchar_t *extension) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && path.extension() == extension;
}

struct TempBagGuard {
    fs::path path;
    bool keep = false;
    ~TempBagGuard() {
        if (!keep) {
            std::error_code ec;
            fs::remove(path, ec);
        }
    }
};

}  // namespace

std::wstring input_kind_label(InputKind kind) {
    switch (kind) {
    case InputKind::OrbbecBag: return L"Orbbec ROSBAG";
    case InputKind::Ros1Bag: return L"ROS 1 bag";
    case InputKind::Ros2Bag: return L"ROS 2 bag";
    case InputKind::K4aMkv: return L"K4A MKV";
    default: return L"Not detected";
    }
}

std::wstring output_kind_label(OutputKind kind) {
    switch (kind) {
    case OutputKind::K4aMkv: return L"K4A MKV";
    case OutputKind::Ros1Bag: return L"ROS 1 bag";
    case OutputKind::Ros2Bag: return L"ROS 2 bag";
    case OutputKind::OrbbecBag: return L"Orbbec bag";
    default: return L"Choose output";
    }
}

std::wstring input_kind_hint(InputKind kind) {
    switch (kind) {
    case InputKind::OrbbecBag: return L"Orbbec SDK RGB-D recording";
    case InputKind::Ros1Bag: return L"Standard ROS 1 bag";
    case InputKind::Ros2Bag: return L"ROS 2 sqlite3 bag directory";
    case InputKind::K4aMkv: return L"Azure Kinect / K4A recording";
    default: return L"Drop a file or folder to begin";
    }
}

std::wstring output_kind_hint(OutputKind kind) {
    switch (kind) {
    case OutputKind::K4aMkv: return L"Portable K4A-compatible recording";
    case OutputKind::Ros1Bag: return L"Standard ROSBAG V2.0";
    case OutputKind::Ros2Bag: return L"metadata.yaml + bag_0.db3";
    case OutputKind::OrbbecBag: return L"Orbbec ROSBAG V2.0";
    default: return {};
    }
}

bool output_supported(InputKind input, OutputKind output) {
    switch (input) {
    case InputKind::OrbbecBag:
        return output == OutputKind::K4aMkv || output == OutputKind::Ros1Bag ||
               output == OutputKind::Ros2Bag;
    case InputKind::K4aMkv:
        return output == OutputKind::OrbbecBag || output == OutputKind::Ros1Bag ||
               output == OutputKind::Ros2Bag;
    case InputKind::Ros1Bag:
    case InputKind::Ros2Bag:
        return output == OutputKind::K4aMkv;
    default:
        return false;
    }
}

std::vector<OutputKind> supported_outputs(InputKind input) {
    std::vector<OutputKind> result;
    for (OutputKind output : {OutputKind::K4aMkv, OutputKind::Ros1Bag, OutputKind::Ros2Bag,
                              OutputKind::OrbbecBag}) {
        if (output_supported(input, output)) result.push_back(output);
    }
    return result;
}

OutputKind default_output(InputKind input) {
    switch (input) {
    case InputKind::OrbbecBag: return OutputKind::K4aMkv;
    case InputKind::K4aMkv: return OutputKind::OrbbecBag;
    default: return OutputKind::K4aMkv;
    }
}

bool is_ros2_bag(const fs::path &path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return false;
    if (!fs::is_regular_file(path / "metadata.yaml", ec)) return false;
    for (const auto &entry : fs::directory_iterator(path, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".db3") return true;
    }
    return false;
}

InputKind detect_input(const fs::path &path, std::wstring &error) {
    error.clear();
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        error = L"Path does not exist.";
        return InputKind::None;
    }
    if (fs::is_directory(path, ec)) {
        if (is_ros2_bag(path)) return InputKind::Ros2Bag;
        error = L"Folder is not a ROS 2 bag. Use Batch for a folder of recordings.";
        return InputKind::None;
    }
    if (is_regular_file(path, L".mkv")) return InputKind::K4aMkv;
    if (is_regular_file(path, L".bag")) {
        try {
            ScanResult scan = scan_bag(utf8(path), false);
            if (!scan.frames.empty() || !scan.profiles.empty() || !scan.properties.empty()) {
                return InputKind::OrbbecBag;
            }
        } catch (...) {
            // A standard ROS1 bag has the same magic, but a different record layout.
        }
        return InputKind::Ros1Bag;
    }
    error = L"Unsupported file. Choose .bag, .mkv, or a ROS 2 bag folder.";
    return InputKind::None;
}

fs::path suggested_output(const fs::path &input, InputKind, OutputKind output_kind) {
    const fs::path base = input;
    const std::wstring stem = base.stem().wstring();
    if (output_kind == OutputKind::Ros2Bag) {
        return base.parent_path() / (stem + L"_ros2");
    }
    fs::path out = base;
    if (output_kind == OutputKind::K4aMkv) out = base.parent_path() / (stem + L".mkv");
    else if (output_kind == OutputKind::OrbbecBag) out = base.parent_path() / (stem + L".bag");
    else if (output_kind == OutputKind::Ros1Bag) out = base.parent_path() / (stem + L".ros1.bag");
    return out;
}

std::vector<SourceItem> scan_batch(const fs::path &root, std::wstring &error) {
    std::vector<SourceItem> items;
    error.clear();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        error = L"Batch input must be a folder.";
        return items;
    }
    if (is_ros2_bag(root)) {
        items.push_back({root, InputKind::Ros2Bag});
        return items;
    }
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            error = L"Cannot scan the input folder: " + widen(ec.message());
            return {};
        }
        const fs::path path = it->path();
        if (it->is_directory(ec)) {
            if (is_ros2_bag(path)) {
                items.push_back({path, InputKind::Ros2Bag});
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const auto ext = path.extension();
        if (ext != L".bag" && ext != L".mkv") continue;
        std::wstring detect_error;
        InputKind kind = detect_input(path, detect_error);
        if (kind == InputKind::None) continue;
        items.push_back({path, kind});
    }
    if (items.empty()) {
        error = L"No .bag, .mkv, or ROS 2 bag folders found.";
        return {};
    }
    std::sort(items.begin(), items.end(), [](const SourceItem &a, const SourceItem &b) {
        return a.path.wstring() < b.path.wstring();
    });
    const InputKind kind = items.front().kind;
    for (const auto &item : items) {
        if (item.kind != kind) {
            error = L"Mixed input formats. Keep Orbbec, ROS 1, ROS 2, or MKV separate.";
            return {};
        }
    }
    return items;
}

std::vector<ConversionJob> build_batch_jobs(const std::vector<SourceItem> &items, const fs::path &input_root,
                                            const fs::path &output_root, OutputKind output_kind, bool force,
                                            bool verify, bool no_calibration, bool preserve_source_color,
                                            bool color_mjpg, const fs::path &calibration_json,
                                            std::wstring &error) {
    std::vector<ConversionJob> jobs;
    error.clear();
    if (items.empty()) {
        error = L"No input recordings.";
        return jobs;
    }
    if (!output_supported(items.front().kind, output_kind)) {
        error = L"This output format is not supported for the selected inputs.";
        return jobs;
    }
    std::set<std::wstring> used;
    for (const auto &item : items) {
        std::error_code ec;
        fs::path rel = fs::relative(item.path, input_root, ec);
        if (ec || rel.empty() || rel == fs::path(".")) rel = item.path.filename();
        fs::path rel_parent = rel.parent_path();
        const std::wstring stem = rel.stem().wstring();
        fs::path out;
        if (output_kind == OutputKind::K4aMkv) out = output_root / rel_parent / (stem + L".mkv");
        else if (output_kind == OutputKind::OrbbecBag) out = output_root / rel_parent / (stem + L".bag");
        else if (output_kind == OutputKind::Ros1Bag) out = output_root / rel_parent / (stem + L".ros1.bag");
        else out = output_root / rel_parent / (stem + L"_ros2");
        std::wstring key = out.wstring();
        std::transform(key.begin(), key.end(), key.begin(), towlower);
        if (!used.insert(key).second) {
            error = L"Two inputs map to the same output: " + out.wstring();
            return {};
        }
        jobs.push_back(ConversionJob{item.path, out, item.kind, output_kind, force, verify, no_calibration,
                                     preserve_source_color, color_mjpg, calibration_json});
    }
    return jobs;
}

JobResult run_job(const ConversionJob &job, const JobLog &log, const JobProgress &progress) {
    JobResult result;
    try {
        std::error_code ec;
        if (job.output_kind != OutputKind::Ros2Bag) {
            if (!job.output.parent_path().empty()) fs::create_directories(job.output.parent_path(), ec);
        } else {
            fs::create_directories(job.output.parent_path(), ec);
        }
        const std::string input = utf8(job.input);
        const std::string output = utf8(job.output);
        const auto report = [&](int percent, const std::wstring &stage) {
            return progress ? progress(std::clamp(percent, 0, 100), stage) : true;
        };

        if (job.input_kind == InputKind::OrbbecBag && job.output_kind == OutputKind::K4aMkv) {
            if (log) log(L"Reading Orbbec ROSBAG and building K4A MKV…");
            ConvertOptions options;
            options.input_bag = input;
            options.output_mkv = output;
            options.force = job.force;
            options.no_calibration = job.no_calibration;
            options.verify_after = job.verify;
            options.preserve_source_color = job.preserve_source_color;
            options.color_output = job.color_mjpg ? ColorOutput::Mjpg : ColorOutput::Bgra;
            options.calibration_json_path = utf8(job.calibration_json);
            ConvertSummary summary = convert_bag(
                options,
                [&](const ProgressInfo &info) {
                    int percent = 0;
                    const wchar_t *stage = L"Working…";
                    switch (info.stage) {
                    case Stage::Scanning: percent = 5; stage = L"Scanning bag"; break;
                    case Stage::Preparing: percent = 20; stage = L"Preparing writer"; break;
                    case Stage::Writing:
                        percent = info.total ? 25 + static_cast<int>(60 * info.current / info.total) : 50;
                        stage = L"Writing frames";
                        break;
                    case Stage::Finalizing: percent = 92; stage = L"Finalizing"; break;
                    case Stage::Verifying: percent = 96; stage = L"Verifying"; break;
                    case Stage::Done: percent = 100; stage = L"Done"; break;
                    }
                    return report(percent, stage);
                },
                [&](const std::string &line) { if (log) log(widen(line)); });
            result.ok = true;
            result.output_size = summary.output_size;
            result.message = L"Converted " + std::to_wstring(summary.color_frames) + L" color / " +
                             std::to_wstring(summary.depth_frames) + L" depth frames.";
        } else if (job.input_kind == InputKind::K4aMkv && job.output_kind == OutputKind::OrbbecBag) {
            if (log) log(L"Rebuilding an Orbbec ROSBAG from K4A MKV…");
            report(20, L"Reading K4A MKV");
            Mkv2BagOptions options;
            options.input_mkv = input;
            options.output_bag = output;
            options.force = job.force;
            Mkv2BagSummary summary = mkv_to_bag(
                options, [&](const std::string &line) { if (log) log(widen(line)); },
                [&](uint64_t current, uint64_t) {
                    const int percent = 30 + static_cast<int>(std::min<uint64_t>(current, 60));
                    return report(percent, L"Rebuilding bag");
                });
            result.ok = true;
            result.output_size = summary.bag_size;
            result.message = L"Rebuilt " + std::to_wstring(summary.color_frames) + L" color / " +
                             std::to_wstring(summary.depth_frames) + L" depth frames.";
        } else if (job.input_kind == InputKind::OrbbecBag && job.output_kind == OutputKind::Ros1Bag) {
            if (log) log(L"Exporting standard ROS 1 bag…");
            report(20, L"Reading Orbbec bag");
            Ros1ExportOptions options{input, output, job.force};
            Ros1ExportSummary summary = export_ob_bag_to_ros1(
                options, [&](const std::string &line) { if (log) log(widen(line)); });
            result.ok = true;
            result.output_size = summary.bag_size;
            result.message = L"ROS 1 bag: " + std::to_wstring(summary.color_frames) + L" color / " +
                             std::to_wstring(summary.depth_frames) + L" depth frames.";
        } else if (job.input_kind == InputKind::OrbbecBag && job.output_kind == OutputKind::Ros2Bag) {
            if (log) log(L"Exporting ROS 2 sqlite3 bag…");
            report(20, L"Reading Orbbec bag");
            Ros2ExportOptions options{input, output, job.force};
            Ros2ExportSummary summary = export_ob_bag_to_ros2(
                options, [&](const std::string &line) { if (log) log(widen(line)); });
            result.ok = true;
            result.message = L"ROS 2 bag: " + std::to_wstring(summary.message_count) + L" messages.";
        } else if (job.input_kind == InputKind::Ros1Bag && job.output_kind == OutputKind::K4aMkv) {
            if (log) log(L"Converting standard ROS 1 bag to K4A MKV…");
            report(20, L"Reading ROS 1 bag");
            Ros1InputOptions options{input, output, job.force, job.color_mjpg};
            Ros1InputSummary summary = convert_ros1_to_mkv(
                options, [&](const std::string &line) { if (log) log(widen(line)); });
            result.ok = true;
            result.output_size = summary.mkv_size;
            result.message = L"Converted " + std::to_wstring(summary.color_frames) + L" color / " +
                             std::to_wstring(summary.depth_frames) + L" depth frames.";
        } else if (job.input_kind == InputKind::Ros2Bag && job.output_kind == OutputKind::K4aMkv) {
            if (log) log(L"Converting ROS 2 bag to K4A MKV…");
            report(20, L"Reading ROS 2 bag");
            Ros2InputOptions options{input, output, job.force, job.color_mjpg};
            Ros2InputSummary summary = convert_ros2_to_mkv(
                options, [&](const std::string &line) { if (log) log(widen(line)); });
            result.ok = true;
            result.output_size = summary.mkv_size;
            result.message = L"Converted " + std::to_wstring(summary.color_frames) + L" color / " +
                             std::to_wstring(summary.depth_frames) + L" depth frames.";
        } else if (job.input_kind == InputKind::K4aMkv) {
            // K4A -> ROS1/ROS2: rebuild a temporary Orbbec bag, then export.
            if (log) log(L"Rebuilding an intermediate Orbbec bag…");
            const fs::path temp = job.output.parent_path() / (job.output.filename().wstring() + L".orbbec.tmp.bag");
            fs::remove(temp, ec);
            TempBagGuard guard{temp};
            Mkv2BagOptions bag_options{input, utf8(temp), true};
            mkv_to_bag(bag_options, [&](const std::string &line) { if (log) log(widen(line)); });
            report(55, L"Exporting ROS bag");
            if (job.output_kind == OutputKind::Ros1Bag) {
                Ros1ExportOptions options{utf8(temp), output, job.force};
                Ros1ExportSummary summary = export_ob_bag_to_ros1(
                    options, [&](const std::string &line) { if (log) log(widen(line)); });
                result.message = L"ROS 1 bag: " + std::to_wstring(summary.color_frames) + L" color / " +
                                 std::to_wstring(summary.depth_frames) + L" depth frames.";
            } else {
                Ros2ExportOptions options{utf8(temp), output, job.force};
                Ros2ExportSummary summary = export_ob_bag_to_ros2(
                    options, [&](const std::string &line) { if (log) log(widen(line)); });
                result.message = L"ROS 2 bag: " + std::to_wstring(summary.message_count) + L" messages.";
            }
            result.ok = true;
        } else {
            result.message = L"This input/output combination is not supported.";
            return result;
        }
        report(100, L"Done");
        return result;
    } catch (const CancelledError &) {
        result.cancelled = true;
        result.message = L"Cancelled.";
        return result;
    } catch (const std::exception &e) {
        result.message = widen(e.what());
        return result;
    }
}

}  // namespace bag2mkv::gui
