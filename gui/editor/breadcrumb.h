#pragma once
/*
 * breadcrumb.h - clickable path navigation strip.
 *
 * Each item carries a display label and an optional stable string id.  The
 * last item is rendered as the active (current) location.  User navigation
 * notifies the listener; set_items()/set_active() are silent so hosts can
 * refresh the path without re-entering their own navigation handler.
 */
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "toolbar.h"
#include "vgui.h"

namespace vgui_editor_kit {

	struct breadcrumb_item
	{
		std::string label;
		std::string id;
	};

	class breadcrumb
	{
	public:
		using navigate_listener = std::function<void(std::size_t,
			const std::string&)>;

		breadcrumb() = default;
		breadcrumb(const breadcrumb&) = delete;
		breadcrumb& operator=(const breadcrumb&) = delete;

		void build(vgui_widget* parent);

		void add_item(std::string label, std::string id = {});
		void set_items(std::vector<breadcrumb_item> items);
		void clear();

		std::size_t count() const noexcept;
		const breadcrumb_item& item(std::size_t index) const;

		/* User-style navigation: notifies the listener for a valid index. */
		bool navigate(std::size_t index);
		/* Programmatic current location; never notifies. */
		void set_active(std::size_t index);
		std::size_t active() const noexcept;

		void set_on_navigate(navigate_listener listener);

		vgui_widget* item_widget(std::size_t index) const noexcept;
		vgui_widget* widget() const noexcept;

	private:
		void add_separator();
		void update_active();

		vgui_widget* panel_ = nullptr;
		std::vector<std::unique_ptr<icon_button>> items_;
		std::vector<vgui_widget*> separators_;
		std::vector<breadcrumb_item> path_;
		std::size_t active_ = 0;
		bool has_active_ = false;
		navigate_listener listener_;
	};

} /* namespace vgui_editor_kit */
