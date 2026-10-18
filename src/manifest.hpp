#pragma once

#include "bag.hpp"

#include <string>
#include <vector>

namespace bag2mkv {

struct Attachment {
    std::string name;
    std::vector<uint8_t> data;
};

struct MetadataBundle {
    std::string manifest_json;
    std::string frames_jsonl;
    std::string calibration_json;
    std::string calibration_source;
    std::vector<Attachment> extra_attachments;
};

std::string synthesize_calibration_json(const ScanResult &scan);
MetadataBundle build_metadata(const ScanResult &scan, const std::string &calibration_json,
                              const std::string &calibration_source, const std::string &color_output,
                              const std::string &source_color_track);

}  // namespace bag2mkv
