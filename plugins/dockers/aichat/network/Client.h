/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _NET_CLIENT_H_
#define _NET_CLIENT_H_

#include <QByteArray>
#include <QJsonArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

#include "Stream.h"
#include "../ai/tools/Fal.h"
#include "../ai/tools/Vision.h"

class QNetworkReply;

namespace AiChat {

/// Everything the HTTP client needs to know about the server. Plain data, safe to pass between threads.
struct RequestSettings {
    QUrl url; ///< the complete ".../chat/completions" URL
    QString apiKey; ///< empty: no Authorization header (local servers)
    QString model;
    QString reasoningEffort; ///< "low", "medium", "high"; empty: the field is not sent
    VisionSettings vision;
    FalSettings fal;
};


/// What one request to the server produced.
struct ClientResult {
    QString text;
    QList<ToolCall> toolCalls; ///< what the model wants us to run, if anything
    QString finishReason;
    QString error; ///< empty on success
    bool canceled {false};
    qint64 totalTokens {-1}; ///< what the server said this request used; -1 if it did not say
};

/**
 * One HTTP round trip to an OpenAI-compatible "chat/completions" endpoint.
 *
 * The request is asynchronous: send() returns at once, the text arrives
 * through textDelta() while the server streams it, and finished() ends it.
 * Create the client in the thread that uses it (it owns a
 * QNetworkAccessManager); nothing here blocks.
 */
class Client : public QObject
{
    Q_OBJECT
public:
    explicit Client(QObject *parent = nullptr);
    ~Client() override;

    /// Starts a request. `messages` are in the OpenAI format ({"role": ..., "content": ...}).
    void send(const RequestSettings &settings, const QJsonArray &messages, const QJsonArray &tools = QJsonArray());

    /// Aborts the running request; finished() is emitted with `canceled` set.
    void cancel();

    bool isBusy() const { return m_reply != nullptr; }

Q_SIGNALS:
    /// A piece of the answer text, as soon as it arrived.
    void textDelta(const QString &delta);
    void finished(const ClientResult &result);

private Q_SLOTS:
    void slotReadyRead();
    void slotFinished();

private:
    void drain();
    void handleEvent(const QString &payload);
    bool isEventStream() const;
    int httpStatus() const;

    QNetworkAccessManager m_manager;
    QNetworkReply *m_reply {nullptr};

    SseParser m_sse;
    CompletionAccumulator m_accumulator;
    QByteArray m_body; ///< whole body when it is not an event stream (plain JSON or an error page)
    bool m_canceled {false};
    bool m_sawDone {false};
};

} // namespace AiChat

Q_DECLARE_METATYPE(AiChat::ClientResult)
Q_DECLARE_METATYPE(AiChat::RequestSettings)

#endif
