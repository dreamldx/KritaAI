/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTextDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <algorithm>
#include <functional>
#include <memory>

#include "../AiChatConfig.h"
#include "../AiChatDock.h"
#include "FakeOpenAiServer.h"

// These tests drive the real panel with the real worker thread, against the fake server.

namespace {

/// The message cards in the order they are shown. They are taken from the layout of the history,
/// so a card that was removed is not in the list, and a card that was just added is (a widget
/// that is added to a layout only becomes visible when the event loop runs again).
QList<QFrame *> messageCards(AiChatDock &dock)
{
    QList<QFrame *> cards;
    QScrollArea *scrollArea = dock.findChild<QScrollArea *>();
    QLayout *layout = scrollArea && scrollArea->widget() ? scrollArea->widget()->layout() : nullptr;
    for (int i = 0; layout && i < layout->count(); ++i) {
        QFrame *frame = qobject_cast<QFrame *>(layout->itemAt(i)->widget());
        if (frame && frame->objectName() == "aiChatMessage") {
            cards << frame;
        }
    }
    return cards;
}

/// The n-th card, or nullptr: safe to use while waiting for the card to appear.
QFrame *cardAt(AiChatDock &dock, int index)
{
    return messageCards(dock).value(index, nullptr);
}

/// The labels of a card that are not dimmed: the name, then the body.
QList<QLabel *> mainLabels(QFrame *card)
{
    QList<QLabel *> labels;
    if (!card) {
        return labels;
    }
    for (QLabel *label : card->findChildren<QLabel *>()) {
        if (label->objectName() != "aiChatDim") {
            labels << label;
        }
    }
    return labels;
}

QString cardName(QFrame *card)
{
    return mainLabels(card).value(0) ? mainLabels(card).value(0)->text() : QString();
}

QString cardBody(QFrame *card)
{
    QLabel *body = mainLabels(card).value(1);
    return body ? body->text().remove(QChar(0x200B)) : QString();
}

bool cardShowsProcessingTime(QFrame *card)
{
    if (!card) {
        return false;
    }
    for (QLabel *label : card->findChildren<QLabel *>()) {
        if (label->toolTip() == "Time the AI needed to answer") {
            return true;
        }
    }
    return false;
}

QAbstractButton *buttonByToolTip(AiChatDock &dock, const QString &toolTip)
{
    for (QAbstractButton *button : dock.findChildren<QAbstractButton *>()) {
        if (button->toolTip() == toolTip) {
            return button;
        }
    }
    return nullptr;
}

} // namespace

class AiChatDockTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    std::unique_ptr<FakeServer> m_server;
    int m_dbCounter {0};

    QString newDatabasePath() { return m_dir.filePath(QString("chat%1.sqlite").arg(++m_dbCounter)); }

    void configure(bool sendEffort = false, const QString &model = "test-model")
    {
        AiChatConfig config;
        config.baseUrl = m_server->chatUrl().toString();
        config.apiKey = "sk-test";
        config.model = model;
        config.sendReasoningEffort = sendEffort;
        config.save();
    }

    std::unique_ptr<AiChatDock> newDock(const QString &databasePath)
    {
        std::unique_ptr<AiChatDock> dock(new AiChatDock(databasePath));
        dock->resize(360, 600);
        dock->show();
        return dock;
    }

    /// Types into the input and waits until the panel is ready to send.
    static QPlainTextEdit *type(AiChatDock &dock, const QString &text)
    {
        QPlainTextEdit *input = dock.findChild<QPlainTextEdit *>();
        input->setPlainText(text);
        return input;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
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

    void testAnswerAppearsPieceByPieceAndTheButtonBecomesStop()
    {
        ScriptedResponse reply;
        reply.chunks = {textChunk("Hel"), textChunk("lo"), finishChunk(), doneEvent()};
        reply.chunkDelayMs = 400; // long enough to look at the half-finished answer
        m_server->enqueue(reply);

        auto dock = newDock(newDatabasePath());
        type(*dock, "Say hello");
        QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
        QVERIFY(send);
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000); // the history is loaded
        send->click();

        // at once: the user's card, an AI card and a stop button
        QVERIFY(dock->isBusy());
        QCOMPARE(messageCards(*dock).size(), 2);
        QCOMPARE(cardName(cardAt(*dock, 0)), QString("You"));
        QCOMPARE(cardBody(cardAt(*dock, 0)), QString("Say hello"));
        QCOMPARE(cardName(cardAt(*dock, 1)), QString("AI"));
        QVERIFY(buttonByToolTip(*dock, "Stop") != nullptr);
        QVERIFY(buttonByToolTip(*dock, "Send (Enter)") == nullptr);
        QVERIFY(send->isEnabled()); // the stop button can be clicked
        QVERIFY(dock->findChild<QPlainTextEdit *>()->toPlainText().isEmpty());

        // the half-finished answer is on screen while the rest is still on its way
        QTRY_COMPARE_WITH_TIMEOUT(cardBody(cardAt(*dock, 1)), QString("Hel"), 5000);
        QVERIFY(dock->isBusy());
        QVERIFY(!cardShowsProcessingTime(cardAt(*dock, 1)));

        // finished
        QTRY_COMPARE_WITH_TIMEOUT(cardBody(cardAt(*dock, 1)), QString("Hello"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy(), 5000);
        QVERIFY(cardShowsProcessingTime(cardAt(*dock, 1)));
        QVERIFY(buttonByToolTip(*dock, "Send (Enter)") != nullptr);
        QCOMPARE(messageCards(*dock).size(), 2);
    }

    void testTokenCountIsShownAtTheBottomLeftOfTheTime()
    {
        ScriptedResponse reply;
        reply.chunks = {textChunk("Hello"), finishChunk(), usageChunk(1000, 234), doneEvent()};
        m_server->enqueue(reply);

        const QString path = newDatabasePath();
        {
            auto dock = newDock(path);
            type(*dock, "Say hello");
            QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
            QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
            send->click();
            QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy() && cardBody(cardAt(*dock, 1)) == "Hello", 5000);

            QFrame *card = cardAt(*dock, 1);
            QLabel *tokens = nullptr;
            for (QLabel *label : card->findChildren<QLabel *>()) {
                if (label->text().endsWith(" tokens")) {
                    tokens = label;
                }
            }
            QVERIFY(tokens);
            QVERIFY2(tokens->text().contains("1") && tokens->text().contains("234"), qPrintable(tokens->text()));

            // the footer is the last row of the card: [stretch] tokens seconds
            QLayout *footer = card->layout()->itemAt(card->layout()->count() - 1)->layout();
            QVERIFY(footer);
            QCOMPARE(footer->count(), 3);
            QCOMPARE(footer->itemAt(1)->widget(), tokens);
            const QString seconds = qobject_cast<QLabel *>(footer->itemAt(2)->widget())->text();
            QVERIFY2(seconds.endsWith(" s") && !seconds.contains(':'), qPrintable(seconds)); // "0.0 s", not a clock time

            // the user's card has no count
            for (QLabel *label : cardAt(*dock, 0)->findChildren<QLabel *>()) {
                QVERIFY(!label->text().endsWith(" tokens"));
            }
        }

        // it is part of the history: it is still there when the panel is opened again
        auto again = newDock(path);
        QTRY_COMPARE_WITH_TIMEOUT(messageCards(*again).size(), 2, 5000);
        bool found = false;
        bool seconds = false;
        for (QLabel *label : cardAt(*again, 1)->findChildren<QLabel *>()) {
            found = found || label->text().endsWith(" tokens");
            seconds = seconds || label->text().endsWith(" s");
            QVERIFY2(label->text().count(':') < 2, qPrintable(label->text())); // no HH:mm:ss
        }
        QVERIFY(found);
        QVERIFY(seconds); // the seconds come from the history too
    }

    void testAnswerShowsSecondsAndNoClockTime()
    {
        m_server->enqueue(textReply("Hello"));

        auto dock = newDock(newDatabasePath());
        type(*dock, "Say hello");
        QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
        send->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy() && cardBody(cardAt(*dock, 1)) == "Hello", 5000);

        const QRegularExpression clock("^\\d\\d:\\d\\d:\\d\\d$");
        const QRegularExpression seconds("^\\d+\\.\\d s$");

        // the user's message shows when it was sent
        bool userHasClock = false;
        for (QLabel *label : cardAt(*dock, 0)->findChildren<QLabel *>()) {
            userHasClock = userHasClock || clock.match(label->text()).hasMatch();
        }
        QVERIFY(userHasClock);

        // the answer shows how many seconds it took, and no clock time anywhere
        bool answerHasSeconds = false;
        for (QLabel *label : cardAt(*dock, 1)->findChildren<QLabel *>()) {
            QVERIFY2(!clock.match(label->text()).hasMatch(), qPrintable(label->text()));
            answerHasSeconds = answerHasSeconds || seconds.match(label->text()).hasMatch();
        }
        QVERIFY(answerHasSeconds);
        QVERIFY(cardShowsProcessingTime(cardAt(*dock, 1)));
    }

    // ---- markdown ----

    /// Sends `message`, lets the server answer with `answer` and returns the body label of the answer.
    QLabel *answerLabel(AiChatDock &dock, const QString &message, const QString &answer)
    {
        m_server->enqueue(textReply(answer));
        type(dock, message);
        QAbstractButton *send = buttonByToolTip(dock, "Send (Enter)");
        if (!send || !waitUntil([&]() { return send->isEnabled(); })) {
            return nullptr;
        }
        send->click();
        if (!waitUntil([&]() { return !dock.isBusy() && messageCards(dock).size() == 2; })) {
            return nullptr;
        }
        return mainLabels(cardAt(dock, 1)).value(1);
    }

    static bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!condition()) {
            if (timer.elapsed() > timeoutMs) {
                return false;
            }
            QTest::qWait(10);
        }
        return true;
    }

    void testAnswersAreMarkdownAndTheUsersMessageIsNot()
    {
        auto dock = newDock(newDatabasePath());
        QLabel *answer = answerLabel(*dock, "**not bold**", "A **bold** word and `code`");
        QVERIFY(answer);

        QCOMPARE(answer->textFormat(), Qt::MarkdownText);
        QCOMPARE(answer->text(), QString("A **bold** word and `code`")); // the source is kept (copying, saving)

        // what you wrote yourself is shown as you wrote it
        QLabel *mine = mainLabels(cardAt(*dock, 0)).value(1);
        QVERIFY(mine);
        QCOMPARE(mine->textFormat(), Qt::PlainText);
        QCOMPARE(mine->text(), QString("**not bold**"));
    }

    void testMarkdownIsRenderedNotShownAsSymbols()
    {
        auto dock = newDock(newDatabasePath());
        QLabel *answer = answerLabel(*dock, "hi", "# Title\n\n- one\n- two\n\n**bold**");
        QVERIFY(answer);

        // the label draws the formatted text: no "#", "-" or "**" is left in what is drawn
        QTextDocument rendered;
        rendered.setMarkdown(answer->text());
        const QString plain = rendered.toPlainText();
        QVERIFY2(!plain.contains("#") && !plain.contains("**"), qPrintable(plain));
        QVERIFY(plain.contains("Title") && plain.contains("one") && plain.contains("bold"));
    }

    void testLongAddressesStayLinksAndCanWrap()
    {
        const QString url = "https://example.org/a/very/long/path/that/has/no/spaces/at/all/and/keeps/going/on.html";
        auto dock = newDock(newDatabasePath());
        QLabel *answer = answerLabel(*dock, "hi", "See " + url + " for more.");
        QVERIFY(answer);

        const QString text = answer->text();
        QVERIFY(text.contains(QChar(0x200B))); // the visible text has places to wrap
        // ... but the target of the link is exactly the address, and the full stop is not part of it
        QVERIFY2(text.contains("](" + url + ")"), qPrintable(text));
        QCOMPARE(QString(text).remove(QChar(0x200B)), QString("See [" + url + "](" + url + ") for more."));
    }

    void testWrapPointsDoNotBreakCodeOrLinks()
    {
        const QString run(60, 'x');
        const QString md = "inline `" + run + "` code\n"
                           "a [link](https://example.org/" + run + ") here\n"
                           "<https://example.org/" + run + ">\n"
                           "```\n" + run + "\n```\n"
                           "plain " + run + " word";
        auto dock = newDock(newDatabasePath());
        QLabel *answer = answerLabel(*dock, "hi", md);
        QVERIFY(answer);

        const QStringList lines = answer->text().split('\n');
        QCOMPARE(lines.size(), 7);
        QCOMPARE(lines[0], QString("inline `" + run + "` code")); // untouched
        QCOMPARE(lines[1], QString("a [link](https://example.org/" + run + ") here"));
        QCOMPARE(lines[2], QString("<https://example.org/" + run + ">"));
        QCOMPARE(lines[4], run); // inside a code block
        QVERIFY(lines[6].contains(QChar(0x200B))); // ordinary text wraps
        QCOMPARE(QString(lines[6]).remove(QChar(0x200B)), QString("plain " + run + " word"));
    }

    void testRawHtmlInAnAnswerIsOnlyText()
    {
        // Qt itself would make a link of this, and a bold word of <b>
        QTextDocument unprotected;
        unprotected.setMarkdown("click <a href=\"file:///C:/Windows/notepad.exe\">here</a>");
        QVERIFY(unprotected.toHtml().contains("file:///C:/Windows/notepad.exe\"")); // (what the guard is for)

        auto dock = newDock(newDatabasePath());
        QLabel *answer = answerLabel(*dock, "hi", "click <a href=\"file:///C:/Windows/notepad.exe\">here</a> and <b>bold</b>");
        QVERIFY(answer);

        QTextDocument shown;
        shown.setMarkdown(answer->text());
        QVERIFY2(!shown.toHtml().contains("href=\"file:"), qPrintable(shown.toHtml()));
        // the tags are visible as the text they are (the invisible wrap points are not text)
        const QString visible = shown.toPlainText().remove(QChar(0x200B));
        QVERIFY2(visible.contains("<a href=\"file:///C:/Windows/notepad.exe\">here</a>"), qPrintable(visible));
        QVERIFY2(visible.contains("<b>bold</b>"), qPrintable(visible));
        QVERIFY(!answer->openExternalLinks()); // Qt must not open anything on its own
    }

    void testWebAndMailAutolinksStillWork()
    {
        auto dock = newDock(newDatabasePath());
        QLabel *answer = answerLabel(*dock, "hi", "Read <https://example.org/page> or write <someone@example.org>.");
        QVERIFY(answer);

        QTextDocument shown;
        shown.setMarkdown(answer->text());
        const QString html = shown.toHtml();
        QVERIFY2(html.contains("href=\"https://example.org/page\""), qPrintable(html));
        QVERIFY2(html.contains("href=\"mailto:someone@example.org\""), qPrintable(html));
    }

    void testNoTokenCountWhenTheServerDoesNotSayIt()
    {
        m_server->enqueue(textReply("Hello")); // no usage chunk

        auto dock = newDock(newDatabasePath());
        type(*dock, "Say hello");
        QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
        send->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy() && cardBody(cardAt(*dock, 1)) == "Hello", 5000);

        for (QLabel *label : cardAt(*dock, 1)->findChildren<QLabel *>()) {
            QVERIFY(!label->text().endsWith(" tokens"));
        }
    }

    void testEnterSendsAndShiftEnterBreaksTheLine()
    {
        m_server->enqueue(textReply("ok"));
        auto dock = newDock(newDatabasePath());
        QPlainTextEdit *input = type(*dock, "first line");
        QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);

        input->setFocus();
        input->moveCursor(QTextCursor::End); // setPlainText() leaves the cursor at the start
        QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
        QVERIFY(input->toPlainText().contains('\n'));
        QCOMPARE(messageCards(*dock).size(), 0); // nothing was sent

        QTest::keyClicks(input, "second line");
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(messageCards(*dock).size(), 2);
        QCOMPARE(cardBody(cardAt(*dock, 0)), QString("first line\nsecond line"));
        QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy(), 5000);
    }

    void testEmptyOrBlankInputIsNotSent()
    {
        auto dock = newDock(newDatabasePath());
        QPlainTextEdit *input = type(*dock, "x");
        QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);

        input->setPlainText("   \n  ");
        QVERIFY(!send->isEnabled());
        input->setFocus();
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(messageCards(*dock).size(), 0);
        QCOMPARE(m_server->requests.size(), 0);
    }

    void testNotConfiguredExplainsAndKeepsTheText()
    {
        configure(false, ""); // no model

        auto dock = newDock(newDatabasePath());
        QPlainTextEdit *input = type(*dock, "Hello?");
        QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
        send->click();

        QCOMPARE(messageCards(*dock).size(), 1);
        QCOMPARE(cardName(cardAt(*dock, 0)), QString("Error"));
        QVERIFY2(cardBody(cardAt(*dock, 0)).contains("Configure Krita"), qPrintable(cardBody(cardAt(*dock, 0))));
        QCOMPARE(input->toPlainText(), QString("Hello?")); // not lost
        QVERIFY(!dock->isBusy());
        QCOMPARE(m_server->requests.size(), 0);
    }

    void testSettingsChangedAfterOpeningAreUsedForTheNextMessage()
    {
        m_server->enqueue(textReply("ok"));
        configure(false, "");
        auto dock = newDock(newDatabasePath());
        QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
        QPlainTextEdit *input = type(*dock, "hi");
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
        send->click(); // not configured: an error
        QCOMPARE(m_server->requests.size(), 0);

        configure(false, "my-model"); // the user fills in the settings
        send->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy() && m_server->requests.size() == 1, 5000);
        QCOMPARE(m_server->requests.first().json().value("model").toString(), QString("my-model"));
        QVERIFY(input->toPlainText().isEmpty());
    }

    void testServerErrorBecomesAnErrorCardInsteadOfAnEmptyAnswer()
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

        auto dock = newDock(newDatabasePath());
        QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
        type(*dock, "hi");
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
        send->click();

        QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy(), 5000);
        const QList<QFrame *> cards = messageCards(*dock);
        QCOMPARE(cards.size(), 2); // the user's message and the error, no empty AI card
        QCOMPARE(cardName(cards[0]), QString("You"));
        QCOMPARE(cardName(cards[1]), QString("Error"));
        QVERIFY(cardBody(cards[1]).contains("401"));
        QVERIFY(cardBody(cards[1]).contains("Incorrect API key provided"));
        QVERIFY(buttonByToolTip(*dock, "Send (Enter)") != nullptr);
    }

    void testStopKeepsWhatHadArrived()
    {
        m_server->enqueue(hangingReply("Part of an answer"));

        auto dock = newDock(newDatabasePath());
        QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
        type(*dock, "hi");
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
        send->click();

        QTRY_COMPARE_WITH_TIMEOUT(cardBody(cardAt(*dock, 1)), QString("Part of an answer"), 5000);
        QVERIFY(dock->isBusy());

        QAbstractButton *stop = buttonByToolTip(*dock, "Stop");
        QVERIFY(stop);
        stop->click();

        QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy(), 5000);
        QCOMPARE(messageCards(*dock).size(), 2);
        QCOMPARE(cardBody(cardAt(*dock, 1)), QString("Part of an answer"));
        QVERIFY(cardShowsProcessingTime(cardAt(*dock, 1)));
        QVERIFY(buttonByToolTip(*dock, "Send (Enter)") != nullptr);
    }

    void testStopBeforeAnyTextRemovesTheEmptyCard()
    {
        ScriptedResponse silent;
        silent.hangAfterChunks = true;
        m_server->enqueue(silent);

        auto dock = newDock(newDatabasePath());
        QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
        type(*dock, "hi");
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
        send->click();
        QTRY_COMPARE_WITH_TIMEOUT(m_server->requests.size(), 1, 5000);

        buttonByToolTip(*dock, "Stop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy(), 5000);
        QCOMPARE(messageCards(*dock).size(), 1); // only the user's message
        QCOMPARE(cardName(cardAt(*dock, 0)), QString("You"));
    }

    void testThinkingEffortIsSentOnlyIfTheSettingSaysSo()
    {
        for (const bool enabled : {false, true}) {
            m_server->requests.clear();
            m_server->enqueue(textReply("ok"));
            configure(enabled);

            auto dock = newDock(newDatabasePath());
            QToolButton *effortButton = nullptr;
            for (QToolButton *button : dock->findChildren<QToolButton *>()) {
                if (button->menu()) {
                    effortButton = button;
                }
            }
            QVERIFY(effortButton);
            for (QAction *action : effortButton->menu()->actions()) {
                if (action->data().isValid() && action->data().toInt() == AiChatDock::High) {
                    action->trigger();
                }
            }
            QCOMPARE(dock->reasoningEffort(), AiChatDock::High);

            QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
            type(*dock, "think hard");
            QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
            send->click();
            QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy() && m_server->requests.size() == 1, 5000);

            const QJsonObject sent = m_server->requests.first().json();
            if (enabled) {
                QCOMPARE(sent.value("reasoning_effort").toString(), QString("high"));
            } else {
                QVERIFY(!sent.contains("reasoning_effort"));
            }
        }
    }

    void testHistoryReturnsWhenThePanelIsOpenedAgain()
    {
        const QString path = newDatabasePath();
        m_server->enqueue(textReply("the answer"));

        {
            auto dock = newDock(path);
            QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
            type(*dock, "the question");
            QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
            send->click();
            QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy(), 5000);
            QCOMPARE(messageCards(*dock).size(), 2);
        } // Krita is closed

        auto again = newDock(path);
        QTRY_COMPARE_WITH_TIMEOUT(messageCards(*again).size(), 2, 5000);
        const QList<QFrame *> cards = messageCards(*again);
        QCOMPARE(cardName(cards[0]), QString("You"));
        QCOMPARE(cardBody(cards[0]), QString("the question"));
        QCOMPARE(cardName(cards[1]), QString("AI"));
        QCOMPARE(cardBody(cards[1]), QString("the answer"));
        QVERIFY(cardShowsProcessingTime(cards[1])); // the time the AI needed was saved too
    }

    void testNewChatEmptiesThePanelAndTheNextStartIsEmpty()
    {
        const QString path = newDatabasePath();
        m_server->enqueue(textReply("answer"));

        {
            auto dock = newDock(path);
            QAbstractButton *newChat = buttonByToolTip(*dock, "New chat");
            QVERIFY(!newChat->isEnabled()); // nothing to clear

            QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
            type(*dock, "question");
            QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
            send->click();
            QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy(), 5000);
            QVERIFY(newChat->isEnabled());

            newChat->click();
            QCOMPARE(messageCards(*dock).size(), 0);
            QVERIFY(!newChat->isEnabled());
        }

        // the next time the panel opens it starts with the new, empty conversation
        auto again = newDock(path);
        QAbstractButton *send = buttonByToolTip(*again, "Send (Enter)");
        type(*again, "x");
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000); // loaded
        QCOMPARE(messageCards(*again).size(), 0);
    }

    void testNewChatWhileAnAnswerIsComingStopsIt()
    {
        m_server->enqueue(hangingReply("Part"));
        auto dock = newDock(newDatabasePath());
        QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
        type(*dock, "hi");
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
        send->click();
        QTRY_COMPARE_WITH_TIMEOUT(cardBody(cardAt(*dock, 1)), QString("Part"), 5000);

        buttonByToolTip(*dock, "New chat")->click();
        QCOMPARE(messageCards(*dock).size(), 0);

        QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy(), 5000);
        QCOMPARE(messageCards(*dock).size(), 0); // the late answer did not come back into the empty panel
    }

    void testClosingThePanelWhileAnAnswerIsComingDoesNotHang()
    {
        m_server->enqueue(hangingReply("Part"));
        QElapsedTimer timer;
        {
            auto dock = newDock(newDatabasePath());
            QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
            type(*dock, "hi");
            QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
            send->click();
            QTRY_COMPARE_WITH_TIMEOUT(cardBody(cardAt(*dock, 1)), QString("Part"), 5000);
            timer.start();
        } // destroyed while the answer is still streaming
        QVERIFY2(timer.elapsed() < 3000, "closing the panel took too long");
    }

    void testApiKeyIsNeverWrittenToTheChatHistory()
    {
        const QString path = newDatabasePath();
        m_server->enqueue(textReply("ok"));
        {
            auto dock = newDock(path);
            QAbstractButton *send = buttonByToolTip(*dock, "Send (Enter)");
            type(*dock, "hi");
            QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
            send->click();
            QTRY_VERIFY_WITH_TIMEOUT(!dock->isBusy(), 5000);
        }

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains("sk-test"));
        QFile wal(path + "-wal");
        if (wal.exists() && wal.open(QIODevice::ReadOnly)) {
            QVERIFY(!wal.readAll().contains("sk-test"));
        }
    }
};

int main(int argc, char **argv)
{
    // never touch the real settings and the real chat history of the person running the tests
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    AiChatDockTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "AiChatDockTest.moc"
