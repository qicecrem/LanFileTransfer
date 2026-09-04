#pragma once

#include <QByteArray>

namespace TransferProtocol {

inline bool isCompletionVerified(bool peerAccepted,
                                 const QByteArray &returnedHash,
                                 const QByteArray &expectedHash) noexcept
{
    return peerAccepted
        && expectedHash.size() == 32
        && returnedHash.size() == 32
        && returnedHash == expectedHash;
}

}
