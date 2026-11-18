/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include "../AiChatStore.h"

class AiChatStoreTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString dbPath(const char *name) const
    {
        return m_dir.filePath(QString::fromLatin1(name));
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY2(QSqlDatabase::drivers().contains("QSQLITE"), "the SQLite driver is not available");
    }

    void testOpenCreatesFileAndFolders()
    {
        const QString path = m_dir.filePath("deep/er/chat.sqlite");
        {
            AiChatStore store(path);
            QVERIFY2(store.open(), qPrintable(store.lastError()));
            QVERIFY(store.isOpen());
        }
        QVERIFY(QFile::exists(path));
    }

    void testEmptyDatabaseHasNoConversation()
    {
        AiChatStore store(dbPath("empty.sqlite"));
        QVERIFY(store.open());
        QCOMPARE(store.latestConversationId(), qint64(-1));
    }

    void testMessagesRoundTrip()
    {
        AiChatStore store(dbPath("roundtrip.sqlite"));
        QVERIFY(store.open());

        const qint64 conversation = store.createConversation();
        QVERIFY(conversation > 0);

        const QDateTime t1 = QDateTime::fromMSecsSinceEpoch(1700000000123);
        const QDateTime t2 = QDateTime::fromMSecsSinceEpoch(1700000005456);
        const qint64 id1 = store.appendMessage(conversation, "user", "Make the sky dramatic", t1);
        const qint64 id2 = store.appendMessage(conversation, "assistant", "Done.\nSecond line, with \"quotes\" and 你好 and 'apostrophes'.", t2, 2300);
        QVERIFY(id1 > 0);
        QVERIFY(id2 > id1);

        const QList<AiChatStoredMessage> messages = store.messages(conversation);
        QCOMPARE(messages.size(), 2);

        QCOMPARE(messages[0].id, id1);
        QCOMPARE(messages[0].role, QString("user"));
        QCOMPARE(messages[0].content, QString("Make the sky dramatic"));
        QCOMPARE(messages[0].createdAt.toMSecsSinceEpoch(), t1.toMSecsSinceEpoch());
        QCOMPARE(messages[0].processingMs, qint64(-1));

        QCOMPARE(messages[1].role, QString("assistant"));
        QCOMPARE(messages[1].content, QString("Done.\nSecond line, with \"quotes\" and 你好 and 'apostrophes'."));
        QCOMPARE(messages[1].createdAt.toMSecsSinceEpoch(), t2.toMSecsSinceEpoch());
        QCOMPARE(messages[1].processingMs, qint64(2300));
    }

    void testSurvivesReopening()
    {
        const QString path = dbPath("persist.sqlite");
        qint64 conversation = -1;
        {
            AiChatStore store(path);
            QVERIFY(store.open());
            conversation = store.createConversation();
            store.appendMessage(conversation, "user", "hello", QDateTime::currentDateTime());
            store.appendMessage(conversation, "assistant", "hi there", QDateTime::currentDateTime(), 10);
        } // closed here, as when Krita quits

        AiChatStore again(path);
        QVERIFY(again.open());
        QCOMPARE(again.latestConversationId(), conversation);
        const QList<AiChatStoredMessage> messages = again.messages(conversation);
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages[0].content, QString("hello"));
        QCOMPARE(messages[1].content, QString("hi there"));
    }

    void testConversationsAreSeparate()
    {
        AiChatStore store(dbPath("separate.sqlite"));
        QVERIFY(store.open());

        const qint64 a = store.createConversation();
        const qint64 b = store.createConversation();
        QVERIFY(a != b);

        store.appendMessage(a, "user", "in a", QDateTime::currentDateTime());
        store.appendMessage(b, "user", "in b", QDateTime::currentDateTime());
        store.appendMessage(b, "assistant", "also in b", QDateTime::currentDateTime());

        QCOMPARE(store.messages(a).size(), 1);
        QCOMPARE(store.messages(b).size(), 2);
        QCOMPARE(store.messages(a).first().content, QString("in a"));
    }

    void testLatestConversationFollowsActivity()
    {
        AiChatStore store(dbPath("latest.sqlite"));
        QVERIFY(store.open());

        const qint64 a = store.createConversation();
        const qint64 b = store.createConversation();
        QCOMPARE(store.latestConversationId(), b); // b is newer

        // a message in the older conversation makes it the latest one
        QTest::qWait(5);
        store.appendMessage(a, "user", "back to a", QDateTime::currentDateTime());
        QCOMPARE(store.latestConversationId(), a);
    }

    void testMessagesKeepInsertionOrder()
    {
        AiChatStore store(dbPath("order.sqlite"));
        QVERIFY(store.open());
        const qint64 c = store.createConversation();

        // identical timestamps: the order must still be the order of insertion
        const QDateTime same = QDateTime::fromMSecsSinceEpoch(1700000000000);
        for (int i = 0; i < 20; ++i) {
            store.appendMessage(c, i % 2 ? "assistant" : "user", QString::number(i), same);
        }
        const QList<AiChatStoredMessage> messages = store.messages(c);
        QCOMPARE(messages.size(), 20);
        for (int i = 0; i < 20; ++i) {
            QCOMPARE(messages[i].content, QString::number(i));
        }
    }

    void testAppendToUnknownConversationFails()
    {
        AiChatStore store(dbPath("unknown.sqlite"));
        QVERIFY(store.open());
        QCOMPARE(store.appendMessage(9999, "user", "x", QDateTime::currentDateTime()), qint64(-1));
        QVERIFY(!store.lastError().isEmpty());
    }

    void testCannotOpenIsReportedNotFatal()
    {
        // a path whose parent is a regular file can never be created
        QFile blocker(m_dir.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();

        AiChatStore store(m_dir.filePath("blocker/chat.sqlite"));
        QVERIFY(!store.open());
        QVERIFY(!store.isOpen());
        QVERIFY(!store.lastError().isEmpty());

        // using a store that is not open must not crash
        QCOMPARE(store.latestConversationId(), qint64(-1));
        QCOMPARE(store.createConversation(), qint64(-1));
        QVERIFY(store.messages(1).isEmpty());
    }

    void testSchemaVersionIsRecorded()
    {
        const QString path = dbPath("version.sqlite");
        {
            AiChatStore store(path);
            QVERIFY(store.open());
        }

        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "version-check");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("PRAGMA user_version"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 2);
        }
        QSqlDatabase::removeDatabase("version-check");
    }

    void testTokensRoundTrip()
    {
        AiChatStore store(dbPath("tokens.sqlite"));
        QVERIFY(store.open());
        const qint64 c = store.createConversation();
        store.appendMessage(c, "assistant", "counted", QDateTime::currentDateTime(), 100, 1234);
        store.appendMessage(c, "assistant", "not counted", QDateTime::currentDateTime(), 100);

        const QList<AiChatStoredMessage> messages = store.messages(c);
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages[0].tokens, qint64(1234));
        QCOMPARE(messages[1].tokens, qint64(-1)); // unknown stays unknown, it is not 0
    }

    void testDatabaseOfTheFirstVersionIsUpgraded()
    {
        // what the first version of the plugin wrote: no "tokens" column, user_version 1
        const QString path = dbPath("v1.sqlite");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "v1-maker");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TABLE conversations (id INTEGER PRIMARY KEY AUTOINCREMENT,"
                               " created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL)"));
            QVERIFY(query.exec("CREATE TABLE messages (id INTEGER PRIMARY KEY AUTOINCREMENT,"
                               " conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,"
                               " role TEXT NOT NULL, content TEXT NOT NULL, created_at INTEGER NOT NULL,"
                               " processing_ms INTEGER)"));
            QVERIFY(query.exec("INSERT INTO conversations (created_at, updated_at) VALUES (1, 1)"));
            QVERIFY(query.exec("INSERT INTO messages (conversation_id, role, content, created_at, processing_ms)"
                               " VALUES (1, 'assistant', 'old answer', 1000, 500)"));
            QVERIFY(query.exec("PRAGMA user_version = 1"));
        }
        QSqlDatabase::removeDatabase("v1-maker");

        AiChatStore store(path);
        QVERIFY2(store.open(), qPrintable(store.lastError()));

        // the old message is still there, without a token count
        QList<AiChatStoredMessage> messages = store.messages(1);
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages[0].content, QString("old answer"));
        QCOMPARE(messages[0].processingMs, qint64(500));
        QCOMPARE(messages[0].tokens, qint64(-1));

        // and new ones can carry a count
        store.appendMessage(1, "assistant", "new answer", QDateTime::currentDateTime(), 10, 99);
        messages = store.messages(1);
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages[1].tokens, qint64(99));
    }

    void testReopeningDoesNotDestroyData()
    {
        const QString path = dbPath("reopen.sqlite");
        for (int round = 0; round < 3; ++round) {
            AiChatStore store(path);
            QVERIFY(store.open());
            qint64 c = store.latestConversationId();
            if (c < 0) {
                c = store.createConversation();
            }
            store.appendMessage(c, "user", QString("round %1").arg(round), QDateTime::currentDateTime());
        }
        AiChatStore store(path);
        QVERIFY(store.open());
        QCOMPARE(store.messages(store.latestConversationId()).size(), 3);
    }
};

QTEST_MAIN(AiChatStoreTest)
#include "AiChatStoreTest.moc"
