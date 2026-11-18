/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_CHAT_DOCK_H_
#define _AI_CHAT_DOCK_H_

#include <QDockWidget>
#include <QList>
#include <QString>

#include "AiChatTypes.h"

class AiChatDocumentAccess;
class AiChatGuiExecutor;
class AiChatToolRegistry;
class AiChatVisionClient;
class AiChatWorker;
class QAbstractButton;
class QFrame;
class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QThread;
class QToolButton;
class QVBoxLayout;
class QWidget;

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
 * The AI service is reached through an AiChatWorker that runs in a thread of
 * its own: the panel never waits for the network or the disk. The service
 * address, the key and the model are read from the settings (Configure Krita
 * > AI Chat) each time a message is sent. The chat history is kept in a
 * SQLite database; the panel shows the latest conversation again when Krita
 * is restarted.
 */
class AiChatDock : public QDockWidget
{
    Q_OBJECT
public:
    /// How much the model should think before answering.
    enum ReasoningEffort {
        Low,
        Medium,
        High,
    };

    /// @param databasePath where the chat history is kept; empty: the default place in Krita's user data folder
    explicit AiChatDock(const QString &databasePath = QString());
    ~AiChatDock() override;

    ReasoningEffort reasoningEffort() const;
    void setReasoningEffort(ReasoningEffort effort);

    /// True while the AI is answering.
    bool isBusy() const { return m_busy; }

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

    void reasoningEffortChanged(AiChatDock::ReasoningEffort effort);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private Q_SLOTS:
    void slotSend();
    void slotSendOrStop();
    void slotNewChat();
    void slotInputChanged();

    // answers of the worker
    void slotConversationLoaded(const QList<AiChatStoredMessage> &messages);
    void slotAssistantDelta(const QString &delta);
    void slotAssistantFinished(const AiChatStoredMessage &message);
    void slotRequestFailed(const QString &error);
    void slotRequestCanceled();
    void slotBusyChanged(bool busy);
    void slotStorageError(const QString &error);

private:
    void updateComposerStyle();
    void updateInputHeight();
    void updateSendButton();
    void setBusy(bool busy);

    void appendMessageWidget(QWidget *messageWidget);
    void addStoredMessage(const AiChatStoredMessage &message);
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
    ReasoningEffort m_effort {Medium};

    // the worker and its thread
    QThread *m_thread {nullptr};
    AiChatWorker *m_worker {nullptr};

    // the tools of the AI: they run in the worker thread and reach Krita through the executor
    AiChatGuiExecutor *m_executor {nullptr};
    AiChatDocumentAccess *m_documentAccess {nullptr};
    AiChatVisionClient *m_vision {nullptr};
    AiChatToolRegistry *m_tools {nullptr};
    bool m_ready {false}; ///< the history was loaded, messages can be sent
    bool m_busy {false};
    bool m_storageWarned {false};

    // the answer that is being received
    QWidget *m_streamingMessage {nullptr};
    QString m_streamingText;

    bool m_followBottom {true};
    int m_messageCount {0};
};

#endif
