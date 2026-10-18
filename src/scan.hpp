#pragma once

#include "bag.hpp"

#include <functional>
#include <string>

namespace bag2mkv {

using ScanProgress = std::function<bool(uint32_t current, uint32_t total)>;

ScanResult scan_bag(const std::string &path, bool hash_frames, const ScanProgress &progress = {});

}  // namespace bag2mkv
