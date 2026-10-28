#include "Store.hpp"
#include "Json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
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

    static std::filesystem::path snapshotFolder() {
        return path().parent_path() / "snapshots";
    }

    static std::optional<SLayout> loadFile(const std::filesystem::path& from) {
        std::ifstream file{from};
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
        layout.names      = ROOT->boolean("names");
        layout.logging    = ROOT->boolean("logging").value_or(false);
        if (const auto* SCROLLED = ROOT->array("scrolled"))
            for (const auto& entry : *SCROLLED)
                if (const auto WS = entry.string("workspace"); WS && entry.number("x") && entry.number("y"))
                    layout.scrolled.emplace_back(*WS, Vector2D{*entry.number("x"), *entry.number("y")});
        if (const auto* FITTED = ROOT->array("fitted"))
            for (const auto& entry : *FITTED)
                if (const auto WS = entry.string("workspace"); WS && entry.number("w") && entry.number("h"))
                    layout.fitted.emplace_back(*WS, Vector2D{*entry.number("w"), *entry.number("h")});
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

    std::optional<SLayout> load() {
        return loadFile(path());
    }

    std::optional<SLayout> load(const SSnapshot& snapshot) {
        return loadFile(snapshot.file);
    }

    static std::string encode(const SLayout& layout) {
        std::string out = std::format("{{\n  \"version\": 1,\n  \"autoCentre\": {},", layout.autoCentre);
        if (layout.hotCorner)
            out += std::format("\n  \"hotCorner\": {},", Json::quote(*layout.hotCorner));
        if (layout.wallpaper)
            out += std::format("\n  \"wallpaper\": {},", *layout.wallpaper);
        if (layout.names)
            out += std::format("\n  \"names\": {},", *layout.names);
        if (layout.logging)
            out += "\n  \"logging\": true,";
        out += "\n  \"scrolled\": [";
        for (size_t i = 0; i < layout.scrolled.size(); ++i)
            out += std::format("{}\n    {{\"workspace\": {}, \"x\": {}, \"y\": {}}}", i ? "," : "", Json::quote(layout.scrolled[i].first), layout.scrolled[i].second.x,
                               layout.scrolled[i].second.y);
        out += "\n  ],\n  \"fitted\": [";
        for (size_t i = 0; i < layout.fitted.size(); ++i)
            out += std::format("{}\n    {{\"workspace\": {}, \"w\": {}, \"h\": {}}}", i ? "," : "", Json::quote(layout.fitted[i].first), layout.fitted[i].second.x,
                               layout.fitted[i].second.y);
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
        return out;
    }

    static void write(const std::string& text, const std::filesystem::path& to) {
        // Written aside and renamed into place, so a crash never leaves half a file.
        std::error_code error;
        std::filesystem::create_directories(to.parent_path(), error);
        const auto TEMP = to.string() + ".next";
        {
            std::ofstream stream{TEMP, std::ios::trunc};
            if (!stream)
                return;
            stream << text;
        }
        std::filesystem::rename(TEMP, to, error);
    }

    bool save(const SLayout& layout) {
        static std::string last;
        auto               text = encode(layout);
        if (text == last)
            return false;
        write(text, path());
        last = std::move(text);
        return true;
    }

    std::vector<SSnapshot> snapshots() {
        std::vector<SSnapshot> out;
        std::error_code        error;
        for (const auto& file : std::filesystem::directory_iterator(snapshotFolder(), error)) {
            const auto STEM = file.path().stem().string();
            if (file.path().extension() != ".json" || STEM.empty() || !std::ranges::all_of(STEM, [](unsigned char c) { return std::isdigit(c); }))
                continue;
            out.push_back({std::stoll(STEM), file.path()});
        }
        std::ranges::sort(out, [](const SSnapshot& a, const SSnapshot& b) { return a.taken > b.taken; });
        return out;
    }

    void saveSnapshot(const SLayout& layout) {
        const auto NOW = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        write(encode(layout), snapshotFolder() / std::format("{}.json", NOW));
        const auto ALL = snapshots();
        std::error_code error;
        for (size_t i = SNAPSHOTS_KEPT; i < ALL.size(); ++i)
            std::filesystem::remove(ALL[i].file, error);
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
