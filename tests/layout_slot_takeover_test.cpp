// A saved canvas spot may be applied only to the client that owns it.
// `make` does not build this file. Run:
//   g++ -std=c++2b -O1 -g tests/layout_slot_takeover_test.cpp src/Store.cpp src/Json.cpp -o .build/layout_slot_takeover_test $(pkg-config --cflags --libs hyprutils)
//   .build/layout_slot_takeover_test
//
// These assertions fail while Store::takeMatch still gives a free saved entry
// to any window with the same app id. Callers that share that match are
// CCanvas::placeFromSaved (exactOnly false: a window alone on its app id, or
// one opened within three seconds of another), CCanvas::restoreCanvas's
// second pass, and CCanvas::restoreLayout. The move, resize, and glide focus
// that follow a match live in the Hyprland plugin and are not executed here.
#include "../src/Store.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

using namespace Expanse;

static int failures = 0;

static void expect(const char* name, bool ok) {
    if (ok)
        return;
    ++failures;
    std::printf("FAIL %s\n", name);
}

// A closed window's spot, as onWindowClose records it: app id, hashed title,
// no address and no pid. The frame is off the usual screen.
static Store::SWindow closedSpot(const std::string& app, const std::string& title, double x) {
    return {
        .app   = app,
        .title = Store::titleKey(title),
        .frame = {x, 80, 960, 640},
    };
}

// What a different process's window passes in: the spoofed app id, some other
// title, and that window's own address and pid.
static std::optional<Store::SWindow> claim(std::vector<Store::SWindow>& pool, const std::string& app, const std::string& title, uint64_t address, int64_t pid,
                                           std::set<uint64_t> open = {}) {
    return Store::takeMatch(app, title, address, pid, open, pool);
}

static bool sameFrame(const Store::SWindow& entry, double x) {
    return entry.frame == CBox{x, 80, 960, 640};
}

int main() {
    const std::string APP   = "org.example.SecretSafe";
    const std::string TITLE = "Accounts";
    constexpr uint64_t OWNER_ADDRESS = 0xA11C0;
    constexpr int64_t  OWNER_PID     = 4242;
    constexpr uint64_t SPOOF_ADDRESS = 0xBEEF;
    constexpr int64_t  SPOOF_PID     = 99;

    // Closed spot, address 0: the first window of the spoofed app id, which is
    // not marked fresh, must not receive it or remove it.
    {
        std::vector<Store::SWindow> pool = {closedSpot(APP, TITLE, -4200)};
        const auto                  got  = claim(pool, APP, "Not the saved window", SPOOF_ADDRESS, SPOOF_PID);
        expect("closed spot is not given to a different client by app id", !got);
        expect("refused app-id claim leaves the closed spot in the pool", pool.size() == 1 && sameFrame(pool[0], -4200) && pool[0].address == 0 && pool[0].pid == 0);
    }

    // Same app id and the same title hash, still a different client.
    {
        std::vector<Store::SWindow> pool = {closedSpot(APP, TITLE, -4200)};
        const auto                  got  = claim(pool, APP, TITLE, SPOOF_ADDRESS, SPOOF_PID);
        expect("closed spot is not given when only the app id and title hash match", !got);
        expect("refused title claim leaves the closed spot in the pool", pool.size() == 1 && sameFrame(pool[0], -4200));
    }

    // After a restart the file can still hold the previous client's address
    // and pid. That client is gone, so the address is not open. A new process
    // with the same app id must not take the spot.
    {
        auto stale   = closedSpot(APP, TITLE, -4200);
        stale.address = OWNER_ADDRESS;
        stale.pid     = OWNER_PID;
        std::vector<Store::SWindow> pool = {stale};
        const auto                  got  = claim(pool, APP, "Other title", SPOOF_ADDRESS, SPOOF_PID, {});
        expect("stale address is not a free app-id match for a new client", !got);
        expect("refused stale claim leaves that entry in the pool", pool.size() == 1 && pool[0].address == OWNER_ADDRESS && pool[0].pid == OWNER_PID && sameFrame(pool[0], -4200));
    }

    // Two saved spots. The impostor must not take the first, and a later
    // window of the real app id (also a new client) must not find it gone.
    {
        std::vector<Store::SWindow> pool = {closedSpot(APP, TITLE, -4200), closedSpot(APP, "Other document", -1800)};
        expect("first spoofed window takes no spot", !claim(pool, APP, "Impostor", SPOOF_ADDRESS, SPOOF_PID));
        expect("second window of the spoofed app id takes no spot", !claim(pool, APP, TITLE, SPOOF_ADDRESS + 1, SPOOF_PID, {SPOOF_ADDRESS}));
        expect("both saved spots remain, first frame first", pool.size() == 2 && sameFrame(pool[0], -4200) && sameFrame(pool[1], -1800));
    }

    // Another window of that app id while one is already open (the case
    // placeFromSaved allows inside the three-second burst). The closed spot
    // stays for the client that owned it.
    {
        std::vector<Store::SWindow> pool = {closedSpot(APP, TITLE, -4200)};
        const auto                  got  = claim(pool, APP, "Second window", SPOOF_ADDRESS, SPOOF_PID, {OWNER_ADDRESS});
        expect("a further window of the app id does not take a closed spot", !got);
        expect("closed spot remains while another window of the app is open", pool.size() == 1 && sameFrame(pool[0], -4200));
    }

    // The client that still owns the entry (same address, pid, and app) gets
    // it even when the title differs, and the entry then leaves the pool.
    // An earlier app-id lookalike must not have been preferred.
    {
        auto owned   = closedSpot(APP, TITLE, -4200);
        owned.address = OWNER_ADDRESS;
        owned.pid     = OWNER_PID;
        auto other   = closedSpot(APP, "Other document", -1800);
        std::vector<Store::SWindow> pool = {other, owned};
        expect("lookalike does not consume either entry", !claim(pool, APP, "Impostor", SPOOF_ADDRESS, SPOOF_PID));
        const auto got = claim(pool, APP, "Title changed since it was saved", OWNER_ADDRESS, OWNER_PID);
        expect("owning client still receives its frame", got && got->frame == CBox{-4200, 80, 960, 640} && got->address == OWNER_ADDRESS && got->pid == OWNER_PID);
        expect("owning client's entry leaves the pool and the other spot stays", pool.size() == 1 && sameFrame(pool[0], -1800));
    }

    // Address and pid of the owner, but a different app id, is not that window.
    {
        auto owned   = closedSpot(APP, TITLE, -4200);
        owned.address = OWNER_ADDRESS;
        owned.pid     = OWNER_PID;
        std::vector<Store::SWindow> pool = {owned};
        const auto                  got  = claim(pool, "org.example.Other", TITLE, OWNER_ADDRESS, OWNER_PID);
        expect("same address and pid with a different app id does not match", !got);
        expect("different app id leaves the entry", pool.size() == 1 && pool[0].app == APP);
    }

    // The spot survives being written and read back, which is how a closed
    // window is still there after a restart, and still is not claimable by
    // app id.
    {
        const auto DIR = std::filesystem::temp_directory_path() / ("expanse-slot-test-" + std::to_string(::getpid()));
        ::setenv("XDG_STATE_HOME", DIR.c_str(), 1);
        Store::SLayout layout;
        layout.windows = {closedSpot(APP, TITLE, -4200)};
        Store::save(layout);
        const auto loaded = Store::load();
        expect("saved closed spot loads", loaded && loaded->windows.size() == 1);
        if (loaded && loaded->windows.size() == 1) {
            auto pool = loaded->windows;
            expect("loaded closed spot kept its off-screen frame and no client", sameFrame(pool[0], -4200) && pool[0].address == 0 && pool[0].pid == 0 && pool[0].app == APP);
            const auto got = claim(pool, APP, "Impostor", SPOOF_ADDRESS, SPOOF_PID);
            expect("loaded closed spot is not given out by app id", !got);
            expect("loaded closed spot remains after the refused claim", pool.size() == 1 && sameFrame(pool[0], -4200));
        }
        std::filesystem::remove_all(DIR);
    }

    if (failures == 0)
        std::printf("layout slot: all cases pass\n");
    else
        std::printf("layout slot: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
