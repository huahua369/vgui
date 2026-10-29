#pragma once
/*
 * code_editor.h - syntax-highlighted code editor and rich log view.
 *
 * The model is a line-based UTF-8 document.  CRLF/CR input is normalized to
 * LF, and every line carries cached token spans so only the visible part of a
 * file is highlighted.  The widget is deliberately host-owned: the editor kit
 * does not own files, documents or language services.  A host can use it as a
 * script editor (editing, undo/redo, find/replace) or as a read-only rich log
 * viewer (`append_log`, per-level colours, auto-scroll).
 *
 * Large files are opened incrementally: `open_file()` starts a background-free
 * streaming read and `tick()`/`process_load_step()` consume at most
 * `load_chunk_size()` bytes per call, so the UI thread is never blocked by a
 * single big read.  `tick()` also drives log auto-scroll; hosts that are sure
 * the editor is never hidden can rely on drawing instead.
 *
 * The visible widget is a column container: a find/replace bar (hidden until
 * opened) plus a VGUI_CUSTOM draw/event surface.  The custom surface handles
 * text input itself through the core custom-text APIs.
 */
#include <array>
#include <cstddef>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "command_stack.h"
#include "search_box.h"
#include <vgui/vgui.h>

namespace vgui_editor_kit {

	enum class code_language
	{
		plain = 0,
		cpp,
		csharp,
		glsl,
		json,
		lua,
		python,
		ini,
		markdown
	};

	const char* code_language_name(code_language language) noexcept;
	bool code_language_from_name(const std::string& name,
		code_language* language) noexcept;
	/* Detects a language from a file extension; unknown extensions are plain. */
	code_language code_language_from_path(const std::string& path) noexcept;

	enum class code_token_kind
	{
		text = 0,
		keyword,
		type,
		string_literal,
		number,
		comment,
		preprocessor,
		function,
		operator_token,
		punctuation,
		attribute,
		tag
	};

	static constexpr int code_token_kind_count = 12;

	struct code_span
	{
		/* Byte offsets within the line. */
		int start = 0;
		int length = 0;
		code_token_kind kind = code_token_kind::text;
		/* Non-zero overrides the token colour (used by rich log spans). */
		vgui_color color = 0;
	};

	enum class code_log_level
	{
		trace = 0,
		debug,
		info,
		warning,
		error
	};

	static constexpr int code_log_level_count = 5;

	struct code_position
	{
		int line = 0;
		/* Byte offset within the line, always on a UTF-8 boundary. */
		int column = 0;
	};

	inline bool operator==(const code_position& a, const code_position& b)
	{
		return a.line == b.line && a.column == b.column;
	}

	inline bool operator!=(const code_position& a, const code_position& b)
	{
		return !(a == b);
	}

	inline bool operator<(const code_position& a, const code_position& b)
	{
		return a.line < b.line ||
			(a.line == b.line && a.column < b.column);
	}

	inline bool operator>(const code_position& a, const code_position& b)
	{
		return b < a;
	}

	inline bool operator<=(const code_position& a, const code_position& b)
	{
		return !(b < a);
	}

	inline bool operator>=(const code_position& a, const code_position& b)
	{
		return !(a < b);
	}

	struct code_match
	{
		int line = 0;
		int column = 0;
		int length = 0;
	};

	struct code_editor_state
	{
		static constexpr int current_version = 1;

		int version = current_version;
		code_language language = code_language::plain;
		bool read_only = false;
		bool show_line_numbers = true;
		int tab_size = 4;
		code_position caret{};
		float scroll_x = 0.0f;
		float scroll_y = 0.0f;
	};

	std::string code_editor_state_to_json(const code_editor_state& state);
	bool code_editor_state_from_json(const std::string& text,
		code_editor_state& out, std::string* error = nullptr);

	class code_edit_command;

	class code_editor
	{
	public:
		using change_listener = std::function<void()>;
		using clipboard_getter = std::function<std::string()>;
		using clipboard_setter = std::function<void(const std::string&)>;

		code_editor();
		~code_editor();

		code_editor(const code_editor&) = delete;
		code_editor& operator=(const code_editor&) = delete;

		void build(vgui_widget* parent);
		bool built() const noexcept;
		vgui_widget* widget() const noexcept;
		vgui_widget* editor_widget() const noexcept;
		vgui_widget* find_bar() const noexcept;
		void request_focus();
		bool focused() const;

		/* ---- document ------------------------------------------------- */
		/* Replaces the document and resets caret, selection, scroll and the
		 * undo history.  Invalid CRLF/CR sequences are normalized to LF. */
		void set_text(std::string text);
		std::string text() const;
		void clear();
		/* Appends text at the end without moving the caret or recording undo;
		 * mainly useful for log/document streaming. */
		void append_text(std::string text);

		std::size_t line_count() const noexcept;
		/* Returns an empty string for an out-of-range line. */
		std::string line_text(int line) const;
		bool set_line_text(int line, std::string text);
		/* Token spans for a line, computed lazily.  The reference stays valid
		 * until the next edit/language change. */
		const std::vector<code_span>& line_spans(int line);
		/* Replaces the spans of one line and disables tokenization for it until
		 * the line is edited.  Rich log helpers use this internally. */
		bool set_line_spans(int line, std::vector<code_span> spans);
		bool highlight_valid_upto(std::size_t* count) const noexcept;

		/* ---- language / presentation ---------------------------------- */
		void set_language(code_language language);
		code_language language() const noexcept;
		void set_language_from_path(const std::string& path);
		void set_read_only(bool read_only);
		bool read_only() const noexcept;
		void set_show_line_numbers(bool show);
		bool show_line_numbers() const noexcept;
		void set_tab_size(int tab_size);
		int tab_size() const noexcept;
		void set_font_size(float size);
		float font_size() const noexcept;

		void set_token_color(code_token_kind kind, vgui_color color);
		vgui_color token_color(code_token_kind kind) const noexcept;
		void reset_token_colors();

		/* ---- editing -------------------------------------------------- */
		bool insert_text(std::string text);
		bool backspace();
		bool delete_forward();
		bool newline();
		bool indent_selection();
		bool outdent_selection();

		bool undo();
		bool redo();
		bool can_undo() const;
		bool can_redo() const;
		/* Edits are pushed to this stack (and host undo commands call back into
		 * the editor).  Without a stack the editor keeps a local history. */
		void set_command_stack(command_stack* stack);
		command_stack* command_stack() const noexcept;

		/* ---- caret / selection ---------------------------------------- */
		code_position caret() const noexcept;
		void set_caret(code_position position);
		void set_caret(int line, int column);
		int caret_line() const noexcept;
		int caret_column() const noexcept;

		code_position selection_anchor() const noexcept;
		bool has_selection() const noexcept;
		void set_selection(code_position anchor, code_position caret);
		void select_all();
		void clear_selection();
		std::string selected_text() const;

		/* ---- clipboard ------------------------------------------------ */
		/* The default bridge uses a hidden VGUI_EDIT so copy/paste goes through
		 * the platform clipboard.  Tests and hosts can inject handlers. */
		void set_clipboard_handlers(clipboard_getter getter,
			clipboard_setter setter);
		void clear_clipboard_handlers();
		bool copy();
		bool cut();
		bool paste();

		/* ---- find / replace ------------------------------------------- */
		void set_find_query(std::string text);
		const std::string& find_query() const noexcept;
		void set_find_case_sensitive(bool sensitive);
		bool find_case_sensitive() const noexcept;
		/* Opens the find bar (Ctrl+F).  replace_mode focuses the replace field. */
		void open_find(bool replace_mode = false);
		void close_find();
		bool find_visible() const noexcept;
		bool find_next();
		bool find_previous();
		std::vector<code_match> find_all(const std::string& needle,
			bool case_sensitive = false) const;
		const std::vector<code_match>& matches() const noexcept;
		int active_match() const noexcept;
		bool replace_current(std::string replacement);
		int replace_all(std::string needle, std::string replacement,
			bool case_sensitive = false);

		/* ---- rich log ------------------------------------------------- */
		void append_log(code_log_level level, std::string text,
			std::string timestamp = {});
		void set_log_level_color(code_log_level level, vgui_color color);
		vgui_color log_level_color(code_log_level level) const noexcept;
		void set_auto_scroll(bool enabled);
		bool auto_scroll() const noexcept;
		void set_max_lines(std::size_t max_lines);
		std::size_t max_lines() const noexcept;
		std::size_t dropped_lines() const noexcept;

		/* ---- incremental file loading --------------------------------- */
		/* Starts streaming path into an empty document.  Returns false when the
		 * file cannot be opened; the previous document is left untouched. */
		bool open_file(const std::string& path, std::string* error = nullptr);
		/* Starts an in-memory incremental load (handy for tests and archives). */
		void begin_incremental_load(std::string text);
		bool loading() const noexcept;
		float load_progress() const noexcept;
		void set_load_chunk_size(std::size_t bytes);
		std::size_t load_chunk_size() const noexcept;
		/* Consumes one chunk.  Returns true while more data remains. */
		bool process_load_step();
		void cancel_load();
		void tick(float delta_time);

		/* ---- scrolling ------------------------------------------------- */
		void set_scroll(float x, float y);
		float scroll_x() const noexcept;
		float scroll_y() const noexcept;
		void scroll_to_caret();

		/* ---- state / notifications ------------------------------------ */
		code_editor_state capture_state() const;
		bool apply_state(const code_editor_state& state,
			std::string* error = nullptr);
		void set_change_listener(change_listener listener);

	private:
		struct line_record
		{
			std::string text;
			std::vector<code_span> spans;
			bool manual_spans = false;
			int lex_after = 0;
		};

		struct text_change
		{
			code_position start{};
			std::string removed;
			std::string inserted;
			int merge_serial = 0;
		};

		struct geometry
		{
			float x = 0.0f;
			float y = 0.0f;
			float width = 0.0f;
			float height = 0.0f;
			float gutter = 0.0f;
			float text_x = 0.0f;
			float text_width = 0.0f;
			float text_height = 0.0f;
			float line_height = 0.0f;
			float vertical_bar = 0.0f;
			float horizontal_bar = 0.0f;
			bool vertical_scroll = false;
			bool horizontal_scroll = false;
		};

		friend class code_edit_command;

		void notify_change();
		void set_document_raw(std::string text);
		void normalize_after_change(int first_line);
		void ensure_highlight(int line);
		void tokenize_line(const std::string& text, int lex_in,
			std::vector<code_span>& spans, int* lex_out) const;

		code_position clamp_position(code_position value) const;
		int line_length(int line) const;
		int clamp_column(int line, int column) const;
		static int advance_column(const std::string& text, int column);
		static int retreat_column(const std::string& text, int column);
		static code_position advance_position(const std::vector<line_record>& lines,
			code_position start, const std::string& text);
		std::string text_range(code_position start, code_position end) const;
		std::string text_range_raw(const std::vector<line_record>& lines,
			code_position start, code_position end) const;

		bool apply_change(code_position start, const std::string& removed,
			const std::string& inserted);
		bool replace_range(code_position start, code_position end,
			std::string inserted, int merge_serial, const char* command_name);
		bool push_edit_command(code_position start, std::string removed,
			std::string inserted, int merge_serial, const char* command_name);
		bool apply_local_undo();
		bool apply_local_redo();

		void set_caret_internal(code_position position, bool keep_selection);
		void move_caret(code_position position, bool extend_selection);
		void select_word_at(code_position position);
		void select_line_at(int line);
		bool selection_range(code_position* start, code_position* end) const;

		std::string read_clipboard() const;
		void write_clipboard(const std::string& text);

		void rebuild_matches();
		void select_match(int index);
		bool match_at_caret() const;
		bool replace_match(int index, const std::string& replacement);
		void update_find_visibility();
		void update_text_input_rect();
		void toggle_find_case();

		float ui_scale() const;
		geometry editor_geometry() const;
		float content_width() const;
		float x_for_column(int line, int column) const;
		int column_at_x(int line, float x) const;
		std::string display_prefix(int line, int column) const;
		std::string display_segment(int line, int start_column,
			int end_column) const;
		void ensure_caret_visible();
		void clamp_scroll();
		void scroll_to_line(int line);

		static void on_draw(vgui_widget* widget, struct ovg_ctx_cb* cb,
			struct rvg_t* vg, float abs_x, float abs_y, float abs_w,
			float abs_h, float delta_time, void* user);
		static int on_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_find_changed(const std::string& text, void* user);
		static void on_find_submit(const std::string& text, void* user);
		static void on_replace_event(vgui_widget* widget,
			const vgui_event* event, void* user);
		static void on_button_event(vgui_widget* widget,
			const vgui_event* event, void* user);

		vgui_context* context_ = nullptr;
		vgui_widget* root_ = nullptr;
		vgui_widget* find_bar_ = nullptr;
		vgui_widget* editor_ = nullptr;
		vgui_widget* replace_edit_ = nullptr;
		vgui_widget* prev_button_ = nullptr;
		vgui_widget* next_button_ = nullptr;
		vgui_widget* case_button_ = nullptr;
		vgui_widget* close_find_button_ = nullptr;
		vgui_widget* replace_button_ = nullptr;
		vgui_widget* replace_all_button_ = nullptr;
		vgui_widget* clipboard_sink_ = nullptr;
		search_box search_;

		std::vector<line_record> lines_;
		std::size_t highlight_valid_ = 0;
		std::size_t max_line_bytes_ = 0;
		code_language language_ = code_language::plain;
		bool read_only_ = false;
		bool show_line_numbers_ = true;
		int tab_size_ = 4;
		float font_size_ = 13.0f;
		int caret_width = 1;
		code_position caret_{};
		code_position anchor_{};
		float scroll_x_ = 0.0f;
		float scroll_y_ = 0.0f;

		int drag_mode_ = 0;		// 0 none, 1 text, 2 vertical bar, 3 horizontal bar
		float drag_grab_ = 0.0f;
		int click_mode_ = 0;	// 1 character, 2 word, 3 line
		int edit_group_ = 1;

		std::string find_query_;
		bool find_case_sensitive_ = false;
		bool find_open_ = false;
		bool find_replace_mode_ = false;
		bool updating_find_ = false;
		std::vector<code_match> matches_;
		int active_match_ = -1;

		std::size_t max_lines_ = 0;
		std::size_t dropped_lines_ = 0;
		bool auto_scroll_ = true;
		vgui_color log_colors_[code_log_level_count] = {};
		vgui_color token_colors_[code_token_kind_count] = {};

		class command_stack* command_stack_ = nullptr;
		std::vector<text_change> local_history_;
		std::size_t local_history_index_ = 0;

		clipboard_getter clipboard_getter_;
		clipboard_setter clipboard_setter_;

		bool loading_ = false;
		bool load_from_memory_ = false;
		std::ifstream load_stream_;
		std::string load_memory_;
		std::size_t load_memory_offset_ = 0;
		std::string load_pending_;
		std::size_t load_total_bytes_ = 0;
		std::size_t load_read_bytes_ = 0;
		std::size_t load_chunk_size_ = 64 * 1024;
		bool load_need_new_line_ = false;

		float tick_time_ = 0.0f;
		change_listener change_listener_;
	};

} /* namespace vgui_editor_kit */
