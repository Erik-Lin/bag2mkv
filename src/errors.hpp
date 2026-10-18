#pragma once

#include <stdexcept>

namespace bag2mkv {

class CancelledError : public std::runtime_error {
public:
    CancelledError() : std::runtime_error("operation cancelled") {}
};

}  // namespace bag2mkv
