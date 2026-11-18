/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_CHAT_WORKER_H_
#define _AI_CHAT_WORKER_H_

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <memory>

#include "AiChatTypes.h"

class AiChatAgent;
class AiChatStore;
class AiChatToolProvider;

/**
 * Everything that must not run in the GUI thread: the AI loop, the network
 * and the database.
 *
 * Make one, move it to a QThread and talk to it only through its slots and
 * signals (queued connections, the Qt default between threads):
 *
 *     thread->start()  ->  init()  ->  conversationLoaded()
 *     submit(...)      ->  assistantDelta()... -> assistantFinished()
 *                          or requestFailed() / requestCanceled()
 *
 * The network manager and the database connection are created inside
 * init(), so they belong to the worker thread.
 */
class AiChatWorker : public QObject
{
    Q_OBJECT
public:
    explicit AiChatWorker(const QString &databasePath, QObject *parent = nullptr);
    ~AiChatWorker() override;

    /// Where the chat history lives by default: a file in Krita's user data folder.
    static QString defaultDatabasePath();

    /// Set these before the worker is moved to its thread.
    void setToolProvider(AiChatToolProvider *provider);
    void setMaxToolRounds(int rounds);

public Q_SLOTS:
    /// Opens the database and loads the latest conversation. Call it when the thread has started.
    void init();

    /// Cancels a running request and cleanly stops the worker. Call it before the thread is stopped.
    void shutdown();

    /// Sends a message of the user. The answer is announced by the signals below.
    void submit(const QString &text, const AiChatRequestSettings &settings);

    /// Stops the request in progress, if any. A partial answer is kept.
    void cancel();

    /// Starts a new conversation; the old one stays in the database.
    void startNewConversation();

Q_SIGNALS:
    /// The conversation that was loaded from the database (may be empty). Sent once, by init().
    void conversationLoaded(const QList<AiChatStoredMessage> &messages);

    /// The database does not work. The chat keeps working, but nothing is saved.
    void storageError(const QString &error);

    /// A piece of the answer that is being received.
    void assistantDelta(const QString &delta);

    /// The answer is complete (or, after cancel() or an error, what had arrived of it) and was saved.
    void assistantFinished(const AiChatStoredMessage &message);

    void requestFailed(const QString &error);
    void requestCanceled();

    /// True while a request is in progress.
    void busyChanged(bool busy);

private Q_SLOTS:
    void onAgentFinished(const QString &text);
    void onAgentFailed(const QString &error, const QString &partialText);
    void onAgentCanceled(const QString &partialText);

private:
    void setBusy(bool busy);
    void endRequest();
    void saveAndAnnounceAnswer(const QString &text);
    void createConversation();
    void persist(AiChatStoredMessage *message);

    QString m_databasePath;
    std::unique_ptr<AiChatStore> m_store;
    AiChatAgent *m_agent {nullptr};
    AiChatToolProvider *m_toolProvider {nullptr};
    int m_maxToolRounds {10};

    qint64 m_conversationId {-1};
    QList<AiChatStoredMessage> m_history;

    bool m_busy {false};
    bool m_newConversationAfterRequest {false};
    QElapsedTimer m_requestTimer;
};

#endif
