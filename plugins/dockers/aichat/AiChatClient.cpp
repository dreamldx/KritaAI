/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AiChatClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <klocalizedstring.h>

namespace {
// No data for this long: give up. (A streamed answer keeps resetting it.)
const int kTransferTimeoutMs = 60 * 1000;
} // namespace

AiChatClient::AiChatClient(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<AiChatClientResult>();
}

AiChatClient::~AiChatClient()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void AiChatClient::send(const AiChatRequestSettings &settings, const QJsonArray &messages, const QJsonArray &tools)
{
    Q_ASSERT(!m_reply);

    m_sse = AiChatSseParser();
    m_accumulator = AiChatCompletionAccumulator();
    m_body.clear();
    m_canceled = false;
    m_sawDone = false;

    QJsonObject body;
    body["model"] = settings.model;
    body["messages"] = messages;
    body["stream"] = true;
    // ask for the token count: a streamed answer only carries it if this is set
    QJsonObject streamOptions;
    streamOptions["include_usage"] = true;
    body["stream_options"] = streamOptions;
    if (!tools.isEmpty()) {
        body["tools"] = tools;
    }
    if (!settings.reasoningEffort.isEmpty()) {
        body["reasoning_effort"] = settings.reasoningEffort;
    }

    QNetworkRequest request(settings.url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "text/event-stream, application/json");
    request.setRawHeader("User-Agent", "Krita-AIChat");
    if (!settings.apiKey.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + settings.apiKey.toUtf8());
    }
    request.setTransferTimeout(kTransferTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_reply = m_manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QIODevice::readyRead, this, &AiChatClient::slotReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &AiChatClient::slotFinished);
}

void AiChatClient::cancel()
{
    QNetworkReply *reply = m_reply;
    if (!reply) {
        return;
    }
    m_canceled = true;
    reply->abort(); // emits finished(), which is handled in slotFinished()
}

int AiChatClient::httpStatus() const
{
    return m_reply ? m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
}

bool AiChatClient::isEventStream() const
{
    return m_reply
        && m_reply->header(QNetworkRequest::ContentTypeHeader).toString().contains(QStringLiteral("text/event-stream"), Qt::CaseInsensitive);
}

void AiChatClient::slotReadyRead()
{
    drain();
}

void AiChatClient::drain()
{
    if (!m_reply) {
        return;
    }

    const QByteArray data = m_reply->readAll();
    if (data.isEmpty()) {
        return;
    }

    // An error page or a plain JSON answer is only parsed once it is complete.
    if (httpStatus() >= 400 || !isEventStream()) {
        m_body.append(data);
        return;
    }

    const QStringList events = m_sse.feed(data);
    for (const QString &event : events) {
        handleEvent(event);
    }
}

void AiChatClient::handleEvent(const QString &payload)
{
    if (payload.trimmed() == QLatin1String("[DONE]")) {
        m_sawDone = true;
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8());
    if (!doc.isObject()) {
        return; // keep-alive or something we do not understand
    }

    const QString delta = m_accumulator.addChunk(doc.object());
    if (!delta.isEmpty()) {
        Q_EMIT textDelta(delta);
    }
}

void AiChatClient::slotFinished()
{
    drain();

    QNetworkReply *reply = m_reply;
    if (!reply) {
        return;
    }
    const int status = httpStatus();
    const bool eventStream = isEventStream();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();

    // the reply is done: from here on the client may be used again, also from a slot of finished()
    m_reply = nullptr;
    reply->deleteLater();

    AiChatClientResult result;

    if (m_canceled) {
        result.canceled = true;
    } else if (status >= 400) {
        result.error = aiChatHttpErrorMessage(status, m_body);
    } else if (eventStream) {
        const QStringList events = m_sse.flush();
        for (const QString &event : events) {
            handleEvent(event);
        }
        // A complete answer ends with a finish reason or with "[DONE]". Without either the
        // connection was cut, and what arrived must not pass for the whole answer.
        const bool complete = m_sawDone || !m_accumulator.finishReason().isEmpty();
        if (m_accumulator.hasError()) {
            result.error = m_accumulator.error();
        } else if (!complete) {
            result.error = networkError != QNetworkReply::NoError
                ? networkErrorText
                : i18n("The connection was closed before the answer was complete.");
        }
    } else if (networkError != QNetworkReply::NoError) {
        result.error = networkErrorText;
    } else {
        // not streamed: the body is one JSON document
        const QJsonDocument doc = QJsonDocument::fromJson(m_body);
        if (!doc.isObject()) {
            result.error = i18n("The server sent an answer that is not valid JSON.");
        } else {
            const QString text = m_accumulator.setFullResponse(doc.object());
            if (m_accumulator.hasError()) {
                result.error = m_accumulator.error();
            } else if (!text.isEmpty()) {
                Q_EMIT textDelta(text);
            }
        }
    }

    result.text = m_accumulator.text();
    result.toolCalls = m_accumulator.toolCalls();
    result.finishReason = m_accumulator.finishReason();
    result.totalTokens = m_accumulator.totalTokens();
    Q_EMIT finished(result);
}
