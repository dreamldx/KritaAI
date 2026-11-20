/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _MODEL_REQUEST_H_
#define _MODEL_REQUEST_H_

#include <QMetaType>

#include "../config/Config.h"
#include "../network/Client.h"

namespace AiChat {

/// How much the model should think before answering.
enum class ReasoningEffort {
    Low,
    Medium,
    High,
};

/// What goes to the worker for one message: the settings of Configure Krita (read by the caller, so a
/// change counts from the next message on) and the thinking effort the user chose.
/// `reasoning_effort` is only sent when the settings say so.
RequestSettings makeRequestSettings(const Config &config, ReasoningEffort effort);

} // namespace AiChat

Q_DECLARE_METATYPE(AiChat::ReasoningEffort)

#endif
