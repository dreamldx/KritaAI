/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_CHAT_STREAM_H_
#define _AI_CHAT_STREAM_H_

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include "AiChatTypes.h"

/**
 * Splits a Server-Sent-Events byte stream into events.
 *
 * Feed it the bytes as they arrive from the network, in chunks of any size
 * (a chunk may end in the middle of a line or even of a UTF-8 character).
 * It returns the "data" of every event that is complete.
 */
class AiChatSseParser
{
public:
    QStringList feed(const QByteArray &chunk);

    /// Call when the stream ended: dispatches an event that was not closed by an empty line.
    QStringList flush();

private:
    void processLine(const QByteArray &line, QStringList *events);

    QByteArray m_buffer; ///< bytes of the line being received
    QByteArray m_data; ///< "data" of the event being received
    bool m_hasData {false};
};

/**
 * Collects the pieces of a chat completion: the text, the tool calls the
 * model asks for, the finish reason and an error if the server reported one.
 *
 * Works for the streamed form (a sequence of "delta" chunks) and for the
 * plain, non-streamed response.
 */
class AiChatCompletionAccumulator
{
public:
    /// Add one streamed chunk. Returns the text that this chunk added (possibly empty).
    QString addChunk(const QJsonObject &chunk);

    /// Use a complete, non-streamed response. Returns its text.
    QString setFullResponse(const QJsonObject &response);

    QString text() const { return m_text; }
    QList<AiChatToolCall> toolCalls() const { return m_toolCalls.values(); } ///< ordered by index
    QString finishReason() const { return m_finishReason; }
    bool hasError() const { return m_hasError; }
    QString error() const { return m_error; }

    /// Tokens the request used (prompt and answer, thinking included), -1 if the server did not report them.
    qint64 totalTokens() const { return m_totalTokens; }

private:
    void setErrorFrom(const QJsonValue &errorValue);
    void readUsage(const QJsonObject &object);

    qint64 m_totalTokens {-1};
    QString m_text;
    QMap<int, AiChatToolCall> m_toolCalls;
    QString m_finishReason;
    bool m_hasError {false};
    QString m_error;
};

/// "HTTP 401: Incorrect API key provided" - uses the server's error message if the body has one.
QString aiChatHttpErrorMessage(int status, const QByteArray &body);

#endif
