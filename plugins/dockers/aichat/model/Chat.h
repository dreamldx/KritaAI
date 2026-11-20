/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _MODEL_CHAT_H_
#define _MODEL_CHAT_H_

#include <QList>
#include <QObject>
#include <QString>

#include "../utils/Store.h"
#include "Request.h"

class QThread;

namespace AiChat {

class DocumentAccess;
class FalClient;
class GuiExecutor;
class Progress;
class ToolRegistry;
class VisionClient;
class Worker;

/**
 * The state of the conversation, without any widget: whether the history is loaded, whether the AI is
 * answering, the thinking effort, the text that has arrived so far. The panel (`Dock`) only shows it.
 *
 * It owns the worker thread, the worker and the tools of the AI. The AI loop, the network and the
 * database run in that thread and are reached only through queued signals and slots, so nothing in
 * here waits for them. Use it in the GUI thread.
 *
 * `documentAccess` and `progress` are not owned and must outlive the chat. (`progress` is the modal
 * window of the slow tools; the tools only call its three slots.)
 */
class Chat : public QObject
{
    Q_OBJECT
public:
    /// @param databasePath where the chat history is kept; empty: the default place in Krita's user data folder
    Chat(const QString &databasePath, DocumentAccess *documentAccess, Progress *progress, QObject *parent = nullptr);
    ~Chat() override;

    /// True once the history was loaded: messages can be sent.
    bool isReady() const { return m_ready; }

    /// True while the AI is answering.
    bool isBusy() const { return m_busy; }

    ReasoningEffort reasoningEffort() const { return m_effort; }
    void setReasoningEffort(ReasoningEffort effort);

    /// Sends a message. False if nothing was sent: the AI is busy, the history is not loaded yet, the
    /// text is blank or the AI service is not set up (then errorReported() tells the user).
    bool send(const QString &text);

    /// Stops the answer that is on its way.
    void stop();

    /// Starts an empty conversation; the old one stays in the database. A running answer is stopped.
    void newChat();

Q_SIGNALS:
    void reasoningEffortChanged(AiChat::ReasoningEffort effort);
    void busyChanged(bool busy);

    /// The history was loaded (isReady() is true now).
    void historyLoaded(const QList<AiChat::StoredMessage> &messages);

    /// The answer that is on its way: what arrived now, and everything so far.
    void assistantDelta(const QString &delta, const QString &textSoFar);

    /// The answer is complete (and stored).
    void assistantFinished(const AiChat::StoredMessage &message);

    /// A request failed. An answer that had started was finished by assistantFinished() before.
    void requestFailed(const QString &error);
    void requestCanceled();

    /// Something the user should know that is not part of the conversation.
    void errorReported(const QString &text);

private Q_SLOTS:
    void slotConversationLoaded(const QList<AiChat::StoredMessage> &messages);
    void slotAssistantDelta(const QString &delta);
    void slotAssistantFinished(const AiChat::StoredMessage &message);
    void slotRequestFailed(const QString &error);
    void slotRequestCanceled();
    void slotBusyChanged(bool busy);
    void slotStorageError(const QString &error);

private:
    void setBusy(bool busy);

    QThread *m_thread {nullptr};
    Worker *m_worker {nullptr};

    // the tools of the AI: they run in the worker thread and reach Krita through the executor
    GuiExecutor *m_executor {nullptr};
    DocumentAccess *m_documentAccess {nullptr}; // not owned
    VisionClient *m_vision {nullptr};
    FalClient *m_fal {nullptr}; // used by the worker thread only
    ToolRegistry *m_tools {nullptr};

    ReasoningEffort m_effort {ReasoningEffort::Medium};
    bool m_ready {false};
    bool m_busy {false};
    bool m_storageWarned {false};
    QString m_streamingText;
};

} // namespace AiChat

#endif
