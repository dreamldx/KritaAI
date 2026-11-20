/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Chat.h"

#include <QMetaObject>
#include <QThread>

#include <klocalizedstring.h>

#include "../ai/tools/DocumentTools.h"
#include "../ai/tools/Fal.h"
#include "../ai/tools/FalTools.h"
#include "../ai/tools/LayerTools.h"
#include "../ai/tools/Tools.h"
#include "../ai/tools/Vision.h"
#include "../config/Config.h"
#include "../ui/Progress.h"
#include "../utils/Worker.h"

namespace AiChat {

Chat::Chat(const QString &databasePath, DocumentAccess *documentAccess, Progress *progress, QObject *parent)
    : QObject(parent)
    , m_documentAccess(documentAccess)
{
    // The AI loop, the network and the database run in a thread of their own. They are reached
    // only through queued signals and slots, so nothing here waits for any of them.
    qRegisterMetaType<StoredMessage>("StoredMessage"); // plain names: queued calls look types up by the name moc writes
    qRegisterMetaType<QList<StoredMessage>>("QList<StoredMessage>");
    qRegisterMetaType<RequestSettings>("RequestSettings");

    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("AiChatWorker"));
    m_worker = new Worker(databasePath.isEmpty() ? Worker::defaultDatabasePath() : databasePath);

    // The tools. The executor lives here, in the GUI thread: the tools call it from the worker thread.
    m_executor = new GuiExecutor(this);
    m_vision = new VisionClient(); // used by the worker thread only
    m_tools = new ToolRegistry();
    addDocumentTools(m_tools, m_executor, m_documentAccess, m_vision);
    m_fal = new FalClient(); // used by the worker thread only
    // the Cancel button of the progress window stops the job (a direct call: the worker thread is waiting)
    connect(progress, &Progress::cancelRequested, this, [this]() { m_fal->cancel(); }, Qt::DirectConnection);
    addFalTools(m_tools, m_executor, m_documentAccess, m_fal, progress);
    addLayerTools(m_tools, m_executor, m_documentAccess);
    m_worker->setToolProvider(m_tools);

    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_thread, &QThread::started, m_worker, &Worker::init);
    connect(m_worker, &Worker::conversationLoaded, this, &Chat::slotConversationLoaded);
    connect(m_worker, &Worker::assistantDelta, this, &Chat::slotAssistantDelta);
    connect(m_worker, &Worker::assistantFinished, this, &Chat::slotAssistantFinished);
    connect(m_worker, &Worker::requestFailed, this, &Chat::slotRequestFailed);
    connect(m_worker, &Worker::requestCanceled, this, &Chat::slotRequestCanceled);
    connect(m_worker, &Worker::busyChanged, this, &Chat::slotBusyChanged);
    connect(m_worker, &Worker::storageError, this, &Chat::slotStorageError);

    m_thread->start();
}

Chat::~Chat()
{
    // A tool that waits for the GUI thread would block the shutdown below: let it go first.
    m_executor->close();

    if (m_thread->isRunning()) {
        // The worker stops a running request and releases its network and database
        // connections inside its own thread; only then the thread may end.
        QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
        m_thread->quit();
        if (!m_thread->wait(5000)) {
            qWarning("AI Chat: the worker thread did not stop in time");
        }
    }
    // the worker deletes itself when the thread has finished

    // The tools are used by the worker thread only, which has ended (or, after the warning above,
    // is stuck and must not be touched: then they are left alone and leak rather than crash).
    if (!m_thread->isRunning()) {
        delete m_tools;
        delete m_vision;
        delete m_fal;
    }
}

void Chat::setReasoningEffort(ReasoningEffort effort)
{
    if (effort == m_effort) {
        return;
    }
    m_effort = effort;
    Q_EMIT reasoningEffortChanged(effort);
}

bool Chat::send(const QString &text)
{
    // One request at a time; and nothing can be sent before the history is loaded.
    if (m_busy || !m_ready) {
        return false;
    }

    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    // The settings are read now, so a change in Configure Krita counts from the next message on.
    const Config config = Config::load();
    if (!config.isComplete()) {
        Q_EMIT errorReported(i18n("The AI service is not set up yet. Open Settings > Configure Krita > AI Chat "
                                  "and enter the server address and the model."));
        return false;
    }

    const RequestSettings request = makeRequestSettings(config, m_effort);

    m_streamingText.clear();
    setBusy(true); // at once; the worker confirms it with busyChanged()

    QMetaObject::invokeMethod(m_worker, "submit", Qt::QueuedConnection,
                              Q_ARG(QString, trimmed), Q_ARG(RequestSettings, request));
    return true;
}

void Chat::stop()
{
    QMetaObject::invokeMethod(m_worker, "cancel", Qt::QueuedConnection);
}

void Chat::newChat()
{
    m_streamingText.clear();
    // a request that is still running is stopped by the worker; the old conversation stays in the database
    QMetaObject::invokeMethod(m_worker, "startNewConversation", Qt::QueuedConnection);
}

void Chat::setBusy(bool busy)
{
    m_busy = busy;
    Q_EMIT busyChanged(busy);
}

void Chat::slotConversationLoaded(const QList<StoredMessage> &messages)
{
    m_ready = true;
    Q_EMIT historyLoaded(messages);
}

void Chat::slotAssistantDelta(const QString &delta)
{
    m_streamingText += delta;
    Q_EMIT assistantDelta(delta, m_streamingText);
}

void Chat::slotAssistantFinished(const StoredMessage &message)
{
    m_streamingText.clear();
    Q_EMIT assistantFinished(message);
}

void Chat::slotRequestFailed(const QString &error)
{
    m_streamingText.clear();
    Q_EMIT requestFailed(error);
}

void Chat::slotRequestCanceled()
{
    m_streamingText.clear();
    Q_EMIT requestCanceled();
}

void Chat::slotBusyChanged(bool busy)
{
    setBusy(busy);
}

void Chat::slotStorageError(const QString &error)
{
    // once is enough: the chat keeps working, it just cannot save
    if (m_storageWarned) {
        return;
    }
    m_storageWarned = true;
    Q_EMIT errorReported(i18n("The chat history cannot be saved: %1", error));
}

} // namespace AiChat
