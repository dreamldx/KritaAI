/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_TOOLS_FAL_H_
#define _AI_TOOLS_FAL_H_

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

#include <atomic>
#include <functional>

class QEventLoop;
class QNetworkAccessManager;

namespace AiChat {

/// fal.ai, where the Topaz tools ("upscale_image", "denoise_image") send the picture to.
struct FalSettings {
    QString apiKey; ///< empty: the tools are not offered
    QUrl queueUrl {QStringLiteral("https://queue.fal.run")}; ///< the tests point it at a fake server
    QUrl restUrl {QStringLiteral("https://rest.alpha.fal.ai")}; ///< where files are uploaded to (storage/upload/initiate)
    QUrl deleteUrl {QStringLiteral("https://rest.fal.ai")}; ///< where uploaded files are deleted (storage/files/delete)

    bool isValid() const { return !apiKey.isEmpty() && queueUrl.isValid(); }
};


/**
 * Runs one job on fal.ai through its queue API and fetches the picture it makes.
 *
 *   POST  {queue}/{endpoint}        the job (JSON); the answer names the request
 *   GET   status_url                IN_QUEUE / IN_PROGRESS / COMPLETED, until done
 *   GET   response_url              {"image": {"url": ...}}
 *   GET   image.url                 the picture
 *
 * run() blocks the calling thread (it runs a local event loop), so use it in the
 * worker thread. cancel() may be called from any thread.
 */
class FalClient : public QObject
{
    Q_OBJECT
public:
    explicit FalClient(QObject *parent = nullptr);

    /// Called with a short text ("Waiting in the queue (position 3)") whenever the state changes.
    using ProgressFunction = std::function<void(const QString &status)>;

    /// `endpoint` is the path of the model, e.g. "topaz/upscale/image/precision".
    /// Returns the bytes of the resulting picture, or an empty array with the reason in `*error`.
    ///
    /// `pngImage` (optional) is the picture to process. It is uploaded to the file storage of fal.ai first and
    /// its address becomes "image_url" of the job. A picture is too big to travel inside the job itself (a
    /// 10 MB one made the server hang up), so only if the upload fails it is sent as a data URI after all.
    QByteArray run(const FalSettings &settings, const QString &endpoint, const QJsonObject &input,
                   const ProgressFunction &progress, QString *error, int timeoutMs = 600000, int pollIntervalMs = 1000,
                   const QByteArray &pngImage = QByteArray());

    /// Makes a running run() return with an error. Does nothing if none is running.
    void cancel();

    /// The text for the user for a status answer of the queue.
    static QString statusText(const QJsonObject &status);

    /// The status and result addresses of a request: those the server sent, otherwise the documented
    /// ones ({queue}/{first two parts of the endpoint}/requests/{id}[/status]).
    static QUrl statusUrl(const QUrl &queue, const QString &endpoint, const QJsonObject &submitted);
    static QUrl resultUrl(const QUrl &queue, const QString &endpoint, const QJsonObject &submitted);

private:
    /// One HTTP exchange. False, with `*error`, on failure, timeout or cancel.
    bool exchange(QNetworkAccessManager &manager, const QByteArray &verb, const QUrl &url, const QByteArray &body,
                  const QByteArray &apiKey, qint64 deadlineMs, int *status, QByteArray *data, QString *error,
                  const QByteArray &contentType = "application/json");
    /// Puts the picture into the file storage; returns the address to use as image_url (empty, with `*error`, on failure).
    QUrl upload(QNetworkAccessManager &manager, const FalSettings &settings, const QByteArray &png, qint64 timeoutMs, QString *error);
    /// Waits, but returns early on cancel().
    void pause(int ms);

    std::atomic<QEventLoop *> m_loop {nullptr};
    std::atomic<bool> m_canceled {false};
    bool m_ignoreCancel {false}; ///< while the uploaded file is deleted: a canceled job must still clean up
};

} // namespace AiChat

#endif
