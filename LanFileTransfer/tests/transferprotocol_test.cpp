#include "transferprotocol.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>

namespace {
bool require(bool condition, const char *message)
{
    if (!condition) qCritical() << message;
    return condition;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QByteArray expected(32, 'a');
    const QByteArray different(32, 'b');

    if (!require(TransferProtocol::isCompletionVerified(true, expected, expected),
                 "matching accepted checksum was rejected")) return 1;
    if (!require(!TransferProtocol::isCompletionVerified(false, expected, expected),
                 "peer rejection was treated as verified")) return 2;
    if (!require(!TransferProtocol::isCompletionVerified(true, different, expected),
                 "mismatched checksum was treated as verified")) return 3;
    if (!require(!TransferProtocol::isCompletionVerified(true, QByteArray(31, 'a'), expected),
                 "malformed returned checksum was treated as verified")) return 4;
    if (!require(!TransferProtocol::isCompletionVerified(true, expected, {}),
                 "missing expected checksum was treated as verified")) return 5;
    return 0;
}
