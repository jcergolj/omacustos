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
#include <fcntl.h>

extern "C" ssize_t write(int fd, const void *buffer, size_t count)
{
    static const auto realWrite = reinterpret_cast<ssize_t (*)(int, const void *, size_t)>(dlsym(RTLD_NEXT, "write"));
    const char *quota = std::getenv("FIXTURE_STAGING_QUOTA");
    const char *checkpoint = std::getenv("FIXTURE_CHECKPOINT_FAILURE");
    const char *capacity = std::getenv("FIXTURE_STAGING_CAPACITY");
    const char *archiveQuota = std::getenv("FIXTURE_ARCHIVE_QUOTA");
    const char *mutate = std::getenv("FIXTURE_MUTATE_DURING_STAGING");
    if (quota || checkpoint || capacity || archiveQuota || mutate) {
        char link[64], path[4096];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        const ssize_t length = ::readlink(link, path, sizeof(path) - 1);
        if (length >= 0) {
            path[length] = 0;
            if (archiveQuota && std::strstr(path, "/attempt-") && std::strstr(path, ".tar.gz")) {
                errno = EDQUOT;
                return -1;
            }
            static bool mutated = false;
            if (mutate && !mutated && std::strstr(path, "/attempt-") && std::strstr(path, "/payloads/")) {
                mutated = true;
                const ssize_t written = realWrite(fd, buffer, count);
                if (std::getenv("FIXTURE_REPLACE_DURING_STAGING")) {
                    ::unlink(mutate);
                    const int source = ::open(mutate, O_WRONLY | O_CREAT, 0600);
                    if (source >= 0) { realWrite(source, "replacement", 11); ::close(source); }
                } else {
                    const int source = ::open(mutate, O_WRONLY);
                    if (source >= 0) {
                        char changed[4096];
                        std::memset(changed, 'b', sizeof(changed));
                        for (off_t offset = 1024 * 1024; offset < 3 * 1024 * 1024; offset += sizeof(changed))
                            ::pwrite(source, changed, sizeof(changed), offset);
                        ::close(source);
                    }
                }
                return written;
            }
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
