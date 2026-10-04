#pragma once

#include "backupprovider.h"

namespace PayloadMetadataPolicy {

// Existing bytes need positive checksum evidence before an upload can be skipped.
inline bool matchesForReuse(const RemoteFile &remote, qint64 size, const QByteArray &checksum)
{
    return remote.size == size && !remote.checksum.isEmpty() && remote.checksum == checksum;
}

// After transfer, size-only provider metadata is sufficient, but a supplied
// checksum must match. This also governs catalog restore/retention eligibility.
inline bool matchesAfterTransfer(const RemoteFile &remote, qint64 size, const QByteArray &checksum)
{
    return remote.size == size && (remote.checksum.isEmpty() || remote.checksum == checksum);
}

}
