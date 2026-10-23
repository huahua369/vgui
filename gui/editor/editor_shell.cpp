#include "editor_shell.h"

#include <utility>

namespace vgui_editor_kit {

	namespace {
		constexpr vgui_color kRootBg = 0xFF14171C;
		constexpr vgui_color kHeaderBg = 0xFF1E232B;
		constexpr vgui_color kSectionBg = 0xFF1B1E24;
		constexpr vgui_color kBorder = 0xFF2E3440;
		constexpr vgui_color kTitleText = 0xFFB8C4D6;
		constexpr vgui_color kMutedText = 0xFF7F8B9C;
		constexpr vgui_color kStatusText = 0xFF9CC7A4;

		vgui_widget* make_section(vgui_widget* parent, const char* title,
			float width, float height, float grow)
		{
			vgui_widget* section = vgui_widget_add(parent, VGUI_PANEL, nullptr);
			vgui_widget_set_size(section, width, height);
			vgui_widget_set_grow(section, grow);
			vgui_widget_set_align_self(section, VGUI_ALIGN_STRETCH);
			vgui_widget_set_layout(section, VGUI_LAYOUT_COLUMN);
			vgui_widget_set_padding(section, 0, 0, 0, 0);
			vgui_widget_set_gap(section, 0);
			vgui_widget_set_background(section, kSectionBg);
			vgui_widget_set_border(section, kBorder, 1);
			if (width > 0.0f)
				vgui_widget_set_min_size(section, width, 0);

			vgui_widget* heading = vgui_widget_add(section, VGUI_LABEL, title);
			vgui_widget_set_size(heading, 0, 24);
			vgui_widget_set_align_self(heading, VGUI_ALIGN_STRETCH);
			vgui_widget_set_font_size(heading, 11);
			vgui_widget_set_text_color(heading, kMutedText);
			vgui_widget_set_text_align(heading, 0.0f, 0.5f);
			vgui_widget_set_padding(heading, 8, 0, 8, 0);

			vgui_widget* body = vgui_widget_add(section, VGUI_PANEL, nullptr);
			vgui_widget_set_grow(body, 1.0f);
			vgui_widget_set_layout(body, VGUI_LAYOUT_COLUMN);
			vgui_widget_set_padding(body, 6, 6, 6, 6);
			vgui_widget_set_gap(body, 4);
			return body;
		}
	}

	editor_shell::editor_shell(editor_context& context)
		: context_(context)
	{
	}

	void editor_shell::build()
	{
		if (root_)
			return;

		vgui_widget* host = context_.root();
		if (!host)
			return;

		vgui_widget_set_layout(host, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(host, 0, 0, 0, 0);
		vgui_widget_set_gap(host, 0);
		vgui_widget_set_align_items(host, VGUI_ALIGN_STRETCH);

		root_ = vgui_widget_add(host, VGUI_PANEL, nullptr);
		vgui_widget_set_grow(root_, 1.0f);
		vgui_widget_set_layout(root_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(root_, 4, 4, 4, 4);
		vgui_widget_set_gap(root_, 4);
		vgui_widget_set_background(root_, kRootBg);

		vgui_widget* header = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_size(header, 0, 34);
		vgui_widget_set_align_self(header, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(header, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(header, 8, 0, 8, 0);
		vgui_widget_set_gap(header, 8);
		vgui_widget_set_background(header, kHeaderBg);
		vgui_widget_set_border(header, kBorder, 1);

		vgui_widget* title = vgui_widget_add(header, VGUI_LABEL,
			"vgui_editor_kit  ·  MVP skeleton");
		vgui_widget_set_grow(title, 1.0f);
		vgui_widget_set_size(title, 0, 24);
		vgui_widget_set_font_size(title, 14);
		vgui_widget_set_text_color(title, kTitleText);
		vgui_widget_set_text_align(title, 0.0f, 0.5f);

		vgui_widget* undo = vgui_widget_add(header, VGUI_BUTTON, "Undo");
		vgui_widget_set_size(undo, 58, 24);
		vgui_widget_set_font_size(undo, 12);
		vgui_widget_on_event(undo, &editor_shell::on_undo, this);

		vgui_widget* redo = vgui_widget_add(header, VGUI_BUTTON, "Redo");
		vgui_widget_set_size(redo, 58, 24);
		vgui_widget_set_font_size(redo, 12);
		vgui_widget_on_event(redo, &editor_shell::on_redo, this);

		status_ = vgui_widget_add(header, VGUI_LABEL, "ready");
		vgui_widget_set_size(status_, 220, 24);
		vgui_widget_set_font_size(status_, 12);
		vgui_widget_set_text_color(status_, kStatusText);
		vgui_widget_set_text_align(status_, 1.0f, 0.5f);

		vgui_widget* body = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_grow(body, 1.0f);
		vgui_widget_set_layout(body, VGUI_LAYOUT_ROW);
		vgui_widget_set_gap(body, 4);
		vgui_widget_set_align_items(body, VGUI_ALIGN_STRETCH);

		vgui_widget* left_body = make_section(body, "OUTLINER", 230, 0, 0);
		vgui_widget* center_body = make_section(body, "VIEWPORT", 0, 0, 1);
		vgui_widget* right_body = make_section(body, "PROPERTIES", 300, 0, 0);
		vgui_widget* bottom_body = make_section(root_, "ASSETS", 0, 180, 0);

		scene_.build(context_, left_body);
		viewport_.build(center_body);
		properties_.build(right_body);
		assets_.build(bottom_body);
	}

	bool editor_shell::built() const noexcept
	{
		return root_ != nullptr;
	}

	vgui_widget* editor_shell::widget() const noexcept
	{
		return root_;
	}

	outliner_view& editor_shell::scene() noexcept
	{
		return scene_;
	}

	property_grid& editor_shell::properties() noexcept
	{
		return properties_;
	}

	asset_browser& editor_shell::assets() noexcept
	{
		return assets_;
	}

	viewport_view& editor_shell::viewport() noexcept
	{
		return viewport_;
	}

	void editor_shell::set_status(std::string text)
	{
		if (status_)
			vgui_widget_set_text(status_, text.c_str());
	}

	void editor_shell::on_undo(vgui_widget* widget, const vgui_event* event, void* user)
	{
		(void)widget;
		if (event && event->type == VGUI_EVENT_CLICK)
		{
			auto* self = static_cast<editor_shell*>(user);
			self->set_status(self->context_.commands().undo() ? "undo" : "nothing to undo");
		}
	}

	void editor_shell::on_redo(vgui_widget* widget, const vgui_event* event, void* user)
	{
		(void)widget;
		if (event && event->type == VGUI_EVENT_CLICK)
		{
			auto* self = static_cast<editor_shell*>(user);
			self->set_status(self->context_.commands().redo() ? "redo" : "nothing to redo");
		}
	}

} /* namespace vgui_editor_kit */
