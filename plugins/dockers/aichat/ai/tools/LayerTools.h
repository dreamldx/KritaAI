/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_TOOLS_LAYER_TOOLS_H_
#define _AI_TOOLS_LAYER_TOOLS_H_

namespace AiChat {

class DocumentAccess;
class GuiExecutor;
class ToolRegistry;

/**
 * Adds the tools that change the layers of the open document:
 *
 *  add_layer, add_group, rename_layer, set_layer_visibility, set_layer_opacity, set_layer_blend_mode,
 *  move_layer, duplicate_layer, delete_layer, reorder_layer, move_layer_to_group, scale_image.
 *
 * Each call is one entry of the undo history of the document: the user can take back everything
 * the AI did with Edit > Undo.
 *
 * None of the pointers is owned; both must outlive the registry.
 */
void addLayerTools(ToolRegistry *registry, GuiExecutor *executor, DocumentAccess *access);

} // namespace AiChat

#endif
