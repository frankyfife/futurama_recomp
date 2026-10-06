// Minimal strict JSON reader for the tests of the tool modes (tests/cli_tests.cpp, tests/schema_tests.cpp).
// RFC 8259 grammar; rejects trailing data, duplicate object keys, control characters in strings and invalid
// UTF-8 (the launcher's QJsonDocument would reject or mangle such documents).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace testjson {

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;      // String; Number: the literal as written
    std::vector<Value> items;                            // Array
    std::vector<std::pair<std::string, Value>> members;  // Object (document order)

    bool IsNull() const { return kind == Kind::Null; }
    bool IsBool() const { return kind == Kind::Bool; }
    bool IsNumber() const { return kind == Kind::Number; }
    bool IsString() const { return kind == Kind::String; }
    bool IsArray() const { return kind == Kind::Array; }
    bool IsObject() const { return kind == Kind::Object; }
    bool IsInteger() const { return kind == Kind::Number && text.find_first_of(".eE") == std::string::npos; }

    const Value* Find(std::string_view key) const {
        for (const auto& [k, v] : members) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
    bool Has(std::string_view key) const { return Find(key) != nullptr; }
    // Missing members / indices give a shared null value.
    const Value& operator[](std::string_view key) const {
        const Value* v = Find(key);
        return v != nullptr ? *v : Null();
    }
    const Value& operator[](std::size_t index) const { return index < items.size() ? items[index] : Null(); }
    std::size_t size() const { return kind == Kind::Array ? items.size() : members.size(); }

    static const Value& Null() {
        static const Value null;
        return null;
    }
};

namespace detail {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    bool Document(Value& out, std::string& error) {
        SkipBlanks();
        if (!ParseValue(out, 0)) {
            error = error_ + " at byte " + std::to_string(pos_);
            return false;
        }
        SkipBlanks();
        if (pos_ != s_.size()) {
            error = "trailing data at byte " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    bool Fail(const char* what) {
        if (error_.empty()) {
            error_ = what;
        }
        return false;
    }
    void SkipBlanks() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }
    bool Literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) {
            return Fail("bad literal");
        }
        pos_ += word.size();
        return true;
    }
    bool ParseValue(Value& v, int depth) {
        if (depth > 64) {
            return Fail("nesting too deep");
        }
        if (pos_ >= s_.size()) {
            return Fail("unexpected end");
        }
        const char c = s_[pos_];
        if (c == '{') {
            return ParseObject(v, depth);
        }
        if (c == '[') {
            return ParseArray(v, depth);
        }
        if (c == '"') {
            v.kind = Value::Kind::String;
            return ParseString(v.text);
        }
        if (c == 't') {
            v.kind = Value::Kind::Bool;
            v.boolean = true;
            return Literal("true");
        }
        if (c == 'f') {
            v.kind = Value::Kind::Bool;
            v.boolean = false;
            return Literal("false");
        }
        if (c == 'n') {
            v.kind = Value::Kind::Null;
            return Literal("null");
        }
        return ParseNumber(v);
    }
    bool ParseObject(Value& v, int depth) {
        v.kind = Value::Kind::Object;
        ++pos_;
        SkipBlanks();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipBlanks();
            std::string key;
            if (pos_ >= s_.size() || s_[pos_] != '"' || !ParseString(key)) {
                return Fail("expected a member name");
            }
            if (v.Has(key)) {
                return Fail("duplicate member name");
            }
            SkipBlanks();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                return Fail("expected ':'");
            }
            ++pos_;
            SkipBlanks();
            Value member;
            if (!ParseValue(member, depth + 1)) {
                return false;
            }
            v.members.emplace_back(std::move(key), std::move(member));
            SkipBlanks();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }
    bool ParseArray(Value& v, int depth) {
        v.kind = Value::Kind::Array;
        ++pos_;
        SkipBlanks();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipBlanks();
            Value item;
            if (!ParseValue(item, depth + 1)) {
                return false;
            }
            v.items.push_back(std::move(item));
            SkipBlanks();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }
    static void AppendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool Hex4(std::uint32_t& out) {
        if (pos_ + 4 > s_.size()) {
            return Fail("short \\u escape");
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return Fail("bad \\u escape");
            }
        }
        return true;
    }
    // Validates one UTF-8 sequence starting at pos_ and appends it.
    bool Utf8Sequence(std::string& out) {
        const auto b0 = static_cast<unsigned char>(s_[pos_]);
        int length = 0;
        std::uint32_t cp = 0;
        if (b0 >= 0xC2 && b0 <= 0xDF) {
            length = 2;
            cp = b0 & 0x1F;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            length = 3;
            cp = b0 & 0x0F;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            length = 4;
            cp = b0 & 0x07;
        } else {
            return Fail("invalid UTF-8 lead byte");
        }
        if (pos_ + static_cast<std::size_t>(length) > s_.size()) {
            return Fail("truncated UTF-8 sequence");
        }
        for (int i = 1; i < length; ++i) {
            const auto b = static_cast<unsigned char>(s_[pos_ + static_cast<std::size_t>(i)]);
            if ((b & 0xC0) != 0x80) {
                return Fail("invalid UTF-8 continuation byte");
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        if ((length == 3 && cp < 0x800) || (length == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return Fail("overlong or invalid UTF-8 code point");
        }
        out.append(s_.substr(pos_, static_cast<std::size_t>(length)));
        pos_ += static_cast<std::size_t>(length);
        return true;
    }
    bool ParseString(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        while (pos_ < s_.size()) {
            const auto c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) {
                return Fail("control character in string");
            }
            if (c >= 0x80) {
                if (!Utf8Sequence(out)) {
                    return false;
                }
                continue;
            }
            if (c != '\\') {
                out += static_cast<char>(c);
                ++pos_;
                continue;
            }
            if (++pos_ >= s_.size()) {
                return Fail("unterminated escape");
            }
            const char e = s_[pos_++];
            switch (e) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!Hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    std::uint32_t low = 0;
                    if (s_.substr(pos_, 2) != "\\u") {
                        return Fail("unpaired surrogate");
                    }
                    pos_ += 2;
                    if (!Hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        return Fail("bad low surrogate");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return Fail("unpaired low surrogate");
                }
                AppendUtf8(out, cp);
                break;
            }
            default:
                return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }
    bool ParseNumber(Value& v) {
        const std::size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') {
            ++pos_;
        }
        auto digits = [&] {
            const std::size_t d = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                ++pos_;
            }
            return pos_ - d;
        };
        const std::size_t intStart = pos_;
        if (digits() == 0) {
            return Fail("expected a value");
        }
        if (s_[intStart] == '0' && pos_ - intStart > 1) {
            return Fail("leading zero");
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (digits() == 0) {
                return Fail("digits expected after '.'");
            }
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
                ++pos_;
            }
            if (digits() == 0) {
                return Fail("digits expected in the exponent");
            }
        }
        v.kind = Value::Kind::Number;
        v.text = std::string(s_.substr(start, pos_ - start));
        v.number = std::strtod(v.text.c_str(), nullptr);
        return true;
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    std::string error_;
};

}  // namespace detail

inline bool Parse(std::string_view text, Value& out, std::string& error) {
    out = Value{};
    return detail::Parser(text).Document(out, error);
}

}  // namespace testjson
