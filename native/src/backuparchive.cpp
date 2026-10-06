#include "backuparchive.h"

#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QScopeGuard>
#include <QSet>
#include <cerrno>
#include <cstring>

namespace BackupArchiveIO {
namespace {
// A tar reader may stop at its end marker before consuming the gzip trailer.
// Independently require a complete single gzip stream (CRC/ISIZE included) and
// two tar end blocks; no truncated/concatenated/trailing stream is certified.
bool completeGzipTar(const QString &path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return false;
    z_stream stream {};
    if (inflateInit2(&stream, 31) != Z_OK) return false;
    const auto release = qScopeGuard([&] { inflateEnd(&stream); });
    QByteArray compressed;
    QByteArray tail;
    qint64 total = 0;
    char buffer[64 * 1024];
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stream.avail_in == 0) {
            compressed = input.read(64 * 1024);
            if (compressed.isEmpty() || input.error() != QFileDevice::NoError) return false;
            stream.next_in = reinterpret_cast<Bytef *>(compressed.data());
            stream.avail_in = compressed.size();
        }
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof(buffer);
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) return false;
        const int count = sizeof(buffer) - stream.avail_out;
        total += count;
        tail.append(buffer, count);
        if (tail.size() > 1024) tail = tail.right(1024);
    }
    return stream.avail_in == 0 && input.atEnd() && total % 512 == 0
        && tail == QByteArray(1024, '\0');
}
}
bool safeMember(const QString &path)
{
    return !path.isEmpty() && !path.startsWith('/') && !path.contains('\\')
        && !path.contains(QChar::Null) && QDir::cleanPath(path) == path
        && path != "." && !path.split('/').contains("..");
}

bool verify(const QString &path, qint64 size, const QByteArray &checksum)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() != size) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) && hash.result() == checksum;
}

bool pack(const QString &tree, const QString &remoteRoot, const QString &output, qint64 maxBytes,
    QVector<BackupEntry> *entries, QString *error, const std::function<bool()> &stopped)
{
    struct archive *writer = archive_write_new();
    struct Output { QFile file; qint64 limit; } sink {QFile(output), maxBytes};
    const auto release = qScopeGuard([&] { archive_write_free(writer); });
    const auto fail = [&] {
        const int code = archive_errno(writer) ? archive_errno(writer) : errno;
        if (error) *error = QStringLiteral("Archive preparation failed: %1 (%2)").arg(
            QString::fromUtf8(archive_error_string(writer) ? archive_error_string(writer) : "source/staging read or write failure"),
            QString::fromLocal8Bit(std::strerror(code)));
        return false;
    };
    if (!sink.file.open(QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        if (error) *error = QStringLiteral("Archive staging could not be opened: %1").arg(sink.file.errorString());
        return false;
    }
    const auto write = [](struct archive *archive, void *context, const void *buffer, size_t count) -> la_ssize_t {
        auto &output = *static_cast<Output *>(context);
        if (qint64(count) > output.limit - output.file.pos()) {
            archive_set_error(archive, ENOSPC, "Archive exceeded its staging allowance");
            return -1;
        }
        const qint64 written = output.file.write(static_cast<const char *>(buffer), count);
        if (written != qint64(count)) {
            archive_set_error(archive, errno ? errno : EIO, "Archive staging write failed: %s", output.file.errorString().toUtf8().constData());
            return -1;
        }
        return written;
    };
    const auto close = [](struct archive *archive, void *context) -> int {
        auto &output = *static_cast<Output *>(context);
        if (!output.file.flush()) {
            archive_set_error(archive, errno ? errno : EIO, "Archive staging flush failed: %s", output.file.errorString().toUtf8().constData());
            return ARCHIVE_FATAL;
        }
        return ARCHIVE_OK;
    };
    if (archive_write_set_format_pax_restricted(writer) != ARCHIVE_OK
        || archive_write_set_bytes_in_last_block(writer, 1) != ARCHIVE_OK
        || archive_write_add_filter_gzip(writer) != ARCHIVE_OK
        || archive_write_set_filter_option(writer, "gzip", "compression-level", "3") != ARCHIVE_OK
        || archive_write_open(writer, &sink, nullptr, write, close) != ARCHIVE_OK) return fail();
    for (BackupEntry &entry : *entries) {
        if (stopped && stopped()) return fail();
        const QString member = entry.remotePath.mid(remoteRoot.size() + 1);
        if (!safeMember(member)) return fail();
        QFile file(QDir(tree).filePath(member));
        if (!file.open(QIODevice::ReadOnly) || file.size() != entry.size) return fail();
        struct archive_entry *header = archive_entry_new();
        const auto freeHeader = qScopeGuard([&] { archive_entry_free(header); });
        archive_entry_set_pathname_utf8(header, member.toUtf8().constData());
        archive_entry_set_filetype(header, AE_IFREG);
        archive_entry_set_perm(header, 0600);
        archive_entry_set_size(header, entry.size);
        if (archive_write_header(writer, header) != ARCHIVE_OK) return fail();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 size = 0;
        while (!file.atEnd()) {
            if (stopped && stopped()) return fail();
            const QByteArray bytes = file.read(1024 * 1024);
            if (file.error() != QFileDevice::NoError || bytes.isEmpty()
                || archive_write_data(writer, bytes.constData(), bytes.size()) != bytes.size()) return fail();
            size += bytes.size();
            hash.addData(bytes);
        }
        if (size != entry.size || hash.result() != entry.checksum
            || archive_write_finish_entry(writer) != ARCHIVE_OK) return fail();
        entry.memberPath = member;
    }
    if (archive_write_close(writer) != ARCHIVE_OK) return fail();
    return true;
}

bool extract(const QString &input, const QVector<BackupEntry> &selected,
    const QString &workspace, QVector<BackupEntry> *payloads, QString *error)
{
    struct archive *reader = archive_read_new();
    const auto release = qScopeGuard([&] { archive_read_free(reader); });
    const auto fail = [&] {
        if (error) *error = QStringLiteral("The archive is unsafe, damaged, or failed member verification.");
        payloads->clear();
        return false;
    };
    if (!completeGzipTar(input)) return fail();
    archive_read_support_filter_gzip(reader);
    archive_read_support_format_tar(reader);
    // Validate headers beyond an early tar end marker as well; appended members
    // cannot hide outside the authoritative index in the same gzip stream.
    if (archive_read_set_format_option(reader, "tar", "read_concatenated_archives", "1") != ARCHIVE_OK) return fail();
    if (archive_read_open_filename(reader, QFile::encodeName(input).constData(), 1024 * 1024) != ARCHIVE_OK) return fail();
    QHash<QString, BackupEntry> wanted;
    const QStringList indexed = selected.isEmpty() ? QStringList() : selected.first().archive.members;
    const QSet<QString> allowed(indexed.cbegin(), indexed.cend());
    if (allowed.isEmpty()) return fail();
    for (const auto &entry : selected) {
        if (!safeMember(entry.memberPath) || wanted.contains(entry.memberPath)) return fail();
        wanted.insert(entry.memberPath, entry);
    }
    QSet<QString> seen;
    struct archive_entry *header;
    int status;
    while ((status = archive_read_next_header(reader, &header)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname_utf8(header);
        if (!name) return fail();
        const QString member = QString::fromUtf8(name);
        if (!safeMember(member) || !allowed.contains(member) || seen.contains(member) || archive_entry_filetype(header) != AE_IFREG
            || archive_entry_symlink(header) || archive_entry_hardlink(header)
            || archive_entry_size(header) < 0) return fail();
        seen.insert(member);
        const bool keep = wanted.contains(member);
        QFile output(QDir(workspace).filePath(QString::number(payloads->size())));
        if (keep && (!output.open(QIODevice::WriteOnly) || archive_entry_size(header) != wanted.value(member).size)) return fail();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 size = 0;
        char buffer[64 * 1024];
        la_ssize_t count;
        // Read even unselected data: skipped/truncated gzip must not certify a copy.
        while ((count = archive_read_data(reader, buffer, sizeof(buffer))) > 0) {
            size += count;
            if (keep) {
                if (size > wanted.value(member).size || output.write(buffer, count) != count) return fail();
                hash.addData(QByteArrayView(buffer, count));
            }
        }
        if (count < 0 || size != archive_entry_size(header)) return fail();
        if (keep) {
            BackupEntry entry = wanted.value(member);
            if (size != entry.size || hash.result() != entry.checksum || !output.flush()) return fail();
            entry.remotePath = output.fileName();
            entry.archive = {};
            entry.memberPath.clear();
            payloads->append(entry);
        }
    }
    if (status != ARCHIVE_EOF || archive_filter_code(reader, 0) != ARCHIVE_FILTER_GZIP
        || seen != allowed || payloads->size() != selected.size() || archive_read_close(reader) != ARCHIVE_OK) return fail();
    return true;
}
}
