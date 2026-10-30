#pragma once
/*
 * Read-only Markdown preview component.
 *
 * The view renders Markdown, supports mouse selection plus Ctrl+A / Ctrl+C,
 * and never edits the document, follows links, or loads external resources.
 * Images and videos are reserved as host-supplied textures: the view asks a
 * resolver for an md_media_info and only ever draws the returned texture.
 *
 * Layout and metrics follow the code_editor conventions: public geometry is
 * in effective pixels, while set_font_size()/set_max_media_height() take
 * logical units and scale with the context ui_scale.
 */
#include <vgui/vgui.h>

#include <vgui/editor/code_editor.h>

#include <functional>
#include <memory>
#include <string>

namespace vgui_editor_kit {

	enum class md_media_kind
	{
		image,
		video
	};

	enum class md_media_status
	{
		pending,
		ready,
		failed
	};

	/* Media state owned by the host.  texture is borrowed and must stay alive
	 * while it is returned; the view never destroys textures.  aspect is
	 * width / height and falls back to the texture dimensions, then 16:9. */
	struct md_media_info
	{
		md_media_status status = md_media_status::pending;
		vgui_texture* texture = nullptr;
		int pixel_width = 0;
		int pixel_height = 0;
		float aspect = 0.0f;
		std::string error;
	};

	using md_media_resolver = std::function<md_media_info(
		const std::string& source, md_media_kind kind)>;

	class markdown_view
	{
	public:
		markdown_view();
		~markdown_view();
		markdown_view(const markdown_view&) = delete;
		markdown_view& operator=(const markdown_view&) = delete;

		/* Builds the view inside parent.  The widget grows to fill its slot. */
		void build(vgui_widget* parent);
		bool built() const noexcept;
		vgui_widget* widget() const noexcept;

		/* Replaces the document.  Input beyond 1 MiB is truncated on a UTF-8
		 * boundary and reflected by truncated(). */
		void set_markdown(std::string text);
		const std::string& markdown() const noexcept;
		bool truncated() const noexcept;

		/* Plain text as Ctrl+C would produce: markup markers removed, table
		 * cells tab separated, blocks separated by newlines, media captions
		 * replaced by alt text (or the file name). */
		std::string rendered_text() const;

		void set_font_size(float size);
		float font_size() const noexcept;

		void set_highlight_theme(const code_highlight_theme& theme);
		const code_highlight_theme& highlight_theme() const noexcept;
		bool set_highlight_preset(const std::string& name);

		/* Effective-pixel vertical content size for the current widget width. */
		float content_height() const;
		void set_scroll(float y);
		float scroll_y() const noexcept;
		/* Scrolls the block into view; center puts it in the middle. */
		void scroll_to_block(int block, bool center = false);
		void scroll_to_source_line(int line, bool center = false);

		int block_count() const noexcept;
		int block_source_line(int block) const noexcept;
		int block_source_end_line(int block) const noexcept;
		/* Block index containing the 0-based source line, or -1. */
		int block_for_source_line(int line) const noexcept;
		/* 0-based source line at local (effective-pixel) coordinates, clamped
		 * to the document. */
		int source_line_at(float x, float y) const;

		/* Tints the block covering this source line.  -1 clears it. */
		void set_active_source_line(int line);
		int active_source_line() const noexcept;

		bool has_selection() const noexcept;
		void select_all();
		void clear_selection();
		std::string selected_text() const;
		bool copy();
		/* Replaces the system-clipboard bridge; useful for tests and hosts
		 * that manage the clipboard themselves. */
		void set_clipboard_setter(std::function<void(const std::string&)> setter);

		/* Media reservation.  The resolver is called for every media item when
		 * the document changes and again from invalidate_media().  It should
		 * return quickly (cached state); hosts push a new status by calling
		 * invalidate_media(source) once an asynchronous load finishes. */
		void set_media_resolver(md_media_resolver resolver);
		void invalidate_media(const std::string& source = {});
		void set_max_media_height(float logical_height);
		float max_media_height() const noexcept;

	private:
		struct impl;
		std::unique_ptr<impl> impl_;
	};

} // namespace vgui_editor_kit
