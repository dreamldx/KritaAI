/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "../ai/tools/DocumentTools.h"
#include "../config/Config.h"
#include "../model/Chat.h"
#include "../ui/Progress.h"
#include "FakeOpenAiServer.h"

using namespace AiChat;

// These tests drive the model without any panel: the real worker thread and the real database,
// against the fake server.

class ChatTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    std::unique_ptr<FakeServer> m_server;
    int m_dbCounter {0};

    // what the tools need from the GUI thread; they are not used here
    KritaDocumentAccess m_access;
    Progress m_progress;

    QString newDatabasePath() { return m_dir.filePath(QString("chat%1.sqlite").arg(++m_dbCounter)); }

    void configure(bool sendEffort = false, const QString &model = "test-model")
    {
        Config config;
        config.baseUrl = m_server->chatUrl().toString();
        config.apiKey = "sk-test";
        config.model = model;
        config.sendReasoningEffort = sendEffort;
        config.save();
    }

    std::unique_ptr<Chat> newChat() { return std::unique_ptr<Chat>(new Chat(newDatabasePath(), &m_access, &m_progress)); }

    static bool waitReady(Chat &chat)
    {
        return QTest::qWaitFor([&]() { return chat.isReady(); }, 5000);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qRegisterMetaType<ReasoningEffort>();
    }

    void init()
    {
        m_server.reset(new FakeServer());
        QVERIFY(m_server->start());
        configure();
    }

    void cleanup()
    {
        m_server.reset();
    }

    void testTheHistoryIsLoadedOnceAndTheChatIsThenReady()
    {
        auto chat = newChat();
        QSignalSpy loaded(chat.get(), &Chat::historyLoaded);
        QVERIFY(!chat->isReady());
        QVERIFY(waitReady(*chat));
        QCOMPARE(loaded.count(), 1);
        QVERIFY(loaded.first().first().value<QList<StoredMessage>>().isEmpty());
        QVERIFY(!chat->isBusy());
    }

    void testNothingIsSentBeforeTheHistoryIsLoaded()
    {
        auto chat = newChat();
        QVERIFY(!chat->send("too early"));
        QVERIFY(!chat->isBusy());
        QCOMPARE(m_server->requests.size(), 0);
    }

    void testBlankTextIsNotSent()
    {
        auto chat = newChat();
        QVERIFY(waitReady(*chat));
        QVERIFY(!chat->send(""));
        QVERIFY(!chat->send("  \n\t "));
        QVERIFY(!chat->isBusy());
        QCOMPARE(m_server->requests.size(), 0);
    }

    void testNotConfiguredReportsAnErrorAndSendsNothing()
    {
        configure(false, ""); // no model
        auto chat = newChat();
        QVERIFY(waitReady(*chat));
        QSignalSpy errors(chat.get(), &Chat::errorReported);

        QVERIFY(!chat->send("Hello?"));
        QCOMPARE(errors.count(), 1);
        QVERIFY2(errors.first().first().toString().contains("Configure Krita"), qPrintable(errors.first().first().toString()));
        QVERIFY(!chat->isBusy());
        QCOMPARE(m_server->requests.size(), 0);
    }

    void testTheAnswerArrivesPieceByPiece()
    {
        ScriptedResponse reply;
        reply.chunks = {textChunk("Hel"), textChunk("lo"), finishChunk(), doneEvent()};
        reply.chunkDelayMs = 200;
        m_server->enqueue(reply);

        auto chat = newChat();
        QVERIFY(waitReady(*chat));
        QSignalSpy busy(chat.get(), &Chat::busyChanged);
        QSignalSpy deltas(chat.get(), &Chat::assistantDelta);
        QSignalSpy finished(chat.get(), &Chat::assistantFinished);

        QVERIFY(chat->send("  Say hello  "));
        QVERIFY(chat->isBusy()); // at once, before the worker has said anything
        QVERIFY(!chat->send("a second message")); // one request at a time

        QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!chat->isBusy(), 5000);

        QVERIFY(deltas.count() >= 1);
        QCOMPARE(deltas.last().at(1).toString(), QString("Hello")); // everything so far
        const StoredMessage message = finished.first().first().value<StoredMessage>();
        QCOMPARE(message.content, QString("Hello"));
        QCOMPARE(message.role, QString("assistant"));
        QCOMPARE(busy.first().first().toBool(), true);
        QCOMPARE(busy.last().first().toBool(), false);

        // the text of the user went to the server trimmed
        QCOMPARE(m_server->requests.size(), 1);
        QCOMPARE(m_server->requests.first().messages().last().toObject().value("content").toString(), QString("Say hello"));
    }

    void testStopEndsTheAnswerThatWasOnItsWay()
    {
        m_server->enqueue(hangingReply("Part of an answer"));
        auto chat = newChat();
        QVERIFY(waitReady(*chat));
        QSignalSpy deltas(chat.get(), &Chat::assistantDelta);

        QVERIFY(chat->send("go"));
        QTRY_VERIFY_WITH_TIMEOUT(deltas.count() >= 1, 5000);
        QVERIFY(chat->isBusy());

        chat->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!chat->isBusy(), 5000);
    }

    void testNewChatWhileAnAnswerIsComingStopsIt()
    {
        m_server->enqueue(hangingReply("Part"));
        auto chat = newChat();
        QVERIFY(waitReady(*chat));
        QSignalSpy deltas(chat.get(), &Chat::assistantDelta);

        QVERIFY(chat->send("go"));
        QTRY_VERIFY_WITH_TIMEOUT(deltas.count() >= 1, 5000);

        chat->newChat();
        QTRY_VERIFY_WITH_TIMEOUT(!chat->isBusy(), 5000);
    }

    void testTheThinkingEffortIsChosenInTheModelAndSentOnlyIfTheSettingSaysSo()
    {
        for (const bool enabled : {false, true}) {
            m_server->requests.clear();
            m_server->enqueue(textReply("ok"));
            configure(enabled);

            auto chat = newChat();
            QSignalSpy changed(chat.get(), &Chat::reasoningEffortChanged);
            QVERIFY(chat->reasoningEffort() == ReasoningEffort::Medium);
            chat->setReasoningEffort(ReasoningEffort::Medium); // no change, no signal
            QCOMPARE(changed.count(), 0);
            chat->setReasoningEffort(ReasoningEffort::High);
            QCOMPARE(changed.count(), 1);
            QVERIFY(chat->reasoningEffort() == ReasoningEffort::High);

            QVERIFY(waitReady(*chat));
            QVERIFY(chat->send("think hard"));
            QTRY_VERIFY_WITH_TIMEOUT(!chat->isBusy() && m_server->requests.size() == 1, 5000);

            const QJsonObject sent = m_server->requests.first().json();
            if (enabled) {
                QCOMPARE(sent.value("reasoning_effort").toString(), QString("high"));
            } else {
                QVERIFY(!sent.contains("reasoning_effort"));
            }
        }
    }

    void testSettingsChangedAfterOpeningCountFromTheNextMessageOn()
    {
        m_server->enqueue(textReply("ok"));
        configure(false, "");
        auto chat = newChat();
        QVERIFY(waitReady(*chat));
        QVERIFY(!chat->send("hi")); // not configured

        configure(false, "my-model"); // the user fills in the settings
        QVERIFY(chat->send("hi"));
        QTRY_VERIFY_WITH_TIMEOUT(!chat->isBusy() && m_server->requests.size() == 1, 5000);
        QCOMPARE(m_server->requests.first().json().value("model").toString(), QString("my-model"));
    }

    void testTheHistoryComesBackWhenTheChatIsOpenedAgain()
    {
        m_server->enqueue(textReply("Hello"));
        const QString path = newDatabasePath();
        {
            Chat chat(path, &m_access, &m_progress);
            QVERIFY(waitReady(chat));
            QVERIFY(chat.send("hi"));
            QTRY_VERIFY_WITH_TIMEOUT(!chat.isBusy() && m_server->requests.size() == 1, 5000);
        }

        Chat again(path, &m_access, &m_progress);
        QSignalSpy loaded(&again, &Chat::historyLoaded);
        QVERIFY(waitReady(again));
        QCOMPARE(loaded.count(), 1);
        const QList<StoredMessage> messages = loaded.first().first().value<QList<StoredMessage>>();
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages.at(0).role, QString("user"));
        QCOMPARE(messages.at(1).content, QString("Hello"));
    }

    void testClosingTheChatWhileAnAnswerIsComingDoesNotHang()
    {
        m_server->enqueue(hangingReply("Part"));
        auto chat = newChat();
        QVERIFY(waitReady(*chat));
        QSignalSpy deltas(chat.get(), &Chat::assistantDelta);
        QVERIFY(chat->send("go"));
        QTRY_VERIFY_WITH_TIMEOUT(deltas.count() >= 1, 5000);
        chat.reset(); // must return
    }

    // ---- the request that is made from the settings

    void testTheRequestComesFromTheSettings()
    {
        Config config;
        config.baseUrl = "https://example.com/v1";
        config.apiKey = "sk-chat";
        config.model = "chat-model";
        config.sendReasoningEffort = true;
        config.visionBaseUrl = "https://vision.example.com/v1";
        config.visionApiKey = "sk-vision";
        config.visionModel = "  vision-model ";
        config.visionDisableThinking = false;
        config.visionProviderBlacklist = "Nebius, Foo";
        config.falApiKey = " fal-key ";

        const RequestSettings low = makeRequestSettings(config, ReasoningEffort::Low);
        QCOMPARE(low.url, QUrl("https://example.com/v1/chat/completions"));
        QCOMPARE(low.apiKey, QString("sk-chat"));
        QCOMPARE(low.model, QString("chat-model"));
        QCOMPARE(low.reasoningEffort, QString("low"));
        QCOMPARE(makeRequestSettings(config, ReasoningEffort::Medium).reasoningEffort, QString("medium"));
        QCOMPARE(makeRequestSettings(config, ReasoningEffort::High).reasoningEffort, QString("high"));

        QCOMPARE(low.vision.url, QUrl("https://vision.example.com/v1/chat/completions"));
        QCOMPARE(low.vision.apiKey, QString("sk-vision"));
        QCOMPARE(low.vision.model, QString("vision-model")); // trimmed
        QVERIFY(!low.vision.disableThinking);
        QCOMPARE(low.vision.ignoreProviders, (QStringList {"Nebius", "Foo"}));
        QCOMPARE(low.fal.apiKey, QString("fal-key")); // trimmed

        // the effort is only sent when the settings say so
        config.sendReasoningEffort = false;
        QVERIFY(makeRequestSettings(config, ReasoningEffort::High).reasoningEffort.isEmpty());

        // without a vision model nothing of it is set
        config.visionBaseUrl.clear();
        QVERIFY(!makeRequestSettings(config, ReasoningEffort::High).vision.isValid());
    }
};

int main(int argc, char **argv)
{
    // never touch the real settings and the real chat history of the person running the tests
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    ChatTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ChatTest.moc"
