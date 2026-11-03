/*
 * binding_undo.cpp - undo commands for reactive data-binding writebacks.
 */
#include <vgui/editor/binding_undo.h>

#include <chrono>
#include <memory>
#include <utility>

namespace vgui_editor_kit
{
	namespace
	{
		using clock_type = std::chrono::steady_clock;

		/* Replays one model-side change (the new value on execute, the old value
		 * on undo).  Offset bindings go through vgui_model_write_raw(), which
		 * marks the model dirty so bound widgets resync on the next update. */
		class bound_write_command final : public command
		{
		public:
			bound_write_command(std::string name, vgui_context* ctx,
				const vgui_bind_change& change, int merge_window_ms)
				: name_(std::move(name)),
				  ctx_(ctx),
				  binding_(change.binding),
				  kind_(change.kind),
				  model_(change.model),
				  offset_(change.offset),
				  cb_user_(change.cb_user),
				  cb_set_(change.cb_set),
				  value_type_(change.value_type),
				  old_number_(change.old_number),
				  new_number_(change.new_number),
				  old_text_(change.old_text ? change.old_text : ""),
				  new_text_(change.new_text ? change.new_text : ""),
				  stamp_(clock_type::now()),
				  merge_window_ms_(merge_window_ms)
			{
			}

			const char* name() const noexcept override { return name_.c_str(); }

			bool execute() override
			{
				write(new_number_, new_text_);
				return true;
			}

			void undo() override
			{
				write(old_number_, old_text_);
			}

			/* Same field, still inside the merge window -> absorb the newer
			 * change (slider drags collapse into one undo record). */
			bool merge_with(command& next) override
			{
				auto* other = dynamic_cast<bound_write_command*>(&next);
				if (!other) return false;
				if (other->model_ != model_ || other->offset_ != offset_ ||
					other->value_type_ != value_type_ || other->binding_ != binding_ ||
					other->ctx_ != ctx_)
					return false;
				if (merge_window_ms_ <= 0) return false;
				const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
					other->stamp_ - stamp_).count();
				if (dt < 0 || dt > merge_window_ms_) return false;
				new_number_ = other->new_number_;
				new_text_ = other->new_text_;
				stamp_ = other->stamp_;
				return true;
			}

		private:
			void write(double number, const std::string& text)
			{
				if (!ctx_) return;
				if (model_)
				{
					vgui_model_write_raw(ctx_, model_, offset_, value_type_, number,
						text.c_str());
					return;
				}
				/* Callback binding: replay through the host setter.  The binding
				 * (and its widget) may have been destroyed since the edit. */
				if (!cb_set_ || !vgui_binding_alive(ctx_, binding_)) return;
				if (value_type_ == VGUI_BINDV_TEXT)
				{
					const char* p = text.c_str();
					cb_set_(cb_user_, kind_, &p);
				}
				else if (value_type_ == VGUI_BINDV_INT)
				{
					int v = (int)number;
					cb_set_(cb_user_, kind_, &v);
				}
				else
				{
					float v = (float)number;
					cb_set_(cb_user_, kind_, &v);
				}
				vgui_binding_invalidate(binding_);
			}

			std::string name_;
			vgui_context* ctx_ = nullptr;
			vgui_binding* binding_ = nullptr;
			vgui_bind_kind kind_ = VGUI_BIND_VALUE;
			void* model_ = nullptr;
			std::size_t offset_ = 0;
			void* cb_user_ = nullptr;
			vgui_bind_set_fn cb_set_ = nullptr;
			int value_type_ = VGUI_BINDV_FLOAT;
			double old_number_ = 0.0;
			double new_number_ = 0.0;
			std::string old_text_;
			std::string new_text_;
			clock_type::time_point stamp_;
			int merge_window_ms_ = 500;
		};
	} /* namespace */

	binding_undo::binding_undo(vgui_context* ctx, command_stack& stack)
		: ctx_(ctx), stack_(&stack)
	{
		if (!ctx_) return;
		previous_ = vgui_get_edit_recorder(ctx_, &previous_user_);
		vgui_set_edit_recorder(ctx_, &binding_undo::record_trampoline, this);
		attached_ = true;
	}

	binding_undo::~binding_undo()
	{
		if (!attached_ || !ctx_) return;
		void* user = nullptr;
		if (vgui_get_edit_recorder(ctx_, &user) == &binding_undo::record_trampoline &&
			user == this)
			vgui_set_edit_recorder(ctx_, previous_, previous_user_);
		attached_ = false;
	}

	void binding_undo::set_label(std::string label)
	{
		label_ = std::move(label);
	}

	const std::string& binding_undo::label() const noexcept
	{
		return label_;
	}

	void binding_undo::set_merge_window(int milliseconds)
	{
		merge_window_ms_ = milliseconds < 0 ? 0 : milliseconds;
	}

	int binding_undo::merge_window() const noexcept
	{
		return merge_window_ms_;
	}

	void binding_undo::set_enabled(bool enabled)
	{
		enabled_ = enabled;
	}

	bool binding_undo::enabled() const noexcept
	{
		return enabled_;
	}

	std::size_t binding_undo::recorded() const noexcept
	{
		return recorded_;
	}

	bool binding_undo::attached() const noexcept
	{
		return attached_;
	}

	bool binding_undo::owns_recorder(vgui_context* ctx)
	{
		if (!ctx) return false;
		return vgui_get_edit_recorder(ctx, nullptr) == &binding_undo::record_trampoline;
	}

	int binding_undo::record_trampoline(vgui_context* ctx,
		const vgui_bind_change* change, void* user)
	{
		(void)ctx;
		if (!change || !user) return 0;
		static_cast<binding_undo*>(user)->on_change(*change);
		return 0;
	}

	void binding_undo::on_change(const vgui_bind_change& change)
	{
		if (!attached_ || !enabled_ || !ctx_ || !stack_) return;
		if (change.value_type == VGUI_BINDV_TEXT)
		{
			const std::string old_t = change.old_text ? change.old_text : "";
			const std::string new_t = change.new_text ? change.new_text : "";
			if (old_t == new_t) return;
		}
		else if (change.old_number == change.new_number)
		{
			return;
		}

		std::string name = label_;
		name += ' ';
		const char* id = change.widget ? vgui_widget_get_id(change.widget) : nullptr;
		name += (id && *id) ? id : "binding";

		stack_->push(std::make_unique<bound_write_command>(std::move(name), ctx_,
			change, merge_window_ms_));
		++recorded_;
	}
} /* namespace vgui_editor_kit */
