// The small JSON parser (recursive descent with a depth limit) and writer.
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

/**
 * A small parser that tracks its position while parsing.
 */
class Parser {
public:
    /**
     * @param source the string to parse
     */
    explicit Parser(const std::string& source) : src_(source) {}

    /**
     * Parse the whole document.
     * @param out destination for the result
     * @param error reason for failure
     * @return true on success
     */
    bool parseDocument(JsonValue& out, std::string& error) {
        skipSpace();
        if (!parseValue(out, 0)) {
            error = error_ + " (near character " + std::to_string(pos_) + ")";
            return false;
        }
        skipSpace();
        if (pos_ != src_.size()) {
            error = "unexpected trailing characters after the value (near character " + std::to_string(pos_) + ")";
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 32;

    const std::string& src_;
    size_t pos_ = 0;
    std::string error_;

    /**
     * Record a failure reason.
     * @param message the reason
     * @return always false (so callers can write `return fail(...)`)
     */
    bool fail(const std::string& message) {
        if (error_.empty()) error_ = message;
        return false;
    }

    /** Skip whitespace and newlines. */
    void skipSpace() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    /**
     * Check that the given string follows exactly and consume it.
     * @param word the expected string
     * @return true if it matched
     */
    bool consumeWord(const char* word) {
        size_t i = 0;
        while (word[i] != '\0') {
            if (pos_ + i >= src_.size() || src_[pos_ + i] != word[i]) return false;
            ++i;
        }
        pos_ += i;
        return true;
    }

    /**
     * Parse a single value.
     * @param out destination for the result
     * @param depth current nesting depth
     * @return true on success
     */
    bool parseValue(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return fail("nesting is too deep");
        skipSpace();
        if (pos_ >= src_.size()) return fail("expected a value");
        const char c = src_[pos_];
        if (c == '{') return parseObject(out, depth);
        if (c == '[') return parseArray(out, depth);
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return parseString(out.text);
        }
        if (consumeWord("true")) {
            out.type = JsonValue::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (consumeWord("false")) {
            out.type = JsonValue::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (consumeWord("null")) {
            out.type = JsonValue::Type::Null;
            return true;
        }
        return parseNumber(out);
    }

    /**
     * Parse a number.
     * @param out destination for the result
     * @return true on success
     */
    bool parseNumber(JsonValue& out) {
        const size_t start = pos_;
        if (pos_ < src_.size() && (src_[pos_] == '-' || src_[pos_] == '+')) ++pos_;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            const bool numberChar = (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+';
            if (!numberChar) break;
            ++pos_;
        }
        if (pos_ == start) return fail("unexpected character");
        const std::string token = src_.substr(start, pos_ - start);
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0') return fail("malformed number: " + token);
        out.type = JsonValue::Type::Number;
        out.number = value;
        out.integer = token.find_first_of(".eE") == std::string::npos;
        return true;
    }

    /**
     * Read 4 hex digits (for \uXXXX).
     * @param code destination for the value read
     * @return true on success
     */
    bool parseHex4(unsigned& code) {
        if (pos_ + 4 > src_.size()) return fail("too few characters after \\u");
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = src_[pos_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("expected hex digits after \\u");
        }
        return true;
    }

    /**
     * Append a code point to a string as UTF-8.
     * @param code the code point
     * @param out destination to append to
     */
    static void appendUtf8(unsigned code, std::string& out) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    /**
     * Parse a string (starting at the opening ").
     * @param out destination for the result
     * @return true on success
     */
    bool parseString(std::string& out) {
        ++pos_;  // opening "
        out.clear();
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= src_.size()) break;
            const char e = src_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned code = 0;
                    if (!parseHex4(code)) return false;
                    // surrogate pair
                    if (code >= 0xD800 && code <= 0xDBFF && pos_ + 6 <= src_.size() && src_[pos_] == '\\' &&
                        src_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned low = 0;
                        if (!parseHex4(low)) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(code, out);
                    break;
                }
                default: return fail("invalid escape sequence in string");
            }
        }
        return fail("unterminated string");
    }

    /**
     * Parse an array (starting at the opening [).
     * @param out destination for the result
     * @param depth current nesting depth
     * @return true on success
     */
    bool parseArray(JsonValue& out, int depth) {
        ++pos_;  // [
        out.type = JsonValue::Type::Array;
        skipSpace();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue item;
            if (!parseValue(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            skipSpace();
            if (pos_ >= src_.size()) return fail("unterminated array");
            const char c = src_[pos_++];
            if (c == ']') return true;
            if (c != ',') return fail("expected , or ] between array elements");
        }
    }

    /**
     * Parse an object (starting at the opening {).
     * @param out destination for the result
     * @param depth current nesting depth
     * @return true on success
     */
    bool parseObject(JsonValue& out, int depth) {
        ++pos_;  // {
        out.type = JsonValue::Type::Object;
        skipSpace();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skipSpace();
            if (pos_ >= src_.size() || src_[pos_] != '"') return fail("expected a \"...\" key");
            std::string key;
            if (!parseString(key)) return false;
            skipSpace();
            if (pos_ >= src_.size() || src_[pos_] != ':') return fail("expected : after key");
            ++pos_;
            JsonValue value;
            if (!parseValue(value, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(value));
            skipSpace();
            if (pos_ >= src_.size()) return fail("unterminated object");
            const char c = src_[pos_++];
            if (c == '}') return true;
            if (c != ',') return fail("expected , or } between members");
        }
    }
};

}  // namespace

const JsonValue* JsonValue::get(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& member : members) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

std::string validUtf8(const std::string& text) {
    static const char kReplacement[] = "\xEF\xBF\xBD";
    std::string out;
    out.reserve(text.size());
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = byte(i);
        size_t length = 0;
        unsigned int code = 0;
        if (lead < 0x80) {
            length = 1;
            code = lead;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            code = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            code = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            code = lead & 0x07;
        }
        bool ok = length > 0 && i + length <= text.size();
        for (size_t k = 1; ok && k < length; ++k) {
            ok = (byte(i + k) & 0xC0) == 0x80;
            code = (code << 6) | (byte(i + k) & 0x3F);
        }
        // Shortest form only, no surrogates, nothing past U+10FFFF
        if (ok && length == 3) ok = code >= 0x800 && (code < 0xD800 || code > 0xDFFF);
        if (ok && length == 4) ok = code >= 0x10000 && code <= 0x10FFFF;
        if (ok) {
            out.append(text, i, length);
            i += length;
        } else {
            out += kReplacement;
            ++i;
        }
    }
    return out;
}

size_t utf8Prefix(const std::string& text, size_t max) {
    if (max >= text.size()) return text.size();
    size_t end = max;
    // Back over continuation bytes to the start of the character the cut would split
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return end;
}

bool parseJson(const std::string& source, JsonValue& out, std::string& error) {
    out = JsonValue();
    Parser parser(source);
    return parser.parseDocument(out, error);
}

void JsonValue::set(const std::string& key, JsonValue value) {
    if (type != Type::Object) {
        *this = JsonValue();
        type = Type::Object;
    }
    for (auto& member : members) {
        if (member.first == key) {
            member.second = std::move(value);
            return;
        }
    }
    members.emplace_back(key, std::move(value));
}

JsonValue JsonValue::makeNumber(double value, bool isInteger) {
    JsonValue v;
    v.type = Type::Number;
    v.number = value;
    v.integer = isInteger;
    return v;
}

JsonValue JsonValue::makeBool(bool value) {
    JsonValue v;
    v.type = Type::Bool;
    v.boolean = value;
    return v;
}

JsonValue JsonValue::makeString(const std::string& value) {
    JsonValue v;
    v.type = Type::String;
    v.text = value;
    return v;
}

namespace {

/**
 * Append a string as a JSON string literal (with quotes and escapes).
 * @param text the UTF-8 string
 * @param out where to append
 */
void writeString(const std::string& text, std::string& out) {
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                    out += escaped;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

/**
 * Append a number. Integers are written as integers; other numbers with up to 6 significant digits and always
 * with a decimal point, so that readers expecting a float see one.
 * @param value the value
 * @param out where to append
 */
void writeNumber(const JsonValue& value, std::string& out) {
    if (!std::isfinite(value.number)) {
        out += "null";  // JSON has no NaN or infinity
        return;
    }
    char text[64];
    if (value.integer && std::fabs(value.number) < 1e15) {
        std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(std::llround(value.number)));
        out += text;
        return;
    }
    std::snprintf(text, sizeof(text), "%.6g", value.number);
    out += text;
    if (std::string(text).find_first_of(".eE") == std::string::npos) out += ".0";
}

/**
 * Append a value with the given indentation depth.
 * @param value the value
 * @param depth nesting depth (2 spaces each)
 * @param out where to append
 */
void writeValue(const JsonValue& value, int depth, std::string& out) {
    const std::string pad(static_cast<size_t>(depth + 1) * 2, ' ');
    const std::string closePad(static_cast<size_t>(depth) * 2, ' ');
    switch (value.type) {
        case JsonValue::Type::Null: out += "null"; return;
        case JsonValue::Type::Bool: out += value.boolean ? "true" : "false"; return;
        case JsonValue::Type::Number: writeNumber(value, out); return;
        case JsonValue::Type::String: writeString(value.text, out); return;
        case JsonValue::Type::Array:
            if (value.items.empty()) {
                out += "[]";
                return;
            }
            out += "[\n";
            for (size_t i = 0; i < value.items.size(); ++i) {
                out += pad;
                writeValue(value.items[i], depth + 1, out);
                out += i + 1 < value.items.size() ? ",\n" : "\n";
            }
            out += closePad + "]";
            return;
        case JsonValue::Type::Object:
            if (value.members.empty()) {
                out += "{}";
                return;
            }
            out += "{\n";
            for (size_t i = 0; i < value.members.size(); ++i) {
                out += pad;
                writeString(value.members[i].first, out);
                out += ": ";
                writeValue(value.members[i].second, depth + 1, out);
                out += i + 1 < value.members.size() ? ",\n" : "\n";
            }
            out += closePad + "}";
            return;
    }
}

}  // namespace

std::string writeJson(const JsonValue& value) {
    std::string out;
    writeValue(value, 0, out);
    out += "\n";
    return out;
}
