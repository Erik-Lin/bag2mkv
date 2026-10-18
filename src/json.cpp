#include "json.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace bag2mkv {
namespace {

class Parser {
public:
    explicit Parser(const std::string &text) : text_(text) {}

    Json parse() {
        skip_space();
        Json value = parse_value();
        skip_space();
        if (pos_ != text_.size()) {
            fail("trailing characters");
        }
        return value;
    }

private:
    const std::string &text_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const std::string &message) const {
        throw std::runtime_error("JSON parse error at byte " + std::to_string(pos_) + ": " + message);
    }

    void skip_space() {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool consume(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char c) {
        if (!consume(c)) fail(std::string("expected '") + c + "'");
    }

    Json parse_value() {
        if (pos_ >= text_.size()) fail("unexpected end of input");
        char c = text_[pos_];
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') {
            Json v;
            v.type = Json::Type::String;
            v.string = parse_string();
            return v;
        }
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        fail("unexpected character");
    }

    Json parse_object() {
        Json v;
        v.type = Json::Type::Object;
        expect('{');
        skip_space();
        if (consume('}')) return v;
        while (true) {
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != '"') fail("expected object key");
            std::string key = parse_string();
            skip_space();
            expect(':');
            skip_space();
            v.object[key] = parse_value();
            skip_space();
            if (consume('}')) break;
            expect(',');
        }
        return v;
    }

    Json parse_array() {
        Json v;
        v.type = Json::Type::Array;
        expect('[');
        skip_space();
        if (consume(']')) return v;
        while (true) {
            skip_space();
            v.array.push_back(parse_value());
            skip_space();
            if (consume(']')) break;
            expect(',');
        }
        return v;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return out;
            if (c < 0x20) fail("control character in string");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= text_.size()) fail("truncated escape");
            char esc = text_[pos_++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (pos_ + 4 > text_.size()) fail("truncated unicode escape");
                uint32_t cp = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = text_[pos_++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= uint32_t(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= uint32_t(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= uint32_t(h - 'A' + 10);
                    else fail("invalid unicode escape");
                }
                // Encode BMP code points as UTF-8. Surrogate pairs are not
                // needed by the manifest; preserve them as replacement chars.
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                if (cp < 0x80) {
                    out.push_back(char(cp));
                } else if (cp < 0x800) {
                    out.push_back(char(0xC0 | (cp >> 6)));
                    out.push_back(char(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(char(0xE0 | (cp >> 12)));
                    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(char(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: fail("invalid escape");
            }
        }
        fail("unterminated string");
    }

    Json parse_bool() {
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            Json v;
            v.type = Json::Type::Bool;
            v.boolean = true;
            return v;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            Json v;
            v.type = Json::Type::Bool;
            v.boolean = false;
            return v;
        }
        fail("invalid boolean");
    }

    Json parse_null() {
        if (text_.compare(pos_, 4, "null") != 0) fail("invalid null");
        pos_ += 4;
        return Json{};
    }

    Json parse_number() {
        const char *begin = text_.c_str() + pos_;
        char *end = nullptr;
        double value = std::strtod(begin, &end);
        if (end == begin) fail("invalid number");
        pos_ += static_cast<size_t>(end - begin);
        Json v;
        v.type = Json::Type::Number;
        v.number = value;
        return v;
    }
};

}  // namespace

const Json *Json::find(const std::string &key) const {
    if (type != Type::Object) return nullptr;
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

std::string Json::as_string(const std::string &fallback) const {
    return type == Type::String ? string : fallback;
}

uint64_t Json::as_u64(uint64_t fallback) const {
    if (type != Type::Number || number < 0.0) return fallback;
    return static_cast<uint64_t>(number);
}

double Json::as_double(double fallback) const {
    return type == Type::Number ? number : fallback;
}

bool Json::as_bool(bool fallback) const {
    return type == Type::Bool ? boolean : fallback;
}

Json parse_json(const std::string &text) {
    return Parser(text).parse();
}

}  // namespace bag2mkv
