/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _UI_DOCK_H_
#define _UI_DOCK_H_

#include <QDockWidget>
#include <QList>
#include <QString>

#include "../model/Request.h"
#include "../utils/Store.h"

class QAbstractButton;
class QFrame;
class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace AiChat {

class Chat;
class DocumentAccess;
class Progress;

/**
 * A docker with a scrolling chat history on top and, at the bottom, a box
 * with a text input and a toolbar (new chat, add image, thinking effort and
 * the round send button).
 *
 * Every message is shown as a card:
 *  - a header with an icon (AI / user), the name and, for AI messages,
 *    how long the AI needed to answer,
 *  - the message body,
 *  - the time the message was sent.
 *
 * Enter sends the message, Shift+Enter inserts a new line. While the AI
 * answers, the send button is a "stop" button and the answer appears piece
 * by piece.
 *
 * The panel only shows the conversation. Its state, the worker thread
 * (so the panel never waits for the network or the disk) and the tools of
 * the AI belong to a `Chat` (model/Chat.h). The service address, the key
 * and the model are read from the settings (Configure Krita > AI Chat)
 * each time a message is sent. The chat history is kept in a SQLite
 * database; the panel shows the latest conversation again when Krita is
 * restarted.
 */
class Dock : public QDockWidget
{
    Q_OBJECT
public:
    /// @param databasePath where the chat history is kept; empty: the default place in Krita's user data folder
    explicit Dock(const QString &databasePath = QString());
    ~Dock() override;

    ReasoningEffort reasoningEffort() const;
    void setReasoningEffort(ReasoningEffort effort);

    /// True while the AI is answering.
    bool isBusy() const;

    /// Append a message written by the user to the history (only shows it, does not send it).
    void addUserMessage(const QString &text);

    /// Append a message written by the assistant to the history (only shows it).
    /// @param processingMs how long the assistant needed, in milliseconds; negative: unknown
    void addAssistantMessage(const QString &text, qint64 processingMs = -1);

    /// Append a message that tells the user about a problem. It is not part of the conversation.
    void addErrorMessage(const QString &text);

    /// Remove every message from the history (only from the panel, not from the database).
    void clearHistory();

Q_SIGNALS:
    /// Emitted after the user sent a (non-empty) message.
    void messageSubmitted(const QString &text);

    /// Emitted after the history was cleared by the "New chat" button.
    void newChatRequested();

    /// Emitted when the user clicks the "Add image" button.
    void addImageRequested();

    void reasoningEffortChanged(AiChat::ReasoningEffort effort);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private Q_SLOTS:
    void slotSend();
    void slotSendOrStop();
    void slotNewChat();
    void slotInputChanged();

    // what the model reports
    void slotHistoryLoaded(const QList<StoredMessage> &messages);
    void slotAssistantDelta(const QString &delta, const QString &textSoFar);
    void slotAssistantFinished(const StoredMessage &message);
    void slotRequestFailed(const QString &error);
    void slotRequestCanceled();
    void slotBusyChanged(bool busy);

private:
    void updateComposerStyle();
    void updateInputHeight();
    void updateSendButton();
    void updateEffortButton(ReasoningEffort effort);

    void appendMessageWidget(QWidget *messageWidget);
    void addStoredMessage(const StoredMessage &message);
    void startStreamingMessage();
    void discardStreamingMessage();

    /**
     * Derive every font of the panel from the docker's own font, which
     * Krita sets from the user's "interface font" preference. The message
     * bodies and the input box are a bit larger than the rest.
     */
    void updateFonts();

    QScrollArea *m_scrollArea {nullptr};
    QWidget *m_messagesContainer {nullptr};
    QVBoxLayout *m_messagesLayout {nullptr};
    QLabel *m_emptyHint {nullptr};

    // composer: text input on top, toolbar below
    QFrame *m_composer {nullptr};
    QPlainTextEdit *m_input {nullptr};
    QAbstractButton *m_newChatButton {nullptr};
    QAbstractButton *m_addImageButton {nullptr};
    QToolButton *m_effortButton {nullptr};
    QAbstractButton *m_sendButton {nullptr};

    // the model, and what the tools of the AI need from the GUI thread
    Chat *m_chat {nullptr};
    DocumentAccess *m_documentAccess {nullptr}; // owned; the tools reach Krita's document through it
    Progress *m_progress {nullptr}; // the modal window of the slow tools; a child of the panel

    // the card of the answer that is being received
    QWidget *m_streamingMessage {nullptr};

    bool m_followBottom {true};
    int m_messageCount {0};
};

} // namespace AiChat

#endif
