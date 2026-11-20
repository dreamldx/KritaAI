/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_TOOLS_H_
#define _AI_TOOLS_H_

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>

#include "../Agent.h"

namespace AiChat {

/**
 * One tool the model may use.
 *
 * `run` is called in the thread of the AI loop (not the GUI thread). A tool that
 * needs Krita's image has to go through an GuiExecutor.
 */
struct Tool {
    QString name; ///< letters, digits and underscores
    QString description; ///< tells the model when and how to use the tool
    QJsonObject parameters; ///< JSON schema of the arguments: {"type": "object", "properties": {...}}

    /// Returns the text for the model. Set `*ok` to false for a failure (the text is the reason).
    std::function<QString(const QJsonObject &arguments, const RequestSettings &settings, bool *ok)> run;

    /// Optional. If it returns false the tool is not offered in this run.
    std::function<bool(const RequestSettings &settings)> isAvailable;
};

/**
 * The tools of the AI chat. Hand it to Worker::setToolProvider().
 *
 * It builds the "tools" list of the request, checks the arguments the model
 * sent and turns every problem (unknown tool, broken JSON, an exception-free
 * failure of the tool) into a text the model can read and react to.
 */
class ToolRegistry : public AgentToolProvider
{
public:
    /// Adds a tool; a tool of the same name is replaced.
    void add(const Tool &tool);

    QStringList names() const;

    // AgentToolProvider
    void beginRun(const RequestSettings &settings) override;
    QJsonArray toolDefinitions() const override;
    QString runTool(const QString &name, const QString &argumentsJson, bool *ok) override;
    void cancel() override;

    /// Called by cancel(); a tool that waits (for the network) registers what makes it return.
    void setCancelHook(std::function<void()> hook)
    {
        m_cancelHooks.clear();
        addCancelHook(std::move(hook));
    }
    /// Like setCancelHook(), but keeps the hooks that are already there.
    void addCancelHook(std::function<void()> hook) { m_cancelHooks.append(std::move(hook)); }

private:
    QList<Tool> m_tools;
    RequestSettings m_settings;
    QList<std::function<void()>> m_cancelHooks;
};

/**
 * Runs a function in the GUI thread and waits for it.
 *
 * Krita's images and documents belong to the GUI thread; the AI loop does not.
 * Make the executor in the GUI thread, call run() from the AI thread.
 *
 * Unlike a BlockingQueuedConnection it cannot deadlock when the panel is being
 * closed: close() makes every waiting run() return at once (a function that is
 * already running in the GUI thread is still waited for, because it may use the
 * variables of the caller).
 */
class GuiExecutor : public QObject
{
    Q_OBJECT
public:
    explicit GuiExecutor(QObject *parent = nullptr);

    /// Returns true if `function` ran. Otherwise `*error` says why (closed, or not started in time).
    /// Called from the GUI thread itself, it simply calls the function.
    bool run(const std::function<void()> &function, QString *error, int timeoutMs = 30000);

    /// After this run() fails at once. Call it in the GUI thread before the worker thread is stopped.
    void close();
    bool isClosed() const { return m_closed.load(); }

private:
    std::atomic<bool> m_closed {false};
};

} // namespace AiChat

#endif
