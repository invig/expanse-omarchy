#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Just enough JSON for the layout file, so the plugin needs no library.
namespace Expanse::Json {

    struct SValue;
    using Array  = std::vector<SValue>;
    using Object = std::vector<std::pair<std::string, SValue>>;

    struct SValue {
        std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value = nullptr;

        const SValue*              get(std::string_view key) const; // in an object
        std::optional<double>      number(std::string_view key) const;
        std::optional<std::string> string(std::string_view key) const;
        std::optional<bool>        boolean(std::string_view key) const;
        const Array*               array(std::string_view key) const;
    };

    std::optional<SValue> parse(std::string_view text);
    std::string           quote(std::string_view text);
}
