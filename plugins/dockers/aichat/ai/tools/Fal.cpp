/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Fal.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <klocalizedstring.h>

#include "../../utils/Log.h"
#include "../../network/Stream.h"

namespace AiChat {

FalClient::FalClient(QObject *parent)
    : QObject(parent)
{
}

QString FalClient::statusText(const QJsonObject &status)
{
    const QString state = status.value("status").toString();
    if (state == QLatin1String("IN_QUEUE")) {
        const QJsonValue position = status.value("queue_position");
        return position.isDouble() ? i18n("Waiting in the queue (position %1)", position.toInt() + 1) : i18n("Waiting in the queue");
    }
    if (state == QLatin1String("IN_PROGRESS")) {
        return i18n("Topaz is working on the image");
    }
    if (state == QLatin1String("COMPLETED")) {
        return i18n("Downloading the result");
    }
    return i18n("Waiting for fal.ai");
}

namespace {
/// "topaz/upscale/image/precision" -> "topaz/upscale": the status and result live under the app, not the endpoint
QString appOf(const QString &endpoint)
{
    return endpoint.split(QLatin1Char('/'), Qt::SkipEmptyParts).mid(0, 2).join(QLatin1Char('/'));
}

QUrl queueChild(const QUrl &queue, const QString &path)
{
    QUrl url = queue;
    QString base = url.path();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    url.setPath(base + QLatin1Char('/') + path);
    return url;
}
} // namespace

QUrl FalClient::statusUrl(const QUrl &queue, const QString &endpoint, const QJsonObject &submitted)
{
    const QUrl sent(submitted.value("status_url").toString());
    if (sent.isValid() && !sent.isEmpty()) {
        return sent;
    }
    return queueChild(queue, QStringLiteral("%1/requests/%2/status").arg(appOf(endpoint), submitted.value("request_id").toString()));
}

QUrl FalClient::resultUrl(const QUrl &queue, const QString &endpoint, const QJsonObject &submitted)
{
    const QUrl sent(submitted.value("response_url").toString());
    if (sent.isValid() && !sent.isEmpty()) {
        return sent;
    }
    return queueChild(queue, QStringLiteral("%1/requests/%2").arg(appOf(endpoint), submitted.value("request_id").toString()));
}

bool FalClient::exchange(QNetworkAccessManager &manager, const QByteArray &verb, const QUrl &url, const QByteArray &body,
                               const QByteArray &apiKey, qint64 timeoutMs, int *status, QByteArray *data, QString *error,
                               const QByteArray &contentType)
{
    if (m_canceled && !m_ignoreCancel) {
        *error = i18n("Canceled.");
        return false;
    }
    if (timeoutMs <= 0) {
        *error = i18n("fal.ai did not finish in time.");
        return false;
    }

    QNetworkRequest request(url);
    if (!apiKey.isEmpty()) {
        request.setRawHeader("Authorization", "Key " + apiKey);
    }
    if (!body.isEmpty()) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(contentType));
    }

    QNetworkReply *reply = verb == "POST" ? manager.post(request, body) : verb == "PUT" ? manager.put(request, body) : manager.get(request);

    QEventLoop loop;
    m_loop = &loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;
    connect(&timeout, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(int(qMin<qint64>(timeoutMs, 0x7fffffff)));
    const bool canceledNow = m_canceled && !m_ignoreCancel;
    if (!reply->isFinished() && !canceledNow) {
        loop.exec();
    }
    m_loop = nullptr;

    bool ok = false;
    if (m_canceled && !m_ignoreCancel) {
        *error = i18n("Canceled.");
    } else if (timedOut) {
        *error = i18n("fal.ai did not answer in time.");
    } else if (!reply->isFinished()) {
        *error = i18n("Interrupted."); // cancel() while the clean-up runs
    } else {
        *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        *data = reply->readAll();
        if (*status >= 400) {
            *error = httpErrorMessage(*status, *data);
        } else if (reply->error() != QNetworkReply::NoError) {
            *error = reply->errorString();
        } else {
            ok = true;
        }
    }

    reply->disconnect();
    if (!reply->isFinished()) {
        reply->abort();
    }
    reply->deleteLater();
    return ok;
}

void FalClient::pause(int ms)
{
    if (m_canceled) {
        return;
    }
    QEventLoop loop;
    m_loop = &loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    if (!m_canceled) {
        loop.exec();
    }
    m_loop = nullptr;
}

QUrl FalClient::upload(QNetworkAccessManager &manager, const FalSettings &settings, const QByteArray &png,
                             qint64 timeoutMs, QString *error)
{
    int status = 0;
    QByteArray data;

    // 1. ask for a place: the answer has "upload_url" (where to PUT) and "file_url" (what the job gets)
    QJsonObject ask;
    ask["content_type"] = "image/png";
    ask["file_name"] = "krita.png";
    QUrl initiate = queueChild(settings.restUrl, QStringLiteral("storage/upload/initiate"));
    initiate.setQuery(QStringLiteral("storage_type=fal-cdn-v3"));
    if (!exchange(manager, "POST", initiate, QJsonDocument(ask).toJson(QJsonDocument::Compact), settings.apiKey.toUtf8(), timeoutMs,
                  &status, &data, error)) {
        return QUrl();
    }
    const QJsonObject answer = QJsonDocument::fromJson(data).object();
    const QUrl uploadUrl(answer.value("upload_url").toString());
    const QUrl fileUrl(answer.value("file_url").toString());
    if (!uploadUrl.isValid() || uploadUrl.isEmpty() || !fileUrl.isValid() || fileUrl.isEmpty()) {
        *error = i18n("fal.ai did not say where to upload the picture.");
        return QUrl();
    }

    // 2. put the bytes there (the address is signed: no key)
    if (!exchange(manager, "PUT", uploadUrl, png, QByteArray(), timeoutMs, &status, &data, error, "image/png")) {
        return QUrl();
    }
    return fileUrl;
}

QByteArray FalClient::run(const FalSettings &settings, const QString &endpoint, const QJsonObject &inputWithoutImage,
                                const ProgressFunction &progress, QString *error, int timeoutMs, int pollIntervalMs,
                                const QByteArray &pngImage)
{
    QJsonObject input = inputWithoutImage;
    if (!settings.isValid()) {
        *error = i18n("No fal.ai key is set up. Open Settings > Configure Krita > AI Chat.");
        return QByteArray();
    }

    m_canceled = false;
    QElapsedTimer total;
    total.start();
    const QByteArray key = settings.apiKey.toUtf8();
    auto remaining = [&]() { return qint64(timeoutMs) - total.elapsed(); };
    auto report = [&](const QString &text) {
        if (progress) {
            progress(text);
        }
    };
    auto fail = [&](const QString &step, const QString &reason) {
        writeLog(QStringLiteral("fal %1: %2 failed after %3 ms: %4").arg(endpoint, step).arg(total.elapsed()).arg(reason));
        *error = reason;
        return QByteArray();
    };

    // created here, so it belongs to the thread that calls run()
    QNetworkAccessManager manager;
    int httpStatus = 0;
    QByteArray data;

    // The picture in the file storage is removed again however the job ends (done, failed, canceled):
    // it is the user's picture, and it would stay on the CDN, public to anyone who has the address.
    QUrl uploadedFile;
    struct Cleanup {
        std::function<void()> function;
        ~Cleanup() { function(); }
    } cleanup {[&]() {
        if (uploadedFile.isEmpty()) {
            return;
        }
        report(i18n("Removing the image from fal.ai"));
        QJsonObject request;
        request["file_url"] = uploadedFile.toString();
        QString deleteError;
        int deleteStatus = 0;
        QByteArray deleteData;
        m_ignoreCancel = true;
        const bool deleted = exchange(manager, "POST", queueChild(settings.deleteUrl, QStringLiteral("storage/files/delete")),
                                      QJsonDocument(request).toJson(QJsonDocument::Compact), key, 15000, &deleteStatus, &deleteData,
                                      &deleteError);
        m_ignoreCancel = false;
        // not an error for the user, the result is already there; but the log tells (the key needs "assets:write")
        writeLog(deleted ? QStringLiteral("fal %1: removed the uploaded picture from the storage").arg(endpoint)
                          : QStringLiteral("fal %1: the uploaded picture could NOT be removed from the storage: %2").arg(endpoint, deleteError));
    }};

    // ---- the picture, into the file storage (or, if that does not work, into the job itself)
    report(i18n("Sending the image to fal.ai"));
    if (!pngImage.isEmpty()) {
        QString uploadError;
        const QUrl uploaded = upload(manager, settings, pngImage, remaining(), &uploadError);
        if (m_canceled) {
            return fail(QStringLiteral("upload"), i18n("Canceled."));
        }
        if (uploaded.isValid() && !uploaded.isEmpty()) {
            uploadedFile = uploaded;
            input["image_url"] = uploaded.toString();
            writeLog(QStringLiteral("fal %1: uploaded %2 KB at %3 ms").arg(endpoint).arg(pngImage.size() / 1024).arg(total.elapsed()));
        } else {
            writeLog(QStringLiteral("fal %1: upload failed (%2), the picture goes inside the job").arg(endpoint, uploadError));
            input["image_url"] = QStringLiteral("data:image/png;base64,%1").arg(QString::fromLatin1(pngImage.toBase64()));
        }
    }

    // ---- submit
    const QByteArray body = QJsonDocument(input).toJson(QJsonDocument::Compact);
    if (!exchange(manager, "POST", queueChild(settings.queueUrl, endpoint), body, key, remaining(), &httpStatus, &data, error)) {
        return fail(QStringLiteral("submit"), *error);
    }
    const QJsonObject submitted = QJsonDocument::fromJson(data).object();
    if (submitted.value("request_id").toString().isEmpty() && submitted.value("status_url").toString().isEmpty()) {
        return fail(QStringLiteral("submit"), i18n("fal.ai did not accept the job."));
    }
    const QUrl statusAddress = statusUrl(settings.queueUrl, endpoint, submitted);
    const QUrl resultAddress = resultUrl(settings.queueUrl, endpoint, submitted);
    const qint64 submitMs = total.elapsed();

    // ---- wait until it is done
    QString lastText;
    for (;;) {
        if (!exchange(manager, "GET", statusAddress, QByteArray(), key, remaining(), &httpStatus, &data, error)) {
            return fail(QStringLiteral("status"), *error);
        }
        const QJsonObject state = QJsonDocument::fromJson(data).object();
        const QString text = statusText(state);
        if (text != lastText) {
            lastText = text;
            report(text);
        }
        if (state.value("status").toString() == QLatin1String("COMPLETED")) {
            break;
        }
        pause(qMin<qint64>(pollIntervalMs, qMax<qint64>(remaining(), 0)));
        if (m_canceled) {
            return fail(QStringLiteral("status"), i18n("Canceled."));
        }
        if (remaining() <= 0) {
            return fail(QStringLiteral("status"), i18n("fal.ai did not finish in time."));
        }
    }
    const qint64 doneMs = total.elapsed();

    // ---- the result: where the picture is
    if (!exchange(manager, "GET", resultAddress, QByteArray(), key, remaining(), &httpStatus, &data, error)) {
        return fail(QStringLiteral("result"), *error);
    }
    const QJsonObject result = QJsonDocument::fromJson(data).object();
    const QUrl imageUrl(result.value("image").toObject().value("url").toString());
    if (!imageUrl.isValid() || imageUrl.isEmpty()) {
        const QString detail = result.value("detail").isString() ? result.value("detail").toString() : QString();
        return fail(QStringLiteral("result"), detail.isEmpty() ? i18n("fal.ai sent no picture.") : i18n("fal.ai sent no picture: %1", detail));
    }

    // ---- the picture itself (this address needs no key)
    report(i18n("Downloading the result"));
    QByteArray picture;
    if (!exchange(manager, "GET", imageUrl, QByteArray(), QByteArray(), remaining(), &httpStatus, &picture, error)) {
        return fail(QStringLiteral("download"), *error);
    }
    if (picture.isEmpty()) {
        return fail(QStringLiteral("download"), i18n("The result picture is empty."));
    }

    writeLog(QStringLiteral("fal %1: ok, submitted at %2 ms, done at %3 ms, downloaded %4 KB at %5 ms")
                  .arg(endpoint).arg(submitMs).arg(doneMs).arg(picture.size() / 1024).arg(total.elapsed()));
    return picture;
}

void FalClient::cancel()
{
    m_canceled = true;
    if (QEventLoop *loop = m_loop.load()) {
        loop->quit(); // thread-safe
    }
}

} // namespace AiChat
