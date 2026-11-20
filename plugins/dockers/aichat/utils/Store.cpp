/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Store.h"

#include <QAtomicInt>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace AiChat {

namespace {

// Bump this, and add a migration to open(), when the tables change.
const int kSchemaVersion = 2; // 2: messages.tokens

QAtomicInt g_connectionCounter;

} // namespace

Store::Store(const QString &path)
    : m_path(path)
    , m_connection(QStringLiteral("aichat-store-%1").arg(g_connectionCounter.fetchAndAddRelaxed(1)))
{
}

Store::~Store()
{
    if (QSqlDatabase::contains(m_connection)) {
        {
            // the handle must be gone before the connection can be removed
            QSqlDatabase db = QSqlDatabase::database(m_connection, false);
            if (db.isOpen()) {
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(m_connection);
    }
}

bool Store::execute(QSqlQuery &query) const
{
    if (query.exec()) {
        return true;
    }
    m_lastError = query.lastError().text();
    return false;
}

bool Store::open()
{
    m_open = false;
    m_lastError.clear();

    if (!QSqlDatabase::drivers().contains(QStringLiteral("QSQLITE"))) {
        m_lastError = QStringLiteral("The SQLite database driver is not available.");
        return false;
    }

    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        m_lastError = QStringLiteral("Cannot create the folder for %1").arg(m_path);
        return false;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(m_path);
    if (!db.open()) {
        m_lastError = db.lastError().text();
        return false;
    }

    QSqlQuery query(db);
    // Write-ahead logging: appending a message never has to rewrite the whole file.
    query.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    query.exec(QStringLiteral("PRAGMA foreign_keys=ON"));

    int version = 0;
    if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next()) {
        version = query.value(0).toInt();
    }

    if (version > kSchemaVersion) {
        m_lastError = QStringLiteral("The chat database was written by a newer version (schema %1).").arg(version);
        return false;
    }

    if (version < 1) {
        if (!db.transaction()) {
            m_lastError = db.lastError().text();
            return false;
        }

        const QStringList statements = {
            QStringLiteral("CREATE TABLE IF NOT EXISTS conversations ("
                           " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                           " created_at INTEGER NOT NULL,"
                           " updated_at INTEGER NOT NULL)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS messages ("
                           " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                           " conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,"
                           " role TEXT NOT NULL,"
                           " content TEXT NOT NULL,"
                           " created_at INTEGER NOT NULL,"
                           " processing_ms INTEGER,"
                           " tokens INTEGER)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_conversation ON messages(conversation_id, id)"),
            QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion),
        };

        for (const QString &statement : statements) {
            if (!query.exec(statement)) {
                m_lastError = query.lastError().text();
                db.rollback();
                return false;
            }
        }

        if (!db.commit()) {
            m_lastError = db.lastError().text();
            return false;
        }
    } else if (version < 2) {
        // a database of version 1 has the table without the column
        if (!db.transaction()) {
            m_lastError = db.lastError().text();
            return false;
        }
        if (!query.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN tokens INTEGER"))
            || !query.exec(QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion))) {
            m_lastError = query.lastError().text();
            db.rollback();
            return false;
        }
        if (!db.commit()) {
            m_lastError = db.lastError().text();
            return false;
        }
    }

    m_open = true;
    return true;
}

qint64 Store::createConversation()
{
    if (!m_open) {
        return -1;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("INSERT INTO conversations (created_at, updated_at) VALUES (?, ?)"));
    query.addBindValue(now);
    query.addBindValue(now);
    if (!execute(query)) {
        return -1;
    }
    return query.lastInsertId().toLongLong();
}

qint64 Store::latestConversationId() const
{
    if (!m_open) {
        return -1;
    }

    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT id FROM conversations ORDER BY updated_at DESC, id DESC LIMIT 1"));
    if (!execute(query) || !query.next()) {
        return -1;
    }
    return query.value(0).toLongLong();
}

QList<StoredMessage> Store::messages(qint64 conversationId) const
{
    QList<StoredMessage> result;
    if (!m_open) {
        return result;
    }

    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT id, role, content, created_at, processing_ms, tokens"
                                 " FROM messages WHERE conversation_id = ? ORDER BY id"));
    query.addBindValue(conversationId);
    if (!execute(query)) {
        return result;
    }

    while (query.next()) {
        StoredMessage message;
        message.id = query.value(0).toLongLong();
        message.role = query.value(1).toString();
        message.content = query.value(2).toString();
        message.createdAt = QDateTime::fromMSecsSinceEpoch(query.value(3).toLongLong());
        message.processingMs = query.value(4).isNull() ? -1 : query.value(4).toLongLong();
        message.tokens = query.value(5).isNull() ? -1 : query.value(5).toLongLong();
        result.append(message);
    }
    return result;
}

qint64 Store::appendMessage(qint64 conversationId,
                                  const QString &role,
                                  const QString &content,
                                  const QDateTime &createdAt,
                                  qint64 processingMs,
                                  qint64 tokens)
{
    if (!m_open) {
        return -1;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connection);
    if (!db.transaction()) {
        m_lastError = db.lastError().text();
        return -1;
    }

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral("INSERT INTO messages (conversation_id, role, content, created_at, processing_ms, tokens)"
                                  " VALUES (?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(conversationId);
    insert.addBindValue(role);
    insert.addBindValue(content);
    insert.addBindValue(createdAt.toMSecsSinceEpoch());
    insert.addBindValue(processingMs >= 0 ? QVariant(processingMs) : QVariant(QVariant::LongLong)); // NULL if unknown
    insert.addBindValue(tokens >= 0 ? QVariant(tokens) : QVariant(QVariant::LongLong));
    if (!execute(insert)) {
        db.rollback();
        return -1;
    }
    const qint64 id = insert.lastInsertId().toLongLong();

    QSqlQuery touch(db);
    touch.prepare(QStringLiteral("UPDATE conversations SET updated_at = ? WHERE id = ?"));
    touch.addBindValue(QDateTime::currentMSecsSinceEpoch());
    touch.addBindValue(conversationId);
    if (!execute(touch)) {
        db.rollback();
        return -1;
    }

    if (!db.commit()) {
        m_lastError = db.lastError().text();
        return -1;
    }
    return id;
}

} // namespace AiChat
