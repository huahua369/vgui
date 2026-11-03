#pragma once
/*
 * collection.h - adapt a std::vector<Row> to vgui_row_source.
 *
 * Lists and trees can render model data either by materialising items
 * (vgui_list_add_item) or through a virtual row source (vgui_row_source).  The
 * virtual path keeps a million-row model cheap, but hand-writing the callback
 * table for every model is tedious; this adapter does it once:
 *
 *   std::vector<Entity> entities;
 *   vgui::vector_rows<Entity> rows(&entities,
 *       [](const Entity& e) { return e.name; },
 *       [](const Entity& e) { return e.kind; });
 *   rows.attach(list);                  // list or tree
 *   entities.push_back({...});
 *   rows.refresh();                     // structural change -> rebuild rows
 *
 * Notes:
 *   - the vector must outlive the adapter; the adapter must outlive the widget;
 *   - rows are addressed by index, so item ids default to index + 1.  Keep the
 *     vector order stable between refreshes, or supply an explicit id accessor;
 *   - returned text lives in internal scratch buffers that stay valid until the
 *     next row_info() call, matching the vgui_row_source contract;
 *   - scalar fields still bind through vgui_bind()/vgui_bind_cb(); a collection
 *     adapter only feeds the view, which is the pull half of the same model.
 */
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <vgui/vgui.h>

namespace vgui
{
	template <class Row>
	class vector_rows
	{
	public:
		using text_fn = std::function<std::string(const Row&)>;
		using id_fn = std::function<vgui_item_id(const Row&)>;
		using enabled_fn = std::function<bool(const Row&)>;

		vector_rows(std::vector<Row>* rows, text_fn label, text_fn detail = {},
			id_fn id = {}, enabled_fn enabled = {})
			: rows_(rows),
			  label_(std::move(label)),
			  detail_(std::move(detail)),
			  id_(std::move(id)),
			  enabled_(std::move(enabled))
		{
			source_.row_count = &vector_rows::row_count_cb;
			source_.row_info = &vector_rows::row_info_cb;
			source_.row_id = &vector_rows::row_id_cb;
			source_.row_id_index = &vector_rows::row_id_index_cb;
			if (enabled_) source_.row_enabled = &vector_rows::row_enabled_cb;
			source_.user = this;
		}

		vector_rows(const vector_rows&) = delete;
		vector_rows& operator=(const vector_rows&) = delete;

		const vgui_row_source* source() const noexcept { return &source_; }

		/* Installs the source on a list or a tree (other kinds are ignored). */
		void attach(vgui_widget* view)
		{
			if (!view) return;
			view_ = view;
			switch (vgui_widget_get_kind(view))
			{
			case VGUI_LIST: vgui_list_set_row_source(view, &source_); break;
			case VGUI_TREE: vgui_tree_set_row_source(view, &source_); break;
			default: break;
			}
		}

		/* Releases the view's row source.  The control only keeps a pointer to
		 * source_, so call this before destroying the view if the adapter may
		 * outlive it.  This clears whatever source the view has (the adapter
		 * cannot query ownership); it is a no-op when no view is attached. */
		void detach()
		{
			if (!view_) return;
			switch (vgui_widget_get_kind(view_))
			{
			case VGUI_LIST: vgui_list_set_row_source(view_, nullptr); break;
			case VGUI_TREE: vgui_tree_set_row_source(view_, nullptr); break;
			default: break;
			}
			view_ = nullptr;
		}

		/* Structural change (rows added/removed/reordered).  Merging the public
		 * list and tree entry points keeps host code shape-agnostic. */
		void refresh()
		{
			if (!view_) return;
			switch (vgui_widget_get_kind(view_))
			{
			case VGUI_LIST: vgui_list_source_changed(view_); break;
			case VGUI_TREE: vgui_tree_source_changed(view_); break;
			default: break;
			}
		}

		/* Content-only change for the visible range (cheaper than refresh()). */
		void invalidate_rows(int first, int count)
		{
			if (!view_) return;
			switch (vgui_widget_get_kind(view_))
			{
			case VGUI_LIST: vgui_list_source_rows_changed(view_, first, count); break;
			case VGUI_TREE: vgui_tree_source_rows_changed(view_, first, count); break;
			default: break;
			}
		}

		void clear()
		{
			label_buf_.clear();
			detail_buf_.clear();
		}

	private:
		bool valid(int index) const
		{
			return rows_ && index >= 0 && (std::size_t)index < rows_->size();
		}

		static int row_count_cb(void* user)
		{
			auto* self = static_cast<vector_rows*>(user);
			return self->rows_ ? (int)self->rows_->size() : 0;
		}

		static int row_info_cb(void* user, int index, const char** text,
			const char** detail, void** item_user)
		{
			auto* self = static_cast<vector_rows*>(user);
			if (!self->valid(index)) return 0;
			const Row& row = (*self->rows_)[(std::size_t)index];
			self->label_buf_ = self->label_ ? self->label_(row) : std::string();
			self->detail_buf_ = self->detail_ ? self->detail_(row) : std::string();
			if (text) *text = self->label_buf_.c_str();
			if (detail) *detail = self->detail_buf_.c_str();
			if (item_user) *item_user = nullptr;
			return 1;
		}

		static vgui_item_id row_id_cb(void* user, int index)
		{
			auto* self = static_cast<vector_rows*>(user);
			if (!self->valid(index)) return 0;
			if (self->id_) return self->id_((*self->rows_)[(std::size_t)index]);
			return (vgui_item_id)(index + 1);
		}

		static int row_id_index_cb(void* user, vgui_item_id item)
		{
			auto* self = static_cast<vector_rows*>(user);
			if (!self->rows_ || !item) return -1;
			if (!self->id_) return (int)item - 1;
			for (std::size_t i = 0; i < self->rows_->size(); ++i)
				if (self->id_((*self->rows_)[i]) == item) return (int)i;
			return -1;
		}

		static int row_enabled_cb(void* user, int index)
		{
			auto* self = static_cast<vector_rows*>(user);
			if (!self->valid(index) || !self->enabled_) return 0;
			return self->enabled_((*self->rows_)[(std::size_t)index]) ? 1 : 0;
		}

		std::vector<Row>* rows_ = nullptr;
		text_fn label_;
		text_fn detail_;
		id_fn id_;
		enabled_fn enabled_;
		vgui_widget* view_ = nullptr;
		vgui_row_source source_ = {};
		/* Must outlive the callback that hands them to the control. */
		std::string label_buf_;
		std::string detail_buf_;
	};
} /* namespace vgui */
