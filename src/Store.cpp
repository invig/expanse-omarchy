#include "Store.hpp"
#include "Json.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace Expanse::Store {

    static std::filesystem::path path() {
        const char*           STATE = std::getenv("XDG_STATE_HOME");
        const char*           HOME  = std::getenv("HOME");
        std::filesystem::path base  = STATE && *STATE ? STATE : std::filesystem::path{HOME ? HOME : "/tmp"} / ".local/state";
        return base / "expanse" / "layout.json";
    }

    static std::optional<CBox> box(const Json::SValue& value, const char* x, const char* y, const char* w, const char* h) {
        const auto X = value.number(x), Y = value.number(y), W = value.number(w), H = value.number(h);
        if (!X || !Y || !W || !H)
            return std::nullopt;
        return CBox{*X, *Y, *W, *H};
    }

    std::optional<SLayout> load() {
        std::ifstream file{path()};
        if (!file)
            return std::nullopt;
        std::stringstream text;
        text << file.rdbuf();
        const auto ROOT = Json::parse(text.str());
        if (!ROOT)
            return std::nullopt;

        SLayout layout;
        layout.autoCentre = ROOT->boolean("autoCentre").value_or(true);
        layout.hotCorner  = ROOT->string("hotCorner");
        layout.wallpaper  = ROOT->boolean("wallpaper");
        if (const auto* SCROLLED = ROOT->array("scrolled"))
            for (const auto& entry : *SCROLLED)
                if (const auto WS = entry.string("workspace"); WS && entry.number("x") && entry.number("y"))
                    layout.scrolled.emplace_back(*WS, Vector2D{*entry.number("x"), *entry.number("y")});
        if (const auto* WINDOWS = ROOT->array("windows"))
            for (const auto& entry : *WINDOWS) {
                const auto FRAME = box(entry, "x", "y", "w", "h");
                if (!FRAME)
                    continue;
                layout.windows.push_back({
                    .app       = entry.string("app").value_or(""),
                    .title     = entry.string("title").value_or(""),
                    .workspace = entry.string("workspace").value_or(""),
                    .frame     = *FRAME,
                    .wasTiled  = entry.boolean("wasTiled").value_or(false),
                    .original  = box(entry, "ox", "oy", "ow", "oh"),
                    .preferred = entry.number("pw") && entry.number("ph") ? std::optional{Vector2D{*entry.number("pw"), *entry.number("ph")}} : std::nullopt,
                    .home      = entry.number("hx") && entry.number("hy") ? std::optional{Vector2D{*entry.number("hx"), *entry.number("hy")}} : std::nullopt,
                    .address   = static_cast<uint64_t>(entry.number("address").value_or(0)),
                    .pid       = static_cast<int64_t>(entry.number("pid").value_or(0)),
                });
            }
        return layout;
    }

    void save(const SLayout& layout) {
        std::string out = std::format("{{\n  \"version\": 1,\n  \"autoCentre\": {},", layout.autoCentre);
        if (layout.hotCorner)
            out += std::format("\n  \"hotCorner\": {},", Json::quote(*layout.hotCorner));
        if (layout.wallpaper)
            out += std::format("\n  \"wallpaper\": {},", *layout.wallpaper);
        out += "\n  \"scrolled\": [";
        for (size_t i = 0; i < layout.scrolled.size(); ++i)
            out += std::format("{}\n    {{\"workspace\": {}, \"x\": {}, \"y\": {}}}", i ? "," : "", Json::quote(layout.scrolled[i].first), layout.scrolled[i].second.x,
                               layout.scrolled[i].second.y);
        out += "\n  ],\n  \"windows\": [";
        for (size_t i = 0; i < layout.windows.size(); ++i) {
            const auto& W = layout.windows[i];
            out += std::format("{}\n    {{\"app\": {}, \"title\": {}, \"workspace\": {}, \"x\": {}, \"y\": {}, \"w\": {}, \"h\": {}, \"wasTiled\": {}", i ? "," : "",
                               Json::quote(W.app), Json::quote(W.title), Json::quote(W.workspace), W.frame.x, W.frame.y, W.frame.w, W.frame.h, W.wasTiled);
            if (W.original)
                out += std::format(", \"ox\": {}, \"oy\": {}, \"ow\": {}, \"oh\": {}", W.original->x, W.original->y, W.original->w, W.original->h);
            if (W.preferred)
                out += std::format(", \"pw\": {}, \"ph\": {}", W.preferred->x, W.preferred->y);
            if (W.home)
                out += std::format(", \"hx\": {}, \"hy\": {}", W.home->x, W.home->y);
            // Addresses stay below 2^53 on 64-bit Linux, so a JSON number holds them exactly.
            out += std::format(", \"address\": {}, \"pid\": {}}}", W.address, W.pid);
        }
        out += "\n  ]\n}\n";

        // Written aside and renamed into place, so a crash never leaves half a file.
        const auto      FILE = path();
        std::error_code error;
        std::filesystem::create_directories(FILE.parent_path(), error);
        const auto TEMP = FILE.string() + ".next";
        {
            std::ofstream stream{TEMP, std::ios::trunc};
            if (!stream)
                return;
            stream << out;
        }
        std::filesystem::rename(TEMP, FILE, error);
    }

    std::string titleKey(const std::string& title) {
        if (title.empty())
            return "";
        // FNV-1a, 64-bit.
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (const unsigned char C : title) {
            hash ^= C;
            hash *= 0x100000001b3ULL;
        }
        return std::format("fnv1a:{:016x}", hash);
    }

    std::optional<SWindow> takeMatch(const std::string& app, const std::string& title, uint64_t address, int64_t pid, const std::set<uint64_t>& open,
                                     std::vector<SWindow>& pool) {
        const auto take = [&](auto&& matches) -> std::optional<SWindow> {
            for (auto it = pool.begin(); it != pool.end(); ++it)
                if (matches(*it)) {
                    SWindow out = std::move(*it);
                    pool.erase(it);
                    return out;
                }
            return std::nullopt;
        };
        const auto FREE = [&](const SWindow& entry) { return entry.address == 0 || !open.contains(entry.address); };
        const auto KEY  = titleKey(title);

        if (auto exact = take([&](const SWindow& e) { return e.address == address && e.pid == pid && e.app == app; }))
            return exact;
        if (auto titled = take([&](const SWindow& e) { return e.app == app && e.title == KEY && FREE(e); }))
            return titled;
        return take([&](const SWindow& e) { return e.app == app && FREE(e); });
    }
}
