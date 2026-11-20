/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_AGENT_H_
#define _AI_AGENT_H_

#include <QElapsedTimer>
#include <QJsonArray>
#include <QObject>
#include <QString>

#include "../network/Client.h"

namespace AiChat {

/**
 * What the model may ask us to do. The loop offers `toolDefinitions()` to the
 * model and calls runTool() when the model wants one of them.
 *
 * runTool() is called in the thread of the Agent, one call at a time,
 * and blocks that thread until it returns. A tool that has to touch Krita's
 * image (which belongs to the GUI thread) has to hand the work over to the
 * GUI thread itself.
 */
class AgentToolProvider
{
public:
    virtual ~AgentToolProvider() = default;

    /// The tools in the OpenAI format: [{"type": "function", "function": {"name": ..., ...}}, ...]
    virtual QJsonArray toolDefinitions() const = 0;

    /// Runs a tool. `argumentsJson` is the JSON the model produced. Returns the text that
    /// is handed back to the model; set `*ok` to false if the tool failed (the text is the reason).
    virtual QString runTool(const QString &name, const QString &argumentsJson, bool *ok) = 0;

    /// Called when a run starts, before toolDefinitions(): tools may depend on the settings of the
    /// run (the vision tool is only offered if a vision model is set up).
    virtual void beginRun(const RequestSettings &settings) { Q_UNUSED(settings); }

    /// Called in the thread of the agent when the user cancels. A tool that waits for something
    /// (the network) should return from runTool() soon.
    virtual void cancel() {}
};

/**
 * The AI loop.
 *
 * It sends the conversation to the model. If the model answers with text, that
 * is the end. If the model asks for tools, it runs them, adds their results to
 * the conversation and asks again, until the model answers with text, an error
 * happens, the user cancels, or the number of rounds is used up.
 *
 * Everything is asynchronous (it only reacts to the network), so it can live
 * in any thread, but it must be created and used in one thread.
 */
class Agent : public QObject
{
    Q_OBJECT
public:
    explicit Agent(QObject *parent = nullptr);

    /// The tools to offer to the model. Not owned. Without a provider the model gets no tools.
    void setToolProvider(AgentToolProvider *provider) { m_provider = provider; }

    /// How many requests one run may make, so that a model that keeps asking for tools cannot loop forever.
    void setMaxRounds(int rounds) { m_maxRounds = qMax(1, rounds); }

    bool isRunning() const { return m_running; }

    /// Tokens used by all the requests of the current (or last) run, as the server reported them;
    /// -1 if it reported none. Valid when finished(), failed() or canceled() is emitted.
    qint64 totalTokens() const { return m_tokens; }

    /// Starts the loop. `conversation` is in the OpenAI format and ends with the user's message.
    void start(const RequestSettings &settings, const QJsonArray &conversation);

    /// Stops the loop; canceled() follows.
    void cancel();

Q_SIGNALS:
    /// A piece of the answer, as it arrives. Text of several rounds is joined by a blank line.
    void textDelta(const QString &delta);

    /// The loop is done and `text` is the answer.
    void finished(const QString &text);

    /// The loop stopped because of a problem. `partialText` is what had been received (may be empty).
    void failed(const QString &error, const QString &partialText);

    /// The loop was canceled. `partialText` is what had been received (may be empty).
    void canceled(const QString &partialText);

private Q_SLOTS:
    void onClientDelta(const QString &delta);
    void onClientFinished(const ClientResult &result);

private:
    void startRound();
    void fail(const QString &error);

    Client m_client;
    AgentToolProvider *m_provider {nullptr};
    int m_maxRounds {10};

    RequestSettings m_settings;
    QJsonArray m_messages;
    int m_round {0};
    qint64 m_tokens {-1};
    QElapsedTimer m_roundTimer;
    bool m_running {false};
    bool m_canceled {false};
    bool m_firstDeltaOfRound {true};
    QString m_text; ///< everything the model said so far, over all rounds
};

} // namespace AiChat

#endif
