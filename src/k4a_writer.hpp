#pragma once

#include <k4a/k4a.h>
#include <k4arecord/record.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bag2mkv {

enum class ColorOutput {
    Bgra,
    Mjpg,
};

struct WriterConfig {
    bool color_enabled = false;
    bool depth_enabled = false;
    uint32_t color_width = 0;
    uint32_t color_height = 0;
    uint32_t depth_width = 0;
    uint32_t depth_height = 0;
    k4a_image_format_t color_format = K4A_IMAGE_FORMAT_COLOR_MJPG;
    k4a_color_resolution_t color_resolution = K4A_COLOR_RESOLUTION_OFF;
    k4a_depth_mode_t depth_mode = K4A_DEPTH_MODE_OFF;
    int camera_fps = 30;
    ColorOutput color_output = ColorOutput::Mjpg;
    bool preserve_source_color = false;
};

class K4aWriter {
public:
    K4aWriter(const std::string &path, const WriterConfig &config);
    ~K4aWriter();

    K4aWriter(const K4aWriter &) = delete;
    K4aWriter &operator=(const K4aWriter &) = delete;

    void add_attachment(const std::string &name, const std::vector<uint8_t> &data);
    void add_tag(const std::string &name, const std::string &value);
    void write_header();
    void write_color_bgra(const uint8_t *bgra, size_t size, uint64_t timestamp_usec);
    void write_color_mjpg(const uint8_t *jpeg, size_t size, uint64_t timestamp_usec);
    void write_original_color_mjpg(const uint8_t *jpeg, size_t size, uint64_t timestamp_usec);
    void write_depth(const uint8_t *depth_le, size_t size, uint64_t timestamp_usec);
    void close();

private:
    void write_color_image(k4a_image_format_t format, uint32_t stride, const uint8_t *data, size_t size,
                           uint64_t timestamp_usec);

    std::string path_;
    WriterConfig config_;
    k4a_record_t record_ = nullptr;
    bool header_written_ = false;
    bool closed_ = false;
};

}  // namespace bag2mkv
