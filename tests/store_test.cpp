// The layout file and how windows find their saved spots. `make test`.
#include "../src/Json.hpp"
#include "../src/Store.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace Expanse;

static int failures = 0;

static void expect(const char* name, bool ok) {
    if (ok)
        return;
    ++failures;
    std::printf("FAIL %s\n", name);
}

int main() {
    const auto DIR = std::filesystem::temp_directory_path() / ("expanse-store-test-" + std::to_string(::getpid()));
    ::setenv("XDG_STATE_HOME", DIR.c_str(), 1);

    expect("nothing saved yet", !Store::load());

    Store::SLayout layout;
    layout.autoCentre = false;
    layout.scrolled   = {{"1", {120, -40}}, {"web \"two\"", {0, 5}}};
    layout.windows    = {
        {.app = "foot", .title = Store::titleKey("notes"), .workspace = "1", .frame = {10, 20, 800, 600}, .wasTiled = true, .original = CBox{0, 30, 1500, 900}, .address = 0x5555deadbeef, .pid = 42, .id = 9},
        {.app = "org.gnome.Nautilus", .title = Store::titleKey("Home"), .workspace = "web \"two\"", .frame = {-300.5, 7, 1200, 720}},
    };
    Store::save(layout);

    const auto BACK = Store::load();
    expect("saved layout loads", BACK.has_value());
    if (BACK) {
        expect("auto-centre kept", BACK->autoCentre == false);
        expect("scroll per workspace kept", BACK->scrolled.size() == 2 && BACK->scrolled[1].first == "web \"two\"" && BACK->scrolled[0].second == Vector2D{120, -40});
        expect("both windows kept", BACK->windows.size() == 2);
        if (BACK->windows.size() == 2) {
            const auto& A = BACK->windows[0];
            expect("frame kept", A.frame == CBox{10, 20, 800, 600});
            expect("original kept", A.wasTiled && A.original && *A.original == CBox{0, 30, 1500, 900});
            expect("identity kept", A.address == 0x5555deadbeef && A.pid == 42 && A.id == 9);
            expect("fractional coordinates kept", BACK->windows[1].frame.x == -300.5);
            expect("no original stays none", !BACK->windows[1].original);
        }
    }

    expect("titles are hashed", Store::titleKey("notes") != "notes" && Store::titleKey("notes") == Store::titleKey("notes") && Store::titleKey("") == "");

    // Matching: only the window that owned the entry (address, pid, app and
    // stable ID), never another by app or title.
    std::vector<Store::SWindow> pool = {
        {.app = "foot", .title = Store::titleKey("a"), .frame = {1, 0, 1, 1}, .address = 100, .pid = 7, .id = 11},
        {.app = "foot", .title = Store::titleKey("b"), .frame = {2, 0, 1, 1}, .address = 200, .pid = 7, .id = 12},
        {.app = "foot", .title = Store::titleKey("c"), .frame = {3, 0, 1, 1}},
    };
    auto pick = [&](const std::string& title, uint64_t address, int64_t pid, uint64_t id) {
        auto copy = pool;
        auto m    = Store::takeMatch("foot", title, address, pid, {}, copy, id);
        return m ? m->frame.x : 0.0;
    };
    expect("the same window gets its entry, whatever its title", pick("c", 200, 7, 12) == 2);
    expect("app and title alone match nothing", pick("c", 999, 8, 0) == 0);
    expect("app alone matches nothing", pick("zzz", 999, 8, 0) == 0);
    expect("an entry with no address matches nothing", pick("c", 0, 0, 0) == 0);
    expect("a recycled address and pid with another stable ID match nothing", pick("a", 100, 7, 99) == 0);
    expect("another app matches nothing", !Store::takeMatch("kitty", "a", 100, 7, {}, pool, 11));

    auto taken = pool;
    Store::takeMatch("foot", "a", 100, 7, {}, taken, 11);
    expect("a matched entry leaves the pool", taken.size() == 2 && taken[0].frame.x == 2);
    Store::takeMatch("foot", "c", 999, 8, {}, taken);
    expect("a refused match leaves the pool as it was", taken.size() == 2);

    expect("malformed JSON is refused", !Json::parse("{\"a\": [1, 2,}"));
    expect("escapes round-trip", Json::parse(Json::quote("tab\there \"q\" \\ line\n"))->value.index() == 3);

    std::filesystem::remove_all(DIR);
    if (failures == 0)
        std::printf("store: all cases pass\n");
    return failures == 0 ? 0 : 1;
}
