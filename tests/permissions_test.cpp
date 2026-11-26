// Owner-only modes for the canvas state directory, the layout file (and the
// temporary file renamed over it), layout snapshots, and the detailed log.
//
// These fail while those paths are created with the process umask. They pass
// once the expanse directory and snapshots directory are mode 0700 and every
// layout, snapshot, temporary, and log file is mode 0600, including when the
// directory or file already exists and when umask is 000 or 022.
//
// `make test` builds and runs this. CCanvas::saveNow is not linked: it needs
// a running compositor. It stores the window pointer, pid, and app id on the
// same SLayout that Store::save and Store::saveSnapshot write here.

#include "../src/Log.hpp"
#include "../src/Store.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace Expanse;

static int failures = 0;
static int64_t nextPid = 424242;

static void expect(const char* name, bool ok) {
    if (ok)
        return;
    ++failures;
    std::printf("FAIL %s\n", name);
}

static mode_t permBits(const std::filesystem::path& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        return static_cast<mode_t>(~mode_t{0});
    return st.st_mode & 0777;
}

static void expectMode(const char* name, const std::filesystem::path& path, mode_t want) {
    const mode_t got = permBits(path);
    if (got == want)
        return;
    ++failures;
    std::printf("FAIL %s: %s is %04o, want %04o\n", name, path.c_str(), got, want);
}

struct Umask {
    mode_t previous;
    explicit Umask(mode_t next) : previous(::umask(next)) {}
    ~Umask() { ::umask(previous); }
};

static std::filesystem::path freshBase(const char* tag) {
    const auto base = std::filesystem::temp_directory_path() / ("expanse-perm-" + std::string(tag) + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(base);
    std::filesystem::create_directory(base);
    // A traversable parent, as ~/.local/state is when the home directory is.
    ::chmod(base.c_str(), 0755);
    ::setenv("XDG_STATE_HOME", base.c_str(), 1);
    return base;
}

static Store::SLayout layoutWith(int64_t pid) {
    Store::SLayout layout;
    layout.autoCentre = false;
    layout.windows.push_back({
        .app       = "foot-secret",
        .title     = Store::titleKey("notes"),
        .workspace = "1",
        .frame     = CBox{10, 20, 800, 600},
        .address   = 0x112233445566ULL,
        .pid       = pid,
    });
    return layout;
}

static std::string readAll(const std::filesystem::path& path) {
    std::ifstream in{path};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

static void expectPrivateLayout(const char* label, const std::filesystem::path& base, int64_t pid) {
    const auto dir  = base / "expanse";
    const auto file = dir / "layout.json";
    const auto temp = std::filesystem::path{file.string() + ".next"};
    expectMode(label, dir, 0700);
    expectMode(label, file, 0600);
    if (std::filesystem::exists(temp))
        expectMode(label, temp, 0600);

    const auto text = readAll(file);
    const auto address = std::to_string(0x112233445566ULL);
    const auto pidText = std::to_string(pid);
    expect("layout file holds the app id", text.find("\"app\": \"foot-secret\"") != std::string::npos);
    expect("layout file holds the window address", text.find("\"address\": " + address) != std::string::npos);
    expect("layout file holds the process id", text.find("\"pid\": " + pidText) != std::string::npos);

    const auto loaded = Store::load();
    expect("owner can still load the private layout", loaded && loaded->windows.size() == 1 && loaded->windows[0].address == 0x112233445566ULL &&
                                                           loaded->windows[0].pid == pid && loaded->windows[0].app == "foot-secret");
    std::printf("CASE %s dir=%04o file=%04o bytes=%zu\n", label, permBits(dir), permBits(file), text.size());
}

static void saveFresh(const char* tag, mode_t umaskBits) {
    const auto base = freshBase(tag);
    const auto pid  = nextPid++;
    {
        Umask guard{umaskBits};
        expect("layout save wrote a file", Store::save(layoutWith(pid)));
    }
    expectPrivateLayout(tag, base, pid);
    std::filesystem::remove_all(base);
}

static void saveOverExisting(const char* tag, mode_t umaskBits) {
    const auto base = freshBase(tag);
    const auto dir  = base / "expanse";
    const auto file = dir / "layout.json";
    std::filesystem::create_directory(dir);
    ::chmod(dir.c_str(), 0755);
    {
        std::ofstream prior{file};
        prior << "{\"version\":1}\n";
    }
    ::chmod(file.c_str(), 0600);
    const auto pid = nextPid++;
    {
        Umask guard{umaskBits};
        expect("replacing a layout wrote a file", Store::save(layoutWith(pid)));
    }
    expectPrivateLayout(tag, base, pid);
    std::filesystem::remove_all(base);
}

static void saveSnapshotPrivate(const char* tag, mode_t umaskBits) {
    const auto base = freshBase(tag);
    const auto pid  = nextPid++;
    {
        Umask guard{umaskBits};
        Store::saveSnapshot(layoutWith(pid));
    }
    const auto shots = Store::snapshots();
    expect("a snapshot was written", shots.size() == 1);
    if (!shots.empty()) {
        expectMode(tag, base / "expanse", 0700);
        expectMode(tag, shots[0].file.parent_path(), 0700);
        expectMode(tag, shots[0].file, 0600);
        const auto temp = std::filesystem::path{shots[0].file.string() + ".next"};
        if (std::filesystem::exists(temp))
            expectMode("snapshot temporary file", temp, 0600);
        const auto text = readAll(shots[0].file);
        expect("snapshot holds the window address", text.find("\"address\": " + std::to_string(0x112233445566ULL)) != std::string::npos);
        expect("snapshot holds the app id", text.find("foot-secret") != std::string::npos);
        const auto loaded = Store::load(shots[0]);
        expect("owner can still load the snapshot", loaded && loaded->windows.size() == 1 && loaded->windows[0].pid == pid);
        std::printf("CASE %s dir=%04o file=%04o\n", tag, permBits(shots[0].file.parent_path()), permBits(shots[0].file));
    }
    std::filesystem::remove_all(base);
}

static void expectPrivateLog(const char* label, const std::filesystem::path& base, const std::filesystem::path& file) {
    expectMode(label, base / "expanse", 0700);
    expectMode(label, file, 0600);
    std::printf("CASE %s dir=%04o file=%04o\n", label, permBits(base / "expanse"), permBits(file));
}

static void logFresh(const char* tag, mode_t umaskBits) {
    const auto base = freshBase(tag);
    {
        Umask guard{umaskBits};
        Log::setFile(false);
        Log::setFile(true);
        Log::line("marker-" + std::string(tag));
    }
    const auto file = std::filesystem::path{Log::filePath()};
    expect("log file was created", std::filesystem::is_regular_file(file));
    expect("log file holds the marker line", readAll(file).find("marker-" + std::string(tag)) != std::string::npos);
    expectPrivateLog(tag, base, file);
    std::filesystem::remove_all(base);
}

static void logOverExisting(const char* tag, mode_t umaskBits) {
    const auto base = freshBase(tag);
    const auto dir  = base / "expanse";
    std::filesystem::create_directory(dir);
    ::chmod(dir.c_str(), 0755);
    const auto file = dir / "expanse.log";
    {
        std::ofstream prior{file};
        prior << "old\n";
    }
    ::chmod(file.c_str(), 0644);
    {
        Umask guard{umaskBits};
        Log::setFile(true);
        Log::line(std::string("marker-") + tag);
    }
    expect("appended log holds the new line", readAll(file).find(std::string("marker-") + tag) != std::string::npos);
    expectPrivateLog(tag, base, file);
    std::filesystem::remove_all(base);
}

static void logRotatesPrivate(const char* tag, mode_t umaskBits) {
    const auto base = freshBase(tag);
    const std::string chunk(100000, 'L');
    {
        Umask guard{umaskBits};
        Log::setFile(true);
        // Log.cpp keeps the previous file once it passes 5_000_000 bytes.
        for (int i = 0; i < 60; ++i)
            Log::line(chunk);
    }
    const auto current = std::filesystem::path{Log::filePath()};
    const auto older   = current.parent_path() / "expanse.1.log";
    expect("rotation kept the older log", std::filesystem::is_regular_file(older));
    expectPrivateLog(tag, base, current);
    expectMode(tag, older, 0600);
    std::printf("CASE %s older=%04o current=%04o\n", tag, permBits(older), permBits(current));
    std::filesystem::remove_all(base);
}

int main() {
    const mode_t saved = ::umask(022);
    ::umask(saved);

    saveFresh("umask-000", 0000);
    saveFresh("umask-022", 0022);
    saveOverExisting("replace-umask-000", 0000);
    saveOverExisting("replace-umask-022", 0022);
    saveSnapshotPrivate("snapshot-umask-000", 0000);
    saveSnapshotPrivate("snapshot-umask-022", 0022);

    logFresh("log-umask-000", 0000);
    logFresh("log-umask-022", 0022);
    logOverExisting("log-replace-umask-000", 0000);
    logOverExisting("log-replace-umask-022", 0022);
    logRotatesPrivate("log-rotate-umask-000", 0000);
    logRotatesPrivate("log-rotate-umask-022", 0022);

    ::umask(saved);
    if (failures == 0)
        std::printf("permissions: all cases pass\n");
    else
        std::printf("permissions: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
