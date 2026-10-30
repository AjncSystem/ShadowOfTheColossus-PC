// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: minimal JSON reader and writer for the SotC Lab routes (gpu/sotc/autopilot.cpp). Header-only,
// standard library only. Numbers are doubles; object members keep their order; a repeated key is kept
// and Get() returns the first one. Parse() rejects trailing text, trailing commas and raw control
// characters in strings, and reports the byte offset of the first error. Dump() writes two-space
// indented JSON; numbers are written with the fewest digits that read back as the same float.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace SotcJson {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type{Type::Null};
    bool boolean{};
    double number{};
    std::string str;
    std::vector<Value> items;       ///< Array elements, or the object member values
    std::vector<std::string> keys;  ///< Object member names: keys[i] names items[i]

    static Value MakeObject() {
        Value v;
        v.type = Type::Object;
        return v;
    }
    static Value MakeArray() {
        Value v;
        v.type = Type::Array;
        return v;
    }
    static Value MakeNumber(double n) {
        Value v;
        v.type = Type::Number;
        v.number = n;
        return v;
    }
    static Value MakeString(std::string s) {
        Value v;
        v.type = Type::String;
        v.str = std::move(s);
        return v;
    }
    static Value MakeBool(bool b) {
        Value v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    }

    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool IsBool() const { return type == Type::Bool; }

    /// Sets a member: replaces the first member with that key, else appends. Object only.
    void Set(const std::string& key, Value value) {
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                items[i] = std::move(value);
                return;
            }
        }
        keys.push_back(key);
        items.push_back(std::move(value));
    }

    /// Appends an element. Array only.
    void Push(Value value) { items.push_back(std::move(value)); }

    /// Member of an object, or nullptr when absent or when this is not an object.
    const Value* Get(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) return &items[i];
        }
        return nullptr;
    }
};

namespace detail {

constexpr int kMaxDepth = 128;

inline void AppendUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
        out->push_back(char(cp));
    } else if (cp < 0x800) {
        out->push_back(char(0xC0 | (cp >> 6)));
        out->push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(char(0xE0 | (cp >> 12)));
        out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(char(0xF0 | (cp >> 18)));
        out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(char(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool Run(Value* out, std::string* err) {
        SkipWs();
        if (!ParseValue(out, 0)) {
            if (err) *err = err_;
            return false;
        }
        SkipWs();
        if (pos_ != s_.size()) {
            Error("unexpected text after the value");
            if (err) *err = err_;
            return false;
        }
        return true;
    }

private:
    bool Error(const char* what) {
        if (err_.empty()) err_ = std::string(what) + " at byte " + std::to_string(pos_);
        return false;
    }

    char Peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }

    void SkipWs() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    bool Match(const char* word) {
        size_t n = 0;
        while (word[n] != '\0') ++n;
        if (s_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }

    bool ParseValue(Value* out, int depth) {
        if (pos_ >= s_.size()) return Error("unexpected end of input");
        const char c = s_[pos_];
        if (c == '{') return ParseObject(out, depth + 1);
        if (c == '[') return ParseArray(out, depth + 1);
        if (c == '"') {
            out->type = Value::Type::String;
            return ParseString(&out->str);
        }
        if (Match("true")) {
            out->type = Value::Type::Bool;
            out->boolean = true;
            return true;
        }
        if (Match("false")) {
            out->type = Value::Type::Bool;
            out->boolean = false;
            return true;
        }
        if (Match("null")) {
            out->type = Value::Type::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
        return Error("unexpected character");
    }

    bool ParseObject(Value* out, int depth) {
        if (depth > kMaxDepth) return Error("nesting too deep");
        out->type = Value::Type::Object;
        ++pos_;  // '{'
        SkipWs();
        if (Peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipWs();
            if (Peek() != '"') return Error("expected a member name");
            std::string key;
            if (!ParseString(&key)) return false;
            SkipWs();
            if (Peek() != ':') return Error("expected ':'");
            ++pos_;
            SkipWs();
            Value member;
            if (!ParseValue(&member, depth)) return false;
            out->keys.push_back(std::move(key));
            out->items.push_back(std::move(member));
            SkipWs();
            const char c = Peek();
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == '}') {
                ++pos_;
                return true;
            }
            return Error("expected ',' or '}'");
        }
    }

    bool ParseArray(Value* out, int depth) {
        if (depth > kMaxDepth) return Error("nesting too deep");
        out->type = Value::Type::Array;
        ++pos_;  // '['
        SkipWs();
        if (Peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipWs();
            Value item;
            if (!ParseValue(&item, depth)) return false;
            out->items.push_back(std::move(item));
            SkipWs();
            const char c = Peek();
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == ']') {
                ++pos_;
                return true;
            }
            return Error("expected ',' or ']'");
        }
    }

    bool Hex4(uint32_t* v) {
        if (pos_ + 4 > s_.size()) return false;
        uint32_t r = 0;
        for (size_t i = 0; i < 4; ++i) {
            const char c = s_[pos_ + i];
            uint32_t d = 0;
            if (c >= '0' && c <= '9') {
                d = uint32_t(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = uint32_t(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                d = uint32_t(c - 'A' + 10);
            } else {
                return false;
            }
            r = r * 16 + d;
        }
        pos_ += 4;
        *v = r;
        return true;
    }

    bool ParseString(std::string* out) {
        ++pos_;  // opening quote
        out->clear();
        while (pos_ < s_.size()) {
            const unsigned char c = (unsigned char)s_[pos_];
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) return Error("control character in string");
            if (c != '\\') {
                out->push_back(char(c));
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= s_.size()) break;
            const char e = s_[pos_++];
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!Hex4(&cp)) return Error("bad \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo = 0;
                        if (!Match("\\u") || !Hex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) {
                            return Error("unpaired surrogate");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return Error("unpaired surrogate");
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    return Error("bad escape in string");
            }
        }
        return Error("unterminated string");
    }

    bool Digits() {
        const size_t from = pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        return pos_ > from;
    }

    bool ParseNumber(Value* out) {
        const size_t start = pos_;
        if (Peek() == '-') ++pos_;
        if (!Digits()) return Error("bad number");
        if (Peek() == '.') {
            ++pos_;
            if (!Digits()) return Error("bad number");
        }
        if (Peek() == 'e' || Peek() == 'E') {
            ++pos_;
            if (Peek() == '+' || Peek() == '-') ++pos_;
            if (!Digits()) return Error("bad number");
        }
        const double v = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        if (!std::isfinite(v)) return Error("number out of range");
        out->type = Value::Type::Number;
        out->number = v;
        return true;
    }

    const std::string& s_;
    size_t pos_{};
    std::string err_;
};

inline void WriteString(const std::string& s, std::string* out) {
    out->push_back('"');
    for (const char ch : s) {
        const unsigned char c = (unsigned char)ch;
        switch (c) {
            case '"': out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '\n': out->append("\\n"); break;
            case '\r': out->append("\\r"); break;
            case '\t': out->append("\\t"); break;
            case '\b': out->append("\\b"); break;
            case '\f': out->append("\\f"); break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", unsigned(c));
                    out->append(buf);
                } else {
                    out->push_back(ch);
                }
        }
    }
    out->push_back('"');
}

inline void WriteNumber(double n, std::string* out) {
    if (!std::isfinite(n)) {
        out->push_back('0');
        return;
    }
    char buf[40];
    for (int prec = 1; prec <= 9; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, n);
        if (float(std::strtod(buf, nullptr)) == float(n)) break;
    }
    out->append(buf);
}

inline void WriteValue(const Value& v, int indent, std::string* out) {
    const std::string pad(size_t(indent) * 2, ' ');
    const std::string inner(size_t(indent + 1) * 2, ' ');
    switch (v.type) {
        case Value::Type::Null: out->append("null"); break;
        case Value::Type::Bool: out->append(v.boolean ? "true" : "false"); break;
        case Value::Type::Number: WriteNumber(v.number, out); break;
        case Value::Type::String: WriteString(v.str, out); break;
        case Value::Type::Array:
            if (v.items.empty()) {
                out->append("[]");
                break;
            }
            out->append("[\n");
            for (size_t i = 0; i < v.items.size(); ++i) {
                out->append(inner);
                WriteValue(v.items[i], indent + 1, out);
                out->append(i + 1 < v.items.size() ? ",\n" : "\n");
            }
            out->append(pad).append("]");
            break;
        case Value::Type::Object:
            if (v.items.empty()) {
                out->append("{}");
                break;
            }
            out->append("{\n");
            for (size_t i = 0; i < v.items.size(); ++i) {
                out->append(inner);
                WriteString(v.keys[i], out);
                out->append(": ");
                WriteValue(v.items[i], indent + 1, out);
                out->append(i + 1 < v.items.size() ? ",\n" : "\n");
            }
            out->append(pad).append("}");
            break;
    }
}

} // namespace detail

/// Parses one JSON document. On failure returns false and, when err is set, the reason and offset.
inline bool Parse(const std::string& text, Value* out, std::string* err = nullptr) {
    *out = Value{};
    detail::Parser parser(text);
    return parser.Run(out, err);
}

/// Pretty-printed JSON text (no trailing newline).
inline std::string Dump(const Value& v) {
    std::string out;
    detail::WriteValue(v, 0, &out);
    return out;
}

} // namespace SotcJson
