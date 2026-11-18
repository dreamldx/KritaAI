/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_CHAT_LOG_H_
#define _AI_CHAT_LOG_H_

#include <QString>

/**
 * Appends one line, with the time, to the log file of the AI chat
 * (AppDataLocation/aichat/aichat.log; on Windows %APPDATA%\krita\aichat\aichat.log)
 * and prints it with qInfo(). Safe to call from any thread.
 *
 * Krita started from the Start menu has no console, so a file is the only place where
 * "why was that slow" can be read afterwards. Never log API keys or the picture itself.
 * The file is cut to its last part when it grows over 1 MB.
 */
void aiChatLog(const QString &line);

#endif
