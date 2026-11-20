/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Agent.h"

#include <QElapsedTimer>
#include <QJsonObject>

#include <klocalizedstring.h>

#include "../utils/Log.h"

namespace AiChat {

Agent::Agent(QObject *parent)
    : QObject(parent)
    , m_client(this)
{
    connect(&m_client, &Client::textDelta, this, &Agent::onClientDelta);
    connect(&m_client, &Client::finished, this, &Agent::onClientFinished);
}

void Agent::start(const RequestSettings &settings, const QJsonArray &conversation)
{
    if (m_running) {
        return;
    }

    m_settings = settings;
    m_messages = conversation;
    m_round = 0;
    m_tokens = -1;
    m_running = true;
    m_canceled = false;
    m_text.clear();

    if (m_provider) {
        m_provider->beginRun(settings);
    }
    startRound();
}

void Agent::cancel()
{
    if (!m_running) {
        return;
    }
    m_canceled = true;
    if (m_provider) {
        m_provider->cancel(); // a tool that is waiting returns
    }
    m_client.cancel(); // finished() follows, from where the loop ends
}

void Agent::startRound()
{
    if (++m_round > m_maxRounds) {
        fail(i18n("The AI kept asking for tools and was stopped after %1 rounds.", m_maxRounds));
        return;
    }

    m_firstDeltaOfRound = true;
    m_roundTimer.start();
    const QJsonArray tools = m_provider ? m_provider->toolDefinitions() : QJsonArray();
    m_client.send(m_settings, m_messages, tools);
}

void Agent::fail(const QString &error)
{
    m_running = false;
    Q_EMIT failed(error, m_text);
}

void Agent::onClientDelta(const QString &delta)
{
    // text of a later round is set apart from the earlier text by a blank line
    if (m_firstDeltaOfRound && !m_text.isEmpty()) {
        m_text += QStringLiteral("\n\n");
        Q_EMIT textDelta(QStringLiteral("\n\n"));
    }
    m_firstDeltaOfRound = false;

    m_text += delta;
    Q_EMIT textDelta(delta);
}

void Agent::onClientFinished(const ClientResult &result)
{
    if (!m_running) {
        return;
    }

    // every round sends the whole conversation again, so the rounds add up
    if (result.totalTokens >= 0) {
        m_tokens = qMax<qint64>(m_tokens, 0) + result.totalTokens;
    }

    writeLog(QStringLiteral("chat round %1: model answered after %2 ms (%3 tool calls, %4 chars, %5 tokens%6)")
                  .arg(m_round)
                  .arg(m_roundTimer.elapsed())
                  .arg(result.toolCalls.size())
                  .arg(result.text.size())
                  .arg(result.totalTokens)
                  .arg(result.error.isEmpty() ? QString() : QStringLiteral(", error: ") + result.error));

    if (result.canceled || m_canceled) {
        m_running = false;
        Q_EMIT canceled(m_text);
        return;
    }

    if (!result.error.isEmpty()) {
        fail(result.error);
        return;
    }

    if (result.toolCalls.isEmpty()) {
        m_running = false;
        Q_EMIT finished(m_text);
        return;
    }

    if (!m_provider) {
        fail(i18n("The AI asked to use a tool, but no tools are available."));
        return;
    }

    // The model's turn, including the tool calls, goes into the conversation...
    QJsonArray calls;
    for (const ToolCall &call : result.toolCalls) {
        QJsonObject function;
        function["name"] = call.name;
        function["arguments"] = call.arguments;
        QJsonObject entry;
        entry["id"] = call.id;
        entry["type"] = "function";
        entry["function"] = function;
        calls.append(entry);
    }
    QJsonObject assistant;
    assistant["role"] = "assistant";
    assistant["content"] = result.text.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(result.text);
    assistant["tool_calls"] = calls;
    m_messages.append(assistant);

    // ...then every tool is run and its result follows.
    for (const ToolCall &call : result.toolCalls) {
        bool ok = true;
        QElapsedTimer toolTimer;
        toolTimer.start();
        const QString output = m_provider->runTool(call.name, call.arguments, &ok);
        writeLog(QStringLiteral("tool %1 %2 after %3 ms (%4 chars)")
                      .arg(call.name, ok ? QStringLiteral("finished") : QStringLiteral("FAILED"))
                      .arg(toolTimer.elapsed())
                      .arg(output.size()));

        QJsonObject tool;
        tool["role"] = "tool";
        tool["tool_call_id"] = call.id;
        tool["content"] = ok ? output : i18n("Error: %1", output);
        m_messages.append(tool);

        if (m_canceled) {
            break;
        }
    }

    if (m_canceled) {
        m_running = false;
        Q_EMIT canceled(m_text);
        return;
    }

    startRound();
}

} // namespace AiChat
