#include "json_mini.hpp"

#include <cctype>
#include <cstdio>
#include <sstream>

namespace dirlocker {

namespace {

void escape_string(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

void dump_impl(const JsonValue& v, int indent, int depth, std::string& out) {
    auto add_indent = [&](int d) {
        if (indent >= 0) {
            out.push_back('\n');
            out.append(static_cast<size_t>(indent * d), ' ');
        }
    };

    switch (v.type()) {
        case JsonValue::Type::Null:
            out += "null";
            break;
        case JsonValue::Type::Bool:
            out += (v.as_bool() ? "true" : "false");
            break;
        case JsonValue::Type::Int:
            out += std::to_string(v.as_int());
            break;
        case JsonValue::Type::String:
            escape_string(v.as_string(), out);
            break;
        case JsonValue::Type::Array: {
            const auto& arr = v.as_array();
            if (arr.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i > 0) out.push_back(',');
                add_indent(depth + 1);
                dump_impl(arr[i], indent, depth + 1, out);
            }
            add_indent(depth);
            out.push_back(']');
            break;
        }
        case JsonValue::Type::Object: {
            const auto& obj = v.as_object();
            if (obj.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            bool first = true;
            for (const auto& kv : obj) {
                if (!first) out.push_back(',');
                first = false;
                add_indent(depth + 1);
                escape_string(kv.first, out);
                out.push_back(':');
                if (indent >= 0) out.push_back(' ');
                dump_impl(kv.second, indent, depth + 1, out);
            }
            add_indent(depth);
            out.push_back('}');
            break;
        }
    }
}

class Parser {
public:
    explicit Parser(const std::string& in) : in_(in) {}

    bool parse(JsonValue& out, std::string* err) {
        skip_ws();
        if (!parse_value(out, 0)) {
            if (err) *err = err_;
            return false;
        }
        skip_ws();
        if (pos_ != in_.size()) {
            set_err("Trailing characters after JSON value");
            if (err) *err = err_;
            return false;
        }
        return true;
    }

private:
    void skip_ws() {
        while (pos_ < in_.size()) {
            char c = in_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool set_err(const std::string& msg) {
        err_ = msg + " at offset " + std::to_string(pos_);
        return false;
    }

    bool parse_value(JsonValue& out, int depth) {
        if (depth > 64) return set_err("Maximum JSON depth exceeded");
        skip_ws();
        if (pos_ >= in_.size()) return set_err("Unexpected end of input");

        char c = in_[pos_];
        if (c == '{') return parse_object(out, depth + 1);
        if (c == '[') return parse_array(out, depth + 1);
        if (c == '"') {
            std::string s;
            if (!parse_string(s)) return false;
            out = JsonValue(std::move(s));
            return true;
        }
        if (c == 't') return parse_literal("true", JsonValue(true), out);
        if (c == 'f') return parse_literal("false", JsonValue(false), out);
        if (c == 'n') return parse_literal("null", JsonValue(nullptr), out);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
        return set_err("Unexpected token");
    }

    bool parse_literal(const char* lit, JsonValue val, JsonValue& out) {
        size_t len = std::char_traits<char>::length(lit);
        if (in_.compare(pos_, len, lit) != 0) return set_err("Invalid literal");
        pos_ += len;
        out = std::move(val);
        return true;
    }

    bool parse_number(JsonValue& out) {
        size_t start = pos_;
        if (in_[pos_] == '-') ++pos_;
        if (pos_ >= in_.size() || !std::isdigit(static_cast<unsigned char>(in_[pos_]))) {
            return set_err("Invalid number");
        }
        while (pos_ < in_.size() && std::isdigit(static_cast<unsigned char>(in_[pos_]))) {
            ++pos_;
        }
        if (pos_ < in_.size() && (in_[pos_] == '.' || in_[pos_] == 'e' || in_[pos_] == 'E')) {
            return set_err("Floating point numbers are not supported");
        }
        try {
            size_t idx = 0;
            std::string sub = in_.substr(start, pos_ - start);
            int64_t val = std::stoll(sub, &idx, 10);
            if (idx != sub.size()) return set_err("Malformed integer");
            out = JsonValue(val);
            return true;
        } catch (...) {
            return set_err("Integer out of range");
        }
    }

    static void append_utf8(uint32_t cp, std::string& out) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_hex4(uint32_t& code) {
        if (pos_ + 4 > in_.size()) return set_err("Truncated \\u escape");
        code = 0;
        for (int i = 0; i < 4; ++i) {
            char c = in_[pos_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<uint32_t>(c - 'A' + 10);
            else return set_err("Invalid hex digit in \\u escape");
        }
        return true;
    }

    bool parse_string(std::string& out) {
        if (pos_ >= in_.size() || in_[pos_] != '"') return set_err("Expected '\"'");
        ++pos_;
        out.clear();
        while (pos_ < in_.size()) {
            unsigned char c = static_cast<unsigned char>(in_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return set_err("Unescaped control char in string");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= in_.size()) return set_err("Unterminated escape");
            char esc = in_[pos_++];
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
                    uint32_t cp = 0;
                    if (!parse_hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 2 <= in_.size() && in_[pos_] == '\\' && in_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t low = 0;
                            if (!parse_hex4(low)) return false;
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                            } else {
                                return set_err("Invalid low surrogate");
                            }
                        } else {
                            return set_err("Unpaired high surrogate");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return set_err("Unexpected low surrogate");
                    }
                    append_utf8(cp, out);
                    break;
                }
                default:
                    return set_err("Invalid escape sequence");
            }
        }
        return set_err("Unterminated string");
    }

    bool parse_array(JsonValue& out, int depth) {
        ++pos_;  // '['
        JsonValue::Array arr;
        skip_ws();
        if (pos_ < in_.size() && in_[pos_] == ']') {
            ++pos_;
            out = JsonValue(std::move(arr));
            return true;
        }
        while (true) {
            JsonValue elem;
            if (!parse_value(elem, depth)) return false;
            arr.push_back(std::move(elem));
            skip_ws();
            if (pos_ >= in_.size()) return set_err("Unterminated array");
            if (in_[pos_] == ']') {
                ++pos_;
                out = JsonValue(std::move(arr));
                return true;
            }
            if (in_[pos_] != ',') return set_err("Expected ',' or ']' in array");
            ++pos_;
        }
    }

    bool parse_object(JsonValue& out, int depth) {
        ++pos_;  // '{'
        JsonValue::Object obj;
        skip_ws();
        if (pos_ < in_.size() && in_[pos_] == '}') {
            ++pos_;
            out = JsonValue(std::move(obj));
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos_ >= in_.size() || in_[pos_] != ':') return set_err("Expected ':'");
            ++pos_;
            JsonValue val;
            if (!parse_value(val, depth)) return false;
            obj[std::move(key)] = std::move(val);
            skip_ws();
            if (pos_ >= in_.size()) return set_err("Unterminated object");
            if (in_[pos_] == '}') {
                ++pos_;
                out = JsonValue(std::move(obj));
                return true;
            }
            if (in_[pos_] != ',') return set_err("Expected ',' or '}' in object");
            ++pos_;
        }
    }

    const std::string& in_;
    size_t pos_ = 0;
    std::string err_;
};

}  // namespace

std::string JsonValue::dump(int indent) const {
    std::string out;
    dump_impl(*this, indent, 0, out);
    return out;
}

bool JsonValue::parse(const std::string& input, JsonValue& out, std::string* err) {
    Parser p(input);
    return p.parse(out, err);
}

}  // namespace dirlocker

