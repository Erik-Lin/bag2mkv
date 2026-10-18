#include "converter.hpp"

#include "bag.hpp"
#include "bz2_util.hpp"
#include "errors.hpp"
#include "jpeg_decoder.hpp"
#include "manifest.hpp"
#include "util.hpp"

#include <k4a/k4a.h>
#include <k4arecord/playback.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace bag2mkv {
namespace {

void k4a_log_callback(void *, k4a_log_level_t, const char *file, int line, const char *message) {
    if (message != nullptr && std::strstr(message, "Disk write speed is too low") != nullptr) {
        return;
    }
    std::fprintf(stderr, "[k4a] %s:%d %s\n", file != nullptr ? file : "", line, message != nullptr ? message : "");
}

void log_line(const LogCallback &log, const std::string &line) {
    if (log) {
        log(line);
    }
}

void report(const ProgressCallback &progress, Stage stage, uint64_t current, uint64_t total,
            const std::string &message) {
    if (progress && !progress(ProgressInfo{stage, current, total, message})) {
        throw CancelledError();
    }
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

const FrameMeta *first_frame(const ScanResult &scan, const std::string &kind) {
    for (const auto &f : scan.frames) {
        if (f.kind == kind) {
            return &f;
        }
    }
    return nullptr;
}

uint64_t frame_count(const ScanResult &scan, const std::string &kind) {
    auto it = scan.frame_counts.find(kind);
    return it == scan.frame_counts.end() ? 0 : it->second;
}

k4a_color_resolution_t color_resolution_for(uint32_t width, uint32_t height) {
    if (width == 1920 && height == 1080) return K4A_COLOR_RESOLUTION_1080P;
    if (width == 3840 && height == 2160) return K4A_COLOR_RESOLUTION_2160P;
    if (width == 2560 && height == 1440) return K4A_COLOR_RESOLUTION_1440P;
    if (width == 2048 && height == 1536) return K4A_COLOR_RESOLUTION_1536P;
    if (width == 4096 && height == 3072) return K4A_COLOR_RESOLUTION_3072P;
    if (width == 1280 && height == 720) return K4A_COLOR_RESOLUTION_720P;
    throw std::runtime_error("unsupported color resolution: " + std::to_string(width) + "x" + std::to_string(height));
}

WriterConfig build_writer_config(const ScanResult &scan, ColorOutput color_output, bool preserve_source_color) {
    WriterConfig cfg;
    for (const auto &kv : scan.frame_counts) {
        if (kv.first != "COLOR" && kv.first != "DEPTH" && kv.first != "IR") {
            throw std::runtime_error("unsupported stream in this version: " + kv.first);
        }
    }

    const FrameMeta *color = first_frame(scan, "COLOR");
    const FrameMeta *depth = first_frame(scan, "DEPTH");
    const FrameMeta *ir = first_frame(scan, "IR");
    if (color == nullptr && depth == nullptr && ir == nullptr) {
        throw std::runtime_error("bag contains no color, depth or IR frames");
    }

    if (color != nullptr) {
        cfg.color_enabled = true;
        cfg.color_width = color->width;
        cfg.color_height = color->height;
        cfg.color_output = color_output;
        cfg.color_resolution = color_resolution_for(cfg.color_width, cfg.color_height);
        const std::string enc = lower(color->encoding);
        if (enc == "mjpg" || enc == "jpeg") {
            if (color_output == ColorOutput::Bgra) {
                cfg.color_format = K4A_IMAGE_FORMAT_COLOR_BGRA32;
                cfg.preserve_source_color = preserve_source_color;
            } else {
                cfg.color_format = K4A_IMAGE_FORMAT_COLOR_MJPG;
            }
        } else if (enc == "bgra" || enc == "bgra8") {
            if (color_output == ColorOutput::Mjpg) {
                throw std::runtime_error("bag is already BGRA; cannot output MJPEG without lossy encoding");
            }
            cfg.color_format = K4A_IMAGE_FORMAT_COLOR_BGRA32;
        } else {
            throw std::runtime_error("unsupported color encoding: " + color->encoding);
        }
    }

    if (depth != nullptr) {
        cfg.depth_enabled = true;
        cfg.depth_width = depth->width;
        cfg.depth_height = depth->height;
        const std::string enc = lower(depth->encoding);
        if (enc != "mono16" && enc != "y16" && enc != "16uc1") {
            throw std::runtime_error("unsupported depth encoding: " + depth->encoding);
        }
        if (cfg.depth_width == 640 && cfg.depth_height == 576) {
            cfg.depth_mode = K4A_DEPTH_MODE_NFOV_UNBINNED;
        } else if (cfg.depth_width == 320 && cfg.depth_height == 288) {
            cfg.depth_mode = K4A_DEPTH_MODE_NFOV_2X2BINNED;
        } else if (cfg.depth_width == 512 && cfg.depth_height == 512) {
            cfg.depth_mode = K4A_DEPTH_MODE_WFOV_2X2BINNED;
        } else if (cfg.depth_width == 1024 && cfg.depth_height == 1024) {
            cfg.depth_mode = K4A_DEPTH_MODE_WFOV_UNBINNED;
        } else {
            throw std::runtime_error("unsupported depth resolution: " + std::to_string(cfg.depth_width) + "x" +
                                     std::to_string(cfg.depth_height));
        }
    }

    if (ir != nullptr) {
        cfg.ir_enabled = true;
        cfg.ir_width = ir->width;
        cfg.ir_height = ir->height;
        const std::string enc = lower(ir->encoding);
        if (enc != "mono16" && enc != "y16" && enc != "16uc1") {
            throw std::runtime_error("unsupported IR encoding: " + ir->encoding);
        }
        if (cfg.depth_mode == K4A_DEPTH_MODE_OFF) {
            if (cfg.ir_width == 640 && cfg.ir_height == 576) {
                cfg.depth_mode = K4A_DEPTH_MODE_NFOV_UNBINNED;
            } else if (cfg.ir_width == 320 && cfg.ir_height == 288) {
                cfg.depth_mode = K4A_DEPTH_MODE_NFOV_2X2BINNED;
            } else if (cfg.ir_width == 512 && cfg.ir_height == 512) {
                cfg.depth_mode = K4A_DEPTH_MODE_WFOV_2X2BINNED;
            } else if (cfg.ir_width == 1024 && cfg.ir_height == 1024) {
                cfg.depth_mode = K4A_DEPTH_MODE_WFOV_UNBINNED;
            } else {
                throw std::runtime_error("unsupported IR resolution: " + std::to_string(cfg.ir_width) + "x" +
                                         std::to_string(cfg.ir_height));
            }
        }
        if (cfg.depth_enabled &&
            (cfg.ir_width != cfg.depth_width || cfg.ir_height != cfg.depth_height)) {
            throw std::runtime_error("IR and depth resolutions differ; K4A requires the same depth mode");
        }
    }

    int fps = 30;
    auto color_profile = scan.profiles.find(2);
    auto depth_profile = scan.profiles.find(3);
    if (color_profile != scan.profiles.end() && color_profile->second.fps != 0) {
        fps = color_profile->second.fps;
    } else if (depth_profile != scan.profiles.end() && depth_profile->second.fps != 0) {
        fps = depth_profile->second.fps;
    }
    cfg.camera_fps = (fps == 5 || fps == 15 || fps == 25 || fps == 30) ? fps : 30;

    for (const auto &f : scan.frames) {
        if (f.kind == "COLOR") {
            if (f.width != cfg.color_width || f.height != cfg.color_height ||
                lower(f.encoding) != lower(color->encoding)) {
                throw std::runtime_error("color frame format changes within the bag");
            }
            const bool source_bgra = lower(color->encoding) == "bgra" || lower(color->encoding) == "bgra8";
            if (source_bgra && f.data_size != uint64_t(f.width) * f.height * 4) {
                throw std::runtime_error("BGRA frame size mismatch");
            }
        } else if (f.kind == "DEPTH") {
            if (f.width != cfg.depth_width || f.height != cfg.depth_height ||
                lower(f.encoding) != lower(depth->encoding)) {
                throw std::runtime_error("depth frame format changes within the bag");
            }
            if (f.data_size != uint64_t(f.width) * f.height * 2) {
                throw std::runtime_error("depth frame size mismatch");
            }
        } else if (f.kind == "IR") {
            if (f.width != cfg.ir_width || f.height != cfg.ir_height ||
                lower(f.encoding) != lower(ir->encoding)) {
                throw std::runtime_error("IR frame format changes within the bag");
            }
            if (f.data_size != uint64_t(f.width) * f.height * 2) {
                throw std::runtime_error("IR frame size mismatch");
            }
        }
    }
    return cfg;
}

std::string get_tag(k4a_playback_t playback, const std::string &name) {
    size_t size = 0;
    k4a_buffer_result_t result = k4a_playback_get_tag(playback, name.c_str(), nullptr, &size);
    if (result != K4A_BUFFER_RESULT_TOO_SMALL || size == 0) {
        return {};
    }
    std::vector<char> buffer(size + 1, 0);
    result = k4a_playback_get_tag(playback, name.c_str(), buffer.data(), &size);
    if (result != K4A_BUFFER_RESULT_SUCCEEDED) {
        return {};
    }
    return std::string(buffer.data());
}

std::string format_bytes(uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.2f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

uint64_t estimate_output_bytes(const ScanResult &scan, const WriterConfig &config) {
    uint64_t total = 0;
    for (const auto &frame : scan.frames) {
        if (frame.kind == "COLOR" && config.color_enabled) {
            if (config.color_format == K4A_IMAGE_FORMAT_COLOR_BGRA32) {
                total += uint64_t(frame.width) * frame.height * 4;
            } else {
                total += frame.data_size;
            }
            if (config.preserve_source_color) {
                total += frame.data_size;
            }
        } else if (frame.kind == "DEPTH" && config.depth_enabled) {
            total += uint64_t(frame.width) * frame.height * 2;
        } else if (frame.kind == "IR" && config.ir_enabled) {
            total += uint64_t(frame.width) * frame.height * 2;
        }
    }
    // Container index/cues/attachments/JSONL, plus a small safety margin.
    total += 16ull * 1024ull * 1024ull;
    total += total / 50;  // ~2%
    return total;
}

}  // namespace

void install_k4a_log_filter() {
    (void)k4a_set_debug_message_handler(k4a_log_callback, nullptr, K4A_LOG_LEVEL_ERROR);
}

std::string version() {
#ifdef BAG2MKV_VERSION
    return BAG2MKV_VERSION;
#else
    return "0.4.3";
#endif
}

ScanResult inspect_bag(const std::string &bag_path, bool hash_frames) {
    return scan_bag(bag_path, hash_frames);
}

bool self_test() {
    bool ok = true;
    const std::string hash = sha256_hex(reinterpret_cast<const uint8_t *>("abc"), 3);
    if (hash != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        ok = false;
    }
    if (base64_encode(reinterpret_cast<const uint8_t *>("hello"), 5) != "aGVsbG8=") {
        ok = false;
    }
    if (json_escape("a\"b\\c\n") != "a\\\"b\\\\c\\n") {
        ok = false;
    }
    {
        const uint8_t bz2[] = {
            0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0xf2, 0x85, 0x24, 0xf0, 0x00, 0x00,
            0x02, 0xd9, 0x80, 0x00, 0x10, 0x40, 0x00, 0x10, 0x00, 0x12, 0x44, 0x80, 0x10, 0x20, 0x00, 0x31,
            0x06, 0x4c, 0x41, 0x00, 0xd3, 0xd2, 0x58, 0xf4, 0x43, 0x87, 0xe2, 0xee, 0x48, 0xa7, 0x0a, 0x12,
            0x1e, 0x50, 0xa4, 0x9e, 0x00};
        try {
            std::vector<uint8_t> decoded = bz2_decompress(std::vector<uint8_t>(bz2, bz2 + sizeof(bz2)));
            const std::string text(decoded.begin(), decoded.end());
            if (text != "hello bz2\n") {
                ok = false;
            }
        } catch (...) {
            ok = false;
        }
    }
    const uint8_t src[4] = {0x01, 0x02, 0x03, 0x04};
    std::vector<uint8_t> dst;
    swap16_copy(src, 4, dst);
    if (dst.size() != 4 || dst[0] != 0x02 || dst[1] != 0x01 || dst[2] != 0x04 || dst[3] != 0x03) {
        ok = false;
    }
    return ok;
}

ConvertSummary convert_bag(const ConvertOptions &options, const ProgressCallback &progress, const LogCallback &log) {
    if (options.input_bag.empty()) {
        throw std::runtime_error("input bag path is empty");
    }
    if (options.output_mkv.empty()) {
        throw std::runtime_error("output MKV path is empty");
    }
    {
        std::error_code ec_in;
        std::error_code ec_out;
        fs::path in_abs = fs::weakly_canonical(fs::path(options.input_bag), ec_in);
        fs::path out_abs = fs::weakly_canonical(fs::path(options.output_mkv), ec_out);
        if (!ec_in && !ec_out && in_abs == out_abs) {
            throw std::runtime_error("input bag and output MKV must be different files");
        }
    }

    report(progress, Stage::Scanning, 0, 1, "scanning bag");
    log_line(log, "scanning: " + options.input_bag);
    ScanResult scan = scan_bag(options.input_bag, true, [&](uint32_t current, uint32_t total) {
        return !progress || progress(ProgressInfo{Stage::Scanning, current, total ? total : 1, "scanning bag"});
    });
    if (scan.frames.empty()) {
        throw std::runtime_error("bag contains no image frames");
    }
    report(progress, Stage::Scanning, 1, 1, "scan complete");
    log_line(log, "frames: " + std::to_string(scan.frames.size()) + " (COLOR " +
                       std::to_string(frame_count(scan, "COLOR")) + ", DEPTH " +
                       std::to_string(frame_count(scan, "DEPTH")) + ")");

    WriterConfig config = build_writer_config(scan, options.color_output, options.preserve_source_color);

    report(progress, Stage::Preparing, 0, 1, "preparing metadata");
    std::string calibration_json;
    std::string calibration_source = "none";
    if (!options.no_calibration) {
        if (!options.calibration_json_path.empty()) {
            std::vector<uint8_t> raw = read_file(options.calibration_json_path);
            calibration_json.assign(raw.begin(), raw.end());
            calibration_source = "provided";
            log_line(log, "calibration: using " + options.calibration_json_path);
        } else {
            calibration_json = synthesize_calibration_json(scan);
            if (calibration_json.empty()) {
                log_line(log, "warning: no stream profiles, calibration will be omitted");
            } else {
                calibration_source = "synthesized_from_stream_profiles";
            }
        }
    }

    MetadataBundle metadata =
        build_metadata(scan, calibration_json, calibration_source,
                       config.color_output == ColorOutput::Bgra ? "bgra" : "mjpg",
                       config.preserve_source_color ? "ORIGINAL_COLOR_MJPG" : "");

    const fs::path output_path(options.output_mkv);
    uint64_t reclaimable = 0;
    if (fs::exists(output_path)) {
        if (!options.force) {
            throw std::runtime_error("output already exists (use --force): " + options.output_mkv);
        }
        std::error_code size_ec;
        reclaimable = fs::file_size(output_path, size_ec);
        if (size_ec) reclaimable = 0;
    }
    if (output_path.has_parent_path() && !output_path.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(output_path.parent_path(), ec);
        if (ec) {
            throw std::runtime_error("cannot create output directory: " + output_path.parent_path().string());
        }
    }
    {
        const uint64_t estimate = estimate_output_bytes(scan, config);
        std::error_code space_ec;
        fs::path space_path = output_path.has_parent_path() ? output_path.parent_path() : fs::path(".");
        fs::space_info space = fs::space(space_path, space_ec);
        if (!space_ec && space.available > 0) {
            uint64_t available = space.available + reclaimable;
            log_line(log, "estimate: ~" + format_bytes(estimate) + " output; free: " + format_bytes(available));
            if (estimate > available) {
                throw std::runtime_error("not enough free disk space for output (need ~" + format_bytes(estimate) +
                                         ", available " + format_bytes(available) + ")");
            }
        }
    }
    if (fs::exists(output_path)) {
        fs::remove(output_path);
    }
    const fs::path temp_path = output_path.string() + ".tmp";
    fs::remove(temp_path);

    try {
        K4aWriter writer(temp_path.string(), config);
        if (!metadata.calibration_json.empty()) {
            std::vector<uint8_t> bytes(metadata.calibration_json.begin(), metadata.calibration_json.end());
            writer.add_attachment("calibration.json", bytes);
            writer.add_tag("K4A_CALIBRATION_FILE", "calibration.json");
        }
        {
            std::vector<uint8_t> bytes(metadata.manifest_json.begin(), metadata.manifest_json.end());
            writer.add_attachment("orbbec_manifest.json", bytes);
        }
        {
            std::vector<uint8_t> bytes(metadata.frames_jsonl.begin(), metadata.frames_jsonl.end());
            writer.add_attachment("orbbec_frames.jsonl", bytes);
        }
        for (const auto &attachment : metadata.extra_attachments) {
            writer.add_attachment(attachment.name, attachment.data);
        }
        if (scan.device.present) {
            if (!scan.device.sn.empty()) {
                writer.add_tag("K4A_DEVICE_SERIAL_NUMBER", scan.device.sn);
            }
            if (!scan.device.fw_version.empty()) {
                writer.add_tag("K4A_COLOR_FIRMWARE_VERSION", scan.device.fw_version);
                writer.add_tag("K4A_DEPTH_FIRMWARE_VERSION", scan.device.fw_version);
            }
            if (!scan.device.full_name.empty()) {
                writer.add_tag("ORBBEC_DEVICE_NAME", scan.device.full_name);
            }
            if (!scan.device.connection_type.empty()) {
                writer.add_tag("ORBBEC_CONNECTION_TYPE", scan.device.connection_type);
            }
        }
        writer.write_header();
        report(progress, Stage::Preparing, 1, 1, "metadata ready");

        BagReader reader(options.input_bag);
        JpegDecoder decoder;
        std::vector<uint8_t> bgra;
        std::vector<uint8_t> chunk;
        uint64_t current_chunk = std::numeric_limits<uint64_t>::max();
        const uint64_t total = scan.frames.size();
        for (size_t index = 0; index < scan.frames.size(); ++index) {
            const FrameMeta &frame = scan.frames[index];
            if (frame.chunk_offset != current_chunk) {
                chunk = reader.read_chunk(frame.chunk_offset);
                current_chunk = frame.chunk_offset;
            }
            if (uint64_t(frame.payload_offset) + frame.data_size > chunk.size()) {
                throw std::runtime_error("frame payload is outside its chunk");
            }
            const uint8_t *data = chunk.data() + frame.payload_offset;
            if (frame.kind == "COLOR") {
                if (config.color_output == ColorOutput::Bgra) {
                    const std::string enc = lower(frame.encoding);
                    if (enc == "mjpg" || enc == "jpeg") {
                        uint32_t w = 0;
                        uint32_t h = 0;
                        decoder.decode(data, static_cast<size_t>(frame.data_size), bgra, w, h);
                        if (w != config.color_width || h != config.color_height) {
                            throw std::runtime_error("decoded JPEG dimensions do not match the color profile");
                        }
                        writer.write_color_bgra(bgra.data(), bgra.size(), frame.timestamp_usec);
                        writer.write_original_color_mjpg(data, static_cast<size_t>(frame.data_size),
                                                         frame.timestamp_usec);
                    } else {
                        writer.write_color_bgra(data, static_cast<size_t>(frame.data_size), frame.timestamp_usec);
                    }
                } else {
                    writer.write_color_mjpg(data, static_cast<size_t>(frame.data_size), frame.timestamp_usec);
                }
            } else if (frame.kind == "DEPTH") {
                writer.write_depth(data, static_cast<size_t>(frame.data_size), frame.timestamp_usec);
            } else if (frame.kind == "IR") {
                writer.write_ir(data, static_cast<size_t>(frame.data_size), frame.timestamp_usec);
            } else {
                throw std::runtime_error("unsupported frame kind during write: " + frame.kind);
            }

            const uint64_t done = index + 1;
            report(progress, Stage::Writing, done, total, "writing " + frame.kind);
            if (done % 500 == 0) {
                log_line(log, "  wrote " + std::to_string(done) + "/" + std::to_string(total) + " frames");
            }
            // The K4A writer has a bounded in-memory cluster queue. BGRA frames are much
            // larger than MJPEG, so throttle them more to keep the queue and memory bounded.
            const int delay_ms = config.color_output == ColorOutput::Bgra ? 10 : 2;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        writer.close();
        report(progress, Stage::Finalizing, 0, 1, "finalizing file");
        fs::rename(temp_path, output_path);
        report(progress, Stage::Finalizing, 1, 1, "file finalized");
    } catch (...) {
        std::error_code ec;
        fs::remove(temp_path, ec);
        throw;
    }

    ConvertSummary summary;
    summary.output_path = options.output_mkv;
    summary.output_size = fs::file_size(output_path);
    summary.total_frames = scan.frames.size();
    summary.color_frames = frame_count(scan, "COLOR");
    summary.depth_frames = frame_count(scan, "DEPTH");
    summary.ir_frames = frame_count(scan, "IR");
    summary.first_timestamp_usec = scan.first_timestamp_usec;
    summary.last_timestamp_usec = scan.last_timestamp_usec;
    summary.color_width = config.color_width;
    summary.color_height = config.color_height;
    summary.bgra = config.color_output == ColorOutput::Bgra;
    summary.source_track = config.preserve_source_color;
    summary.calibration_source = calibration_source;

    log_line(log, "wrote: " + summary.output_path + " (" + std::to_string(summary.output_size) + " bytes)");
    log_line(log, "duration: " + std::to_string(double(summary.last_timestamp_usec - summary.first_timestamp_usec) / 1e6) +
                       " s");
    log_line(log, std::string("color: ") + (summary.bgra ? "BGRA " : "MJPEG ") +
                       std::to_string(summary.color_width) + "x" + std::to_string(summary.color_height) +
                       (summary.source_track ? " + ORIGINAL_COLOR_MJPG track" : ""));
    if (summary.ir_frames) {
        log_line(log, "IR: " + std::to_string(summary.ir_frames) + " frames, " +
                           std::to_string(config.ir_width) + "x" + std::to_string(config.ir_height));
    }
    log_line(log, "calibration: " + summary.calibration_source);

    if (options.verify_after) {
        VerifySummary verify = verify_bag(options.input_bag, options.output_mkv, progress, log);
        summary.verified = verify.ok;
        if (!verify.ok) {
            throw std::runtime_error("verification failed");
        }
    }

    report(progress, Stage::Done, summary.total_frames, summary.total_frames, "done");
    return summary;
}

VerifySummary verify_bag(const std::string &bag_path, const std::string &mkv_path, const ProgressCallback &progress,
                         const LogCallback &log) {
    VerifySummary summary;
    report(progress, Stage::Scanning, 0, 1, "scanning bag for verification");
    ScanResult expected = scan_bag(bag_path, true, [&](uint32_t current, uint32_t total) {
        return !progress || progress(ProgressInfo{Stage::Scanning, current, total ? total : 1, "scanning bag"});
    });
    if (expected.frames.empty()) {
        throw std::runtime_error("verify: bag contains no image frames");
    }
    report(progress, Stage::Scanning, 1, 1, "scan complete");

    k4a_playback_t playback = nullptr;
    k4a_result_t result = k4a_playback_open(mkv_path.c_str(), &playback);
    if (result != K4A_RESULT_SUCCEEDED) {
        throw std::runtime_error("verify: cannot open MKV: " + mkv_path);
    }
    struct PlaybackGuard {
        k4a_playback_t &playback;
        ~PlaybackGuard() {
            if (playback != nullptr) {
                k4a_playback_close(playback);
                playback = nullptr;
            }
        }
    } playback_guard{playback};

    k4a_record_configuration_t record_config = {};
    result = k4a_playback_get_record_configuration(playback, &record_config);
    if (result != K4A_RESULT_SUCCEEDED) {
        throw std::runtime_error("verify: cannot read recording configuration");
    }
    const bool bgra_output = record_config.color_format == K4A_IMAGE_FORMAT_COLOR_BGRA32;
    WriterConfig cfg = build_writer_config(expected, bgra_output ? ColorOutput::Bgra : ColorOutput::Mjpg, true);

    bool ok = true;
    summary.recording_length_usec = k4a_playback_get_recording_length_usec(playback);
    log_line(log, "verify: recording length " + std::to_string(summary.recording_length_usec) + " usec");
    if (record_config.color_resolution != cfg.color_resolution) {
        log_line(log, "verify: color resolution mismatch");
        ok = false;
    }
    if (record_config.depth_mode != cfg.depth_mode) {
        log_line(log, "verify: depth mode mismatch");
        ok = false;
    }
    if (cfg.color_enabled && record_config.color_format != cfg.color_format) {
        log_line(log, "verify: color format mismatch");
        ok = false;
    }

    std::map<uint64_t, std::vector<const FrameMeta *>> color_map;
    std::map<uint64_t, std::vector<const FrameMeta *>> depth_map;
    std::map<uint64_t, std::vector<const FrameMeta *>> ir_map;
    for (const auto &f : expected.frames) {
        if (f.kind == "COLOR") {
            color_map[f.timestamp_usec].push_back(&f);
        } else if (f.kind == "DEPTH") {
            depth_map[f.timestamp_usec].push_back(&f);
        } else if (f.kind == "IR") {
            ir_map[f.timestamp_usec].push_back(&f);
        }
    }
    const FrameMeta *first_color = first_frame(expected, "COLOR");
    const std::string source_color_encoding = first_color ? lower(first_color->encoding) : std::string();
    const bool source_is_mjpg = source_color_encoding == "mjpg" || source_color_encoding == "jpeg";
    const uint64_t expected_color = frame_count(expected, "COLOR");
    const uint64_t expected_depth = frame_count(expected, "DEPTH");
    const uint64_t expected_ir = frame_count(expected, "IR");
    const uint64_t expected_total = expected_color + expected_depth + expected_ir;

    std::set<const FrameMeta *> used;
    uint64_t color_seen = 0;
    uint64_t depth_seen = 0;
    uint64_t ir_seen = 0;
    uint64_t last_color_ts = 0;
    uint64_t last_depth_ts = 0;
    uint64_t last_ir_ts = 0;

    BagReader bag_reader(bag_path);
    JpegDecoder jpeg_decoder;
    std::vector<uint8_t> chunk;
    std::vector<uint8_t> decoded_bgra;
    uint64_t cached_chunk_offset = std::numeric_limits<uint64_t>::max();
    auto frame_payload = [&](const FrameMeta &frame) -> const uint8_t * {
        if (frame.chunk_offset != cached_chunk_offset) {
            chunk = bag_reader.read_chunk(frame.chunk_offset);
            cached_chunk_offset = frame.chunk_offset;
        }
        if (uint64_t(frame.payload_offset) + frame.data_size > chunk.size()) {
            throw std::runtime_error("frame payload is outside its chunk");
        }
        return chunk.data() + frame.payload_offset;
    };

    auto match_depth = [&](uint64_t ts, const std::string &hash, size_t actual_size) -> bool {
        auto it = depth_map.find(ts);
        if (it == depth_map.end()) {
            log_line(log, "verify: no expected DEPTH frame at timestamp " + std::to_string(ts));
            return false;
        }
        for (const FrameMeta *frame : it->second) {
            if (used.count(frame) == 0 && frame->sha256 == hash) {
                used.insert(frame);
                return true;
            }
        }
        std::string line = "verify: DEPTH payload mismatch at timestamp " + std::to_string(ts) + " size " +
                           std::to_string(actual_size);
        if (!it->second.empty()) {
            line += " expected_size " + std::to_string(it->second.front()->data_size) + " expected_sha " +
                    it->second.front()->sha256.substr(0, 16) + " actual_sha " + hash.substr(0, 16);
        }
        log_line(log, line);
        return false;
    };

    auto match_ir = [&](uint64_t ts, const std::string &hash, size_t actual_size) -> bool {
        auto it = ir_map.find(ts);
        if (it == ir_map.end()) {
            log_line(log, "verify: no expected IR frame at timestamp " + std::to_string(ts));
            return false;
        }
        for (const FrameMeta *frame : it->second) {
            if (used.count(frame) == 0 && frame->sha256 == hash) {
                used.insert(frame);
                return true;
            }
        }
        log_line(log, "verify: IR payload mismatch at timestamp " + std::to_string(ts) + " size " +
                          std::to_string(actual_size));
        return false;
    };

    auto match_color = [&](uint64_t ts, k4a_image_t color) -> bool {
        auto it = color_map.find(ts);
        if (it == color_map.end()) {
            log_line(log, "verify: no expected COLOR frame at timestamp " + std::to_string(ts));
            return false;
        }
        const uint8_t *actual = k4a_image_get_buffer(color);
        const size_t actual_size = size_t(k4a_image_get_size(color));
        const uint32_t width = uint32_t(k4a_image_get_width_pixels(color));
        const uint32_t height = uint32_t(k4a_image_get_height_pixels(color));
        const uint32_t stride = uint32_t(k4a_image_get_stride_bytes(color));

        for (const FrameMeta *frame : it->second) {
            if (used.count(frame) != 0) {
                continue;
            }
            if (!bgra_output) {
                if (frame->sha256 == sha256_hex(actual, actual_size)) {
                    used.insert(frame);
                    return true;
                }
                continue;
            }
            if (!source_is_mjpg) {
                if (frame->sha256 == sha256_hex(actual, actual_size)) {
                    used.insert(frame);
                    return true;
                }
                continue;
            }
            try {
                uint32_t w = 0;
                uint32_t h = 0;
                jpeg_decoder.decode(frame_payload(*frame), static_cast<size_t>(frame->data_size), decoded_bgra, w, h);
                if (w != width || h != height || decoded_bgra.size() != size_t(width) * height * 4 ||
                    actual_size < size_t(stride) * height) {
                    continue;
                }
                bool equal = true;
                for (uint32_t y = 0; y < height && equal; ++y) {
                    equal = std::memcmp(decoded_bgra.data() + size_t(y) * width * 4,
                                        actual + size_t(y) * stride, size_t(width) * 4) == 0;
                }
                if (equal) {
                    used.insert(frame);
                    return true;
                }
            } catch (const std::exception &e) {
                log_line(log, "verify: failed to decode bag JPEG at " + std::to_string(ts) + ": " + e.what());
            }
        }
        log_line(log, "verify: COLOR pixels mismatch at timestamp " + std::to_string(ts) + " size " +
                          std::to_string(actual_size));
        return false;
    };

    k4a_capture_t capture = nullptr;
    while (true) {
        k4a_stream_result_t sr = k4a_playback_get_next_capture(playback, &capture);
        if (sr == K4A_STREAM_RESULT_EOF) {
            break;
        }
        if (sr != K4A_STREAM_RESULT_SUCCEEDED) {
            throw std::runtime_error("verify: playback failed with result " + std::to_string(int(sr)));
        }
        k4a_image_t color = k4a_capture_get_color_image(capture);
        k4a_image_t depth = k4a_capture_get_depth_image(capture);
        if (color != nullptr) {
            ++color_seen;
            uint64_t ts = k4a_image_get_device_timestamp_usec(color);
            last_color_ts = ts;
            if (!match_color(ts, color)) {
                ok = false;
            }
            if (k4a_image_get_format(color) != (cfg.color_enabled ? cfg.color_format : K4A_IMAGE_FORMAT_CUSTOM)) {
                log_line(log, "verify: unexpected COLOR image format");
                ok = false;
            }
            k4a_image_release(color);
        }
        if (depth != nullptr) {
            ++depth_seen;
            uint64_t ts = k4a_image_get_device_timestamp_usec(depth);
            last_depth_ts = ts;
            std::string hash = sha256_hex(k4a_image_get_buffer(depth), size_t(k4a_image_get_size(depth)));
            if (!match_depth(ts, hash, size_t(k4a_image_get_size(depth)))) {
                ok = false;
            }
            if (k4a_image_get_format(depth) != K4A_IMAGE_FORMAT_DEPTH16) {
                log_line(log, "verify: unexpected DEPTH image format");
                ok = false;
            }
            k4a_image_release(depth);
        }
        k4a_image_t ir = k4a_capture_get_ir_image(capture);
        if (ir != nullptr) {
            ++ir_seen;
            uint64_t ts = k4a_image_get_device_timestamp_usec(ir);
            last_ir_ts = ts;
            std::string hash = sha256_hex(k4a_image_get_buffer(ir), size_t(k4a_image_get_size(ir)));
            if (!match_ir(ts, hash, size_t(k4a_image_get_size(ir)))) {
                ok = false;
            }
            if (k4a_image_get_format(ir) != K4A_IMAGE_FORMAT_IR16) {
                log_line(log, "verify: unexpected IR image format");
                ok = false;
            }
            k4a_image_release(ir);
        }
        k4a_capture_release(capture);
        report(progress, Stage::Verifying, color_seen + depth_seen + ir_seen, expected_total, "verifying");
    }

    if (color_seen != expected_color) {
        log_line(log, "verify: COLOR count " + std::to_string(color_seen) + " != expected " +
                          std::to_string(expected_color));
        ok = false;
    }
    if (depth_seen != expected_depth) {
        log_line(log, "verify: DEPTH count " + std::to_string(depth_seen) + " != expected " +
                          std::to_string(expected_depth));
        ok = false;
    }
    if (ir_seen != expected_ir) {
        log_line(log, "verify: IR count " + std::to_string(ir_seen) + " != expected " +
                          std::to_string(expected_ir));
        ok = false;
    }
    for (const auto &f : expected.frames) {
        if ((f.kind == "COLOR" || f.kind == "DEPTH" || f.kind == "IR") && used.count(&f) == 0) {
            log_line(log, "verify: missing frame " + f.kind + " at " + std::to_string(f.timestamp_usec));
            ok = false;
        }
    }

    summary.has_source_track = k4a_playback_check_track_exists(playback, "ORIGINAL_COLOR_MJPG");
    if (summary.has_source_track) {
        uint64_t source_seen = 0;
        k4a_playback_data_block_t block = nullptr;
        while (true) {
            k4a_stream_result_t sr = k4a_playback_get_next_data_block(playback, "ORIGINAL_COLOR_MJPG", &block);
            if (sr == K4A_STREAM_RESULT_EOF) {
                break;
            }
            if (sr != K4A_STREAM_RESULT_SUCCEEDED) {
                log_line(log, "verify: failed to read ORIGINAL_COLOR_MJPG data block");
                ok = false;
                break;
            }
            ++source_seen;
            uint64_t ts = k4a_playback_data_block_get_device_timestamp_usec(block);
            const uint8_t *data = k4a_playback_data_block_get_buffer(block);
            size_t size = k4a_playback_data_block_get_buffer_size(block);
            auto it = color_map.find(ts);
            bool matched = false;
            if (it != color_map.end() && data != nullptr) {
                std::string hash = sha256_hex(data, size);
                for (const FrameMeta *frame : it->second) {
                    if (frame->data_size == size && frame->sha256 == hash) {
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) {
                log_line(log, "verify: ORIGINAL_COLOR_MJPG mismatch at timestamp " + std::to_string(ts));
                ok = false;
            }
            k4a_playback_data_block_release(block);
        }
        summary.source_color_frames = source_seen;
        if (source_seen != expected_color) {
            log_line(log, "verify: ORIGINAL_COLOR_MJPG count " + std::to_string(source_seen) + " != expected " +
                              std::to_string(expected_color));
            ok = false;
        }
        if (ok) {
            log_line(log, "verify: ORIGINAL_COLOR_MJPG OK (" + std::to_string(source_seen) + " frames)");
        }
    }

    if (expected.device.present && !expected.device.sn.empty()) {
        std::string serial = get_tag(playback, "K4A_DEVICE_SERIAL_NUMBER");
        if (!serial.empty() && serial != expected.device.sn) {
            log_line(log, "verify: serial mismatch");
            ok = false;
        }
    }

    k4a_calibration_t calibration = {};
    result = k4a_playback_get_calibration(playback, &calibration);
    if (result == K4A_RESULT_SUCCEEDED) {
        const auto &d = calibration.depth_camera_calibration;
        const auto &c = calibration.color_camera_calibration;
        log_line(log, "calibration: depth " + std::to_string(d.resolution_width) + "x" +
                          std::to_string(d.resolution_height) + " fx=" + std::to_string(d.intrinsics.parameters.param.fx) +
                          " fy=" + std::to_string(d.intrinsics.parameters.param.fy) + " cx=" +
                          std::to_string(d.intrinsics.parameters.param.cx) + " cy=" +
                          std::to_string(d.intrinsics.parameters.param.cy));
        log_line(log, "calibration: color " + std::to_string(c.resolution_width) + "x" +
                          std::to_string(c.resolution_height) + " fx=" + std::to_string(c.intrinsics.parameters.param.fx) +
                          " fy=" + std::to_string(c.intrinsics.parameters.param.fy) + " cx=" +
                          std::to_string(c.intrinsics.parameters.param.cx) + " cy=" +
                          std::to_string(c.intrinsics.parameters.param.cy));
    } else {
        log_line(log, "calibration: unavailable");
    }

    summary.ok = ok;
    summary.color_frames = color_seen;
    summary.depth_frames = depth_seen;
    summary.ir_frames = ir_seen;
    if (ok) {
        log_line(log, "verify: OK (" + std::to_string(color_seen) + " color, " + std::to_string(depth_seen) +
                          " depth, " + std::to_string(ir_seen) + " IR)");
    }
    log_line(log, "verify: seen color=" + std::to_string(color_seen) + " last=" + std::to_string(last_color_ts) +
                      ", depth=" + std::to_string(depth_seen) + " last=" + std::to_string(last_depth_ts) +
                      ", IR=" + std::to_string(ir_seen) + " last=" + std::to_string(last_ir_ts) +
                      ", expected color=" + std::to_string(expected_color) + " depth=" +
                      std::to_string(expected_depth) + " IR=" + std::to_string(expected_ir));
    return summary;
}

}  // namespace bag2mkv
