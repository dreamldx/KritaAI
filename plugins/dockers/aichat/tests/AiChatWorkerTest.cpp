/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QPointer>
#include <QSignalSpy>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <memory>

#include "../AiChatAgent.h"
#include "../AiChatStore.h"
#include "../AiChatWorker.h"
#include "FakeOpenAiServer.h"

// These tests talk to a small HTTP server that runs inside the test, so no real AI service is
// needed. JSON is built in code and not written as raw string literals: moc 5.15 gives up on a
// file that has a raw string with an escaped quote in it.

namespace {

// ---- tools ------------------------------------------------------------------------------

class FakeTools : public AiChatToolProvider
{
public:
    QJsonArray toolDefinitions() const override
    {
        QJsonObject function;
        function["name"] = "paint";
        function["description"] = "Paints something";
        QJsonObject tool;
        tool["type"] = "function";
        tool["function"] = function;
        return QJsonArray{tool};
    }

    QString runTool(const QString &name, const QString &argumentsJson, bool *ok) override
    {
        QMutexLocker locker(&m_mutex);
        m_calls.append(qMakePair(name, argumentsJson));
        m_threads.append(QThread::currentThread());
        *ok = true;
        return "painted " + QJsonDocument::fromJson(argumentsJson.toUtf8()).object().value("color").toString();
    }

    QList<QPair<QString, QString>> calls() const
    {
        QMutexLocker locker(&m_mutex);
        return m_calls;
    }
    QList<QThread *> threads() const
    {
        QMutexLocker locker(&m_mutex);
        return m_threads;
    }

private:
    mutable QMutex m_mutex;
    QList<QPair<QString, QString>> m_calls;
    QList<QThread *> m_threads;
};

// ---- a worker in its own thread, as the panel uses it -----------------------------------

class WorkerHarness
{
public:
    explicit WorkerHarness(const QString &databasePath, AiChatToolProvider *tools = nullptr, int maxRounds = 10)
        : worker(new AiChatWorker(databasePath))
    {
        worker->setToolProvider(tools);
        worker->setMaxToolRounds(maxRounds);
        worker->moveToThread(&thread);

        loaded.reset(new QSignalSpy(worker, &AiChatWorker::conversationLoaded));
        deltas.reset(new QSignalSpy(worker, &AiChatWorker::assistantDelta));
        finished.reset(new QSignalSpy(worker, &AiChatWorker::assistantFinished));
        failed.reset(new QSignalSpy(worker, &AiChatWorker::requestFailed));
        canceled.reset(new QSignalSpy(worker, &AiChatWorker::requestCanceled));
        busy.reset(new QSignalSpy(worker, &AiChatWorker::busyChanged));
        storageErrors.reset(new QSignalSpy(worker, &AiChatWorker::storageError));

        QObject::connect(&thread, &QThread::started, worker, &AiChatWorker::init);
        thread.start();
    }

    ~WorkerHarness()
    {
        // what the panel does when it goes away
        QMetaObject::invokeMethod(worker, "shutdown", Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait(5000);
        delete worker;
    }

    QString deltaText() const
    {
        QString text;
        for (const QList<QVariant> &args : std::as_const(*deltas)) {
            text += args.first().toString();
        }
        return text;
    }

    AiChatStoredMessage lastFinished() const
    {
        return finished->last().first().value<AiChatStoredMessage>();
    }

    QThread thread;
    AiChatWorker *worker;
    std::unique_ptr<QSignalSpy> loaded, deltas, finished, failed, canceled, busy, storageErrors;
};

bool waitForCount(QSignalSpy &spy, int count, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (spy.count() < count && timer.elapsed() < timeoutMs) {
        spy.wait(50);
    }
    return spy.count() >= count;
}

} // namespace

class AiChatWorkerTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    std::unique_ptr<FakeServer> m_server;

    QString dbPath(const char *name) const { return m_dir.filePath(QString::fromLatin1(name)); }

    AiChatRequestSettings settings(const QString &effort = QString()) const
    {
        AiChatRequestSettings s;
        s.url = m_server->chatUrl();
        s.apiKey = "sk-test";
        s.model = "test-model";
        s.reasoningEffort = effort;
        return s;
    }

    static void submit(WorkerHarness &h, const QString &text, const AiChatRequestSettings &s)
    {
        QMetaObject::invokeMethod(h.worker, "submit", Qt::QueuedConnection, Q_ARG(QString, text), Q_ARG(AiChatRequestSettings, s));
    }

    static void invoke(WorkerHarness &h, const char *slot)
    {
        QMetaObject::invokeMethod(h.worker, slot, Qt::QueuedConnection);
    }

    /// What the database holds, read by an independent connection.
    static QList<AiChatStoredMessage> storedMessages(const QString &path, qint64 conversation)
    {
        AiChatStore store(path);
        return store.open() ? store.messages(conversation) : QList<AiChatStoredMessage>();
    }

    static qint64 latestConversation(const QString &path)
    {
        AiChatStore store(path);
        return store.open() ? store.latestConversationId() : -1;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qRegisterMetaType<AiChatStoredMessage>();
        qRegisterMetaType<QList<AiChatStoredMessage>>();
        qRegisterMetaType<AiChatRequestSettings>();
    }

    void testTheBuildCanDoHttps()
    {
        // Real services are "https://": without a working TLS library every request would fail.
        QVERIFY2(QSslSocket::supportsSsl(),
                 qPrintable("TLS is not available: " + QSslSocket::sslLibraryBuildVersionString()
                            + " / " + QSslSocket::sslLibraryVersionString()));
        qInfo() << "TLS library:" << QSslSocket::sslLibraryVersionString();
    }

    void init()
    {
        m_server.reset(new FakeServer());
        QVERIFY(m_server->start());
    }

    void cleanup()
    {
        m_server.reset();
    }

    // ---- the happy path ------------------------------------------------------------------

    void testStreamedReply()
    {
        ScriptedResponse reply;
        reply.chunks = {textChunk("Hel"), textChunk("lo "), textChunk("world"), finishChunk(), doneEvent()};
        reply.chunkDelayMs = 5;
        m_server->enqueue(reply);

        WorkerHarness h(dbPath("stream.sqlite"));
        QVERIFY(waitForCount(*h.loaded, 1));
        QVERIFY(h.loaded->first().first().value<QList<AiChatStoredMessage>>().isEmpty());

        submit(h, "hi", settings());
        QVERIFY(waitForCount(*h.finished, 1));

        QCOMPARE(h.deltaText(), QString("Hello world"));
        QVERIFY(h.deltas->count() >= 3); // it arrived piece by piece, not all at once
        QCOMPARE(h.failed->count(), 0);

        const AiChatStoredMessage answer = h.lastFinished();
        QCOMPARE(answer.role, QString("assistant"));
        QCOMPARE(answer.content, QString("Hello world"));
        QVERIFY(answer.processingMs >= 0);
        QVERIFY(answer.id > 0);

        // busy went up and came down again
        QVERIFY(waitForCount(*h.busy, 2));
        QCOMPARE(h.busy->at(0).first().toBool(), true);
        QCOMPARE(h.busy->at(1).first().toBool(), false);

        // what was sent
        QCOMPARE(m_server->requests.size(), 1);
        const RecordedRequest &request = m_server->requests.first();
        QCOMPARE(request.path, QByteArray("/v1/chat/completions"));
        QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer sk-test"));
        QVERIFY(request.headers.value("content-type").contains("application/json"));
        const QJsonObject body = request.json();
        QCOMPARE(body.value("model").toString(), QString("test-model"));
        QCOMPARE(body.value("stream").toBool(), true);
        // without this a streamed answer carries no token count
        QCOMPARE(body.value("stream_options").toObject().value("include_usage").toBool(), true);
        QVERIFY(!body.contains("reasoning_effort"));
        QVERIFY(!body.contains("tools"));
        QCOMPARE(request.messages().size(), 1);
        QCOMPARE(request.messages().first().toObject().value("role").toString(), QString("user"));
        QCOMPARE(request.messages().first().toObject().value("content").toString(), QString("hi"));
    }

    void testTokensAreReportedAndSaved()
    {
        ScriptedResponse reply;
        reply.chunks = {textChunk("Hi"), finishChunk(), usageChunk(40, 2), doneEvent()};
        m_server->enqueue(reply);
        m_server->enqueue(textReply("no count here")); // a server that sends no usage

        const QString path = dbPath("tokens.sqlite");
        {
            WorkerHarness h(path);
            QVERIFY(waitForCount(*h.loaded, 1));

            submit(h, "one", settings());
            QVERIFY(waitForCount(*h.finished, 1));
            QCOMPARE(h.lastFinished().tokens, qint64(42));

            submit(h, "two", settings());
            QVERIFY(waitForCount(*h.finished, 2));
            QCOMPARE(h.lastFinished().tokens, qint64(-1));
        }

        // the count is in the database, and "unknown" stays unknown
        WorkerHarness again(path);
        QVERIFY(waitForCount(*again.loaded, 1));
        const QList<AiChatStoredMessage> history = again.loaded->first().first().value<QList<AiChatStoredMessage>>();
        QCOMPARE(history.size(), 4);
        QCOMPARE(history[1].tokens, qint64(42));
        QCOMPARE(history[3].tokens, qint64(-1));
        QCOMPARE(history[0].tokens, qint64(-1)); // the user's message
    }

    void testTokensOfAllRoundsAreAddedUp()
    {
        ScriptedResponse call;
        call.chunks = {toolCallChunk(0, "call_1", "paint", "{}"), finishChunk("tool_calls"), usageChunk(100, 10), doneEvent()};
        m_server->enqueue(call);
        ScriptedResponse answer;
        answer.chunks = {textChunk("Done."), finishChunk(), usageChunk(150, 5), doneEvent()};
        m_server->enqueue(answer);

        FakeTools tools;
        WorkerHarness h(dbPath("tokenrounds.sqlite"), &tools);
        QVERIFY(waitForCount(*h.loaded, 1));
        submit(h, "paint", settings());
        QVERIFY(waitForCount(*h.finished, 1));
        QCOMPARE(h.lastFinished().tokens, qint64(100 + 10 + 150 + 5));
    }

    void testNoAuthorizationHeaderWithoutKey()
    {
        m_server->enqueue(textReply("ok"));
        WorkerHarness h(dbPath("nokey.sqlite"));
        QVERIFY(waitForCount(*h.loaded, 1));

        AiChatRequestSettings s = settings();
        s.apiKey.clear();
        submit(h, "hi", s);
        QVERIFY(waitForCount(*h.finished, 1));
        QVERIFY(!m_server->requests.first().headers.contains("authorization"));
    }

    void testReasoningEffortIsSentOnlyWhenSet()
    {
        m_server->enqueue(textReply("one"));
        m_server->enqueue(textReply("two"));
        WorkerHarness h(dbPath("effort.sqlite"));
        QVERIFY(waitForCount(*h.loaded, 1));

        submit(h, "a", settings());
        QVERIFY(waitForCount(*h.finished, 1));
        submit(h, "b", settings("high"));
        QVERIFY(waitForCount(*h.finished, 2));

        QVERIFY(!m_server->requests[0].json().contains("reasoning_effort"));
        QCOMPARE(m_server->requests[1].json().value("reasoning_effort").toString(), QString("high"));
    }

    void testWholeConversationIsTheContext()
    {
        m_server->enqueue(textReply("first answer"));
        m_server->enqueue(textReply("second answer"));
        WorkerHarness h(dbPath("context.sqlite"));
        QVERIFY(waitForCount(*h.loaded, 1));

        submit(h, "first question", settings());
        QVERIFY(waitForCount(*h.finished, 1));
        submit(h, "second question", settings());
        QVERIFY(waitForCount(*h.finished, 2));

        const QJsonArray messages = m_server->requests[1].messages();
        QCOMPARE(messages.size(), 3);
        QCOMPARE(messages[0].toObject().value("role").toString(), QString("user"));
        QCOMPARE(messages[0].toObject().value("content").toString(), QString("first question"));
        QCOMPARE(messages[1].toObject().value("role").toString(), QString("assistant"));
        QCOMPARE(messages[1].toObject().value("content").toString(), QString("first answer"));
        QCOMPARE(messages[2].toObject().value("role").toString(), QString("user"));
        QCOMPARE(messages[2].toObject().value("content").toString(), QString("second question"));
    }

    void testAnswerThatIsNotStreamed()
    {
        QJsonObject message;
        message["role"] = "assistant";
        message["content"] = "All done.";
        QJsonObject choice;
        choice["message"] = message;
        choice["finish_reason"] = "stop";
        QJsonObject response;
        response["choices"] = QJsonArray{choice};

        ScriptedResponse reply;
        reply.contentType = "application/json";
        reply.chunks = {QJsonDocument(response).toJson(QJsonDocument::Compact)};
        m_server->enqueue(reply);

        WorkerHarness h(dbPath("plainjson.sqlite"));
        QVERIFY(waitForCount(*h.loaded, 1));
        submit(h, "hi", settings());
        QVERIFY(waitForCount(*h.finished, 1));
        QCOMPARE(h.lastFinished().content, QString("All done."));
        QCOMPARE(h.deltaText(), QString("All done."));
    }

    // ---- threads -------------------------------------------------------------------------

    void testEverythingRunsOutsideTheCallingThread()
    {
        m_server->enqueue(textReply("hello"));
        WorkerHarness h(dbPath("threads.sqlite"));
        QVERIFY(waitForCount(*h.loaded, 1));

        QVERIFY(h.worker->thread() == &h.thread);
        QVERIFY(h.worker->thread() != QThread::currentThread());

        // A direct connection runs in the thread that emits the signal: that shows where the
        // network handling happens.
        QThread *deltaThread = nullptr;
        QObject::connect(h.worker, &AiChatWorker::assistantDelta, h.worker, [&deltaThread](const QString &) {
            deltaThread = QThread::currentThread();
        }, Qt::DirectConnection);

        submit(h, "hi", settings());
        QVERIFY(waitForCount(*h.finished, 1));

        QVERIFY(deltaThread != nullptr);
        QVERIFY(deltaThread == &h.thread);
        QVERIFY(deltaThread != QThread::currentThread());
    }

    void testCallerIsNotBlockedWhileTheServerThinks()
    {
        ScriptedResponse slow = textReply("finally");
        slow.firstDelayMs = 400;
        m_server->enqueue(slow);

        WorkerHarness h(dbPath("slow.sqlite"));
        QVERIFY(waitForCount(*h.loaded, 1));

        QElapsedTimer timer;
        timer.start();
        submit(h, "hi", settings());
        // submit() only queues the call: the caller is free at once
        QVERIFY2(timer.elapsed() < 100, "submit() blocked the calling thread");

        QVERIFY(waitForCount(*h.finished, 1));
        QVERIFY(timer.elapsed() >= 350); // the answer really took that long
        QVERIFY(h.lastFinished().processingMs >= 350);
    }

    // ---- errors --------------------------------------------------------------------------

    void testHttpErrorIsReportedAndTheNextRequestWorks()
    {
        QJsonObject error;
        error["message"] = "Incorrect API key provided";
        QJsonObject body;
        body["error"] = error;
        ScriptedResponse unauthorized;
        unauthorized.status = 401;
        unauthorized.contentType = "application/json";
        unauthorized.chunks = {QJsonDocument(body).toJson(QJsonDocument::Compact)};
        m_server->enqueue(unauthorized);
        m_server->enqueue(textReply("works now"));

        const QString path = dbPath("http401.sqlite");
        WorkerHarness h(path);
        QVERIFY(waitForCount(*h.loaded, 1));

        submit(h, "hi", settings());
        QVERIFY(waitForCount(*h.failed, 1));
        const QString message = h.failed->first().first().toString();
        QVERIFY2(message.contains("401"), qPrintable(message));
        QVERIFY2(message.contains("Incorrect API key provided"), qPrintable(message));
        QCOMPARE(h.finished->count(), 0);

        QVERIFY(waitForCount(*h.busy, 2));
        QCOMPARE(h.busy->last().first().toBool(), false); // not stuck

        // the user's message is kept, there is no answer to it
        QCOMPARE(storedMessages(path, latestConversation(path)).size(), 1);

        submit(h, "hi again", settings());
        QVERIFY(waitForCount(*h.finished, 1));
        QCOMPARE(h.lastFinished().content, QString("works now"));
    }

    void testConnectionRefused()
    {
        // a port nobody listens on
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();

        WorkerHarness h(dbPath("refused.sqlite"));
        QVERIFY(waitForCount(*h.loaded, 1));

        AiChatRequestSettings s = settings();
        s.url = QUrl(QString("http://127.0.0.1:%1/v1/chat/completions").arg(port));
        submit(h, "hi", s);
        QVERIFY(waitForCount(*h.failed, 1));
        QVERIFY(!h.failed->first().first().toString().isEmpty());
        QVERIFY(waitForCount(*h.busy, 2));
        QCOMPARE(h.busy->last().first().toBool(), false);
    }

    void testServerReportsAnErrorInTheStream()
    {
        QJsonObject error;
        error["message"] = "The model is overloaded";
        QJsonObject chunk;
        chunk["error"] = error;
        ScriptedResponse reply;
        reply.chunks = {sse(chunk)};
        m_server->enqueue(reply);

        WorkerHarness h(dbPath("streamerror.sqlite"));
        QVERIFY(waitForCount(*h.loaded, 1));
        submit(h, "hi", settings());
        QVERIFY(waitForCount(*h.failed, 1));
        QVERIFY(h.failed->first().first().toString().contains("overloaded"));
    }

    void testEmptyAnswerIsAnError()
    {
        ScriptedResponse reply;
        reply.chunks = {finishChunk(), doneEvent()};
        m_server->enqueue(reply);

        const QString path = dbPath("empty.sqlite");
        WorkerHarness h(path);
        QVERIFY(waitForCount(*h.loaded, 1));
        submit(h, "hi", settings());
        QVERIFY(waitForCount(*h.failed, 1));
        QCOMPARE(h.finished->count(), 0);
    }

    void testAConnectionThatDropsMidAnswerKeepsWhatArrived()
    {
        // text, then the connection is closed without a finish reason or [DONE]
        ScriptedResponse reply;
        reply.chunks = {textChunk("Half an ans")};
        m_server->enqueue(reply);

        const QString path = dbPath("dropped.sqlite");
        WorkerHarness h(path);
        QVERIFY(waitForCount(*h.loaded, 1));
        submit(h, "hi", settings());
        QVERIFY(waitForCount(*h.failed, 1));

        // the partial text was shown, so it is saved as well
        QCOMPARE(h.finished->count(), 1);
        QCOMPARE(h.lastFinished().content, QString("Half an ans"));
        const QList<AiChatStoredMessage> stored = storedMessages(path, latestConversation(path));
        QCOMPARE(stored.size(), 2);
        QCOMPARE(stored.last().content, QString("Half an ans"));
    }

    void testSecondRequestWhileBusyIsRejected()
    {
        m_server->enqueue(hangingReply("working"));
        WorkerHarness h(dbPath("busy.sqlite"));
        QVERIFY(waitForCount(*h.loaded, 1));

        submit(h, "first", settings());
        QVERIFY(waitForCount(*h.deltas, 1));

        submit(h, "second", settings());
        QVERIFY(waitForCount(*h.failed, 1));
        QVERIFY(h.failed->first().first().toString().contains("previous"));
        QCOMPARE(m_server->requests.size(), 1); // the second never reached the server

        invoke(h, "cancel");
        QVERIFY(waitForCount(*h.canceled, 1));
    }

    // ---- cancel --------------------------------------------------------------------------

    void testCancelKeepsThePartialAnswer()
    {
        m_server->enqueue(hangingReply("Part"));
        m_server->enqueue(textReply("fine"));

        const QString path = dbPath("cancel.sqlite");
        WorkerHarness h(path);
        QVERIFY(waitForCount(*h.loaded, 1));

        submit(h, "hi", settings());
        QVERIFY(waitForCount(*h.deltas, 1));
        invoke(h, "cancel");
        QVERIFY(waitForCount(*h.canceled, 1));

        QCOMPARE(h.failed->count(), 0);
        QCOMPARE(h.finished->count(), 1);
        QCOMPARE(h.lastFinished().content, QString("Part"));
        QVERIFY(waitForCount(*h.busy, 2));
        QCOMPARE(h.busy->last().first().toBool(), false);

        QCOMPARE(storedMessages(path, latestConversation(path)).size(), 2);

        // the worker is not stuck, and the partial answer is part of the next context
        submit(h, "again", settings());
        QVERIFY(waitForCount(*h.finished, 2));
        QCOMPARE(m_server->requests[1].messages().size(), 3);
        QCOMPARE(m_server->requests[1].messages()[1].toObject().value("content").toString(), QString("Part"));
    }

    void testCancelBeforeAnyTextSavesNoAnswer()
    {
        ScriptedResponse silent;
        silent.chunks = {};
        silent.hangAfterChunks = true;
        m_server->enqueue(silent);

        const QString path = dbPath("cancelsilent.sqlite");
        WorkerHarness h(path);
        QVERIFY(waitForCount(*h.loaded, 1));

        submit(h, "hi", settings());
        QVERIFY(waitForCount(*h.busy, 1)); // started
        QTRY_COMPARE_WITH_TIMEOUT(m_server->requests.size(), 1, 5000);
        invoke(h, "cancel");
        QVERIFY(waitForCount(*h.canceled, 1));

        QCOMPARE(h.finished->count(), 0);
        QCOMPARE(storedMessages(path, latestConversation(path)).size(), 1); // only the user's message
    }

    void testClosingWhileARequestIsRunningDoesNotHang()
    {
        m_server->enqueue(hangingReply("working"));

        QElapsedTimer timer;
        {
            WorkerHarness h(dbPath("closing.sqlite"));
            QVERIFY(waitForCount(*h.loaded, 1));
            submit(h, "hi", settings());
            QVERIFY(waitForCount(*h.deltas, 1));
            timer.start();
        } // the harness shuts the worker down and joins the thread
        QVERIFY2(timer.elapsed() < 3000, "shutting down took too long");
    }

    // ---- tools: the loop -----------------------------------------------------------------

    void testToolCallsAreRunAndTheLoopContinues()
    {
        m_server->enqueue(paintToolCallReply());
        m_server->enqueue(textReply("Painted it."));

        FakeTools tools;
        WorkerHarness h(dbPath("tools.sqlite"), &tools);
        QVERIFY(waitForCount(*h.loaded, 1));

        submit(h, "paint something red", settings());
        QVERIFY(waitForCount(*h.finished, 1));

        QCOMPARE(h.lastFinished().content, QString("Painted it."));
        QCOMPARE(m_server->requests.size(), 2);

        // the tool was run, with the arguments the model assembled from the pieces
        const QList<QPair<QString, QString>> calls = tools.calls();
        QCOMPARE(calls.size(), 1);
        QCOMPARE(calls[0].first, QString("paint"));
        QCOMPARE(calls[0].second, QString("{\"color\":\"red\"}"));
        // ... in the worker thread, never in the caller's
        QVERIFY(tools.threads().first() == &h.thread);

        // the first request offered the tools
        const QJsonArray offered = m_server->requests[0].json().value("tools").toArray();
        QCOMPARE(offered.size(), 1);
        QCOMPARE(offered[0].toObject().value("function").toObject().value("name").toString(), QString("paint"));

        // the second request carries the model's tool call and the result
        const QJsonArray messages = m_server->requests[1].messages();
        QCOMPARE(messages.size(), 3);
        const QJsonObject assistant = messages[1].toObject();
        QCOMPARE(assistant.value("role").toString(), QString("assistant"));
        const QJsonObject call = assistant.value("tool_calls").toArray().first().toObject();
        QCOMPARE(call.value("id").toString(), QString("call_1"));
        QCOMPARE(call.value("function").toObject().value("name").toString(), QString("paint"));
        const QJsonObject result = messages[2].toObject();
        QCOMPARE(result.value("role").toString(), QString("tool"));
        QCOMPARE(result.value("tool_call_id").toString(), QString("call_1"));
        QCOMPARE(result.value("content").toString(), QString("painted red"));
    }

    void testToolCallWithoutToolsIsAnError()
    {
        m_server->enqueue(paintToolCallReply());

        WorkerHarness h(dbPath("notools.sqlite"));
        QVERIFY(waitForCount(*h.loaded, 1));
        submit(h, "paint", settings());
        QVERIFY(waitForCount(*h.failed, 1));
        QVERIFY(h.failed->first().first().toString().contains("tool"));
        QCOMPARE(m_server->requests.size(), 1);
        QVERIFY(!m_server->requests[0].json().contains("tools"));
    }

    void testARunawayLoopIsStopped()
    {
        for (int i = 0; i < 6; ++i) {
            m_server->enqueue(paintToolCallReply(QString("call_%1").arg(i)));
        }

        FakeTools tools;
        WorkerHarness h(dbPath("runaway.sqlite"), &tools, 3);
        QVERIFY(waitForCount(*h.loaded, 1));
        submit(h, "paint forever", settings());
        QVERIFY(waitForCount(*h.failed, 1));

        QVERIFY(h.failed->first().first().toString().contains("3"));
        QCOMPARE(m_server->requests.size(), 3); // exactly the allowed number of rounds
        QCOMPARE(tools.calls().size(), 3);
        QVERIFY(waitForCount(*h.busy, 2));
        QCOMPARE(h.busy->last().first().toBool(), false);
    }

    void testTextOfSeveralRoundsIsJoined()
    {
        ScriptedResponse first;
        first.chunks = {textChunk("Let me paint."), toolCallChunk(0, "call_1", "paint", "{\"color\":\"blue\"}"), finishChunk("tool_calls"), doneEvent()};
        m_server->enqueue(first);
        m_server->enqueue(textReply("Done."));

        FakeTools tools;
        WorkerHarness h(dbPath("rounds.sqlite"), &tools);
        QVERIFY(waitForCount(*h.loaded, 1));
        submit(h, "paint", settings());
        QVERIFY(waitForCount(*h.finished, 1));
        QCOMPARE(h.lastFinished().content, QString("Let me paint.\n\nDone."));
        QCOMPARE(h.deltaText(), QString("Let me paint.\n\nDone."));
    }

    // ---- the history in the database -----------------------------------------------------

    void testHistorySurvivesARestart()
    {
        const QString path = dbPath("restart.sqlite");
        m_server->enqueue(textReply("the answer"));
        m_server->enqueue(textReply("another answer"));

        {
            WorkerHarness h(path);
            QVERIFY(waitForCount(*h.loaded, 1));
            submit(h, "the question", settings());
            QVERIFY(waitForCount(*h.finished, 1));
        } // Krita is closed

        WorkerHarness again(path);
        QVERIFY(waitForCount(*again.loaded, 1));
        const QList<AiChatStoredMessage> history = again.loaded->first().first().value<QList<AiChatStoredMessage>>();
        QCOMPARE(history.size(), 2);
        QCOMPARE(history[0].role, QString("user"));
        QCOMPARE(history[0].content, QString("the question"));
        QCOMPARE(history[1].role, QString("assistant"));
        QCOMPARE(history[1].content, QString("the answer"));
        QVERIFY(history[1].processingMs >= 0);

        // the loaded conversation is the context of the next request
        submit(again, "a follow-up", settings());
        QVERIFY(waitForCount(*again.finished, 1));
        QCOMPARE(m_server->requests.last().messages().size(), 3);
    }

    void testNewConversationStartsEmptyAndKeepsTheOldOne()
    {
        const QString path = dbPath("newchat.sqlite");
        m_server->enqueue(textReply("answer 1"));
        m_server->enqueue(textReply("answer 2"));

        {
            WorkerHarness h(path);
            QVERIFY(waitForCount(*h.loaded, 1));
            submit(h, "first", settings());
            QVERIFY(waitForCount(*h.finished, 1));

            invoke(h, "startNewConversation");
            submit(h, "second", settings());
            QVERIFY(waitForCount(*h.finished, 2));

            // the model does not see the old conversation
            QCOMPARE(m_server->requests[1].messages().size(), 1);
            QCOMPARE(m_server->requests[1].messages().first().toObject().value("content").toString(), QString("second"));
        }

        const qint64 latest = latestConversation(path);
        const QList<AiChatStoredMessage> current = storedMessages(path, latest);
        QCOMPARE(current.size(), 2);
        QCOMPARE(current.first().content, QString("second"));

        // the first conversation is still in the file
        const QList<AiChatStoredMessage> old = storedMessages(path, latest - 1);
        QCOMPARE(old.size(), 2);
        QCOMPARE(old.first().content, QString("first"));
        QCOMPARE(old.last().content, QString("answer 1"));

        // and a restart opens the new one
        WorkerHarness again(path);
        QVERIFY(waitForCount(*again.loaded, 1));
        QCOMPARE(again.loaded->first().first().value<QList<AiChatStoredMessage>>().first().content, QString("second"));
    }

    void testNewConversationWhileEmptyReusesTheConversation()
    {
        const QString path = dbPath("emptyreuse.sqlite");
        {
            WorkerHarness h(path);
            QVERIFY(waitForCount(*h.loaded, 1));
            invoke(h, "startNewConversation");
            invoke(h, "startNewConversation");
            invoke(h, "startNewConversation");
        }
        QCOMPARE(latestConversation(path), qint64(1)); // still the first and only conversation
    }

    void testNewConversationWhileARequestRuns()
    {
        const QString path = dbPath("newwhilebusy.sqlite");
        m_server->enqueue(hangingReply("Part"));
        m_server->enqueue(textReply("fresh answer"));

        {
            WorkerHarness h(path);
            QVERIFY(waitForCount(*h.loaded, 1));
            submit(h, "old question", settings());
            QVERIFY(waitForCount(*h.deltas, 1));

            invoke(h, "startNewConversation"); // cancels the request, then switches
            QVERIFY(waitForCount(*h.canceled, 1));

            submit(h, "new question", settings());
            QVERIFY(waitForCount(*h.finished, 2));

            // the new request knows nothing of the old conversation
            QCOMPARE(m_server->requests[1].messages().size(), 1);
        }

        const qint64 latest = latestConversation(path);
        const QList<AiChatStoredMessage> current = storedMessages(path, latest);
        QCOMPARE(current.size(), 2);
        QCOMPARE(current.first().content, QString("new question"));
        QCOMPARE(current.last().content, QString("fresh answer"));

        // the partial answer went to the old conversation, not the new one
        const QList<AiChatStoredMessage> old = storedMessages(path, latest - 1);
        QCOMPARE(old.size(), 2);
        QCOMPARE(old.first().content, QString("old question"));
        QCOMPARE(old.last().content, QString("Part"));
    }

    // ---- storage that does not work ------------------------------------------------------

    void testChatWorksWithoutAWorkingDatabase()
    {
        // a path whose parent is a regular file can never be created
        QFile blocker(m_dir.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();

        m_server->enqueue(textReply("still here"));
        WorkerHarness h(m_dir.filePath("blocker/chat.sqlite"));
        QVERIFY(waitForCount(*h.loaded, 1));
        QVERIFY(h.storageErrors->count() >= 1);
        QVERIFY(h.loaded->first().first().value<QList<AiChatStoredMessage>>().isEmpty());

        submit(h, "hi", settings());
        QVERIFY(waitForCount(*h.finished, 1));
        QCOMPARE(h.lastFinished().content, QString("still here"));
    }

    void testSubmitBeforeInitIsRejectedNotFatal()
    {
        AiChatWorker worker(dbPath("notready.sqlite"));
        QSignalSpy failed(&worker, &AiChatWorker::requestFailed);
        worker.submit("hi", settings());
        QCOMPARE(failed.count(), 1);
    }
};

QTEST_MAIN(AiChatWorkerTest)
#include "AiChatWorkerTest.moc"
