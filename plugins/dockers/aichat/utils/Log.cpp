/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Log.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

namespace AiChat {

void writeLog(const QString &line)
{
    static QMutex mutex;
    QMutexLocker locker(&mutex);

    const QString text = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz")) + QLatin1Char(' ') + line;
    qInfo().noquote() << "AI Chat:" << text;

    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("aichat/aichat.log"));
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (file.size() > 1024 * 1024 && file.open(QIODevice::ReadOnly)) {
        file.seek(file.size() - 256 * 1024); // keep the last part
        const QByteArray tail = file.readAll();
        file.close();
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(tail);
            file.close();
        }
    }
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        file.write(text.toUtf8() + '\n');
    }
}

} // namespace AiChat
