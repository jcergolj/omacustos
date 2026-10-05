// Inject write/quota exhaustion without filling the real filesystem. Only the
// engine's owned payload paths are affected, never configuration/checkpoints.
#include <dlfcn.h>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <filesystem>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>

extern "C" ssize_t write(int fd, const void *buffer, size_t count)
{
    static const auto realWrite = reinterpret_cast<ssize_t (*)(int, const void *, size_t)>(dlsym(RTLD_NEXT, "write"));
    const char *quota = std::getenv("FIXTURE_STAGING_QUOTA");
    const char *checkpoint = std::getenv("FIXTURE_CHECKPOINT_FAILURE");
    const char *capacity = std::getenv("FIXTURE_STAGING_CAPACITY");
    if (quota || checkpoint || capacity) {
        char link[64], path[4096];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        const ssize_t length = ::readlink(link, path, sizeof(path) - 1);
        if (length >= 0) {
            path[length] = 0;
            if (checkpoint && std::strstr(path, "/continuations/")
                && std::string_view(static_cast<const char *>(buffer), count).find("\"verified\":true") != std::string_view::npos) {
                errno = EDQUOT;
                return -1;
            }
            if (quota && std::strstr(path, "/attempt-") && std::strstr(path, "/payloads/")) {
                const off_t position = ::lseek(fd, 0, SEEK_CUR);
                if (position >= 0 && position + off_t(count) > std::atoll(quota)) {
                    errno = EDQUOT;
                    return -1;
                }
            }
            if (capacity && std::strstr(path, "/attempt-")) {
                const char *payload = std::strstr(path, "/payloads/");
                if (payload) {
                    const std::filesystem::path root(std::string(path, payload - path + 9));
                    std::error_code error;
                    long long used = 0;
                    for (const auto &entry : std::filesystem::recursive_directory_iterator(root, error)) {
                        if (entry.is_regular_file(error)) used += entry.file_size(error);
                    }
                    if (used + static_cast<long long>(count) > std::atoll(capacity)) {
                        errno = EDQUOT;
                        return -1;
                    }
                }
            }
        }
    }
    return realWrite(fd, buffer, count);
}

extern "C" int statvfs(const char *path, struct statvfs *storage)
{
    static const auto realStat = reinterpret_cast<int (*)(const char *, struct statvfs *)>(dlsym(RTLD_NEXT, "statvfs"));
    const int status = realStat(path, storage);
    const char *space = std::getenv("FIXTURE_STAGING_AVAILABLE");
    if (status == 0 && space) {
        storage->f_frsize = storage->f_bsize = 1;
        storage->f_bavail = storage->f_bfree = std::atoll(space);
    }
    return status;
}

extern "C" int statfs64(const char *path, struct statfs64 *storage)
{
    static const auto realStat = reinterpret_cast<int (*)(const char *, struct statfs64 *)>(dlsym(RTLD_NEXT, "statfs64"));
    const int status = realStat(path, storage);
    const char *space = std::getenv("FIXTURE_STAGING_AVAILABLE");
    if (status == 0 && space) {
        storage->f_bsize = storage->f_frsize = 1;
        storage->f_bavail = storage->f_bfree = std::atoll(space);
    }
    return status;
}
