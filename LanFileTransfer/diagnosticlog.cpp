#include "diagnosticlog.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QUuid>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
constexpr qint64 MaxLogSize = 2 * 1024 * 1024;
constexpr int LogFileCount = 5;
constexpr qint64 MaxBundleFileSize = 16 * 1024 * 1024;
QMutex logMutex;
QString diagnosticsRoot;
QString logsDirectory;
QString crashesDirectory;
QString logPath;
QString sessionPath;
QString sessionId;
qint64 logBytes = 0;
QtMessageHandler previousHandler = nullptr;
std::atomic_bool installed = false;

#ifdef Q_OS_WIN
wchar_t nativeCrashDirectory[32768] = {};
#else
char nativeSignalPath[4096] = {};
#endif

QString levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return QStringLiteral("debug");
    case QtInfoMsg: return QStringLiteral("info");
    case QtWarningMsg: return QStringLiteral("warning");
    case QtCriticalMsg: return QStringLiteral("error");
    case QtFatalMsg: return QStringLiteral("fatal");
    }
    return QStringLiteral("unknown");
}

QString sanitized(QString message)
{
    const QString home = QDir::toNativeSeparators(QDir::homePath());
    const QString appData = QDir::toNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (!appData.isEmpty()) message.replace(appData, QStringLiteral("<app-data>"), Qt::CaseInsensitive);
    if (!home.isEmpty()) message.replace(home, QStringLiteral("<home>"), Qt::CaseInsensitive);
    static const QRegularExpression contentUri(QStringLiteral(R"(content://[^\s"']+)"));
    message.replace(contentUri, QStringLiteral("content://<redacted>"));
    static const QRegularExpression windowsPath(
        QStringLiteral(R"((?<![\w])(?:[A-Za-z]:[\\/])[^\s"']+)"));
    message.replace(windowsPath, QStringLiteral("<path>"));
    return message;
}

void rotateLogsLocked()
{
    QFile::remove(logPath + QStringLiteral(".%1").arg(LogFileCount - 1));
    for (int index = LogFileCount - 2; index >= 1; --index) {
        const QString source = logPath + QStringLiteral(".%1").arg(index);
        if (QFileInfo::exists(source))
            QFile::rename(source, logPath + QStringLiteral(".%1").arg(index + 1));
    }
    if (QFileInfo::exists(logPath)) QFile::rename(logPath, logPath + QStringLiteral(".1"));
    logBytes = 0;
}

void appendJsonLine(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QJsonObject object;
    object.insert(QStringLiteral("ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("level"), levelName(type));
    object.insert(QStringLiteral("category"), QString::fromUtf8(
        context.category ? context.category : "default"));
    object.insert(QStringLiteral("thread"), QStringLiteral("0x%1").arg(
        quintptr(QThread::currentThreadId()), 0, 16));
    object.insert(QStringLiteral("message"), sanitized(message));
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    QMutexLocker locker(&logMutex);
    if (logPath.isEmpty()) return;
    if (logBytes + line.size() > MaxLogSize) rotateLogsLocked();
    QFile file(logPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    file.write(line);
    file.flush();
    logBytes += line.size();
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    appendJsonLine(type, context, message);
    if (previousHandler) previousHandler(type, context, message);
    if (type == QtFatalMsg) std::abort();
}

QJsonObject runtimeMetadata()
{
    QJsonObject metadata;
    metadata.insert(QStringLiteral("formatVersion"), 1);
    metadata.insert(QStringLiteral("generatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    metadata.insert(QStringLiteral("sessionId"), sessionId);
    metadata.insert(QStringLiteral("application"), QCoreApplication::applicationName());
    metadata.insert(QStringLiteral("applicationVersion"), QCoreApplication::applicationVersion());
    metadata.insert(QStringLiteral("qtVersion"), QString::fromLatin1(qVersion()));
    metadata.insert(QStringLiteral("os"), QSysInfo::prettyProductName());
    metadata.insert(QStringLiteral("kernel"), QSysInfo::kernelType() + QLatin1Char(' ') + QSysInfo::kernelVersion());
    metadata.insert(QStringLiteral("cpuArchitecture"), QSysInfo::currentCpuArchitecture());
    metadata.insert(QStringLiteral("buildArchitecture"), QSysInfo::buildCpuArchitecture());
    metadata.insert(QStringLiteral("pid"), qint64(QCoreApplication::applicationPid()));
    return metadata;
}

void writeJson(const QString &filePath, const QJsonObject &object)
{
    QFile file(filePath);
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
        file.flush();
    }
}

quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xffffffffU;
    for (const uchar byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & quint32(-qint32(crc & 1U)));
    }
    return crc ^ 0xffffffffU;
}

struct ZipEntry {
    QByteArray name;
    QByteArray data;
    quint32 crc = 0;
    quint32 offset = 0;
};

bool writeZip(const QString &path, QList<ZipEntry> entries, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = file.errorString();
        return false;
    }
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    const QDateTime now = QDateTime::currentDateTime();
    const QDate date = now.date();
    const QTime time = now.time();
    const quint16 dosTime = quint16((time.hour() << 11) | (time.minute() << 5) | (time.second() / 2));
    const quint16 dosDate = quint16(((qMax(1980, date.year()) - 1980) << 9)
                                    | (date.month() << 5) | date.day());
    for (ZipEntry &entry : entries) {
        entry.crc = crc32(entry.data);
        entry.offset = quint32(file.pos());
        out << quint32(0x04034b50) << quint16(20) << quint16(0x0800) << quint16(0)
            << dosTime << dosDate << entry.crc << quint32(entry.data.size())
            << quint32(entry.data.size()) << quint16(entry.name.size()) << quint16(0);
        out.writeRawData(entry.name.constData(), entry.name.size());
        out.writeRawData(entry.data.constData(), entry.data.size());
    }
    const quint32 centralOffset = quint32(file.pos());
    for (const ZipEntry &entry : std::as_const(entries)) {
        out << quint32(0x02014b50) << quint16(20) << quint16(20) << quint16(0x0800)
            << quint16(0) << dosTime << dosDate << entry.crc << quint32(entry.data.size())
            << quint32(entry.data.size()) << quint16(entry.name.size()) << quint16(0)
            << quint16(0) << quint16(0) << quint16(0) << quint32(0) << entry.offset;
        out.writeRawData(entry.name.constData(), entry.name.size());
    }
    const quint32 centralSize = quint32(file.pos()) - centralOffset;
    out << quint32(0x06054b50) << quint16(0) << quint16(0) << quint16(entries.size())
        << quint16(entries.size()) << centralSize << centralOffset << quint16(0);
    file.flush();
    if (out.status() == QDataStream::Ok) return true;
    if (error) *error = QStringLiteral("Unable to write ZIP archive");
    return false;
}

#ifdef Q_OS_WIN
LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *exception)
{
    wchar_t dumpPath[32768] = {};
    wchar_t textPath[32768] = {};
    swprintf_s(dumpPath, L"%s\\native-crash.dmp", nativeCrashDirectory);
    swprintf_s(textPath, L"%s\\native-crash.txt", nativeCrashDirectory);
    HANDLE dump = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION information;
        information.ThreadId = GetCurrentThreadId();
        information.ExceptionPointers = exception;
        information.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, MiniDumpNormal,
                          &information, nullptr, nullptr);
        CloseHandle(dump);
    }
    HANDLE text = CreateFileW(textPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (text != INVALID_HANDLE_VALUE) {
        char buffer[256] = {};
        const int length = sprintf_s(buffer, "Unhandled Windows exception 0x%08lx at %p\r\n",
            exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress);
        DWORD written = 0;
        WriteFile(text, buffer, DWORD(qMax(0, length)), &written, nullptr);
        CloseHandle(text);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
void nativeSignalHandler(int signalNumber)
{
    const int descriptor = ::open(nativeSignalPath, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (descriptor >= 0) {
        static constexpr char message[] = "Native fatal signal. See landrop.log for preceding events.\n";
        ::write(descriptor, message, sizeof(message) - 1);
        ::close(descriptor);
    }
    std::signal(signalNumber, SIG_DFL);
    std::raise(signalNumber);
}
#endif

void terminateHandler()
{
    QString reason = QStringLiteral("Unhandled C++ exception");
    if (const auto exception = std::current_exception()) {
        try { std::rethrow_exception(exception); }
        catch (const std::exception &error) { reason += QStringLiteral(": ") + QString::fromUtf8(error.what()); }
        catch (...) { reason += QStringLiteral(": unknown exception"); }
    }
    appendJsonLine(QtCriticalMsg, QMessageLogContext(__FILE__, __LINE__, Q_FUNC_INFO, "crash.cpp"), reason);
    std::abort();
}
}

void DiagnosticLog::install(const QString &baseDirectory)
{
    if (installed.exchange(true)) return;
    diagnosticsRoot = baseDirectory.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
              .filePath(QStringLiteral("diagnostics"))
        : baseDirectory;
    logsDirectory = QDir(diagnosticsRoot).filePath(QStringLiteral("logs"));
    crashesDirectory = QDir(diagnosticsRoot).filePath(QStringLiteral("crashes"));
    QDir().mkpath(logsDirectory);
    QDir().mkpath(crashesDirectory);
    logPath = QDir(logsDirectory).filePath(QStringLiteral("landrop.log"));
    logBytes = QFileInfo(logPath).size();
    if (logBytes >= MaxLogSize) {
        QMutexLocker locker(&logMutex);
        rotateLogsLocked();
    }
    const QString previousSession = QDir(diagnosticsRoot).filePath(QStringLiteral("session.json"));
    QFile previous(previousSession);
    if (previous.open(QIODevice::ReadOnly)) {
        const QJsonObject object = QJsonDocument::fromJson(previous.readAll()).object();
        if (object.value(QStringLiteral("status")).toString() == QStringLiteral("running")) {
            QJsonObject unclean = object;
            unclean.insert(QStringLiteral("detectedAt"),
                           QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            writeJson(QDir(crashesDirectory).filePath(QStringLiteral("previous-unclean-exit.json")), unclean);
        }
    }
    sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sessionPath = previousSession;
    QJsonObject session = runtimeMetadata();
    session.insert(QStringLiteral("status"), QStringLiteral("running"));
    session.insert(QStringLiteral("startedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    writeJson(sessionPath, session);

#ifdef Q_OS_WIN
    const std::wstring nativePath = QDir::toNativeSeparators(crashesDirectory).toStdWString();
    wcsncpy_s(nativeCrashDirectory, nativePath.c_str(), _TRUNCATE);
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
#else
    const QByteArray signalFile = QFile::encodeName(
        QDir(crashesDirectory).filePath(QStringLiteral("native-signal.txt")));
    const qsizetype copyLength = qMin<qsizetype>(signalFile.size(), sizeof(nativeSignalPath) - 1);
    std::memcpy(nativeSignalPath, signalFile.constData(), size_t(copyLength));
    nativeSignalPath[copyLength] = '\0';
    std::signal(SIGABRT, nativeSignalHandler);
    std::signal(SIGSEGV, nativeSignalHandler);
    std::signal(SIGILL, nativeSignalHandler);
    std::signal(SIGFPE, nativeSignalHandler);
#ifdef SIGBUS
    std::signal(SIGBUS, nativeSignalHandler);
#endif
#endif
    std::set_terminate(terminateHandler);
    previousHandler = qInstallMessageHandler(messageHandler);
    qInfo() << "Diagnostic logging initialized" << "session" << sessionId
            << "Qt" << qVersion() << "OS" << QSysInfo::prettyProductName();
}

void DiagnosticLog::markCleanShutdown()
{
    if (!installed || sessionPath.isEmpty()) return;
    QJsonObject session = runtimeMetadata();
    session.insert(QStringLiteral("status"), QStringLiteral("clean"));
    session.insert(QStringLiteral("finishedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    writeJson(sessionPath, session);
    qInfo() << "Diagnostic session closed cleanly" << sessionId;
    // Qt and platform plugins may emit messages while global objects are being
    // destroyed. Stop routing them through this module before its own static
    // QStrings and mutex are torn down.
    qInstallMessageHandler(previousHandler);
    previousHandler = nullptr;
}

QString DiagnosticLog::path()
{
    QMutexLocker locker(&logMutex);
    return logPath;
}

QString DiagnosticLog::directory()
{
    QMutexLocker locker(&logMutex);
    return diagnosticsRoot;
}

QString DiagnosticLog::createBundle(const QString &destinationDirectory, QString *error)
{
    if (!installed) {
        if (error) *error = QStringLiteral("Diagnostic logging is not initialized");
        return {};
    }
    const QString destination = destinationDirectory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
        : destinationDirectory;
    if (!QDir().mkpath(destination)) {
        if (error) *error = QStringLiteral("Unable to create destination directory");
        return {};
    }
    QList<ZipEntry> entries;
    entries.append({QByteArrayLiteral("metadata.json"),
                    QJsonDocument(runtimeMetadata()).toJson(QJsonDocument::Indented)});
    entries.append({QByteArrayLiteral("README.txt"), QByteArrayLiteral(
        "LanDrop diagnostic package. Logs are JSON Lines. Chat history, message bodies, "
        "trusted-device keys and transfer databases are intentionally excluded.\n")});
    const auto appendDirectory = [&entries](const QString &root, const QString &prefix) {
        QDir directory(root);
        for (const QFileInfo &info : directory.entryInfoList(QDir::Files, QDir::Time)) {
            if (info.size() > MaxBundleFileSize) continue;
            QFile file(info.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly))
                entries.append({(prefix + QLatin1Char('/') + info.fileName()).toUtf8(), file.readAll()});
        }
    };
    {
        QMutexLocker locker(&logMutex);
        appendDirectory(logsDirectory, QStringLiteral("logs"));
    }
    appendDirectory(crashesDirectory, QStringLiteral("crashes"));
    QFile session(sessionPath);
    if (session.open(QIODevice::ReadOnly))
        entries.append({QByteArrayLiteral("session.json"), session.readAll()});

    const QString fileName = QStringLiteral("LanDrop-diagnostics-%1.zip")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString result = QDir(destination).filePath(fileName);
    if (!writeZip(result, entries, error)) return {};
    qInfo() << "Diagnostic package created" << QFileInfo(result).fileName()
            << "entries" << entries.size();
    return result;
}
