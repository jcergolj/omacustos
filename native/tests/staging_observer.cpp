// Diagnostic-only LD_PRELOAD observer for serial benchmark fixtures. Measures
// logical regular-file bytes in one explicit private staging root. It never
// changes an I/O result, grants authority, imposes quotas, or deletes files.
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace {
using Key = std::pair<dev_t, ino_t>;
struct Observation {
    std::mutex mutex;
    std::string root;
    std::map<Key, long long> files;
    std::map<int, Key> writers;
    long long current = 0;
    long long peak = 0;
};
Observation &observation() { static auto *value = new Observation; return *value; }
thread_local bool observing = false;
bool contained(const std::string &path, const std::string &root)
{ return !root.empty() && path.compare(0, root.size() + 1, root + '/') == 0; }
void update(Observation &value, const Key &key, long long size)
{
    value.current += size - value.files[key];
    value.files[key] = size;
    value.peak = std::max(value.peak, value.current);
}
void forget(Observation &value, const Key &key)
{
    const auto found = value.files.find(key);
    if (found != value.files.end()) { value.current -= found->second; value.files.erase(found); }
}
bool openWriter(const Observation &value, const Key &key)
{
    for (const auto &[fd, inode] : value.writers) if (inode == key) return true;
    return false;
}
void observeWrite(int fd)
{
    if (observing) return;
    observing = true;
    auto &value = observation();
    {
        std::lock_guard<std::mutex> lock(value.mutex);
        if (!value.root.empty()) {
            char link[64], path[4096];
            std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
            const auto size = ::readlink(link, path, sizeof(path) - 1);
            struct stat info {};
            if (size >= 0) {
                path[size] = 0;
                if (contained(path, value.root) && ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
                    const Key key{info.st_dev, info.st_ino};
                    value.writers[fd] = key;
                    update(value, key, info.st_size);
                }
            }
        }
    }
    observing = false;
}
void removed(const struct stat &info)
{
    if (observing || !S_ISREG(info.st_mode) || info.st_nlink > 1) return;
    observing = true;
    auto &value = observation();
    {
        std::lock_guard<std::mutex> lock(value.mutex);
        const Key key{info.st_dev, info.st_ino};
        if (!openWriter(value, key)) forget(value, key);
    }
    observing = false;
}
}

extern "C" void omacustos_staging_observer_reset(const char *root)
{
    observing = true;
    auto &value = observation();
    { std::lock_guard<std::mutex> lock(value.mutex);
      value.root = root; value.files.clear(); value.writers.clear(); value.current = value.peak = 0; }
    observing = false;
}

// Provider copies can use kernel reflinks/copy_file_range instead of write(2).
// The serial fixture calls this at both sides of every provider operation. No
// staging bytes are released between a provider copy and its ending observation.
extern "C" void omacustos_staging_observer_sample()
{
    observing = true;
    auto &value = observation();
    {
        std::lock_guard<std::mutex> lock(value.mutex);
        std::map<Key, long long> files;
        std::error_code error;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(value.root, error)) {
            struct stat info {};
            if (::lstat(entry.path().c_str(), &info) == 0 && S_ISREG(info.st_mode)) files[{info.st_dev, info.st_ino}] = info.st_size;
        }
        for (const auto &[fd, key] : value.writers) {
            struct stat info {};
            if (::fstat(fd, &info) == 0 && info.st_nlink == 0) files[key] = info.st_size;
        }
        value.files = std::move(files); value.current = 0;
        for (const auto &[key, size] : value.files) value.current += size;
        value.peak = std::max(value.peak, value.current);
    }
    observing = false;
}

extern "C" long long omacustos_staging_observer_peak()
{
    auto &value = observation();
    std::lock_guard<std::mutex> lock(value.mutex);
    return value.peak;
}

extern "C" ssize_t write(int fd, const void *buffer, size_t count)
{
    static auto real = reinterpret_cast<ssize_t (*)(int, const void *, size_t)>(dlsym(RTLD_NEXT, "write"));
    const auto result = real(fd, buffer, count); const int saved = errno;
    if (result > 0) observeWrite(fd);
    errno = saved; return result;
}
extern "C" ssize_t pwrite(int fd, const void *buffer, size_t count, off_t offset)
{
    static auto real = reinterpret_cast<ssize_t (*)(int, const void *, size_t, off_t)>(dlsym(RTLD_NEXT, "pwrite"));
    const auto result = real(fd, buffer, count, offset); const int saved = errno;
    if (result > 0) observeWrite(fd);
    errno = saved; return result;
}
extern "C" int ftruncate(int fd, off_t size)
{
    static auto real = reinterpret_cast<int (*)(int, off_t)>(dlsym(RTLD_NEXT, "ftruncate"));
    const auto result = real(fd, size); const int saved = errno;
    if (result == 0) observeWrite(fd);
    errno = saved; return result;
}
extern "C" int close(int fd)
{
    static auto real = reinterpret_cast<int (*)(int)>(dlsym(RTLD_NEXT, "close"));
    struct stat info {}; const bool unlinked = !observing && ::fstat(fd, &info) == 0 && info.st_nlink == 0;
    const auto result = real(fd); const int saved = errno;
    if (!observing && result == 0) {
        observing = true;
        auto &value = observation();
        { std::lock_guard<std::mutex> lock(value.mutex);
          value.writers.erase(fd);
          const Key key{info.st_dev, info.st_ino};
          if (unlinked && !openWriter(value, key)) forget(value, key); }
        observing = false;
    }
    errno = saved; return result;
}
extern "C" int unlink(const char *path)
{
    static auto real = reinterpret_cast<int (*)(const char *)>(dlsym(RTLD_NEXT, "unlink"));
    struct stat info {}; const bool known = !observing && ::lstat(path, &info) == 0;
    const auto result = real(path); const int saved = errno;
    if (result == 0 && known) removed(info);
    errno = saved; return result;
}
extern "C" int unlinkat(int fd, const char *path, int flags)
{
    static auto real = reinterpret_cast<int (*)(int, const char *, int)>(dlsym(RTLD_NEXT, "unlinkat"));
    struct stat info {}; const bool known = !observing && ::fstatat(fd, path, &info, AT_SYMLINK_NOFOLLOW) == 0;
    const auto result = real(fd, path, flags); const int saved = errno;
    if (result == 0 && known) removed(info);
    errno = saved; return result;
}
extern "C" int rename(const char *source, const char *destination)
{
    static auto real = reinterpret_cast<int (*)(const char *, const char *)>(dlsym(RTLD_NEXT, "rename"));
    struct stat info {}; const bool known = !observing && ::lstat(destination, &info) == 0;
    const auto result = real(source, destination); const int saved = errno;
    if (result == 0 && known) removed(info);
    errno = saved; return result;
}
