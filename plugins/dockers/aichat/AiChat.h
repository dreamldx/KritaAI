/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_CHAT_H_
#define _AI_CHAT_H_

#include <QObject>
#include <QVariant>

class AiChatPlugin : public QObject
{
    Q_OBJECT
public:
    AiChatPlugin(QObject *parent, const QVariantList &);
    ~AiChatPlugin() override;
};

#endif
