// Inject write/quota exhaustion without filling the real filesystem. Only the
// engine's owned payload paths are affected, never configuration/checkpoints.
#include <dlfcn.h>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <string>
#include <filesystem>
#include <algorithm>
#include <sys/stat.h>
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
    const char *totalCapacity = std::getenv("FIXTURE_TOTAL_STAGING_CAPACITY");
    const char *peakLog = std::getenv("FIXTURE_STAGING_PEAK_LOG");
    const char *archiveIO = std::getenv("FIXTURE_ARCHIVE_WRITE_EIO_AFTER");
    const char *block = std::getenv("FIXTURE_BLOCK_WORKER_PHASE");
    const char *reuseQuota = std::getenv("FIXTURE_REUSE_DOWNLOAD_QUOTA");
    if (quota || checkpoint || capacity || archiveQuota || mutate || totalCapacity || peakLog || archiveIO || block || reuseQuota) {
        char link[64], path[4096];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        const ssize_t length = ::readlink(link, path, sizeof(path) - 1);
        if (length >= 0) {
            path[length] = 0;
            if (reuseQuota && std::strstr(path, "/attempt-") && std::strstr(path, "/.omacustos-download-")
                && std::strstr(path, ".tar.gz")) {
                errno = EDQUOT;
                return -1;
            }
            if (block) {
                const std::string_view bytes(static_cast<const char *>(buffer), count);
                const bool preparation = std::strcmp(block, "preparation") == 0
                    && std::strstr(path, "/attempt-") && std::strstr(path, "/payloads/");
                const bool compression = std::strcmp(block, "compression") == 0
                    && std::strstr(path, "/attempt-") && std::strstr(path, ".tar.gz");
                const bool checkpointing = std::strcmp(block, "checkpoint") == 0
                    && std::strstr(path, "/continuations/") && bytes.find("\"verified\":true") != std::string_view::npos;
                static int boundaries = 0;
                if ((preparation || compression || checkpointing) && ++boundaries == 2) {
                    const char *marker = std::getenv("FIXTURE_MARKER");
                    const int file = ::open(marker, O_WRONLY | O_CREAT, 0600);
                    if (file >= 0) { realWrite(file, path, length); ::close(file); }
                    while (true) ::usleep(50000);
                }
            }
            if (archiveIO && std::strstr(path, "/attempt-") && std::strstr(path, ".tar.gz")) {
                static std::string previousArchive;
                static int archives = 0;
                if (previousArchive != path) { previousArchive = path; ++archives; }
                if (archives > std::atoi(archiveIO)) { errno = EIO; return -1; }
            }
            if ((totalCapacity || peakLog) && std::strstr(path, "/attempt-")) {
                const char *attempt = std::strstr(path, "/attempt-");
                const char *end = std::strchr(attempt + 1, '/');
                if (end) {
                    const std::filesystem::path root(std::string(path, end - path));
                    std::error_code error;
                    long long used = 0;
                    for (const auto &entry : std::filesystem::recursive_directory_iterator(root, error)) {
                        if (entry.is_regular_file(error)) used += entry.file_size(error);
                    }
                    const off_t position = ::lseek(fd, 0, SEEK_CUR);
                    struct stat current {};
                    if (position >= 0 && ::fstat(fd, &current) == 0)
                        used += std::max(0LL, static_cast<long long>(position + count) - current.st_size);
                    static long long peak = 0;
                    if (peakLog && used > peak) {
                        peak = used;
                        const int log = ::open(peakLog, O_WRONLY | O_CREAT | O_APPEND, 0600);
                        if (log >= 0) {
                            char bytes[64];
                            const int size = std::snprintf(bytes, sizeof(bytes), "%lld\n", peak);
                            realWrite(log, bytes, size);
                            ::close(log);
                        }
                    }
                    if (totalCapacity && used > std::atoll(totalCapacity)) { errno = EDQUOT; return -1; }
                }
            }
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

// QFile's unbuffered sink normally has nothing left to flush. Inject the same
// libarchive close-callback error contract to exercise a failed stream flush.
struct archive;
extern "C" int archive_write_close(struct archive *writer)
{
    static const auto realClose = reinterpret_cast<int (*)(struct archive *)>(dlsym(RTLD_NEXT, "archive_write_close"));
    const int status = realClose(writer);
    const char *after = std::getenv("FIXTURE_ARCHIVE_FLUSH_EIO_AFTER");
    static int archives = 0;
    if (after && status == 0 && ++archives > std::atoi(after)) {
        const auto setError = reinterpret_cast<void (*)(struct archive *, int, const char *, ...)>(dlsym(RTLD_NEXT, "archive_set_error"));
        setError(writer, EIO, "Archive staging flush failed: injected input/output error");
        return -30; // ARCHIVE_FATAL
    }
    return status;
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
