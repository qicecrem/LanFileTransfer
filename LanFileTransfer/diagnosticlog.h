#pragma once

#include <QString>

namespace DiagnosticLog {
void install(const QString &baseDirectory = {});
void markCleanShutdown();
QString path();
QString directory();
QString createBundle(const QString &destinationDirectory = {}, QString *error = nullptr);
}
