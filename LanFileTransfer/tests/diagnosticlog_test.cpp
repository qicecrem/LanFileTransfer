#include "diagnosticlog.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("DiagnosticLogTest"));
    app.setApplicationVersion(QStringLiteral("1.0"));
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    DiagnosticLog::install(temporary.path());
    qWarning().noquote() << "test path C:\\Users\\private\\secret.txt content://provider/private";

    QFile log(DiagnosticLog::path());
    if (!log.open(QIODevice::ReadOnly)) return 2;
    const QByteArray contents = log.readAll();
    if (!contents.contains("\"level\":\"warning\"")
        || contents.contains("secret.txt") || contents.contains("provider/private")) return 3;

    QString error;
    const QString bundle = DiagnosticLog::createBundle(temporary.path(), &error);
    QFile archive(bundle);
    if (bundle.isEmpty() || !archive.open(QIODevice::ReadOnly)) return 4;
    const QByteArray zip = archive.readAll();
    if (!zip.startsWith("PK\x03\x04") || !zip.contains("metadata.json")
        || !zip.contains("logs/landrop.log") || !zip.contains("README.txt")) return 5;
    DiagnosticLog::markCleanShutdown();
    return QFileInfo(bundle).size() > 0 ? 0 : 6;
}
