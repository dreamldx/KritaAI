/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AiChatTools.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>

#include <memory>

#include <klocalizedstring.h>

// ---- registry ---------------------------------------------------------------

void AiChatToolRegistry::add(const AiChatTool &tool)
{
    for (AiChatTool &existing : m_tools) {
        if (existing.name == tool.name) {
            existing = tool;
            return;
        }
    }
    m_tools.append(tool);
}

QStringList AiChatToolRegistry::names() const
{
    QStringList result;
    for (const AiChatTool &tool : m_tools) {
        result << tool.name;
    }
    return result;
}

void AiChatToolRegistry::beginRun(const AiChatRequestSettings &settings)
{
    m_settings = settings;
}

QJsonArray AiChatToolRegistry::toolDefinitions() const
{
    QJsonArray definitions;
    for (const AiChatTool &tool : m_tools) {
        if (tool.isAvailable && !tool.isAvailable(m_settings)) {
            continue;
        }

        QJsonObject parameters = tool.parameters;
        if (parameters.isEmpty()) {
            // a tool without arguments still needs a schema, some servers insist on it
            parameters["type"] = "object";
            parameters["properties"] = QJsonObject();
        }

        QJsonObject function;
        function["name"] = tool.name;
        function["description"] = tool.description;
        function["parameters"] = parameters;

        QJsonObject entry;
        entry["type"] = "function";
        entry["function"] = function;
        definitions.append(entry);
    }
    return definitions;
}

QString AiChatToolRegistry::runTool(const QString &name, const QString &argumentsJson, bool *ok)
{
    *ok = false;

    const AiChatTool *found = nullptr;
    for (const AiChatTool &tool : std::as_const(m_tools)) {
        if (tool.name == name && (!tool.isAvailable || tool.isAvailable(m_settings))) {
            found = &tool;
            break;
        }
    }
    if (!found) {
        return i18n("There is no tool called \"%1\". Available tools: %2.", name, names().join(QStringLiteral(", ")));
    }

    // models send "" or "{}" for a tool without arguments
    QJsonObject arguments;
    if (!argumentsJson.trimmed().isEmpty()) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(argumentsJson.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            return i18n("The arguments of \"%1\" are not a valid JSON object: %2", name, parseError.errorString());
        }
        arguments = document.object();
    }

    *ok = true;
    return found->run(arguments, m_settings, ok);
}

void AiChatToolRegistry::cancel()
{
    if (m_cancelHook) {
        m_cancelHook();
    }
}

// ---- GUI executor -----------------------------------------------------------

namespace {
/// Shared by the waiting thread and the function that is posted to the GUI thread.
struct RunState {
    QMutex mutex;
    QWaitCondition changed;
    bool started {false}; ///< the GUI thread began to run the function
    bool done {false};
    bool abandoned {false}; ///< the caller gave up before the function started: it must not run
};
} // namespace

AiChatGuiExecutor::AiChatGuiExecutor(QObject *parent)
    : QObject(parent)
{
}

void AiChatGuiExecutor::close()
{
    m_closed = true;
}

bool AiChatGuiExecutor::run(const std::function<void()> &function, QString *error, int timeoutMs)
{
    if (QThread::currentThread() == thread()) {
        function();
        return true;
    }
    if (m_closed) {
        *error = i18n("Krita is closing the AI chat.");
        return false;
    }

    auto state = std::make_shared<RunState>();

    // `function` is only referenced: we do not return before it ran or was abandoned.
    const std::function<void()> *functionPointer = &function;
    QMetaObject::invokeMethod(this, [state, functionPointer]() {
        {
            QMutexLocker locker(&state->mutex);
            if (state->abandoned) {
                return;
            }
            state->started = true;
        }
        (*functionPointer)();
        QMutexLocker locker(&state->mutex);
        state->done = true;
        state->changed.wakeAll();
    }, Qt::QueuedConnection);

    QElapsedTimer waited;
    waited.start();

    QMutexLocker locker(&state->mutex);
    while (!state->done) {
        if (!state->started) {
            const bool timedOut = waited.elapsed() > timeoutMs;
            if (m_closed || timedOut) {
                state->abandoned = true;
                *error = m_closed ? i18n("Krita is closing the AI chat.")
                                  : i18n("Krita did not respond in time.");
                return false;
            }
        }
        // short waits, so that close() and the timeout are noticed
        state->changed.wait(&state->mutex, 50);
    }
    return true;
}
