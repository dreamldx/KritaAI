/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_CHAT_STORE_H_
#define _AI_CHAT_STORE_H_

#include <QList>
#include <QString>

#include "AiChatTypes.h"

class QSqlQuery;

/**
 * The chat history in a SQLite file.
 *
 * Qt's SQL connections belong to the thread that opened them: create the
 * store, open() it and use it in one and the same thread. The panel does all
 * of that in the worker thread, so the GUI thread never waits for the disk.
 *
 * Nothing here throws or asserts: when something fails the method returns an
 * "invalid" value (false, -1, an empty list) and lastError() says why. A
 * store that could not be opened can still be called, it just does nothing.
 */
class AiChatStore
{
public:
    explicit AiChatStore(const QString &path);
    ~AiChatStore();
    AiChatStore(const AiChatStore &) = delete;
    AiChatStore &operator=(const AiChatStore &) = delete;

    /// Opens the file (creating it and its folders if needed) and prepares the tables.
    bool open();
    bool isOpen() const { return m_open; }
    QString lastError() const { return m_lastError; }

    /// Starts a new, empty conversation. Returns its id, or -1.
    qint64 createConversation();

    /// The conversation that was used last (or created last), or -1 if there is none.
    qint64 latestConversationId() const;

    /// All messages of a conversation, oldest first.
    QList<AiChatStoredMessage> messages(qint64 conversationId) const;

    /// Adds a message to a conversation. Returns the id of the message, or -1.
    qint64 appendMessage(qint64 conversationId,
                         const QString &role,
                         const QString &content,
                         const QDateTime &createdAt,
                         qint64 processingMs = -1,
                         qint64 tokens = -1);

private:
    bool execute(QSqlQuery &query) const;

    QString m_path;
    QString m_connection;
    bool m_open {false};
    mutable QString m_lastError;
};

#endif
