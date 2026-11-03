#pragma once
/*
 * binding_undo.h - make reactive data-binding edits undoable.
 *
 * The core binding layer (vgui_bind / vgui_bind_cb) reports every writeback
 * caused by a genuine user edit through a context-level recorder.  This adapter
 * installs such a recorder and turns each change into a command on an editor
 * command_stack, so "drag the slider -> Ctrl+Z" restores the model field.
 *
 *   vgui_editor_kit::command_stack stack;
 *   vgui_editor_kit::binding_undo undo(ui, stack);   // RAII: detaches on exit
 *   vgui_bind(ui, sliders[0], VGUI_BIND_VALUE, &settings, VGUI_OFFSET(...));
 *
 * Behaviour:
 *   - offset bindings replay through vgui_model_write_raw(), which also marks
 *     the model dirty so bound widgets resync on the next update;
 *   - callback bindings replay through their set callback and then re-invalidate
 *     the binding (the handle is validated with vgui_binding_alive() first);
 *   - consecutive edits of the same target inside the merge window collapse
 *     into one undo record (slider drags, typed digits);
 *   - programmatic writes (vgui_widget_set_*, vgui_model_write_raw) and
 *     model -> widget flushes are never recorded.
 *
 * Render-thread only, like the rest of the binding layer.
 */
#include <cstddef>
#include <string>

#include <vgui/vgui.h>

#include "command_stack.h"

namespace vgui_editor_kit
{
	class binding_undo
	{
	public:
		/* Installs the recorder; `stack` must outlive this object. */
		binding_undo(vgui_context* ctx, command_stack& stack);
		~binding_undo();

		binding_undo(const binding_undo&) = delete;
		binding_undo& operator=(const binding_undo&) = delete;

		/* Undo label prefix.  The final name is "<label> <widget id>" (or
		 * "<label> binding" when the widget has no id).  Default: "Edit". */
		void set_label(std::string label);
		const std::string& label() const noexcept;

		/* Window used to merge consecutive edits of the same field, in
		 * milliseconds.  0 disables merging.  Default: 500. */
		void set_merge_window(int milliseconds);
		int merge_window() const noexcept;

		/* Temporarily stop recording (the recorder stays installed). */
		void set_enabled(bool enabled);
		bool enabled() const noexcept;

		/* Number of commands handed to the stack (diagnostics/tests). */
		std::size_t recorded() const noexcept;

		bool attached() const noexcept;

		/* True while a binding_undo adapter owns the context recorder
		 * (diagnostics/tests). */
		static bool owns_recorder(vgui_context* ctx);

	private:
		static int record_trampoline(vgui_context* ctx,
			const vgui_bind_change* change, void* user);
		void on_change(const vgui_bind_change& change);

		vgui_context* ctx_ = nullptr;
		command_stack* stack_ = nullptr;
		/* Recorder that was installed before us (restored on destruction). */
		vgui_bind_record_fn previous_ = nullptr;
		void* previous_user_ = nullptr;
		std::string label_ = "Edit";
		int merge_window_ms_ = 500;
		std::size_t recorded_ = 0;
		bool enabled_ = true;
		bool attached_ = false;
	};
} /* namespace vgui_editor_kit */
