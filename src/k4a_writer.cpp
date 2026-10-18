#include "k4a_writer.hpp"
#include "util.hpp"

#include <cstring>
#include <stdexcept>

namespace bag2mkv {
namespace {

constexpr const char *kOriginalColorTrack = "ORIGINAL_COLOR_MJPG";

void check(k4a_result_t result, const char *what) {
    if (result != K4A_RESULT_SUCCEEDED) {
        throw std::runtime_error(std::string("K4A ") + what + " failed");
    }
}

void put_u16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v & 0xFF);
    p[1] = uint8_t(v >> 8);
}

void put_u32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v & 0xFF);
    p[1] = uint8_t((v >> 8) & 0xFF);
    p[2] = uint8_t((v >> 16) & 0xFF);
    p[3] = uint8_t((v >> 24) & 0xFF);
}

}  // namespace

K4aWriter::K4aWriter(const std::string &path, const WriterConfig &config) : path_(path), config_(config) {
    k4a_device_configuration_t cfg = K4A_DEVICE_CONFIG_INIT_DISABLE_ALL;
    cfg.color_format = config_.color_format;
    cfg.color_resolution = config_.color_resolution;
    cfg.depth_mode = config_.depth_mode;
    cfg.camera_fps = config_.camera_fps == 5    ? K4A_FRAMES_PER_SECOND_5
                     : config_.camera_fps == 15 ? K4A_FRAMES_PER_SECOND_15
                     : config_.camera_fps == 25 ? K4A_FRAMES_PER_SECOND_25
                                                : K4A_FRAMES_PER_SECOND_30;
    cfg.synchronized_images_only = true;
    cfg.depth_delay_off_color_usec = 0;
    cfg.wired_sync_mode = K4A_WIRED_SYNC_MODE_STANDALONE;
    cfg.subordinate_delay_off_master_usec = 0;

    k4a_result_t result = k4a_record_create(path_.c_str(), nullptr, cfg, &record_);
    if (result != K4A_RESULT_SUCCEEDED) {
        record_ = nullptr;
        throw std::runtime_error("k4a_record_create failed for: " + path_);
    }

    if (config_.color_enabled && config_.preserve_source_color) {
        // Preserve the original MJPEG bitstream in a separate custom track. The built-in
        // COLOR track is BGRA for compatibility with players that do not decode MJPEG.
        uint8_t codec_context[40] = {};
        put_u32(codec_context + 0, 40);  // biSize
        put_u32(codec_context + 4, config_.color_width);
        put_u32(codec_context + 8, config_.color_height);
        put_u16(codec_context + 12, 1);   // biPlanes
        put_u16(codec_context + 14, 24);  // biBitCount
        std::memcpy(codec_context + 16, "MJPG", 4);
        put_u32(codec_context + 20, 0);  // biSizeImage
        k4a_record_video_settings_t settings = {config_.color_width, config_.color_height,
                                                static_cast<uint64_t>(config_.camera_fps)};
        check(k4a_record_add_custom_video_track(record_, kOriginalColorTrack, "V_MS/VFW/FOURCC", codec_context,
                                                sizeof(codec_context), &settings),
              "k4a_record_add_custom_video_track");
    }
}

K4aWriter::~K4aWriter() {
    if (!closed_) {
        try {
            close();
        } catch (...) {
        }
    }
}

void K4aWriter::add_attachment(const std::string &name, const std::vector<uint8_t> &data) {
    if (header_written_) {
        throw std::runtime_error("K4A attachments must be added before write_header");
    }
    check(k4a_record_add_attachment(record_, name.c_str(), data.data(), data.size()),
          "k4a_record_add_attachment");
}

void K4aWriter::add_tag(const std::string &name, const std::string &value) {
    if (header_written_) {
        throw std::runtime_error("K4A tags must be added before write_header");
    }
    check(k4a_record_add_tag(record_, name.c_str(), value.c_str()), "k4a_record_add_tag");
}

void K4aWriter::write_header() {
    if (header_written_) {
        return;
    }
    check(k4a_record_write_header(record_), "k4a_record_write_header");
    header_written_ = true;
}

void K4aWriter::write_color_image(k4a_image_format_t format, uint32_t stride, const uint8_t *data, size_t size,
                                  uint64_t timestamp_usec) {
    if (!config_.color_enabled) {
        throw std::runtime_error("color frame encountered but color output is disabled");
    }
    if (!header_written_) {
        throw std::runtime_error("K4A header not written");
    }
    k4a_image_t image = nullptr;
    check(k4a_image_create_from_buffer(format, config_.color_width, config_.color_height, stride,
                                       const_cast<uint8_t *>(data), size, nullptr, nullptr, &image),
          "k4a_image_create_from_buffer(color)");
    k4a_image_set_device_timestamp_usec(image, timestamp_usec);

    k4a_capture_t capture = nullptr;
    check(k4a_capture_create(&capture), "k4a_capture_create(color)");
    // The K4A wrapper takes ownership of the image here; do not release it separately.
    k4a_capture_set_color_image(capture, image);
    k4a_result_t write_result = k4a_record_write_capture(record_, capture);
    k4a_capture_release(capture);
    check(write_result, "k4a_record_write_capture(color)");
}

void K4aWriter::write_color_bgra(const uint8_t *bgra, size_t size, uint64_t timestamp_usec) {
    const size_t expected = static_cast<size_t>(config_.color_width) * config_.color_height * 4;
    if (size != expected) {
        throw std::runtime_error("unexpected BGRA buffer size: " + std::to_string(size) + " (expected " +
                                 std::to_string(expected) + ")");
    }
    write_color_image(K4A_IMAGE_FORMAT_COLOR_BGRA32, config_.color_width * 4u, bgra, size, timestamp_usec);
}

void K4aWriter::write_color_mjpg(const uint8_t *jpeg, size_t size, uint64_t timestamp_usec) {
    write_color_image(K4A_IMAGE_FORMAT_COLOR_MJPG, 0, jpeg, size, timestamp_usec);
}

void K4aWriter::write_original_color_mjpg(const uint8_t *jpeg, size_t size, uint64_t timestamp_usec) {
    if (!config_.preserve_source_color) {
        return;
    }
    if (!header_written_) {
        throw std::runtime_error("K4A header not written");
    }
    check(k4a_record_write_custom_track_data(record_, kOriginalColorTrack, timestamp_usec,
                                             const_cast<uint8_t *>(jpeg), size),
          "k4a_record_write_custom_track_data");
}

void K4aWriter::write_depth(const uint8_t *depth_le, size_t size, uint64_t timestamp_usec) {
    if (!config_.depth_enabled) {
        throw std::runtime_error("depth frame encountered but depth output is disabled");
    }
    if (!header_written_) {
        throw std::runtime_error("K4A header not written");
    }
    const size_t expected = static_cast<size_t>(config_.depth_width) * config_.depth_height * 2;
    if (size != expected) {
        throw std::runtime_error("unexpected depth buffer size: " + std::to_string(size) + " (expected " +
                                 std::to_string(expected) + ")");
    }
    // k4a_record_write_capture() expects little-endian DEPTH16 and converts it to
    // the on-disk b16g big-endian format itself. Do not swap here.
    k4a_image_t image = nullptr;
    check(k4a_image_create_from_buffer(K4A_IMAGE_FORMAT_DEPTH16, config_.depth_width, config_.depth_height,
                                       config_.depth_width * 2, const_cast<uint8_t *>(depth_le), size, nullptr, nullptr,
                                       &image),
          "k4a_image_create_from_buffer(depth)");
    k4a_image_set_device_timestamp_usec(image, timestamp_usec);

    k4a_capture_t capture = nullptr;
    check(k4a_capture_create(&capture), "k4a_capture_create(depth)");
    // Ownership is transferred to the capture.
    k4a_capture_set_depth_image(capture, image);
    k4a_result_t write_result = k4a_record_write_capture(record_, capture);
    k4a_capture_release(capture);
    check(write_result, "k4a_record_write_capture(depth)");
}

void K4aWriter::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    if (record_ == nullptr) {
        return;
    }
    k4a_result_t flush_result = k4a_record_flush(record_);
    k4a_record_close(record_);
    record_ = nullptr;
    check(flush_result, "k4a_record_flush");
}

}  // namespace bag2mkv
