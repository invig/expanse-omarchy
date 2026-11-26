#pragma once

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

// Expanse's state (the layout, its snapshots, the detailed log) names the
// user's windows, their processes and where Hyprland keeps them, so it's kept
// to its owner whatever the umask: directories 0700, files 0600. Each file is
// made through the descriptor of a directory already checked to be the
// user's own, so nothing is created before that's settled.
namespace Expanse::StateDir {

    class CFd {
      public:
        CFd() = default;
        explicit CFd(int fd) : m_fd(fd) {}
        CFd(CFd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
        CFd& operator=(CFd&& other) noexcept {
            if (this != &other) {
                reset();
                m_fd = std::exchange(other.m_fd, -1);
            }
            return *this;
        }
        CFd(const CFd&)            = delete;
        CFd& operator=(const CFd&) = delete;
        ~CFd() {
            reset();
        }

        int get() const {
            return m_fd;
        }
        explicit operator bool() const {
            return m_fd >= 0;
        }

      private:
        void reset() {
            if (m_fd >= 0)
                ::close(m_fd);
            m_fd = -1;
        }
        int m_fd = -1;
    };

    // Nothing unless it's a directory this user owns, which is then made 0700.
    inline CFd claimDir(CFd fd) {
        struct stat st {};
        if (!fd || ::fstat(fd.get(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid())
            return {};
        if ((st.st_mode & 07777) != 0700 && ::fchmod(fd.get(), 0700) != 0)
            return {};
        return fd;
    }

    // Missing parents are made 0700 too, as the XDG base directory spec asks.
    inline CFd open(const std::filesystem::path& dir) {
        CFd fd{::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        if (!fd && errno == ENOENT) {
            std::filesystem::path partial;
            for (const auto& part : dir) {
                partial /= part;
                ::mkdir(partial.c_str(), 0700);
            }
            fd = CFd{::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        }
        return claimDir(std::move(fd));
    }

    inline CFd openAt(int parent, const char* name) {
        ::mkdirat(parent, name, 0700);
        return claimDir(CFd{::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)});
    }

    // A regular file in dir, for writing, made 0600 if it was anything else.
    inline CFd openFile(int dir, const char* name, int flags) {
        CFd         fd{::openat(dir, name, flags | O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600)};
        struct stat st {};
        if (!fd || ::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode))
            return {};
        if ((st.st_mode & 07777) != 0600 && ::fchmod(fd.get(), 0600) != 0)
            return {};
        return fd;
    }

    inline bool writeAll(int fd, std::string_view text) {
        while (!text.empty()) {
            const auto N = ::write(fd, text.data(), text.size());
            if (N < 0 && errno == EINTR)
                continue;
            if (N <= 0)
                return false;
            text.remove_prefix(static_cast<size_t>(N));
        }
        return true;
    }

    // Written aside and renamed into place, so a crash never leaves half a file.
    inline bool replace(int dir, const std::string& name, std::string_view text) {
        const auto TEMP = name + ".next";
        ::unlinkat(dir, TEMP.c_str(), 0);
        bool written = false;
        {
            const auto FD = openFile(dir, TEMP.c_str(), O_EXCL);
            written       = FD && writeAll(FD.get(), text);
        }
        if (written && ::renameat(dir, TEMP.c_str(), dir, name.c_str()) == 0)
            return true;
        ::unlinkat(dir, TEMP.c_str(), 0);
        return false;
    }
}
