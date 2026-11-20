/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _UI_PROGRESS_H_
#define _UI_PROGRESS_H_

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>

class QProgressDialog;
class QTimer;

namespace AiChat {


/**
 * The window that blocks Krita while a slow tool works (the Topaz tools take many
 * seconds). It is modal, shows what the tool is doing and how long it has taken,
 * and has a Cancel button.
 *
 * Make it in the GUI thread. The three slots may be called from any thread
 * through a queued call (see the helper functions below), which is how the worker
 * thread uses them. Cancel emits cancelRequested(); the one who listens stops the work.
 */
class Progress : public QObject
{
    Q_OBJECT
public:
    explicit Progress(QObject *parent = nullptr);
    ~Progress() override;

    /// Is the window on the screen? (GUI thread)
    bool isActive() const;

    // Safe to call from another thread: they only post an event.
    void beginLater(const QString &title);
    void updateLater(const QString &text);
    void endLater();

public Q_SLOTS:
    void begin(const QString &title);
    void update(const QString &text);
    void end();

Q_SIGNALS:
    void cancelRequested();

private Q_SLOTS:
    void slotTick();

private:
    void refreshLabel();

    QPointer<QProgressDialog> m_dialog;
    QTimer *m_tick {nullptr};
    QElapsedTimer m_elapsed;
    QString m_text;
};

} // namespace AiChat

#endif
