/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AiChatStream.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

// ---- AiChatSseParser ------------------------------------------------------

QStringList AiChatSseParser::feed(const QByteArray &chunk)
{
    QStringList events;
    m_buffer.append(chunk);

    int newline = m_buffer.indexOf('\n');
    while (newline >= 0) {
        QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        processLine(line, &events);
        newline = m_buffer.indexOf('\n');
    }
    return events;
}

QStringList AiChatSseParser::flush()
{
    QStringList events;
    if (!m_buffer.isEmpty()) {
        QByteArray line = m_buffer;
        m_buffer.clear();
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        processLine(line, &events);
    }
    processLine(QByteArray(), &events); // an empty line closes the pending event
    return events;
}

void AiChatSseParser::processLine(const QByteArray &line, QStringList *events)
{
    if (line.isEmpty()) {
        if (m_hasData) {
            events->append(QString::fromUtf8(m_data));
            m_data.clear();
            m_hasData = false;
        }
        return;
    }

    if (line.startsWith(':')) {
        return; // comment, used as keep-alive
    }

    if (line.startsWith("data:")) {
        QByteArray value = line.mid(5);
        if (value.startsWith(' ')) {
            value.remove(0, 1);
        }
        if (m_hasData) {
            m_data.append('\n');
        }
        m_data.append(value);
        m_hasData = true;
    }
    // "event:", "id:" and "retry:" are of no use to us
}

// ---- AiChatCompletionAccumulator -------------------------------------------

namespace {

/// The text of a message "content", which is a string or a list of {"type":"text","text":...} parts.
QString contentText(const QJsonValue &content)
{
    if (content.isString()) {
        return content.toString();
    }

    QString text;
    if (content.isArray()) {
        const QJsonArray parts = content.toArray();
        for (const QJsonValue &part : parts) {
            text += part.toObject().value(QStringLiteral("text")).toString();
        }
    }
    return text;
}

} // namespace

void AiChatCompletionAccumulator::setErrorFrom(const QJsonValue &errorValue)
{
    m_hasError = true;
    if (errorValue.isString()) {
        m_error = errorValue.toString();
    } else {
        m_error = errorValue.toObject().value(QStringLiteral("message")).toString();
    }
    if (m_error.isEmpty()) {
        m_error = QStringLiteral("Unknown error");
    }
}

void AiChatCompletionAccumulator::readUsage(const QJsonObject &object)
{
    // "usage": {"prompt_tokens": .., "completion_tokens": .., "total_tokens": ..}; the
    // completion tokens already include the thinking of the model
    const QJsonValue usageValue = object.value(QStringLiteral("usage"));
    if (!usageValue.isObject()) {
        return;
    }
    const QJsonObject usage = usageValue.toObject();
    if (usage.value(QStringLiteral("total_tokens")).isDouble()) {
        m_totalTokens = static_cast<qint64>(usage.value(QStringLiteral("total_tokens")).toDouble());
    } else if (usage.value(QStringLiteral("prompt_tokens")).isDouble() || usage.value(QStringLiteral("completion_tokens")).isDouble()) {
        m_totalTokens = static_cast<qint64>(usage.value(QStringLiteral("prompt_tokens")).toDouble()
                                            + usage.value(QStringLiteral("completion_tokens")).toDouble());
    }
}

QString AiChatCompletionAccumulator::addChunk(const QJsonObject &chunk)
{
    if (chunk.contains(QStringLiteral("error"))) {
        setErrorFrom(chunk.value(QStringLiteral("error")));
        return QString();
    }

    readUsage(chunk); // usually in a last chunk of its own, without choices

    const QJsonArray choices = chunk.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return QString();
    }

    const QJsonObject choice = choices.first().toObject();
    const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();

    const QString textDelta = contentText(delta.value(QStringLiteral("content")));
    m_text += textDelta;

    const QJsonArray toolCallDeltas = delta.value(QStringLiteral("tool_calls")).toArray();
    for (const QJsonValue &value : toolCallDeltas) {
        const QJsonObject call = value.toObject();
        const int index = call.value(QStringLiteral("index")).toInt(0);
        AiChatToolCall &target = m_toolCalls[index];

        const QString id = call.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            target.id = id;
        }
        const QJsonObject function = call.value(QStringLiteral("function")).toObject();
        const QString name = function.value(QStringLiteral("name")).toString();
        if (!name.isEmpty()) {
            target.name = name;
        }
        target.arguments += function.value(QStringLiteral("arguments")).toString();
    }

    const QJsonValue finish = choice.value(QStringLiteral("finish_reason"));
    if (finish.isString() && !finish.toString().isEmpty()) {
        m_finishReason = finish.toString();
    }

    return textDelta;
}

QString AiChatCompletionAccumulator::setFullResponse(const QJsonObject &response)
{
    if (response.contains(QStringLiteral("error"))) {
        setErrorFrom(response.value(QStringLiteral("error")));
        return QString();
    }

    readUsage(response);

    const QJsonArray choices = response.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return QString();
    }

    const QJsonObject choice = choices.first().toObject();
    const QJsonObject message = choice.value(QStringLiteral("message")).toObject();

    m_text = contentText(message.value(QStringLiteral("content")));

    m_toolCalls.clear();
    const QJsonArray calls = message.value(QStringLiteral("tool_calls")).toArray();
    int index = 0;
    for (const QJsonValue &value : calls) {
        const QJsonObject call = value.toObject();
        const QJsonObject function = call.value(QStringLiteral("function")).toObject();

        AiChatToolCall toolCall;
        toolCall.id = call.value(QStringLiteral("id")).toString();
        toolCall.name = function.value(QStringLiteral("name")).toString();
        toolCall.arguments = function.value(QStringLiteral("arguments")).toString();
        m_toolCalls.insert(index++, toolCall);
    }

    const QJsonValue finish = choice.value(QStringLiteral("finish_reason"));
    if (finish.isString()) {
        m_finishReason = finish.toString();
    }

    return m_text;
}

// ---- errors ----------------------------------------------------------------

QString aiChatHttpErrorMessage(int status, const QByteArray &body)
{
    const QString prefix = QStringLiteral("HTTP %1").arg(status);

    QString detail;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const QJsonValue error = doc.object().value(QStringLiteral("error"));
        detail = error.isString() ? error.toString() : error.toObject().value(QStringLiteral("message")).toString();
    }
    if (detail.isEmpty()) {
        detail = QString::fromUtf8(body).trimmed();
    }

    const int maxLength = 200;
    if (detail.size() > maxLength) {
        detail = detail.left(maxLength) + QChar(0x2026);
    }

    return detail.isEmpty() ? prefix : prefix + QStringLiteral(": ") + detail;
}
