#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bag2mkv {

// Small JSON DOM used for the manifest/frames attachments. It intentionally
// supports only RFC 8259 values; the bag2mkv manifest is plain JSON.
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    bool is_null() const { return type == Type::Null; }
    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }
    bool is_number() const { return type == Type::Number; }

    const Json *find(const std::string &key) const;
    std::string as_string(const std::string &fallback = {}) const;
    uint64_t as_u64(uint64_t fallback = 0) const;
    double as_double(double fallback = 0.0) const;
    bool as_bool(bool fallback = false) const;
};

Json parse_json(const std::string &text);

}  // namespace bag2mkv
