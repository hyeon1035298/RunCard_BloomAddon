// SPDX-License-Identifier: GPL-3.0-only
//
// Run card's transcript reader: a small JSON parser for one transcript line (UTF-8 in), as the JavaScript
// prototype's tail.mjs reads it (`JSON.parse(buf.toString('utf8').trim())`). One Doc holds one line: a flat array
// of nodes and one buffer for the strings, so a line costs a few allocations however big it is.
//
// It never throws on bad input: Parse() returns false and the line is skipped, as the prototype skips a line
// JSON.parse refuses. A string longer than kMaxString keeps its first kMaxString bytes (cut on a character
// boundary); nothing the board reads comes near that (lot-map cuts labels at 120 characters), and tool results,
// the long strings of a transcript, are only tested for their shape.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <locale.h>
#include <string>
#include <string_view>
#include <vector>

namespace si {
namespace json {

// Numbers read and written in the "C" locale whatever the process has set. The C runtime is the shared
// msvcrt.dll, so any other module in the process may call setlocale and turn the decimal point into a comma
// for plain strtod and snprintf; the _l forms take this locale, made once, instead.
inline _locale_t CLocale() {
    static const _locale_t c = _create_locale(LC_NUMERIC, "C");
    return c;
}
inline double ParseNumber(const char* s) {
    return _strtod_l(s, nullptr, CLocale());
}
// snprintf in the "C" locale, always terminated (_snprintf_l is not when the text does not fit).
template <class... A>
inline void FormatNumbers(char* out, size_t size, const char* format, A... args) {
    int n = _snprintf_l(out, size, format, CLocale(), args...);
    if (n < 0 || size_t(n) >= size) out[size - 1] = '\0';
}

enum class Type : uint8_t { Null, False, True, Number, String, Array, Object };

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr size_t kMaxString = 64 * 1024;  // bytes of one string value kept
constexpr int kMaxDepth = 128;            // deeper nesting: the line is skipped

struct Node {
    Type type = Type::Null;
    uint32_t key = 0, keyLen = 0;  // the member name in its object (into Doc::text)
    uint32_t str = 0, len = 0;     // a string's bytes (into Doc::text)
    double num = 0;
    uint32_t first = kNone, next = kNone;  // children of an array or object, in order
};

class Doc;

// A node of a Doc, or nothing (a missing member reads as nothing, like `undefined`).
class Val {
public:
    Val() = default;
    Val(const Doc* d, uint32_t i) : d_(d), i_(i) {}

    bool Exists() const { return d_ != nullptr && i_ != kNone; }
    Type type() const;
    bool IsNull() const { return Exists() && type() == Type::Null; }
    bool IsBool() const { return Exists() && (type() == Type::True || type() == Type::False); }
    bool IsTrue() const { return Exists() && type() == Type::True; }
    bool IsNumber() const { return Exists() && type() == Type::Number; }
    bool IsString() const { return Exists() && type() == Type::String; }
    bool IsArray() const { return Exists() && type() == Type::Array; }
    bool IsObject() const { return Exists() && type() == Type::Object; }
    // JS `typeof v === 'object' && v !== null` (arrays too).
    bool IsObjectLike() const { return IsArray() || IsObject(); }
    double Num() const;
    std::string_view Str() const;  // empty unless a string
    // A member by name; with a name given twice the last one counts, as with JSON.parse. Nothing unless an object.
    Val operator[](std::string_view key) const;
    // Children in order (array items, or object members).
    Val First() const;
    Val Next() const;
    std::string_view Key() const;  // the member name of a child of an object
    size_t Count() const;

private:
    const Doc* d_ = nullptr;
    uint32_t i_ = kNone;
};

class Doc {
public:
    // Parses one line; false (and an empty doc) when it is not JSON or nests deeper than kMaxDepth.
    bool Parse(std::string_view text) {
        nodes_.clear();
        text_.clear();
        p_ = text.data();
        end_ = text.data() + text.size();
        bad_ = false;
        uint32_t root = Value(0);
        Space();
        if (bad_ || root == kNone || p_ != end_) {
            nodes_.clear();
            text_.clear();
            return false;
        }
        return true;
    }
    Val Root() const { return nodes_.empty() ? Val() : Val(this, 0); }

    const Node& node(uint32_t i) const { return nodes_[i]; }
    std::string_view slice(uint32_t at, uint32_t n) const { return std::string_view(text_).substr(at, n); }

private:
    std::vector<Node> nodes_;
    std::string text_;
    const char* p_ = nullptr;
    const char* end_ = nullptr;
    bool bad_ = false;

    void Space() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) p_++;
    }
    uint32_t Fail() {
        bad_ = true;
        return kNone;
    }
    uint32_t Add(Type t) {
        nodes_.push_back(Node{});
        nodes_.back().type = t;
        return uint32_t(nodes_.size() - 1);
    }
    bool Literal(const char* word, size_t n) {
        if (size_t(end_ - p_) < n || std::string_view(p_, n) != std::string_view(word, n)) return false;
        p_ += n;
        return true;
    }

    uint32_t Value(int depth) {
        if (depth > kMaxDepth) return Fail();
        Space();
        if (p_ >= end_) return Fail();
        switch (*p_) {
            case '{': return ObjectValue(depth);
            case '[': return ArrayValue(depth);
            case '"': {
                uint32_t i = Add(Type::String);
                uint32_t at = 0, n = 0;
                if (!String(at, n)) return Fail();
                nodes_[i].str = at;
                nodes_[i].len = n;
                return i;
            }
            case 't': return Literal("true", 4) ? Add(Type::True) : Fail();
            case 'f': return Literal("false", 5) ? Add(Type::False) : Fail();
            case 'n': return Literal("null", 4) ? Add(Type::Null) : Fail();
            default: return NumberValue();
        }
    }

    uint32_t NumberValue() {
        const char* s = p_;
        if (p_ < end_ && *p_ == '-') p_++;
        if (p_ >= end_) return Fail();
        if (*p_ == '0') {
            p_++;
        } else if (*p_ >= '1' && *p_ <= '9') {
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
        } else {
            return Fail();
        }
        if (p_ < end_ && *p_ == '.') {
            p_++;
            if (p_ >= end_ || *p_ < '0' || *p_ > '9') return Fail();
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            p_++;
            if (p_ < end_ && (*p_ == '+' || *p_ == '-')) p_++;
            if (p_ >= end_ || *p_ < '0' || *p_ > '9') return Fail();
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
        }
        // strtod in the "C" locale (ParseNumber); 1e999 is Infinity as with JSON.parse.
        std::string token(s, size_t(p_ - s));
        uint32_t i = Add(Type::Number);
        nodes_[i].num = ParseNumber(token.c_str());
        return i;
    }

    static int Hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool Hex4(uint32_t& out) {
        if (end_ - p_ < 4) return false;
        out = 0;
        for (int k = 0; k < 4; k++) {
            int h = Hex(p_[k]);
            if (h < 0) return false;
            out = out * 16 + uint32_t(h);
        }
        p_ += 4;
        return true;
    }

    // Appends while under the cap; past it the rest of the string is only checked.
    void Put(const char* s, size_t n, size_t start, bool& full) {
        if (full) return;
        size_t room = kMaxString - (text_.size() - start);
        if (n <= room) {
            text_.append(s, n);
            return;
        }
        text_.append(s, room);
        // Never end inside a UTF-8 sequence: drop a last sequence that lost its tail.
        size_t endp = text_.size();
        size_t lead = endp;
        for (int k = 0; lead > start && k < 3 && (uint8_t(text_[lead - 1]) & 0xC0) == 0x80; k++) lead--;
        if (lead > start) {
            uint8_t b = uint8_t(text_[lead - 1]);
            size_t need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
            if (lead - 1 + need > endp) text_.resize(lead - 1);
        }
        full = true;
    }
    void PutCode(uint32_t c, size_t start, bool& full) {
        char b[4];
        size_t n;
        if (c < 0x80) {
            b[0] = char(c);
            n = 1;
        } else if (c < 0x800) {
            b[0] = char(0xC0 | (c >> 6));
            b[1] = char(0x80 | (c & 0x3F));
            n = 2;
        } else if (c < 0x10000) {
            b[0] = char(0xE0 | (c >> 12));
            b[1] = char(0x80 | ((c >> 6) & 0x3F));
            b[2] = char(0x80 | (c & 0x3F));
            n = 3;
        } else {
            b[0] = char(0xF0 | (c >> 18));
            b[1] = char(0x80 | ((c >> 12) & 0x3F));
            b[2] = char(0x80 | ((c >> 6) & 0x3F));
            b[3] = char(0x80 | (c & 0x3F));
            n = 4;
        }
        if (!full && kMaxString - (text_.size() - start) < n) {
            full = true;
            return;
        }
        Put(b, n, start, full);
    }

    // A string at p_ (on its opening quote), unescaped into text_.
    bool String(uint32_t& at, uint32_t& n) {
        p_++;
        size_t start = text_.size();
        bool full = false;
        for (;;) {
            const char* run = p_;
            while (p_ < end_ && *p_ != '"' && *p_ != '\\' && uint8_t(*p_) >= 0x20) p_++;
            Put(run, size_t(p_ - run), start, full);
            if (p_ >= end_) return false;
            char c = *p_;
            if (c == '"') {
                p_++;
                break;
            }
            if (uint8_t(c) < 0x20) return false;  // a raw control character: JSON.parse refuses it
            p_++;  // the backslash
            if (p_ >= end_) return false;
            char e = *p_++;
            switch (e) {
                case '"': Put("\"", 1, start, full); break;
                case '\\': Put("\\", 1, start, full); break;
                case '/': Put("/", 1, start, full); break;
                case 'b': Put("\b", 1, start, full); break;
                case 'f': Put("\f", 1, start, full); break;
                case 'n': Put("\n", 1, start, full); break;
                case 'r': Put("\r", 1, start, full); break;
                case 't': Put("\t", 1, start, full); break;
                case 'u': {
                    uint32_t u;
                    if (!Hex4(u)) return false;
                    if (u >= 0xD800 && u <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        const char* save = p_;
                        p_ += 2;
                        uint32_t lo;
                        if (!Hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            PutCode(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00), start, full);
                            break;
                        }
                        p_ = save;  // not a pair: the high half stands alone
                    }
                    // A lone surrogate cannot be UTF-8; it reads as U+FFFD.
                    PutCode(u >= 0xD800 && u <= 0xDFFF ? 0xFFFD : u, start, full);
                    break;
                }
                default: return false;
            }
        }
        at = uint32_t(start);
        n = uint32_t(text_.size() - start);
        return true;
    }

    uint32_t ArrayValue(int depth) {
        uint32_t self = Add(Type::Array);
        p_++;
        Space();
        if (p_ < end_ && *p_ == ']') {
            p_++;
            return self;
        }
        uint32_t last = kNone;
        for (;;) {
            uint32_t child = Value(depth + 1);
            if (child == kNone) return Fail();
            if (last == kNone) nodes_[self].first = child;
            else nodes_[last].next = child;
            last = child;
            Space();
            if (p_ >= end_) return Fail();
            if (*p_ == ',') {
                p_++;
                continue;
            }
            if (*p_ == ']') {
                p_++;
                return self;
            }
            return Fail();
        }
    }

    uint32_t ObjectValue(int depth) {
        uint32_t self = Add(Type::Object);
        p_++;
        Space();
        if (p_ < end_ && *p_ == '}') {
            p_++;
            return self;
        }
        uint32_t last = kNone;
        for (;;) {
            Space();
            if (p_ >= end_ || *p_ != '"') return Fail();
            uint32_t kat = 0, kn = 0;
            if (!String(kat, kn)) return Fail();
            Space();
            if (p_ >= end_ || *p_ != ':') return Fail();
            p_++;
            uint32_t child = Value(depth + 1);
            if (child == kNone) return Fail();
            nodes_[child].key = kat;
            nodes_[child].keyLen = kn;
            if (last == kNone) nodes_[self].first = child;
            else nodes_[last].next = child;
            last = child;
            Space();
            if (p_ >= end_) return Fail();
            if (*p_ == ',') {
                p_++;
                continue;
            }
            if (*p_ == '}') {
                p_++;
                return self;
            }
            return Fail();
        }
    }
};

// A look at one line without building it, to put lines in time order before each is parsed once (si_store.h Load).
// `whole`: it holds together as one JSON object (strings closed, no raw control character in them, brackets matched,
// nothing after); a line that does not is one Doc::Parse refuses too. `has`/`value`: the raw text of the last
// top-level member named `key`, when that member's value is a string. `plain` is false when only the real parse can
// say (an escape in a top-level key or in that value, a top that is not an object, nesting near kMaxDepth).
// Doc::Parse stays the judge: a line can hold together here and still be refused there (a bad number, literal or
// escape); the caller then drops it when it parses it.
struct Skim {
    bool whole = false;
    bool plain = true;
    bool has = false;
    std::string_view value;
};
inline Skim SkimTop(std::string_view s, std::string_view key) {
    constexpr int kDeep = 100;  // under kMaxDepth: deeper lines go to the real parse
    Skim k;
    size_t i = 0, n = s.size();
    auto space = [&] {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    };
    space();
    if (i >= n || s[i] != '{') {
        k.whole = true;
        k.plain = false;
        return k;
    }
    char open[kDeep];
    int depth = 0;
    bool wantKey = false, match = false, wantValue = false;
    while (i < n) {
        char c = s[i];
        if (c == '"') {
            size_t start = ++i;
            bool escaped = false;
            for (;;) {
                while (i < n && s[i] != '"' && s[i] != '\\' && uint8_t(s[i]) >= 0x20) i++;
                if (i >= n || uint8_t(s[i]) < 0x20) return k;
                if (s[i] == '"') break;
                escaped = true;
                i += 2;
                if (i > n) return k;
            }
            std::string_view str = s.substr(start, i - start);
            i++;
            if (depth == 1 && wantKey) {
                if (escaped) k.plain = false;
                match = !escaped && str == key;
                wantKey = false;
            } else if (depth == 1 && wantValue) {
                if (match) {
                    if (escaped) k.plain = false;
                    k.has = true;
                    k.value = str;
                }
                wantValue = false;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            i++;
            continue;
        }
        if (depth == 1 && wantValue) {  // a value that is not a string
            if (match) k.has = false, k.value = {};
            wantValue = false;
        }
        if (c == '{' || c == '[') {
            if (depth >= kDeep) {
                k = Skim{};
                k.whole = true;
                k.plain = false;
                return k;
            }
            open[depth++] = c;
            if (depth == 1) wantKey = true;
        } else if (c == '}' || c == ']') {
            if (depth == 0 || open[depth - 1] != (c == '}' ? '{' : '[')) return k;
            if (--depth == 0) {
                i++;
                space();
                k.whole = i == n;
                return k;
            }
        } else if (depth == 1 && c == ',') {
            wantKey = true;
        } else if (depth == 1 && c == ':') {
            wantValue = true;
        }
        i++;
    }
    return k;
}

inline Type Val::type() const {
    return d_->node(i_).type;
}
inline double Val::Num() const {
    return IsNumber() ? d_->node(i_).num : 0;
}
inline std::string_view Val::Str() const {
    if (!IsString()) return {};
    const Node& n = d_->node(i_);
    return d_->slice(n.str, n.len);
}
inline Val Val::operator[](std::string_view key) const {
    if (!IsObject()) return Val();
    uint32_t found = kNone;
    for (uint32_t c = d_->node(i_).first; c != kNone; c = d_->node(c).next) {
        const Node& n = d_->node(c);
        if (d_->slice(n.key, n.keyLen) == key) found = c;
    }
    return found == kNone ? Val() : Val(d_, found);
}
inline Val Val::First() const {
    return IsObjectLike() ? Val(d_, d_->node(i_).first) : Val();
}
inline Val Val::Next() const {
    return Exists() ? Val(d_, d_->node(i_).next) : Val();
}
inline std::string_view Val::Key() const {
    if (!Exists()) return {};
    const Node& n = d_->node(i_);
    return d_->slice(n.key, n.keyLen);
}
inline size_t Val::Count() const {
    size_t n = 0;
    for (Val c = First(); c.Exists(); c = c.Next()) n++;
    return n;
}

// The bytes of a line as String.prototype.trim() leaves them: JS white space and line ends off both ends
// (ASCII ones, NBSP, U+FEFF and the other Unicode spaces).
inline size_t JsSpaceAt(std::string_view s, size_t i) {
    uint8_t c = uint8_t(s[i]);
    if (c == ' ' || (c >= 0x09 && c <= 0x0D)) return 1;
    if (c == 0xC2 && i + 1 < s.size() && uint8_t(s[i + 1]) == 0xA0) return 2;
    if (c == 0xE1 && i + 2 < s.size() && uint8_t(s[i + 1]) == 0x9A && uint8_t(s[i + 2]) == 0x80) return 3;
    if (c == 0xE2 && i + 2 < s.size()) {
        uint8_t b = uint8_t(s[i + 1]), d = uint8_t(s[i + 2]);
        if (b == 0x80 && ((d >= 0x80 && d <= 0x8A) || d == 0xA8 || d == 0xA9 || d == 0xAF)) return 3;
        if (b == 0x81 && d == 0x9F) return 3;
    }
    if (c == 0xE3 && i + 2 < s.size() && uint8_t(s[i + 1]) == 0x80 && uint8_t(s[i + 2]) == 0x80) return 3;
    if (c == 0xEF && i + 2 < s.size() && uint8_t(s[i + 1]) == 0xBB && uint8_t(s[i + 2]) == 0xBF) return 3;
    return 0;
}
inline std::string_view TrimLine(std::string_view s) {
    size_t a = 0;
    while (a < s.size()) {
        size_t n = JsSpaceAt(s, a);
        if (n == 0) break;
        a += n;
    }
    s.remove_prefix(a);
    for (;;) {
        if (s.empty()) break;
        size_t cut = 0;
        for (size_t back = 1; back <= 3 && back <= s.size(); back++) {
            size_t at = s.size() - back;
            if (JsSpaceAt(s, at) == back) {
                cut = back;
                break;
            }
        }
        if (cut == 0) break;
        s.remove_suffix(cut);
    }
    return s;
}

}  // namespace json
}  // namespace si
