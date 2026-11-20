/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Progress.h"

#include <QApplication>
#include <QMetaObject>
#include <QProgressDialog>
#include <QTimer>

#include <klocalizedstring.h>

namespace AiChat {

Progress::Progress(QObject *parent)
    : QObject(parent)
{
    m_tick = new QTimer(this);
    m_tick->setInterval(500);
    connect(m_tick, &QTimer::timeout, this, &Progress::slotTick);
}

Progress::~Progress()
{
    delete m_dialog.data();
}

bool Progress::isActive() const
{
    return m_dialog && m_dialog->isVisible();
}

void Progress::beginLater(const QString &title)
{
    QMetaObject::invokeMethod(this, "begin", Qt::QueuedConnection, Q_ARG(QString, title));
}

void Progress::updateLater(const QString &text)
{
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection, Q_ARG(QString, text));
}

void Progress::endLater()
{
    QMetaObject::invokeMethod(this, "end", Qt::QueuedConnection);
}

void Progress::begin(const QString &title)
{
    end(); // never two windows

    // range 0..0: the bar moves back and forth, as nobody knows how long the server needs
    m_dialog = new QProgressDialog(title, i18n("Cancel"), 0, 0, QApplication::activeWindow());
    m_dialog->setObjectName(QStringLiteral("aiChatProgress"));
    m_dialog->setWindowTitle(i18n("AI Chat"));
    m_dialog->setWindowModality(Qt::ApplicationModal);
    m_dialog->setMinimumDuration(0);
    m_dialog->setAutoClose(false);
    m_dialog->setAutoReset(false);
    m_dialog->setMinimumWidth(360);
    // Closing it with the window button counts as Cancel; the dialog is deleted by end() only
    connect(m_dialog, &QProgressDialog::canceled, this, &Progress::cancelRequested);

    m_text = title;
    m_elapsed.start();
    refreshLabel();
    m_dialog->show();
    m_tick->start();
}

void Progress::update(const QString &text)
{
    if (!m_dialog) {
        return;
    }
    m_text = text;
    refreshLabel();
}

void Progress::end()
{
    m_tick->stop();
    if (m_dialog) {
        m_dialog->disconnect(this); // closing must not look like a Cancel
        m_dialog->hide();
        m_dialog->deleteLater();
        m_dialog = nullptr;
    }
}

void Progress::slotTick()
{
    refreshLabel();
}

void Progress::refreshLabel()
{
    if (m_dialog) {
        m_dialog->setLabelText(i18n("%1\n%2 s", m_text, m_elapsed.elapsed() / 1000));
    }
}

} // namespace AiChat
