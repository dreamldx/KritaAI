/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_TOOLS_FAL_TOOLS_H_
#define _AI_TOOLS_FAL_TOOLS_H_

namespace AiChat {

class DocumentAccess;
class FalClient;
class GuiExecutor;
class Progress;
class ToolRegistry;

/// The most pixels the result of a Topaz tool may have: bigger jobs are refused before anything is sent.
constexpr double kFalMaxResultPixels = 100.0 * 1000 * 1000;

/**
 * Adds the Topaz tools (fal.ai) to the registry. Only offered while a fal.ai key is set up.
 *
 *  - upscale_image(upscale_factor?, model?, sharpen?, denoise?, fix_compression?, face_enhancement?, region?, selection_only?)
 *  - denoise_image(model?, region?, selection_only?)
 *
 * They send the picture (or the region) to fal.ai and put the result into a new layer of the
 * open document (one undo step). While the job runs, a modal progress window
 * with a Cancel button is shown (`progress`).
 *
 * None of the pointers is owned; all must outlive the registry.
 */
void addFalTools(ToolRegistry *registry, GuiExecutor *executor, DocumentAccess *access,
                       FalClient *fal, Progress *progress);

} // namespace AiChat

#endif
