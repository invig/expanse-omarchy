#include "Json.hpp"

#include <charconv>
#include <cstdio>

namespace Expanse::Json {

    const SValue* SValue::get(std::string_view key) const {
        if (const auto* OBJECT = std::get_if<Object>(&value))
            for (const auto& [k, v] : *OBJECT)
                if (k == key)
                    return &v;
        return nullptr;
    }

    std::optional<double> SValue::number(std::string_view key) const {
        const auto* V = get(key);
        const auto* N = V ? std::get_if<double>(&V->value) : nullptr;
        return N ? std::optional{*N} : std::nullopt;
    }

    std::optional<std::string> SValue::string(std::string_view key) const {
        const auto* V = get(key);
        const auto* S = V ? std::get_if<std::string>(&V->value) : nullptr;
        return S ? std::optional{*S} : std::nullopt;
    }

    std::optional<bool> SValue::boolean(std::string_view key) const {
        const auto* V = get(key);
        const auto* B = V ? std::get_if<bool>(&V->value) : nullptr;
        return B ? std::optional{*B} : std::nullopt;
    }

    const Array* SValue::array(std::string_view key) const {
        const auto* V = get(key);
        return V ? std::get_if<Array>(&V->value) : nullptr;
    }

    namespace {
        struct SParser {
            std::string_view text;
            size_t           at    = 0;
            int              depth = 0;

            void skip() {
                while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t'))
                    ++at;
            }

            bool eat(char c) {
                skip();
                if (at < text.size() && text[at] == c) {
                    ++at;
                    return true;
                }
                return false;
            }

            bool literal(std::string_view word) {
                if (text.substr(at, word.size()) != word)
                    return false;
                at += word.size();
                return true;
            }

            std::optional<std::string> string() {
                if (!eat('"'))
                    return std::nullopt;
                std::string out;
                while (at < text.size()) {
                    const char C = text[at++];
                    if (C == '"')
                        return out;
                    if (C != '\\') {
                        out += C;
                        continue;
                    }
                    if (at >= text.size())
                        return std::nullopt;
                    switch (const char E = text[at++]) {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'u': {
                            // The writer only escapes control characters, all below 0x80.
                            if (at + 4 > text.size())
                                return std::nullopt;
                            unsigned code = 0;
                            std::from_chars(text.data() + at, text.data() + at + 4, code, 16);
                            at += 4;
                            out += sc(code);
                            break;
                        }
                        default: out += E;
                    }
                }
                return std::nullopt;
            }

            static char sc(unsigned code) {
                return static_cast<char>(code & 0x7F);
            }

            std::optional<SValue> value() {
                if (++depth > 64)
                    return std::nullopt;
                skip();
                std::optional<SValue> out;
                if (at >= text.size())
                    out = std::nullopt;
                else if (text[at] == '{') {
                    ++at;
                    Object object;
                    if (!eat('}')) {
                        do {
                            auto key = string();
                            if (!key || !eat(':'))
                                return std::nullopt;
                            auto v = value();
                            if (!v)
                                return std::nullopt;
                            object.emplace_back(std::move(*key), std::move(*v));
                        } while (eat(','));
                        if (!eat('}'))
                            return std::nullopt;
                    }
                    out = SValue{std::move(object)};
                } else if (text[at] == '[') {
                    ++at;
                    Array array;
                    if (!eat(']')) {
                        do {
                            auto v = value();
                            if (!v)
                                return std::nullopt;
                            array.push_back(std::move(*v));
                        } while (eat(','));
                        if (!eat(']'))
                            return std::nullopt;
                    }
                    out = SValue{std::move(array)};
                } else if (text[at] == '"') {
                    auto s = string();
                    if (s)
                        out = SValue{std::move(*s)};
                } else if (literal("true"))
                    out = SValue{true};
                else if (literal("false"))
                    out = SValue{false};
                else if (literal("null"))
                    out = SValue{nullptr};
                else {
                    double     number = 0;
                    const auto RESULT = std::from_chars(text.data() + at, text.data() + text.size(), number);
                    if (RESULT.ec == std::errc{}) {
                        at  = RESULT.ptr - text.data();
                        out = SValue{number};
                    }
                }
                --depth;
                return out;
            }
        };
    }

    std::optional<SValue> parse(std::string_view text) {
        SParser parser{text};
        auto    out = parser.value();
        parser.skip();
        if (parser.at != text.size())
            return std::nullopt;
        return out;
    }

    std::string quote(std::string_view text) {
        std::string out = "\"";
        for (const char C : text) {
            if (C == '"' || C == '\\') {
                out += '\\';
                out += C;
            } else if (static_cast<unsigned char>(C) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", C);
                out += buffer;
            } else
                out += C;
        }
        return out + "\"";
    }
}
