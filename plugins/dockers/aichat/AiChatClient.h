/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_CHAT_CLIENT_H_
#define _AI_CHAT_CLIENT_H_

#include <QByteArray>
#include <QJsonArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

#include "AiChatStream.h"
#include "AiChatTypes.h"

class QNetworkReply;

/// What one request to the server produced.
struct AiChatClientResult {
    QString text;
    QList<AiChatToolCall> toolCalls; ///< what the model wants us to run, if anything
    QString finishReason;
    QString error; ///< empty on success
    bool canceled {false};
    qint64 totalTokens {-1}; ///< what the server said this request used; -1 if it did not say
};
Q_DECLARE_METATYPE(AiChatClientResult)

/**
 * One HTTP round trip to an OpenAI-compatible "chat/completions" endpoint.
 *
 * The request is asynchronous: send() returns at once, the text arrives
 * through textDelta() while the server streams it, and finished() ends it.
 * Create the client in the thread that uses it (it owns a
 * QNetworkAccessManager); nothing here blocks.
 */
class AiChatClient : public QObject
{
    Q_OBJECT
public:
    explicit AiChatClient(QObject *parent = nullptr);
    ~AiChatClient() override;

    /// Starts a request. `messages` are in the OpenAI format ({"role": ..., "content": ...}).
    void send(const AiChatRequestSettings &settings, const QJsonArray &messages, const QJsonArray &tools = QJsonArray());

    /// Aborts the running request; finished() is emitted with `canceled` set.
    void cancel();

    bool isBusy() const { return m_reply != nullptr; }

Q_SIGNALS:
    /// A piece of the answer text, as soon as it arrived.
    void textDelta(const QString &delta);
    void finished(const AiChatClientResult &result);

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

    AiChatSseParser m_sse;
    AiChatCompletionAccumulator m_accumulator;
    QByteArray m_body; ///< whole body when it is not an event stream (plain JSON or an error page)
    bool m_canceled {false};
    bool m_sawDone {false};
};

#endif
