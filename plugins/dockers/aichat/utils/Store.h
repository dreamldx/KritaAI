/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _UTILS_STORE_H_
#define _UTILS_STORE_H_

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>

class QSqlQuery;

namespace AiChat {

/// A chat message as it is shown in the panel and kept in the database.
struct StoredMessage {
    qint64 id {-1};
    QString role; ///< "user" or "assistant"
    QString content;
    QDateTime createdAt;
    qint64 processingMs {-1}; ///< assistant messages only: how long the AI needed, -1 if unknown
    qint64 tokens {-1}; ///< assistant messages only: tokens the request(s) used, -1 if the server did not say
};

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
class Store
{
public:
    explicit Store(const QString &path);
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    /// Opens the file (creating it and its folders if needed) and prepares the tables.
    bool open();
    bool isOpen() const { return m_open; }
    QString lastError() const { return m_lastError; }

    /// Starts a new, empty conversation. Returns its id, or -1.
    qint64 createConversation();

    /// The conversation that was used last (or created last), or -1 if there is none.
    qint64 latestConversationId() const;

    /// All messages of a conversation, oldest first.
    QList<StoredMessage> messages(qint64 conversationId) const;

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

} // namespace AiChat

Q_DECLARE_METATYPE(AiChat::StoredMessage)
Q_DECLARE_METATYPE(QList<AiChat::StoredMessage>)

#endif
