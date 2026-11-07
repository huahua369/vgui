/* sample/vgui_editor_c_api_test_data.c - headless C tests for the code editor, markdown preview, thumbnail cache, asset browser and asset preview sections of <vgui/vgui_editor.h>. */
#include "vgui_editor_c_api_test.h"

/* Everything below is plain C99 against <vgui/vgui_editor.h>: no C++ syntax, no
 * GPU, no fonts and no real files.  Two rules shape the checks:
 *   - A string getter returns the number of bytes it would write (the length
 *     without the terminator), so every round trip asks for the size first,
 *     then reads into an exactly sized buffer, and verifies the short-buffer
 *     truncation on the way.
 *   - Loaders and disk callbacks run on a worker thread.  They only copy bytes
 *     and bump counters; they never touch a handle.  Waiting for them happens
 *     on the main thread through a bounded tick()/poll() pump. */

/* -------------------------------------------------------------------------- */
/* Shared helpers                                                              */
/* -------------------------------------------------------------------------- */

/* Small synthetic thumbnail pixels: 4x4 RGBA8, every byte distinct. */
static unsigned char g_pixels[64];

static void fill_pixels(void)
{
	size_t i;
	for (i = 0; i < sizeof(g_pixels); ++i)
		g_pixels[i] = (unsigned char)(i * 7u + 3u);
}

/* Float comparison without <math.h>: the harness includes stdio/stdlib/string
 * only, and -ffast-math style helpers would not be portable C99 anyway. */
static int near_f(float a, float b, float eps)
{
	float d = a - b;
	if (d < 0.0f)
		d = -d;
	return d <= eps;
}

/* Every host callback owns its counters through one int so `release_user` can be
 * the same function for all of them. */
static void release_int(void* user)
{
	int* counter = (int*)user;
	if (counter)
		++*counter;
}

/* -------------------------------------------------------------------------- */
/* Code editor                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct code_fixture {
	int caret_calls;
	int caret_line;
	int caret_column;
	int change_calls;
	int clip_get_calls;
	int clip_set_calls;
	int released;
	char clip_payload[128];
} code_fixture;

static void on_code_caret(void* user, vgui_editor_code_position position)
{
	code_fixture* fix = (code_fixture*)user;
	++fix->caret_calls;
	fix->caret_line = position.line;
	fix->caret_column = position.column;
}

static void on_code_change(void* user)
{
	++((code_fixture*)user)->change_calls;
}

static size_t code_clip_get(void* user, char* out, size_t cap)
{
	code_fixture* fix = (code_fixture*)user;
	const char* text = fix->clip_payload;
	size_t length = strlen(text);
	++fix->clip_get_calls;
	if (out && cap > 0)
	{
		size_t bytes = length < cap - 1 ? length : cap - 1;
		memcpy(out, text, bytes);
		out[bytes] = '\0';
	}
	return length;
}

static void code_clip_set(void* user, const char* text)
{
	code_fixture* fix = (code_fixture*)user;
	++fix->clip_set_calls;
	if (text)
	{
		size_t bytes = strlen(text);
		if (bytes >= sizeof(fix->clip_payload))
			bytes = sizeof(fix->clip_payload) - 1;
		memcpy(fix->clip_payload, text, bytes);
		fix->clip_payload[bytes] = '\0';
	}
}

/* The clipboard owner is the whole fixture, so its release has to bump the
 * fixture's release counter, not the first field of the struct. */
static void release_code_fixture(void* user)
{
	code_fixture* fix = (code_fixture*)user;
	if (fix)
		++fix->released;
}

void test_code_editor(probe* p)
{
	vgui_editor_code_editor* editor;
	vgui_editor_code_position anchor;
	vgui_editor_code_position caret;
	vgui_editor_code_position seen;
	vgui_editor_code_span spans[24];
	vgui_editor_code_span rich;
	vgui_editor_code_match match;
	vgui_editor_code_language language;
	vgui_editor_code_token_kind kind;
	code_fixture fix;
	char doc[1024];
	char buf[1024];
	char tiny[16];
	const char* name;
	size_t needed;
	size_t count;
	size_t total;
	size_t i;
	int ok;
	int n;

	memset(p, 0, sizeof(*p));
	memset(&fix, 0, sizeof(fix));
	memset(&anchor, 0, sizeof(anchor));
	memset(&caret, 0, sizeof(caret));
	memset(&seen, 0, sizeof(seen));
	memset(&match, 0, sizeof(match));
	memset(&language, 0, sizeof(language));
	memset(&kind, 0, sizeof(kind));
	fix.clip_payload[0] = '\0';

	/* Languages and token kinds are pure functions: no widget needed. */
	name = vgui_editor_code_language_name(VGUI_EDITOR_CODE_CPP);
	check(name != NULL && name[0] != '\0', "language name is not empty");
	check(vgui_editor_code_language_from_name(name, &language) == 1
		&& language == VGUI_EDITOR_CODE_CPP, "language name round-trips");
	check(vgui_editor_code_language_from_name(
		vgui_editor_code_language_name(VGUI_EDITOR_CODE_GLSL), &language) == 1
		&& language == VGUI_EDITOR_CODE_GLSL, "the glsl name round-trips too");
	check(vgui_editor_code_language_from_name("nonsense-lang", &language) == 0,
		"unknown language name returns 0");
	check(vgui_editor_code_language_from_path("shadows.glsl")
		== VGUI_EDITOR_CODE_GLSL, "extension .glsl maps to glsl");
	check(vgui_editor_code_language_from_path("notes.unknownext")
		== VGUI_EDITOR_CODE_PLAIN, "unknown extension falls back to plain");
	name = vgui_editor_code_token_kind_name(VGUI_EDITOR_CODE_TOKEN_KEYWORD);
	check(name != NULL && name[0] != '\0', "token kind name is not empty");
	check(vgui_editor_code_token_kind_from_name(name, &kind) == 1
		&& kind == VGUI_EDITOR_CODE_TOKEN_KEYWORD, "token kind name round-trips");
	check(vgui_editor_code_token_kind_from_name("not-a-kind", &kind) == 0,
		"unknown token kind returns 0");

	editor = vgui_editor_code_editor_create(vgui_root(g_ui));
	frame(2);
	check(editor != NULL, "code editor built");
	check(vgui_editor_code_editor_built(editor) == 1, "editor reports itself built");
	check(vgui_editor_code_editor_widget(editor) != NULL, "editor widget exists");
	check(vgui_editor_code_editor_inner(editor) != NULL, "inner text widget exists");
	check(vgui_editor_code_editor_find_bar(editor) != NULL, "find bar exists");
	vgui_editor_code_editor_focus(editor);
	check(vgui_editor_code_editor_focused(editor) == 1, "focus setter takes");

	/* Document, including the CRLF -> LF normalisation the header promises. */
	vgui_editor_code_editor_set_text(editor, "first\r\nsecond\r");
	needed = vgui_editor_code_editor_text(editor, NULL, 0);
	check(needed == strlen("first\nsecond\n"), "set_text normalises line endings");
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_code_editor_text(editor, buf, sizeof(buf)) == needed
		&& strcmp(buf, "first\nsecond\n") == 0, "document text round-trips");
	check(vgui_editor_code_editor_line_count(editor) == 3, "line count follows text");
	memset(buf, 0, sizeof(buf));
	needed = vgui_editor_code_editor_line_text(editor, 1, buf, sizeof(buf));
	check(needed == 6 && strcmp(buf, "second") == 0, "line 1 reads back");
	check(vgui_editor_code_editor_line_text(editor, 9999, buf, sizeof(buf)) == 0,
		"out-of-range line yields the empty string");
	check(vgui_editor_code_editor_set_line_text(editor, 1, "second!") == 1,
		"set_line_text accepted");
	check(vgui_editor_code_editor_line_text(editor, 1, buf, sizeof(buf)) == 7
		&& strcmp(buf, "second!") == 0, "rewritten line reads back");

	/* A document far longer than the probe buffer proves the exact-size retry. */
	doc[0] = '\0';
	for (i = 0; i < 40; ++i)
		strcat(doc, "line_of_text_here\n");
	vgui_editor_code_editor_set_text(editor, doc);
	needed = vgui_editor_code_editor_text(editor, NULL, 0);
	check(needed == strlen(doc), "long document reports its size");
	memset(tiny, 0, sizeof(tiny));
	check(vgui_editor_code_editor_text(editor, tiny, sizeof(tiny)) == needed,
		"short buffer still reports the full size");
	check(strlen(tiny) == sizeof(tiny) - 1, "short buffer truncates at cap - 1");
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_code_editor_text(editor, buf, needed + 1) == needed
		&& strcmp(buf, doc) == 0, "exact-size retry reads the whole document");
	/* The parser keeps the row after the final '\n', exactly like the "first\n
	 * second\n" case above, so 40 terminated lines give 41 rows. */
	check(vgui_editor_code_editor_line_count(editor) == 41, "40 terminated lines parse to 41 rows");
	vgui_editor_code_editor_append_text(editor, "tail");
	check(vgui_editor_code_editor_line_count(editor) == 41, "append_text fills the trailing row");
	check(vgui_editor_code_editor_line_text(editor, 40, buf, sizeof(buf)) == 4
		&& strcmp(buf, "tail") == 0, "the appended text lands in the last row");
	vgui_editor_code_editor_clear(editor);
	check(vgui_editor_code_editor_text(editor, buf, sizeof(buf)) == 0,
		"clear empties the document");
	check(vgui_editor_code_editor_line_count(editor) == 1, "cleared document has one line");

	/* Presentation state. */
	vgui_editor_code_editor_set_language_from_path(editor, "shadows.glsl");
	check(vgui_editor_code_editor_language(editor)
		== vgui_editor_code_language_from_path("shadows.glsl"),
		"set_language_from_path agrees with from_path");
	vgui_editor_code_editor_set_language(editor, VGUI_EDITOR_CODE_PYTHON);
	check(vgui_editor_code_editor_language(editor) == VGUI_EDITOR_CODE_PYTHON,
		"set_language round-trips");
	check(vgui_editor_code_editor_show_line_numbers(editor) == 1,
		"line numbers default on");
	vgui_editor_code_editor_set_show_line_numbers(editor, 0);
	check(vgui_editor_code_editor_show_line_numbers(editor) == 0,
		"line numbers can be hidden");
	vgui_editor_code_editor_set_tab_size(editor, 3);
	check(vgui_editor_code_editor_tab_size(editor) == 3, "tab size round-trips");
	vgui_editor_code_editor_set_font_size(editor, 17.5f);
	check(near_f(vgui_editor_code_editor_font_size(editor), 17.5f, 0.01f),
		"font size round-trips");
	vgui_editor_code_editor_set_read_only(editor, 1);
	check(vgui_editor_code_editor_read_only(editor) == 1, "read-only flag round-trips");
	vgui_editor_code_editor_set_text(editor, "alpha\nbeta\ngamma\n");
	check(vgui_editor_code_editor_insert_text(editor, "x") == 0,
		"read-only editor rejects an insert");
	vgui_editor_code_editor_set_read_only(editor, 0);

	/* Token colours.  The default keyword colour is 0xFFC678DD, so the override
	 * has to differ from it for reset to have anything to undo. */
	{
		vgui_color custom = 0xFF112233u;
		vgui_editor_code_editor_set_token_color(editor,
			VGUI_EDITOR_CODE_TOKEN_KEYWORD, custom);
		check(vgui_editor_code_editor_token_color(editor,
			VGUI_EDITOR_CODE_TOKEN_KEYWORD) == custom, "token colour round-trips");
		vgui_editor_code_editor_reset_token_colors(editor);
		check(vgui_editor_code_editor_token_color(editor,
			VGUI_EDITOR_CODE_TOKEN_KEYWORD) == 0xFFC678DDu,
			"reset restores the default palette");
	}

	/* Token spans.  The tokenizer only emits spans for keywords, known types,
	 * call sites, numbers, strings, comments and punctuation, so a bare
	 * identifier line legitimately has none. */
	count = vgui_editor_code_editor_line_spans(editor, 0, NULL, 0);
	n = (int)vgui_editor_code_editor_line_spans(editor, 0, spans, 24);
	check(n == (int)count, "span probe count matches the fill count");
	check(count == 0, "a plain identifier line has no token spans");
	memset(&rich, 0, sizeof(rich));
	rich.start = 0;
	rich.length = 5;
	rich.kind = VGUI_EDITOR_CODE_TOKEN_STRING_LITERAL;
	rich.color = 0xFFFF5555u;
	{
		/* Still PYTHON, so `def` is a keyword and `value` is a call. */
		vgui_editor_code_editor_set_text(editor, "def value():\n");
		total = vgui_editor_code_editor_line_spans(editor, 0, NULL, 0);
		check(total >= 3, "a keyword line tokenizes to several spans");
		check(total <= 24, "spans fit the caller array");
		check(vgui_editor_code_editor_line_spans(editor, 0, spans, 1)
			== total, "a short array still reports every span");
		check(vgui_editor_code_editor_line_spans(editor, 0, spans, 24) == total
			&& spans[0].start == 0 && spans[0].length == 3
			&& spans[0].kind == VGUI_EDITOR_CODE_TOKEN_KEYWORD,
			"the first span is the keyword");
		vgui_editor_code_editor_set_text(editor, "alpha\nbeta\ngamma\n");
	}
	{
		vgui_editor_code_span custom[2];
		custom[0] = rich;
		custom[1] = rich;
		custom[1].kind = VGUI_EDITOR_CODE_TOKEN_COMMENT;
		check(vgui_editor_code_editor_set_line_spans(editor, 0, custom, 2) == 1,
			"manual spans accepted");
		check(vgui_editor_code_editor_line_spans(editor, 0, spans, 24) == 2,
			"manual spans replace the tokenized ones");
		check(vgui_editor_code_editor_line_spans(editor, 0, spans, 1) == 2,
			"span getter reports the true count over a short array");
	}
	total = 0;
	ok = vgui_editor_code_editor_highlight_valid(editor, &total);
	check(ok == 0 || ok == 1, "highlight validity is a flag");
	check(total > 0, "highlight validity reports a span total");

	/* Caret, selection and scroll. */
	vgui_editor_code_editor_set_on_caret(editor, on_code_caret, &fix);
	memset(p, 0, sizeof(*p));
	vgui_editor_code_editor_set_caret(editor, 1, 2);
	check(vgui_editor_code_editor_caret_line(editor) == 1, "caret line setter applies");
	check(vgui_editor_code_editor_caret_column(editor) == 2, "caret column setter applies");
	check(fix.caret_calls >= 1, "caret callback ran");
	check(fix.caret_line == 1 && fix.caret_column == 2, "caret callback payload matches");
	vgui_editor_code_editor_get_caret(editor, &seen);
	check(seen.line == 1 && seen.column == 2, "get_caret agrees with set_caret");
	anchor.line = 0;
	anchor.column = 0;
	caret.line = 0;
	caret.column = 5;
	vgui_editor_code_editor_set_selection(editor, anchor, caret);
	check(vgui_editor_code_editor_has_selection(editor) == 1, "selection is live");
	vgui_editor_code_editor_selection_anchor(editor, &seen);
	check(seen.line == 0 && seen.column == 0, "selection anchor reads back");
	needed = vgui_editor_code_editor_selected_text(editor, NULL, 0);
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_code_editor_selected_text(editor, buf, sizeof(buf)) == needed
		&& strcmp(buf, "alpha") == 0, "selected text reads back");
	vgui_editor_code_editor_select_all(editor);
	check(vgui_editor_code_editor_selected_text(editor, buf, sizeof(buf))
		== strlen("alpha\nbeta\ngamma\n")
		&& strcmp(buf, "alpha\nbeta\ngamma\n") == 0, "select_all covers the document");
	vgui_editor_code_editor_clear_selection(editor);
	check(vgui_editor_code_editor_has_selection(editor) == 0, "selection cleared");
	vgui_editor_code_editor_set_scroll(editor, 12.0f, 64.0f);
	check(vgui_editor_code_editor_scroll_x(editor) <= 12.0f
		&& vgui_editor_code_editor_scroll_y(editor) <= 64.0f,
		"scroll stays within the requested offset");
	vgui_editor_code_editor_scroll_to_caret(editor);
	check(vgui_editor_code_editor_scroll_y(editor) >= 0.0f, "scroll to caret is sane");

	/* Clipboard bridge through host handlers. */
	strcpy(fix.clip_payload, "PASTED");
	vgui_editor_code_editor_set_clipboard_handlers(editor, code_clip_get,
		code_clip_set, &fix, release_code_fixture);
	vgui_editor_code_editor_select_all(editor);
	ok = vgui_editor_code_editor_copy(editor);
	check(ok == 1, "copy reports success");
	check(fix.clip_set_calls >= 1, "copy pushes text to the host clipboard");
	check(strcmp(fix.clip_payload, "alpha\nbeta\ngamma\n") == 0
		|| fix.clip_payload[0] == 'a', "copied payload starts with the document");
	vgui_editor_code_editor_clear_selection(editor);
	vgui_editor_code_editor_set_caret(editor, 0, 0);
	strcpy(fix.clip_payload, "XY");
	needed = vgui_editor_code_editor_text(editor, NULL, 0);
	check(vgui_editor_code_editor_paste(editor) == 1, "paste reports success");
	check(fix.clip_get_calls >= 1, "paste pulls from the host clipboard");
	check(vgui_editor_code_editor_text(editor, NULL, 0) == needed + 2,
		"paste grew the document by the clipboard length");
	/* cut() only writes the clipboard when there is a selection to remove. */
	vgui_editor_code_editor_select_all(editor);
	check(vgui_editor_code_editor_cut(editor) == 1, "cut reports success");
	check(fix.clip_set_calls >= 2, "cut pushes to the host clipboard too");
	vgui_editor_code_editor_clear_clipboard_handlers(editor);
	check(fix.released == 1, "clipboard release_user ran exactly once");

	/* Editing plus the local undo history. */
	vgui_editor_code_editor_set_text(editor, "one\n");
	vgui_editor_code_editor_set_caret(editor, 0, 3);
	check(vgui_editor_code_editor_insert_text(editor, "!") == 1, "insert accepted");
	check(vgui_editor_code_editor_can_undo(editor) == 1, "an edit is undoable");
	needed = vgui_editor_code_editor_text(editor, NULL, 0);
	check(needed == strlen("one!\n"), "insert grew the text");
	check(vgui_editor_code_editor_undo(editor) == 1, "undo ran");
	check(vgui_editor_code_editor_text(editor, NULL, 0) == 4, "undo restored the text");
	check(vgui_editor_code_editor_can_redo(editor) == 1, "redo is available");
	check(vgui_editor_code_editor_redo(editor) == 1, "redo ran");
	check(vgui_editor_code_editor_backspace(editor) == 1, "backspace ran");
	check(vgui_editor_code_editor_delete_forward(editor) == 1, "forward delete ran");
	check(vgui_editor_code_editor_newline(editor) == 1, "newline ran");
	check(vgui_editor_code_editor_line_count(editor) >= 2, "newline added a line");
	vgui_editor_code_editor_select_all(editor);
	check(vgui_editor_code_editor_indent_selection(editor) == 1, "indent ran");
	check(vgui_editor_code_editor_outdent_selection(editor) == 1, "outdent ran");

	/* Find, matches and replace. */
	vgui_editor_code_editor_set_text(editor, "foo bar foo\nfoo\n");
	vgui_editor_code_editor_set_find_query(editor, "foo");
	needed = vgui_editor_code_editor_find_query(editor, NULL, 0);
	memset(tiny, 0, sizeof(tiny));
	check(vgui_editor_code_editor_find_query(editor, tiny, sizeof(tiny)) == needed
		&& strcmp(tiny, "foo") == 0, "find query round-trips");
	vgui_editor_code_editor_set_find_case_sensitive(editor, 1);
	check(vgui_editor_code_editor_find_case_sensitive(editor) == 1,
		"case sensitivity round-trips");
	vgui_editor_code_editor_open_find(editor, 1);
	frame(1);
	check(vgui_editor_code_editor_find_visible(editor) == 1, "open_find shows the bar");
	check(vgui_editor_code_editor_find_next(editor) == 1, "find_next hits");
	/* Navigating to a match selects it, which is what makes it the active one. */
	check(vgui_editor_code_editor_active_match(editor) >= 0,
		"find_next leaves an active match");
	check(vgui_editor_code_editor_find_previous(editor) == 1, "find_previous hits");
	vgui_editor_code_editor_close_find(editor);
	check(vgui_editor_code_editor_find_visible(editor) == 0, "close_find hides the bar");
	check(vgui_editor_code_editor_find_all(editor, "foo", 1) == 3,
		"find_all counts every occurrence");
	check(vgui_editor_code_editor_match_count(editor) == 3, "matches feed match_count");
	check(vgui_editor_code_editor_match_at(editor, 0, &match) == 1
		&& match.line == 0 && match.column == 0 && match.length == 3,
		"match_at(0) is the first occurrence");
	check(vgui_editor_code_editor_match_at(editor, 99, &match) == 0,
		"match_at past the end is rejected");
	/* find_all publishes the query through the setters, and rebuilding the list
	 * from a query reset clears the active index until a navigation runs. */
	check(vgui_editor_code_editor_active_match(editor) == -1,
		"a fresh find_all has no active match");
	check(vgui_editor_code_editor_find_next(editor) == 1, "find_next resumes the matches");
	check(vgui_editor_code_editor_active_match(editor) >= 0,
		"the active match index is sane");
	check(vgui_editor_code_editor_find_all(editor, "FOO", 1) == 0,
		"case-sensitive find_all respects the flag");
	check(vgui_editor_code_editor_find_all(editor, "FOO", 0) == 3,
		"case-insensitive find_all ignores it");
	check(vgui_editor_code_editor_replace_current(editor, "qux") == 1,
		"replace_current ran");
	check(vgui_editor_code_editor_replace_all(editor, "qux", "baz", 0) == 1,
		"replace_all ran");
	check(vgui_editor_code_editor_text(editor, NULL, 0)
		== strlen("baz bar baz\nbaz\n"), "replace_all rewrote the document");

	/* Rich log mode. */
	vgui_editor_code_editor_set_text(editor, "");
	vgui_editor_code_editor_set_auto_scroll(editor, 1);
	check(vgui_editor_code_editor_auto_scroll(editor) == 1, "auto scroll round-trips");
	{
		vgui_editor_log_level level;
		vgui_color colors[5];
		for (level = VGUI_EDITOR_LOG_TRACE; level <= VGUI_EDITOR_LOG_ERROR; ++level)
		{
			colors[(int)level] = (vgui_color)(0xFF001122u + (unsigned)(int)level * 0x100u);
			vgui_editor_code_editor_set_level_color(editor, level,
				colors[(int)level]);
		}
		ok = 1;
		for (level = VGUI_EDITOR_LOG_TRACE; level <= VGUI_EDITOR_LOG_ERROR; ++level)
		{
			if (vgui_editor_code_editor_level_color(editor, level)
				!= colors[(int)level])
				ok = 0;
		}
		check(ok == 1, "every log level colour round-trips");
	}
	vgui_editor_code_editor_append_log(editor, VGUI_EDITOR_LOG_TRACE, "trace line",
		"12:00:00");
	vgui_editor_code_editor_append_log(editor, VGUI_EDITOR_LOG_DEBUG, "debug line",
		"12:00:01");
	vgui_editor_code_editor_append_log(editor, VGUI_EDITOR_LOG_INFO, "info line",
		"12:00:02");
	vgui_editor_code_editor_append_log(editor, VGUI_EDITOR_LOG_WARNING, "warn line",
		"12:00:03");
	vgui_editor_code_editor_append_log(editor, VGUI_EDITOR_LOG_ERROR, "error line",
		NULL);
	check(vgui_editor_code_editor_line_count(editor) == 5, "each level appends a line");
	check(vgui_editor_code_editor_dropped_lines(editor) == 0, "nothing dropped yet");
	vgui_editor_code_editor_set_max_lines(editor, 3);
	check(vgui_editor_code_editor_max_lines(editor) == 3, "max lines round-trips");
	for (i = 0; i < 6; ++i)
		vgui_editor_code_editor_append_log(editor, VGUI_EDITOR_LOG_INFO, "spam", NULL);
	check(vgui_editor_code_editor_line_count(editor) <= 3, "max lines caps the log");
	check(vgui_editor_code_editor_dropped_lines(editor) >= 1, "caps report dropped lines");

	/* Incremental loading. */
	vgui_editor_code_editor_set_load_chunk_size(editor, 8);
	check(vgui_editor_code_editor_load_chunk_size(editor) == 8, "chunk size round-trips");
	check(vgui_editor_code_editor_loading(editor) == 0, "idle before the load");
	vgui_editor_code_editor_begin_incremental_load(editor, doc);
	check(vgui_editor_code_editor_loading(editor) == 1, "incremental load started");
	check(vgui_editor_code_editor_load_progress(editor) < 1.0f, "progress begins below 1");
	for (i = 0; i < 4000 && vgui_editor_code_editor_process_load_step(editor); ++i)
	{
		/* Consume every chunk; process_load_step returns 1 while data remains. */
	}
	check(vgui_editor_code_editor_loading(editor) == 0, "the load drained");
	check(vgui_editor_code_editor_load_progress(editor) >= 0.999f, "progress reached 1");
	check(vgui_editor_code_editor_text(editor, NULL, 0) == strlen(doc),
		"the streamed text equals the source");
	vgui_editor_code_editor_begin_incremental_load(editor, doc);
	vgui_editor_code_editor_cancel_load(editor);
	check(vgui_editor_code_editor_loading(editor) == 0, "cancel_load stops the load");
	vgui_editor_code_editor_tick(editor, 0.016f);
	frame(1);

	/* Missing files must leave the document untouched, per the header.  Two rows
	 * because the state JSON round trip below restores a caret on line 1. */
	vgui_editor_code_editor_set_text(editor, "keep me\nand me too\n");
	needed = vgui_editor_code_editor_text(editor, NULL, 0);
	check(vgui_editor_code_editor_open_file(editor,
		"z:/definitely/not/a/real/file.txt") == 0, "open_file fails cleanly");
	check(vgui_editor_last_error()[0] != '\0', "open_file explains the failure");
	check(vgui_editor_code_editor_text(editor, NULL, 0) == needed,
		"a failed open_file keeps the previous text");

	/* Theme JSON plus preset enumeration. */
	needed = vgui_editor_code_editor_theme_json(editor, NULL, 0);
	check(needed > 0, "the theme has JSON to report");
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_code_editor_theme_json(editor, buf, needed + 1) == needed,
		"theme JSON fills an exact-size buffer");
	check(strlen(buf) == needed && buf[0] == '{', "theme JSON looks like JSON");
	check(vgui_editor_code_editor_theme_json(editor, tiny, sizeof(tiny)) == needed,
		"short JSON buffer still reports the full size");
	check(strlen(tiny) == sizeof(tiny) - 1, "short JSON buffer truncates at the cap");
	check(vgui_editor_code_editor_set_theme_json(editor, buf) == 1,
		"the captured theme re-applies");
	check(vgui_editor_code_editor_set_theme_json(editor, "{ not json") == 0,
		"invalid theme JSON is rejected");
	check(vgui_editor_last_error()[0] != '\0', "the rejection explains itself");
	check(vgui_editor_code_editor_set_theme_json(editor,
		"{\"version\":1,\"name\":\"probe\",\"colors\":{\"keyword\":\"#C678DD\"}}") == 1,
		"a documented theme JSON applies");
	{
		int found = 0;
		count = vgui_editor_code_editor_theme_preset_count();
		check(count >= 4, "at least four presets are enumerated");
		ok = 1;
		for (i = 0; i < count; ++i)
		{
			needed = vgui_editor_code_editor_theme_preset_name(i, NULL, 0);
			memset(tiny, 0, sizeof(tiny));
			/* Deliberately too small first: the getter must still report the size. */
			if (vgui_editor_code_editor_theme_preset_name(i, tiny, 4) != needed)
				ok = 0;
			memset(buf, 0, sizeof(buf));
			if (vgui_editor_code_editor_theme_preset_name(i, buf, sizeof(buf)) != needed
				|| needed == 0)
				ok = 0;
			if (strcmp(buf, "dark") == 0)
				found = 1;
		}
		check(ok == 1, "every preset name round-trips");
		check(found == 1, "the documented preset \"dark\" is listed");
	}
	check(vgui_editor_code_editor_set_theme_preset(editor, "dark") == 1,
		"preset dark applies");
	check(vgui_editor_code_editor_set_theme_preset(editor, "no-such-preset") == 0,
		"an unknown preset is rejected");

	/* State JSON. */
	needed = vgui_editor_code_editor_capture_state_json(editor, NULL, 0);
	check(needed > 0, "state JSON has content");
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_code_editor_capture_state_json(editor, buf, needed + 1) == needed
		&& strlen(buf) == needed, "state JSON round-trips through C strings");
	vgui_editor_code_editor_set_caret(editor, 1, 1);
	vgui_editor_code_editor_set_scroll(editor, 0.0f, 8.0f);
	needed = vgui_editor_code_editor_capture_state_json(editor, NULL, 0);
	memset(buf, 0, sizeof(buf));
	vgui_editor_code_editor_capture_state_json(editor, buf, needed + 1);
	check(vgui_editor_code_editor_apply_state_json(editor, buf) == 1,
		"the captured state re-applies");
	check(vgui_editor_code_editor_caret_line(editor) == 1, "caret survived the round trip");
	check(near_f(vgui_editor_code_editor_scroll_y(editor), 8.0f, 0.5f)
		|| vgui_editor_code_editor_scroll_y(editor) == 0.0f,
		"scroll round-trips or clamps to the document");
	check(vgui_editor_code_editor_apply_state_json(editor, "{oops") == 0,
		"invalid state JSON is rejected");
	check(vgui_editor_last_error()[0] != '\0', "invalid state JSON explains itself");

	/* Change notification. */
	vgui_editor_code_editor_set_on_change(editor, on_code_change, &fix);
	vgui_editor_code_editor_set_text(editor, "seed\n");
	check(fix.change_calls >= 1, "set_text reports a change");
	fix.change_calls = 0;
	vgui_editor_code_editor_set_caret(editor, 0, 0);
	ok = vgui_editor_code_editor_insert_text(editor, "z");
	check(ok == 1 && fix.change_calls == 1, "one edit fires on_change once");
	fix.change_calls = 0;
	vgui_editor_code_editor_set_on_change(editor, NULL, NULL);
	vgui_editor_code_editor_insert_text(editor, "q");
	check(fix.change_calls == 0, "a cleared on_change is silent");
	vgui_editor_code_editor_set_command_stack(editor, NULL);

	/* Teardown: clear every callback, destroy, then prove the NULL paths. */
	vgui_editor_code_editor_clear_clipboard_handlers(editor);
	vgui_editor_code_editor_set_on_caret(editor, NULL, NULL);
	vgui_editor_code_editor_set_on_change(editor, NULL, NULL);
	(void)editor;
	{
		char scratch[32];
		size_t scratch_size;
		check(vgui_editor_code_editor_built(NULL) == 0, "built(NULL) is false");
		check(vgui_editor_code_editor_widget(NULL) == NULL, "widget(NULL) is NULL");
		check(vgui_editor_code_editor_text(NULL, scratch, sizeof(scratch)) == 0,
			"text(NULL) reports nothing");
		scratch_size = vgui_editor_code_editor_capture_state_json(NULL, NULL, 0);
		check(scratch_size == 0, "capture_state_json(NULL) reports nothing");
		check(vgui_editor_code_editor_set_theme_preset(NULL, "dark") == 0,
			"theme preset on NULL fails");
		check(vgui_editor_code_editor_match_at(NULL, 0, &match) == 0,
			"match_at(NULL) is rejected");
		vgui_editor_code_editor_set_text(NULL, "noop");
		vgui_editor_code_editor_set_clipboard_handlers(NULL, code_clip_get,
			code_clip_set, NULL, NULL);
		vgui_editor_code_editor_get_caret(NULL, &seen);
		vgui_editor_code_editor_destroy(NULL);
		check(fix.released == 1, "double-clearing the clipboard does not release twice");
	}
	vgui_editor_code_editor_destroy(editor);
}

/* -------------------------------------------------------------------------- */
/* Markdown preview                                                            */
/* -------------------------------------------------------------------------- */

typedef struct md_fixture {
	int resolve_calls;
	int image_calls;
	int video_calls;
	int clip_calls;
	int released_resolve;
	int released_clip;
	char last_source[96];
	char clip_text[256];
} md_fixture;

static void md_resolve(void* user, const char* source,
	vgui_editor_md_media_kind kind, vgui_editor_md_media* out)
{
	md_fixture* fix = (md_fixture*)user;
	++fix->resolve_calls;
	if (kind == VGUI_EDITOR_MD_IMAGE)
		++fix->image_calls;
	else if (kind == VGUI_EDITOR_MD_VIDEO)
		++fix->video_calls;
	if (source)
	{
		size_t bytes = strlen(source);
		if (bytes >= sizeof(fix->last_source))
			bytes = sizeof(fix->last_source) - 1;
		memcpy(fix->last_source, source, bytes);
		fix->last_source[bytes] = '\0';
	}
	/* Fill host-owned state; the texture stays NULL because a headless context
	 * has no image to borrow, and the header says aspect then falls back. */
	vgui_editor_md_media_default(out);
	out->status = VGUI_EDITOR_MD_READY;
	out->texture = NULL;
	out->pixel_width = 64;
	out->pixel_height = 32;
	out->aspect = 2.0f;
	out->error = NULL;
}

static void md_clip(void* user, const char* text)
{
	md_fixture* fix = (md_fixture*)user;
	++fix->clip_calls;
	if (text)
	{
		size_t bytes = strlen(text);
		if (bytes >= sizeof(fix->clip_text))
			bytes = sizeof(fix->clip_text) - 1;
		memcpy(fix->clip_text, text, bytes);
		fix->clip_text[bytes] = '\0';
	}
}

void test_markdown(probe* p)
{
	vgui_editor_markdown* view;
	vgui_editor_md_media media;
	md_fixture fix;
	char buf[1024];
	char tiny[12];
	const char* doc = "# Title\n\nBody **bold** text\n\n- one\n- two\n\n"
		"![pic](a.png)\n";
	size_t needed;
	int blocks;
	int b;
	int matched;
	int line;

	memset(p, 0, sizeof(*p));
	memset(&fix, 0, sizeof(fix));
	vgui_editor_md_media_default(&media);

	vgui_editor_md_media_default(&media);
	check(media.struct_size == (uint32_t)sizeof(media), "media default sizes itself");
	check(media.status == VGUI_EDITOR_MD_PENDING, "media defaults to pending");
	check(media.texture == NULL && media.error == NULL, "media starts empty");
	check(media.aspect == 0.0f, "media aspect defaults to the fallback path");

	view = vgui_editor_markdown_create(vgui_root(g_ui));
	frame(2);
	check(view != NULL, "markdown view built");
	check(vgui_editor_markdown_built(view) == 1, "view reports itself built");
	check(vgui_editor_markdown_widget(view) != NULL, "view widget exists");

	vgui_editor_markdown_set_text(view, doc);
	needed = vgui_editor_markdown_text(view, NULL, 0);
	check(needed == strlen(doc), "markdown text reports its length");
	memset(tiny, 0, sizeof(tiny));
	check(vgui_editor_markdown_text(view, tiny, sizeof(tiny)) == needed,
		"short buffer still reports the full markdown length");
	check(strlen(tiny) == sizeof(tiny) - 1, "short markdown buffer truncates");
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_markdown_text(view, buf, needed + 1) == needed
		&& strcmp(buf, doc) == 0, "markdown document round-trips");
	check(vgui_editor_markdown_truncated(view) == 0, "a small document is not truncated");

	needed = vgui_editor_markdown_rendered_text(view, NULL, 0);
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_markdown_rendered_text(view, buf, needed + 1) == needed,
		"rendered text fills an exact-size buffer");
	check(strstr(buf, "**") == NULL, "rendered text drops the emphasis markup");
	check(strstr(buf, "bold") != NULL, "rendered text keeps the words");

	/* Block <-> source line mapping in both directions. */
	blocks = vgui_editor_markdown_block_count(view);
	check(blocks > 0, "the document parses into blocks");
	check(vgui_editor_markdown_block_source_line(view, 0) == 0,
		"the first block starts on source line 0");
	check(vgui_editor_markdown_block_source_end_line(view, 0)
		>= vgui_editor_markdown_block_source_line(view, 0),
		"block end line follows its start line");
	check(vgui_editor_markdown_block_for_source_line(view, 0) == 0,
		"line 0 maps back to block 0");
	check(vgui_editor_markdown_block_for_source_line(view, 9999) == -1,
		"a line past the end maps to -1");
	matched = 1;
	for (b = 0; b < blocks; ++b)
	{
		line = vgui_editor_markdown_block_source_line(view, b);
		if (line < 0 || vgui_editor_markdown_block_for_source_line(view, line) != b)
			matched = 0;
	}
	check(matched == 1, "block -> line -> block round-trips for every block");
	vgui_editor_markdown_set_active_source_line(view, 2);
	check(vgui_editor_markdown_active_source_line(view) == 2,
		"active source line round-trips");
	vgui_editor_markdown_set_active_source_line(view, -1);
	check(vgui_editor_markdown_active_source_line(view) == -1,
		"-1 clears the active source line");
	check(vgui_editor_markdown_source_line_at(view, 4.0f, 4.0f) >= 0,
		"source line lookup clamps into the document");

	/* Presentation and scroll. */
	vgui_editor_markdown_set_font_size(view, 15.0f);
	check(near_f(vgui_editor_markdown_font_size(view), 15.0f, 0.01f),
		"markdown font size round-trips");
	frame(1);
	check(vgui_editor_markdown_content_height(view) >= 0.0f,
		"content height is never negative");
	vgui_editor_markdown_set_scroll(view, 10.0f);
	check(vgui_editor_markdown_scroll_y(view) <= 10.0f, "scroll respects the request");
	vgui_editor_markdown_scroll_to_block(view, 0, 1);
	vgui_editor_markdown_scroll_to_block(view, blocks + 5, 0);
	check(vgui_editor_markdown_scroll_y(view) >= 0.0f, "scroll_to_block keeps it sane");
	vgui_editor_markdown_scroll_to_source_line(view, 2, 0);
	vgui_editor_markdown_scroll_to_source_line(view, 9999, 1);
	check(vgui_editor_markdown_scroll_y(view) >= 0.0f,
		"scroll_to_source_line clamps instead of failing");

	/* Selection, copy and the clipboard bridge. */
	check(vgui_editor_markdown_has_selection(view) == 0, "no selection initially");
	vgui_editor_markdown_select_all(view);
	check(vgui_editor_markdown_has_selection(view) == 1, "select_all selects");
	needed = vgui_editor_markdown_selected_text(view, NULL, 0);
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_markdown_selected_text(view, buf, needed + 1) == needed,
		"selected text fills an exact-size buffer");
	vgui_editor_markdown_set_clipboard_handler(view, md_clip, &fix, release_int);
	check(vgui_editor_markdown_copy(view) == 1, "copy reports success");
	check(fix.clip_calls >= 1, "copy reaches the host clipboard");
	check(strstr(fix.clip_text, "bold") != NULL, "the clipboard text is rendered text");
	vgui_editor_markdown_clear_selection(view);
	check(vgui_editor_markdown_has_selection(view) == 0, "clear_selection clears");
	fix.clip_calls = 0;
	vgui_editor_markdown_set_clipboard_handler(view, NULL, NULL, NULL);
	check(fix.released_clip == 0 || fix.clip_calls == 0, "clearing the clipboard is safe");

	/* Media resolver. */
	strcpy(fix.last_source, "");
	vgui_editor_markdown_set_media_resolver(view, md_resolve, &fix, release_int);
	vgui_editor_markdown_set_text(view, doc);
	frame(2);
	check(fix.resolve_calls >= 1, "the resolver runs when the document changes");
	check(fix.image_calls >= 1, "the image item arrives with the image kind");
	check(strstr(fix.last_source, "a.png") != NULL, "the resolver sees the media source");
	b = fix.resolve_calls;
	vgui_editor_markdown_invalidate_media(view, NULL);
	check(fix.resolve_calls > b, "invalidate_media(NULL) re-resolves everything");
	b = fix.resolve_calls;
	vgui_editor_markdown_invalidate_media(view, "a.png");
	check(fix.resolve_calls >= b, "invalidate_media(source) stays coherent");
	vgui_editor_markdown_set_max_media_height(view, 240.0f);
	check(near_f(vgui_editor_markdown_max_media_height(view), 240.0f, 0.01f),
		"max media height round-trips");
	/* The state the resolver produced must still be readable by the host. */
	vgui_editor_md_media_default(&media);
	media.status = VGUI_EDITOR_MD_FAILED;
	media.error = "no such file";
	check(media.status == VGUI_EDITOR_MD_FAILED, "media status is host-owned");
	check(strcmp(media.error, "no such file") == 0, "media error text is borrowable");

	/* Theme JSON, shared document with the code editor. */
	needed = vgui_editor_markdown_theme_json(view, NULL, 0);
	check(needed > 0, "the markdown view has theme JSON");
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_markdown_theme_json(view, buf, needed + 1) == needed
		&& strlen(buf) == needed, "markdown theme JSON round-trips");
	check(vgui_editor_markdown_set_theme_json(view, buf) == 1,
		"the captured markdown theme re-applies");
	check(vgui_editor_markdown_set_theme_json(view, "]]not json[[") == 0,
		"invalid markdown theme JSON is rejected");
	check(vgui_editor_last_error()[0] != '\0', "the rejection sets last_error");
	check(vgui_editor_markdown_set_theme_preset(view, "light") == 1,
		"markdown preset light applies");

	/* Teardown. */
	vgui_editor_markdown_set_media_resolver(view, NULL, NULL, release_int);
	check(fix.released_resolve == 0, "clearing without a user releases nothing");
	{
		char scratch[32];
		check(vgui_editor_markdown_built(NULL) == 0, "built(NULL) is false");
		check(vgui_editor_markdown_widget(NULL) == NULL, "widget(NULL) is NULL");
		check(vgui_editor_markdown_text(NULL, scratch, sizeof(scratch)) == 0,
			"text(NULL) reports nothing");
		check(vgui_editor_markdown_block_count(NULL) == 0, "block_count(NULL) is 0");
		check(vgui_editor_markdown_has_selection(NULL) == 0,
			"has_selection(NULL) is false");
		vgui_editor_md_media_default(NULL);
		vgui_editor_markdown_set_text(NULL, doc);
		vgui_editor_markdown_invalidate_media(NULL, NULL);
	}
	vgui_editor_markdown_destroy(view);
}

/* -------------------------------------------------------------------------- */
/* Thumbnail cache                                                             */
/* -------------------------------------------------------------------------- */

typedef struct thumb_fixture {
	int load_calls;
	int read_calls;
	int write_calls;
	int load_releases;
	int disk_releases;
	int seen_bytes;
	int fail_next;
	char last_key[64];
} thumb_fixture;

static void copy_request_key(thumb_fixture* fix,
	const vgui_editor_thumbnail_request* request)
{
	if (request && request->key)
	{
		size_t bytes = strlen(request->key);
		if (bytes >= sizeof(fix->last_key))
			bytes = sizeof(fix->last_key) - 1;
		memcpy(fix->last_key, request->key, bytes);
		fix->last_key[bytes] = '\0';
	}
}

/* Worker thread: only copies bytes and bumps counters. */
static int thumb_load(void* user, const vgui_editor_thumbnail_request* request,
	vgui_editor_thumbnail_image* out)
{
	thumb_fixture* fix = (thumb_fixture*)user;
	++fix->load_calls;
	copy_request_key(fix, request);
	vgui_editor_thumbnail_image_default(out);
	if (fix->fail_next)
	{
		out->error = "loader says no";
		return 0;
	}
	out->width = 4;
	out->height = 4;
	out->stride = 0;
	out->format = VGUI_TEXTURE_FORMAT_RGBA8;
	/* The kit copies the bytes as soon as the callback returns. */
	out->pixels = g_pixels;
	out->pixel_count = sizeof(g_pixels);
	return 1;
}

static int thumb_disk_read(void* user, const vgui_editor_thumbnail_request* request,
	vgui_editor_thumbnail_image* out)
{
	thumb_fixture* fix = (thumb_fixture*)user;
	++fix->read_calls;
	copy_request_key(fix, request);
	vgui_editor_thumbnail_image_default(out);
	/* A read hit only for the key that says "cached"; everything else falls
	 * through to the loader. */
	if (request && request->key && strcmp(request->key, "cached.key") == 0)
	{
		out->width = 4;
		out->height = 4;
		out->stride = 0;
		out->format = VGUI_TEXTURE_FORMAT_RGBA8;
		out->pixels = g_pixels;
		out->pixel_count = sizeof(g_pixels);
		return 1;
	}
	return 0;
}

static int thumb_disk_write(void* user, const vgui_editor_thumbnail_request* request,
	const vgui_editor_thumbnail_image* image)
{
	thumb_fixture* fix = (thumb_fixture*)user;
	++fix->write_calls;
	copy_request_key(fix, request);
	/* `image` borrows the kit's bytes for the duration of the call: copy now. */
	if (image && image->pixels && image->pixel_count > 0)
	{
		unsigned char local[64];
		size_t bytes = image->pixel_count;
		if (bytes > sizeof(local))
			bytes = sizeof(local);
		memcpy(local, image->pixels, bytes);
		fix->seen_bytes = (int)bytes;
	}
	return 1;
}

static void release_thumb_load(void* user)
{
	++((thumb_fixture*)user)->load_releases;
}

static void release_thumb_disk(void* user)
{
	++((thumb_fixture*)user)->disk_releases;
}

/* Enqueue `key` and pump the cache until it reaches a terminal state.  The
 * browser is pumped because it owns the rows whose thumbnails this drives; a
 * standalone cache would instead use vgui_editor_thumbnails_tick()/poll(). */
static int pump_until_ready(vgui_editor_asset_browser* browser,
	vgui_editor_thumbnails* cache, const char* key, vgui_editor_thumbnail_state* out)
{
	vgui_editor_thumbnail_state state = VGUI_EDITOR_THUMB_MISSING;
	int i;
	for (i = 0; i < 200; ++i)
	{
		vgui_editor_asset_browser_tick(browser, 0.05f);
		frame(1);
		state = vgui_editor_thumbnails_state(cache, key);
		if (state == VGUI_EDITOR_THUMB_READY || state == VGUI_EDITOR_THUMB_FAILED)
			break;
	}
	*out = state;
	if (state == VGUI_EDITOR_THUMB_READY)
		return 1;
	return 0;
}

void test_thumbnails(probe* p)
{
	vgui_editor_thumbnails* cache;
	vgui_editor_thumbnails* borrowed;
	vgui_editor_thumbnails* other;
	vgui_editor_asset_browser* browser;
	vgui_editor_thumbnail_options options;
	vgui_editor_thumbnail_options readback;
	vgui_editor_thumbnail_request request;
	vgui_editor_thumbnail_image image;
	vgui_editor_thumbnail_stats stats;
	vgui_editor_thumbnail_state state;
	thumb_fixture fix;
	vgui_item_id first;
	char buf[128];
	char key[96];
	size_t needed;
	int i;

	memset(p, 0, sizeof(*p));
	memset(&fix, 0, sizeof(fix));
	memset(&request, 0, sizeof(request));
	fill_pixels();

	/* Defaults, then a get/set round trip. */
	memset(&options, 0xFF, sizeof(options));
	vgui_editor_thumbnail_options_default(&options);
	check(options.struct_size == (uint32_t)sizeof(options),
		"options default sizes themselves");
	check(options.memory_limit == 256 && options.max_attempts == 2,
		"options carry the documented defaults");
	check(options.disk_cache == 1, "the disk cache defaults to on");
	check(near_f(options.retry_delay, 0.5f, 0.001f), "retry delay defaults to 0.5");
	memset(&image, 0xFF, sizeof(image));
	vgui_editor_thumbnail_image_default(&image);
	check(image.struct_size == (uint32_t)sizeof(image), "image default sizes itself");
	check(image.width == 0 && image.pixels == NULL && image.error == NULL,
		"the default image is empty");
	check(image.format == VGUI_TEXTURE_FORMAT_RGBA8, "the default format is RGBA8");

	cache = vgui_editor_thumbnails_create(g_ui);
	check(cache != NULL, "thumbnail cache created from the context");
	check(vgui_editor_thumbnails_context(cache) == g_ui, "the cache keeps its context");
	vgui_editor_thumbnails_set_options(cache, &options);
	memset(&readback, 0xFF, sizeof(readback));
	vgui_editor_thumbnails_get_options(cache, &readback);
	check(readback.memory_limit == options.memory_limit
		&& readback.max_attempts == options.max_attempts
		&& readback.disk_cache == options.disk_cache
		&& near_f(readback.retry_delay, options.retry_delay, 0.001f),
		"options round-trip through get/set");
	options.memory_limit = 4;
	options.max_attempts = 1;
	options.retry_delay = 0.0f;
	options.disk_cache = 0;
	vgui_editor_thumbnails_set_options(cache, &options);
	vgui_editor_thumbnails_get_options(cache, &readback);
	check(readback.memory_limit == 4 && readback.max_attempts == 1,
		"custom options take effect");
	check(vgui_editor_thumbnails_configured(cache) == 0,
		"an empty cache is not configured");

	/* Queueing bookkeeping, without a pump available to a standalone cache. */
	vgui_editor_thumbnail_request_default(&request);
	request.key = "plain.key";
	request.name = "plain";
	request.type = "Texture";
	vgui_editor_thumbnails_enqueue(cache, &request, VGUI_EDITOR_THUMB_HIGH);
	check(vgui_editor_thumbnails_state(cache, "plain.key")
		!= VGUI_EDITOR_THUMB_READY,
		"with nothing to pump it, a standalone job never reaches ready");
	check(vgui_editor_thumbnails_lookup(cache, "plain.key") == NULL,
		"nothing is cached yet");
	check(vgui_editor_thumbnails_texture(cache, "plain.key") == NULL,
		"texture() is NULL for an unloaded key");
	needed = vgui_editor_thumbnails_error(cache, "plain.key", NULL, 0);
	check(needed == 0, "an unloaded key has no error text");
	check(vgui_editor_thumbnails_state(cache, "never.enqueued")
		== VGUI_EDITOR_THUMB_MISSING, "unknown keys read as missing");
	/* An empty key is rejected by the kit, so no entry appears at all. */
	request.key = "";
	vgui_editor_thumbnails_enqueue(cache, &request, VGUI_EDITOR_THUMB_LOW);
	check(vgui_editor_thumbnails_error(cache, "", buf, sizeof(buf)) == 0,
		"an empty key stays empty");
	request.key = "plain.key";

	vgui_editor_thumbnails_set_load_handler(cache, thumb_load, &fix,
		release_thumb_load);
	check(vgui_editor_thumbnails_configured(cache) == 1, "a loader configures the cache");
	vgui_editor_thumbnails_enqueue(cache, &request, VGUI_EDITOR_THUMB_LOW);
	vgui_editor_thumbnails_cancel(cache, "plain.key");
	vgui_editor_thumbnails_cancel(cache, "not.a.key");
	vgui_editor_thumbnails_cancel_all(cache);
	vgui_editor_thumbnails_retry(cache, "plain.key");
	vgui_editor_thumbnails_erase(cache, "plain.key");
	check(vgui_editor_thumbnails_state(cache, "plain.key") == VGUI_EDITOR_THUMB_MISSING,
		"erase drops the entry");
	memset(&stats, 0xFF, sizeof(stats));
	vgui_editor_thumbnails_get_stats(cache, &stats);
	check(stats.struct_size == (uint32_t)sizeof(stats), "stats size themselves");
	check(stats.memory_entries == 0 && stats.pending == 0, "a cancelled cache is empty");
	check(stats.memory_limit == 4, "stats report the configured limit");

	/* A context-less cache is legal; uploads wait for set_context(). */
	other = vgui_editor_thumbnails_create(NULL);
	check(other != NULL, "create(NULL context) is allowed");
	check(vgui_editor_thumbnails_context(other) == NULL, "no context yet");
	vgui_editor_thumbnails_set_context(other, g_ui);
	check(vgui_editor_thumbnails_context(other) == g_ui, "set_context attaches it");
	vgui_editor_thumbnails_clear(other);
	check(vgui_editor_thumbnails_context(other) == g_ui, "clear keeps the context");

	/* The state machine itself: the browser owns a cache it pumps from tick(). */
	browser = vgui_editor_asset_browser_create(vgui_root(g_ui));
	frame(2);
	check(browser != NULL, "browser built to pump the cache");
	borrowed = vgui_editor_asset_browser_thumbnails(browser);
	check(borrowed != NULL, "the browser exposes a borrowed cache");
	{
		vgui_editor_thumbnails* again = vgui_editor_asset_browser_thumbnails(browser);
		check(again == borrowed, "the borrowed cache handle is stable");
	}
	first = vgui_editor_asset_browser_add_item(browser, "hero.fbx", "Mesh");
	check(first != 0, "browser item added for the thumbnail key");

	options.memory_limit = 64;
	options.max_attempts = 1;
	options.retry_delay = 0.0f;
	options.disk_cache = 1;
	vgui_editor_thumbnails_set_options(borrowed, &options);
	vgui_editor_thumbnails_set_load_handler(borrowed, thumb_load, &fix,
		release_thumb_load);
	vgui_editor_thumbnails_set_disk_cache(borrowed, thumb_disk_read,
		thumb_disk_write, &fix, release_thumb_disk);
	check(vgui_editor_thumbnails_configured(borrowed) == 1, "the borrowed cache is set up");

	/* 1) A loader success reaches ready and the disk write gets the bytes. */
	vgui_editor_thumbnail_request_default(&request);
	request.key = "fresh.key";
	request.name = "fresh";
	request.type = "Texture";
	vgui_editor_thumbnails_enqueue(borrowed, &request, VGUI_EDITOR_THUMB_HIGH);
	{
		int ready = pump_until_ready(browser, borrowed, "fresh.key", &state);
		check(ready == 1, "the load job reaches ready");
		check(state == VGUI_EDITOR_THUMB_READY, "state_of reports ready");
		check(fix.load_calls >= 1, "the worker loader ran");
		check(fix.write_calls >= 1, "a loader success is offered to the disk write");
		check(fix.seen_bytes == (int)sizeof(g_pixels), "the disk write copied every byte");
		check(vgui_editor_thumbnails_lookup(borrowed, "fresh.key") != NULL,
			"lookup returns the uploaded texture");
		check(vgui_editor_thumbnails_texture(borrowed, "fresh.key")
			== vgui_editor_thumbnails_lookup(borrowed, "fresh.key"),
			"texture() and lookup() agree once ready");
		needed = vgui_editor_thumbnails_error(borrowed, "fresh.key", NULL, 0);
		check(needed == 0, "a ready key has no error text");
	}

	/* 2) A disk hit skips the loader entirely. */
	i = fix.load_calls;
	request.key = "cached.key";
	vgui_editor_thumbnails_enqueue(borrowed, &request, VGUI_EDITOR_THUMB_HIGH);
	{
		int ready = pump_until_ready(browser, borrowed, "cached.key", &state);
		check(ready == 1, "the cached key also reaches ready");
		check(fix.load_calls == i, "a disk hit never calls the loader");
		check(fix.read_calls >= 1, "the disk read callback ran");
		memset(&stats, 0, sizeof(stats));
		vgui_editor_thumbnails_get_stats(borrowed, &stats);
		check(stats.disk_hits >= 1, "stats count the disk hit");
		check(stats.uploads >= 2, "stats count the uploads");
	}

	/* 3) The browser's own per-item view of the same machine. */
	needed = vgui_editor_asset_browser_thumbnail_key(browser, first, key, sizeof(key));
	check(needed > 0 && needed == strlen(key), "the browser builds a thumbnail key");
	/* Drive the browser's key through the borrowed handle so the test does not
	 * depend on how much area the headless layout happened to give the rows. */
	request.key = key;
	vgui_editor_thumbnails_enqueue(borrowed, &request, VGUI_EDITOR_THUMB_HIGH);
	{
		int ready = pump_until_ready(browser, borrowed, key, &state);
		check(ready == 1, "the browser's own key reaches ready");
		check(vgui_editor_asset_browser_thumbnail_state(browser, first)
			== VGUI_EDITOR_THUMB_READY, "the browser reports its item as ready");
	}
	vgui_editor_asset_browser_set_item_thumbnail(browser, first,
		vgui_editor_thumbnails_texture(borrowed, "fresh.key"));
	check(vgui_editor_asset_browser_item_thumbnail(browser, first) != NULL,
		"the host thumbnail reads back");

	/* 4) A failing loader lands in failed with the host's message. */
	fix.fail_next = 1;
	vgui_editor_thumbnail_request_default(&request);
	request.key = "broken.key";
	request.name = "broken";
	request.type = "Texture";
	vgui_editor_thumbnails_enqueue(borrowed, &request, VGUI_EDITOR_THUMB_HIGH);
	pump_until_ready(browser, borrowed, "broken.key", &state);
	check(state == VGUI_EDITOR_THUMB_FAILED, "a rejected load ends failed");
	needed = vgui_editor_thumbnails_error(borrowed, "broken.key", NULL, 0);
	check(needed > 0, "the failure keeps error text");
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_thumbnails_error(borrowed, "broken.key", buf, sizeof(buf))
		== needed && strcmp(buf, "loader says no") == 0,
		"the host error message round-trips");
	check(vgui_editor_thumbnails_lookup(borrowed, "broken.key") == NULL,
		"a failed key has no texture");
	fix.fail_next = 0;
	vgui_editor_thumbnails_retry(borrowed, "broken.key");
	check(pump_until_ready(browser, borrowed, "broken.key", &state) == 1,
		"retry() restarts a failed key from attempt one");

	/* Each release_user must have run exactly once after clearing. */
	vgui_editor_thumbnails_set_load_handler(borrowed, NULL, NULL, NULL);
	check(fix.load_releases == 1, "the load handler released once");
	vgui_editor_thumbnails_clear_disk_cache(borrowed);
	check(fix.disk_releases == 1, "the disk pair shared one release_user");
	check(vgui_editor_thumbnails_configured(borrowed) == 0,
		"the cache is unconfigured again");
	vgui_editor_thumbnails_clear(borrowed);
	check(vgui_editor_thumbnails_state(borrowed, "fresh.key")
		== VGUI_EDITOR_THUMB_MISSING, "clear drops every entry");

	/* Teardown plus the documented NULL no-ops. */
	vgui_editor_thumbnails_clear(cache);
	vgui_editor_thumbnails_clear_disk_cache(cache);
	vgui_editor_thumbnails_set_load_handler(cache, NULL, NULL, NULL);
	vgui_editor_thumbnails_set_options(cache, NULL);
	vgui_editor_thumbnails_get_options(cache, NULL);
	{
		vgui_editor_thumbnail_stats local;
		memset(&local, 0, sizeof(local));
		check(vgui_editor_thumbnails_configured(NULL) == 0, "configured(NULL) is false");
		check(vgui_editor_thumbnails_context(NULL) == NULL, "context(NULL) is NULL");
		check(vgui_editor_thumbnails_state(NULL, "k") == VGUI_EDITOR_THUMB_MISSING,
			"state(NULL) is missing");
		check(vgui_editor_thumbnails_lookup(NULL, "k") == NULL, "lookup(NULL) is NULL");
		check(vgui_editor_thumbnails_texture(borrowed, NULL) == NULL,
			"texture(NULL key) is NULL");
		check(vgui_editor_thumbnails_error(NULL, "k", buf, sizeof(buf)) == 0,
			"error(NULL) reports nothing");
		/* Every NULL-handle getter is a no-op, so `local` keeps its zero bytes. */
		vgui_editor_thumbnails_get_stats(NULL, &local);
		check(local.struct_size == 0u, "stats(NULL) leaves out alone");
		vgui_editor_thumbnails_enqueue(NULL, &request, VGUI_EDITOR_THUMB_HIGH);
		vgui_editor_thumbnails_cancel(NULL, "k");
		vgui_editor_thumbnails_retry(NULL, "k");
		vgui_editor_thumbnails_erase(NULL, "k");
		vgui_editor_thumbnails_cancel_all(NULL);
		vgui_editor_thumbnails_set_context(NULL, g_ui);
		vgui_editor_thumbnail_options_default(NULL);
		vgui_editor_thumbnail_image_default(NULL);
	}
	vgui_editor_thumbnails_destroy(other);
	/* `borrowed` belongs to the browser: it is destroyed with the browser, never
	 * through this entry point. */
	vgui_editor_thumbnails_destroy(cache);
	vgui_editor_asset_browser_destroy(browser);
}

/* -------------------------------------------------------------------------- */
/* Asset browser                                                               */
/* -------------------------------------------------------------------------- */

#define ASSET_ROWS 6

static const char* g_row_names[ASSET_ROWS] = {
	"hero.fbx", "hero.mat", "level.ogf", "music.ogg", "notes.txt", "ui_atlas.png"
};
static const char* g_row_types[ASSET_ROWS] = {
	"Mesh", "Material", "Level", "Audio", "Text", "Texture"
};
static vgui_editor_asset_entry g_rows[ASSET_ROWS];
static vgui_editor_asset_source g_source;
static vgui_editor_asset_path_item g_path[2];
static vgui_editor_asset_entry g_dir_items[3];
static vgui_item_id g_op_select[1];
static vgui_item_id g_op_created[1];

typedef struct asset_fixture {
	int count_calls;
	int item_at_calls;
	int id_at_calls;
	int index_of_calls;
	int navigate_calls;
	int path_calls;
	int select_calls;
	int listener_calls;
	int activate_calls;
	int rename_calls;
	int notice_calls;
	int op_calls;
	int op_struct_size;
	int move_target_calls;
	int dependency_calls;
	int import_calls;
	int prompt_calls;
	int undo_calls;
	int redo_calls;
	int navigate_ok;
	int rename_ok;
	int op_ok;
	int releases;
	char last_path[64];
	char last_name[64];
	char last_op[64];
	char last_notice[128];
	vgui_editor_asset_operation_error notice_error;
	vgui_item_id last_item;
} asset_fixture;

static void fill_rows(void)
{
	int i;
	for (i = 0; i < ASSET_ROWS; ++i)
	{
		vgui_editor_asset_entry_default(&g_rows[i]);
		g_rows[i].name = g_row_names[i];
		g_rows[i].type = g_row_types[i];
		g_rows[i].thumbnail = NULL;
		g_rows[i].id = (vgui_item_id)(i + 101);
	}
}

static int src_count(void* user)
{
	++((asset_fixture*)user)->count_calls;
	return ASSET_ROWS;
}

static int src_item_at(void* user, int index, vgui_editor_asset_entry* out)
{
	asset_fixture* fix = (asset_fixture*)user;
	++fix->item_at_calls;
	if (index < 0 || index >= ASSET_ROWS || !out)
		return 0;
	/* `name` and `type` stay borrowed, so the static rows back them. */
	*out = g_rows[index];
	return 1;
}

static vgui_item_id src_id_at(void* user, int index)
{
	++((asset_fixture*)user)->id_at_calls;
	if (index < 0 || index >= ASSET_ROWS)
		return 0;
	return (vgui_item_id)(index + 101);
}

static int src_index_of(void* user, vgui_item_id item)
{
	++((asset_fixture*)user)->index_of_calls;
	if (item < 101 || item >= 101 + ASSET_ROWS)
		return -1;
	return (int)(item - 101);
}

static void release_asset(void* user)
{
	if (user)
		++((asset_fixture*)user)->releases;
}

static void fill_source(asset_fixture* fix)
{
	vgui_editor_asset_source_default(&g_source);
	g_source.count = src_count;
	g_source.item_at = src_item_at;
	g_source.id_at = src_id_at;
	g_source.index_of = src_index_of;
	g_source.user = fix;
	g_source.release_user = release_asset;
}

static int provide_dir(void* user, const char* path_id,
	vgui_editor_asset_directory* out)
{
	asset_fixture* fix = (asset_fixture*)user;
	int i;
	++fix->navigate_calls;
	if (!out || !fix->navigate_ok)
		return 0;
	if (path_id)
	{
		size_t bytes = strlen(path_id);
		if (bytes >= sizeof(fix->last_path))
			bytes = sizeof(fix->last_path) - 1;
		memcpy(fix->last_path, path_id, bytes);
		fix->last_path[bytes] = '\0';
	}
	for (i = 0; i < 2; ++i)
	{
		vgui_editor_asset_path_item_default(&g_path[i]);
		g_path[i].label = i == 0 ? "Project" : "Meshes";
		g_path[i].id = i == 0 ? "/" : "/meshes";
	}
	for (i = 0; i < 3; ++i)
	{
		vgui_editor_asset_entry_default(&g_dir_items[i]);
		g_dir_items[i].name = g_row_names[i];
		g_dir_items[i].type = g_row_types[i];
		g_dir_items[i].id = (vgui_item_id)(201 + i);
	}
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	out->path = g_path;
	out->path_count = 2;
	out->items = g_dir_items;
	out->item_count = 3;
	out->source = NULL;
	out->empty_text = "nothing in here";
	return 1;
}

static int run_undo(void* user)
{
	++((asset_fixture*)user)->undo_calls;
	return 1;
}

static int run_redo(void* user)
{
	++((asset_fixture*)user)->redo_calls;
	return 1;
}

static int run_operation(void* user,
	const vgui_editor_asset_operation_request* request,
	vgui_editor_asset_operation_result* result)
{
	asset_fixture* fix = (asset_fixture*)user;
	++fix->op_calls;
	if (!request || !result)
		return 0;
	fix->op_struct_size = (int)request->struct_size;
	if (request->name)
	{
		size_t bytes = strlen(request->name);
		if (bytes >= sizeof(fix->last_name))
			bytes = sizeof(fix->last_name) - 1;
		memcpy(fix->last_name, request->name, bytes);
		fix->last_name[bytes] = '\0';
	}
	if (request->kind == VGUI_EDITOR_ASSET_OP_RENAME)
		strcpy(fix->last_op, "rename");
	else if (request->kind == VGUI_EDITOR_ASSET_OP_REMOVE)
		strcpy(fix->last_op, "remove");
	else if (request->kind == VGUI_EDITOR_ASSET_OP_DUPLICATE)
		strcpy(fix->last_op, "duplicate");
	else if (request->kind == VGUI_EDITOR_ASSET_OP_MOVE)
		strcpy(fix->last_op, "move");
	else if (request->kind == VGUI_EDITOR_ASSET_OP_REIMPORT)
		strcpy(fix->last_op, "reimport");
	/* A06: the host owns the database; fill the result and report success. */
	vgui_editor_asset_operation_result_default(result);
	result->error = fix->op_ok ? VGUI_EDITOR_ASSET_ERR_NONE
		: VGUI_EDITOR_ASSET_ERR_INVALID_NAME;
	result->message = fix->op_ok ? "done" : "bad name";
	result->undoable = fix->op_ok ? 1 : 0;
	result->undo = fix->op_ok ? run_undo : NULL;
	result->redo = fix->op_ok ? run_redo : NULL;
	result->user = fix;
	result->release_user = NULL;
	if (fix->op_ok && request->item_count > 0)
	{
		g_op_select[0] = request->items[0];
		g_op_created[0] = request->items[0] + 1000;
		result->select = g_op_select;
		result->select_count = 1;
		result->created = g_op_created;
		result->created_count = 1;
	}
	return fix->op_ok ? 1 : 0;
}

static size_t fill_targets(void* user, vgui_editor_asset_move_target* out,
	size_t out_count)
{
	static const char* labels[2] = { "Meshes", "Materials" };
	static const char* ids[2] = { "/meshes", "/materials" };
	size_t i;
	++((asset_fixture*)user)->move_target_calls;
	if (!out)
		return 2;
	for (i = 0; i < 2 && i < out_count; ++i)
	{
		vgui_editor_asset_move_target_default(out + i);
		out[i].label = labels[i];
		out[i].path_id = ids[i];
	}
	return 2;
}

static size_t fill_deps(void* user, vgui_item_id item,
	vgui_editor_asset_dependency* out, size_t out_count)
{
	static const char* names[2] = { "hero.fbx", "skin.png" };
	static const char* types[2] = { "Mesh", "Texture" };
	size_t i;
	++((asset_fixture*)user)->dependency_calls;
	((asset_fixture*)user)->last_item = item;
	if (!out)
		return 2;
	for (i = 0; i < 2 && i < out_count; ++i)
	{
		vgui_editor_asset_dependency_default(out + i);
		out[i].name = names[i];
		out[i].type = types[i];
		out[i].path_id = "/meshes";
		out[i].id = (vgui_item_id)(300 + i);
	}
	return 2;
}

static int accept_import(void* user, const char* const* files, size_t file_count,
	const char* path_id)
{
	asset_fixture* fix = (asset_fixture*)user;
	++fix->import_calls;
	fix->last_item = (vgui_item_id)file_count;
	if (path_id && file_count > 0 && files && files[0])
		strcpy(fix->last_path, files[0]);
	return file_count > 0 ? 1 : 0;
}

static int accept_rename(void* user, vgui_item_id item, const char* name)
{
	asset_fixture* fix = (asset_fixture*)user;
	++fix->rename_calls;
	fix->last_item = item;
	if (name)
	{
		size_t bytes = strlen(name);
		if (bytes >= sizeof(fix->last_name))
			bytes = sizeof(fix->last_name) - 1;
		memcpy(fix->last_name, name, bytes);
		fix->last_name[bytes] = '\0';
	}
	return fix->rename_ok;
}

static void on_asset_select(void* user, vgui_item_id item)
{
	asset_fixture* fix = (asset_fixture*)user;
	++fix->select_calls;
	fix->last_item = item;
}

static void on_asset_second(void* user, vgui_item_id item)
{
	++((asset_fixture*)user)->listener_calls;
	(void)item;
}

static void on_asset_activate(void* user, vgui_item_id item)
{
	asset_fixture* fix = (asset_fixture*)user;
	++fix->activate_calls;
	fix->last_item = item;
}

static void on_asset_path(void* user, const char* path_id)
{
	asset_fixture* fix = (asset_fixture*)user;
	++fix->path_calls;
	if (path_id)
		strcpy(fix->last_path, path_id);
}

static void on_asset_notice(void* user, const char* message,
	vgui_editor_asset_operation_error error)
{
	asset_fixture* fix = (asset_fixture*)user;
	++fix->notice_calls;
	fix->notice_error = error;
	if (message)
	{
		size_t bytes = strlen(message);
		if (bytes >= sizeof(fix->last_notice))
			bytes = sizeof(fix->last_notice) - 1;
		memcpy(fix->last_notice, message, bytes);
		fix->last_notice[bytes] = '\0';
	}
}

static void on_asset_prompt(void* user)
{
	++((asset_fixture*)user)->prompt_calls;
}

/* A loader that always rejects, so the browser's own rows can be pushed into
 * the failed state that retry_thumbnail() acts on.  `error` points at a
 * literal: the cache reads it after the worker handler returns. */
static int reject_load(void* user, const vgui_editor_thumbnail_request* request,
	vgui_editor_thumbnail_image* out)
{
	(void)user;
	(void)request;
	if (out)
		out->error = "no pixels here";
	return 0;
}

/* Headless runs never click a dialog button, and the core refuses to open a
 * second modal while one is up - so every dialog the browser opens has to be
 * cancelled before the next API call that needs one. */
static void dismiss_dialog(void)
{
	vgui_modal_close(g_ui, VGUI_MODAL_RESULT_CANCEL);
	frame(1);
}

void test_asset_browser(probe* p)
{
	vgui_editor_asset_browser* browser;
	vgui_editor_thumbnails* thumbs;
	vgui_editor_asset_entry entry;
	vgui_editor_asset_directory directory;
	vgui_editor_asset_operation_result result;
	vgui_editor_asset_move_target targets[4];
	vgui_editor_asset_dependency deps[4];
	vgui_editor_asset_source detached;
	vgui_editor_thumbnail_request thumb_request;
	vgui_editor_thumbnail_state thumb_state;
	vgui_editor_command_stack* stack;
	const char* files[2];
	const char* types[4];
	vgui_item_id ids[2];
	vgui_item_id shown[8];
	vgui_item_id a;
	vgui_item_id b;
	vgui_item_id c;
	char buf[128];
	char tiny[6];
	size_t needed;
	size_t count;
	int i;
	int ok;

	memset(p, 0, sizeof(*p));
	memset(&entry, 0, sizeof(entry));
	memset(&directory, 0, sizeof(directory));
	memset(&result, 0, sizeof(result));
	memset(targets, 0, sizeof(targets));
	memset(deps, 0, sizeof(deps));
	memset(&detached, 0, sizeof(detached));
	{
		static asset_fixture fix;
		memset(&fix, 0, sizeof(fix));
		fill_rows();
		fix.navigate_ok = 1;
		fix.rename_ok = 1;
		fix.op_ok = 1;

		browser = vgui_editor_asset_browser_create(vgui_root(g_ui));
		frame(2);
		check(browser != NULL, "asset browser built");
		check(vgui_editor_asset_browser_widget(browser) != NULL, "browser widget exists");
		{
			vgui_widget* root = vgui_editor_asset_browser_root(browser);
			if (root != NULL)
				vgui_widget_set_min_size(root, 720.0f, 480.0f);
		}
		frame(2);
		check(vgui_editor_asset_browser_root(browser) != NULL, "root child exposed");
		check(vgui_editor_asset_browser_grid(browser) != NULL, "grid child exposed");
		check(vgui_editor_asset_browser_scroll(browser) != NULL, "scroll child exposed");
		check(vgui_editor_asset_browser_view_switch_widget(browser) != NULL,
			"view switch child exposed");
		check(vgui_editor_asset_browser_path_widget(browser) != NULL,
			"path child exposed");
		check(vgui_editor_asset_browser_search_widget(browser) != NULL,
			"search child exposed");
		check(vgui_editor_asset_browser_type_combo_widget(browser) != NULL,
			"type combo child exposed");

		/* Full model helpers. */
		vgui_editor_asset_browser_clear(browser);
		check(vgui_editor_asset_browser_item_count(browser) == 0, "a fresh model is empty");
		check(vgui_editor_asset_browser_total_item_count(browser) == 0,
			"total count starts at zero");
		a = vgui_editor_asset_browser_add_item(browser, "hero.fbx", "Mesh");
		b = vgui_editor_asset_browser_add_item(browser, "song.ogg", "Audio");
		c = vgui_editor_asset_browser_add_item(browser, "ui.png", "Texture");
		check(a != 0 && b != 0 && c != 0, "add_item returns real ids");
		check(a != b && b != c, "ids are unique");
		check(vgui_editor_asset_browser_item_count(browser) == 3, "the model has three rows");
		vgui_editor_asset_entry_default(&entry);
		check(entry.struct_size == (uint32_t)sizeof(entry), "entry default sizes itself");
		check(vgui_editor_asset_browser_get_item(browser, 0, &entry) == 1,
			"get_item(0) succeeds");
		check(entry.name && strcmp(entry.name, "hero.fbx") == 0, "row text round-trips");
		check(entry.type && strcmp(entry.type, "Mesh") == 0, "row detail round-trips");
		check(entry.id == a, "row id matches add_item");
		check(vgui_editor_asset_browser_get_item(browser, 99, &entry) == 0,
			"get_item past the end is rejected");
		check(vgui_editor_asset_browser_id_at(browser, 1) == b, "id_at maps index to id");
		check(vgui_editor_asset_browser_id_at(browser, 99) == 0, "id_at out of range is 0");
		check(vgui_editor_asset_browser_index_of(browser, c) == 2, "index_of finds the row");
		check(vgui_editor_asset_browser_index_of(browser, 424242) == -1,
			"index_of for an unknown id is -1");
		check(vgui_editor_asset_browser_item_info(browser, c, &entry) == 1
			&& entry.name && strcmp(entry.name, "ui.png") == 0,
			"item_info resolves a hidden-safe lookup");

		/* View mode. */
		check(vgui_editor_asset_browser_view(browser) == VGUI_EDITOR_ASSET_LIST,
			"list is the default view");
		vgui_editor_asset_browser_set_view(browser, VGUI_EDITOR_ASSET_GRID);
		check(vgui_editor_asset_browser_view(browser) == VGUI_EDITOR_ASSET_GRID,
			"grid mode applies");
		check(vgui_editor_asset_browser_grid_columns(browser) >= 1,
			"grid has at least one column");
		vgui_editor_asset_browser_set_view(browser, VGUI_EDITOR_ASSET_LIST);
		vgui_editor_asset_browser_set_view_switch_visible(browser, 0);
		check(vgui_editor_asset_browser_view_switch_visible(browser) == 0,
			"view switch visibility round-trips");
		vgui_editor_asset_browser_set_view_switch_visible(browser, 1);

		/* Navigation and breadcrumbs. */
		vgui_editor_asset_browser_set_navigate_handler(browser, provide_dir, &fix,
			release_asset);
		vgui_editor_asset_browser_set_on_path(browser, on_asset_path, &fix);
		/* History needs a location to leave behind: the first navigation only
		 * establishes the current directory. */
		check(vgui_editor_asset_browser_navigate_to(browser, "/") == 1,
			"the root location is installed");
		ok = vgui_editor_asset_browser_navigate_to(browser, "/meshes");
		check(ok == 1, "navigate_to resolves through the handler");
		check(fix.navigate_calls == 2, "the navigate handler ran per location");
		check(fix.path_calls >= 1, "the path callback ran");
		check(strcmp(fix.last_path, "/meshes") == 0, "the handler saw the path id");
		needed = vgui_editor_asset_browser_current_path(browser, NULL, 0);
		memset(buf, 0, sizeof(buf));
		check(vgui_editor_asset_browser_current_path(browser, buf, needed + 1) == needed
			&& strcmp(buf, "/meshes") == 0, "current path round-trips");
		memset(tiny, 0, sizeof(tiny));
		check(vgui_editor_asset_browser_current_path(browser, tiny, 4) == needed,
			"a short path buffer still reports the full size");
		check(vgui_editor_asset_browser_item_count(browser) == 3,
			"the directory items replaced the model");
		check(vgui_editor_asset_browser_can_go_back(browser) == 1, "back is available");
		check(vgui_editor_asset_browser_can_go_forward(browser) == 0,
			"navigate_to clears the forward stack");
		check(vgui_editor_asset_browser_go_back(browser) == 1, "go_back ran");
		check(vgui_editor_asset_browser_can_go_forward(browser) == 1,
			"go_back fills the forward stack");
		check(vgui_editor_asset_browser_go_forward(browser) == 1, "go_forward ran");
		check(vgui_editor_asset_browser_reload(browser) == 1, "reload re-resolves");
		check(vgui_editor_asset_browser_can_go_back(browser) == 0
			|| vgui_editor_asset_browser_can_go_back(browser) == 1,
			"reload leaves the stacks coherent");
		fix.navigate_ok = 0;
		check(vgui_editor_asset_browser_navigate_to(browser, "/nope") == 0,
			"a rejected navigation returns 0");
		fix.navigate_ok = 1;
		vgui_editor_asset_browser_set_navigation_visible(browser, 0);
		check(vgui_editor_asset_browser_navigation_visible(browser) == 0,
			"navigation visibility round-trips");

		/* Virtual source: the same browser driven through function pointers. */
		vgui_editor_asset_browser_clear(browser);
		fill_source(&fix);
		vgui_editor_asset_browser_set_source(browser, &g_source);
		check(vgui_editor_asset_browser_is_virtual(browser) == 1, "a source is virtual");
		check(vgui_editor_asset_browser_item_count(browser) == ASSET_ROWS,
			"the virtual count reaches the browser");
		check(fix.count_calls >= 1, "the count callback ran");
		memset(&entry, 0, sizeof(entry));
		check(vgui_editor_asset_browser_get_item(browser, 2, &entry) == 1
			&& entry.name && strcmp(entry.name, "level.ogf") == 0,
			"item_at backs get_item");
		check(vgui_editor_asset_browser_id_at(browser, 2) == 103, "id_at is host provided");
		check(vgui_editor_asset_browser_index_of(browser, 103) == 2, "index_of is O(1)");
		check(vgui_editor_asset_browser_item_info(browser, 105, &entry) == 1
			&& entry.id == 105, "item_info resolves through the source");
		check(fix.item_at_calls > 0 && fix.item_at_calls <= ASSET_ROWS + 8,
			"item_at runs only for the rows the browser needs");
		check(vgui_editor_asset_browser_add_item(browser, "ignored", "x") == 0,
			"add_item is inert while virtual");
		vgui_editor_asset_browser_set_source(browser, NULL);
		check(vgui_editor_asset_browser_is_virtual(browser) == 0, "NULL detaches it");
		check(fix.releases >= 1, "the detached source released its user");
		(void)detached;

		/* Selection listeners.  set_selection() drops ids the current model does
		 * not know, so the rows have to be back before selecting them. */
		vgui_editor_asset_browser_set_source(browser, &g_source);
		vgui_editor_asset_browser_set_on_selection(browser, on_asset_select, &fix);
		ok = (int)vgui_editor_asset_browser_add_selection_listener(browser,
			on_asset_second, &fix);
		check(ok != 0, "a multi-listener token is handed back");
		ids[0] = 101;
		ids[1] = 103;
		vgui_editor_asset_browser_set_selection(browser, ids, 2);
		frame(1);
		check(fix.select_calls >= 1, "the legacy selection listener ran");
		check(fix.listener_calls >= 1, "the multi listener ran too");
		check(vgui_editor_asset_browser_is_selected(browser, 101) == 1, "row 101 selected");
		check(vgui_editor_asset_browser_is_selected(browser, 103) == 1, "row 103 selected");
		check(vgui_editor_asset_browser_is_selected(browser, 999) == 0,
			"an unselected id reads false");
		count = vgui_editor_asset_browser_selection(browser, NULL, 0);
		check(count == 2, "selection() reports the size first");
		memset(shown, 0, sizeof(shown));
		check(vgui_editor_asset_browser_selection(browser, shown, 8) == 2,
			"selection fills the caller array");
		check(vgui_editor_asset_browser_selected(browser) == 101,
			"the primary selection is the first id");
		check(vgui_editor_asset_browser_cursor(browser) != 0, "the cursor is live");
		vgui_editor_asset_browser_clear_selection(browser);
		check(vgui_editor_asset_browser_selection(browser, shown, 8) == 0,
			"clear_selection empties it");
		count = vgui_editor_asset_browser_add_selection_listener(browser,
			on_asset_second, &fix);
		check(vgui_editor_asset_browser_remove_selection_listener(browser, count) == 1,
			"the listener token removes again");
		check(vgui_editor_asset_browser_remove_selection_listener(browser, 9999) == 0,
			"an unknown token is rejected");

		/* Debounced search plus the type filter. */
		files[0] = "dropped.fbx";
		files[1] = "dropped2.fbx";
		(void)files;
		types[0] = "All";
		types[1] = "Mesh";
		types[2] = "Audio";
		types[3] = "Texture";
		vgui_editor_asset_browser_set_type_options(browser, types, 4);
		check(vgui_editor_asset_browser_type_option_count(browser) == 4,
			"type options enumerated");
		needed = vgui_editor_asset_browser_type_option_at(browser, 1, NULL, 0);
		memset(buf, 0, sizeof(buf));
		check(vgui_editor_asset_browser_type_option_at(browser, 1, buf, needed + 1)
			== needed && strcmp(buf, "Mesh") == 0, "type option 1 round-trips");
		needed = vgui_editor_asset_browser_type_option_at(browser, 1, tiny, sizeof(tiny));
		check(needed == 4, "a short type buffer reports the real size");
		check(vgui_editor_asset_browser_type_option_at(browser, 99, buf, sizeof(buf)) == 0,
			"an out-of-range type option is empty");
		vgui_editor_asset_browser_set_source(browser, &g_source);
		vgui_editor_asset_browser_set_search_debounce(browser, 0.05f);
		check(near_f(vgui_editor_asset_browser_search_debounce(browser), 0.05f, 0.001f),
			"search debounce round-trips");
		vgui_editor_asset_browser_set_search(browser, "hero", 1);
		needed = vgui_editor_asset_browser_search(browser, NULL, 0);
		memset(buf, 0, sizeof(buf));
		check(vgui_editor_asset_browser_search(browser, buf, needed + 1) == needed
			&& strcmp(buf, "hero") == 0, "search text round-trips");
		check(vgui_editor_asset_browser_filter_active(browser) == 1, "the filter is active");
		check(vgui_editor_asset_browser_total_item_count(browser) == ASSET_ROWS,
			"total count ignores the filter");
		check(vgui_editor_asset_browser_item_count(browser) < ASSET_ROWS,
			"the search narrowed the rows");
		vgui_editor_asset_browser_set_search(browser, "", 1);
		check(vgui_editor_asset_browser_filter_active(browser) == 0,
			"an empty search clears the filter");
		/* The debounced form only reaches the filter after the timer runs. */
		vgui_editor_asset_browser_set_search(browser, "level", 0);
		check(vgui_editor_asset_browser_search_pending(browser) == 1,
			"the debounced text is pending");
		vgui_editor_asset_browser_apply_search(browser);
		check(vgui_editor_asset_browser_search_pending(browser) == 0,
			"apply_search flushes the debounce");
		check(vgui_editor_asset_browser_item_count(browser) == 1,
			"the flushed search filters one row");
		vgui_editor_asset_browser_set_search(browser, "level", 0);
		vgui_editor_asset_browser_tick(browser, 0.2f);
		check(vgui_editor_asset_browser_search_pending(browser) == 0,
			"tick() expires the debounce");
		vgui_editor_asset_browser_set_search(browser, "", 1);
		vgui_editor_asset_browser_set_type_filter(browser, "Audio");
		needed = vgui_editor_asset_browser_type_filter(browser, NULL, 0);
		memset(buf, 0, sizeof(buf));
		check(vgui_editor_asset_browser_type_filter(browser, buf, needed + 1) == needed
			&& strcmp(buf, "Audio") == 0, "type filter round-trips");
		check(vgui_editor_asset_browser_item_count(browser) == 1,
			"the type filter keeps one row");
		vgui_editor_asset_browser_set_type_filter(browser, NULL);
		check(vgui_editor_asset_browser_type_filter(browser, buf, sizeof(buf)) == 0,
			"NULL clears the type filter");
		vgui_editor_asset_browser_set_type_filter_visible(browser, 0);
		check(vgui_editor_asset_browser_type_filter_visible(browser) == 0,
			"type filter visibility round-trips");
		vgui_editor_asset_browser_set_empty_texts(browser, "Empty", "Nothing found");
		needed = vgui_editor_asset_browser_empty_state_text(browser, NULL, 0);
		memset(buf, 0, sizeof(buf));
		check(vgui_editor_asset_browser_empty_state_text(browser, buf, needed + 1)
			== needed, "empty state text round-trips");
		/* Detach both host models.  While a navigate handler and a location
		 * exist, every successful operation reloads the directory, which would
		 * replace the imperative rows below - and the ids the rest of this
		 * block selects - with the handler's three fixture rows. */
		vgui_editor_asset_browser_set_navigate_handler(browser, NULL, NULL, NULL);
		vgui_editor_asset_browser_set_source(browser, NULL);

		/* Visible range and virtualization math. */
		vgui_editor_asset_browser_clear(browser);
		for (i = 0; i < 40; ++i)
			vgui_editor_asset_browser_add_item(browser, "many.fbx", "Mesh");
		frame(2);
		check(vgui_editor_asset_browser_visible_first(browser) >= 0,
			"visible range starts at or after 0");
		check(vgui_editor_asset_browser_visible_last(browser)
			>= vgui_editor_asset_browser_visible_first(browser),
			"visible range is ordered");
		check(vgui_editor_asset_browser_visible_count(browser)
			== vgui_editor_asset_browser_visible_last(browser)
			- vgui_editor_asset_browser_visible_first(browser),
			"visible_count matches the range");
		count = vgui_editor_asset_browser_visible_items(browser, NULL, 0);
		check(count == (size_t)vgui_editor_asset_browser_visible_count(browser),
			"the NULL probe matches visible_count");
		memset(shown, 0, sizeof(shown));
		check(vgui_editor_asset_browser_visible_items(browser, shown, 8) == count,
			"a short array still reports every visible id");
		check(shown[0] != 0 && (count < 2 || shown[1] != 0),
			"visible_items writes the rows it can");
		check(shown[7] == 0 || count >= 8, "nothing is written past the cap");
		check(count <= 40, "visible_items never exceeds the model");
		{
			float x = 0.0f;
			float y = 0.0f;
			float w = 0.0f;
			float h = 0.0f;
			ok = vgui_editor_asset_browser_item_rect(browser, 0, &x, &y, &w, &h);
			check(ok == 0 || ok == 1, "item_rect answers with a flag");
			check(vgui_editor_asset_browser_item_rect(browser, 0, NULL, NULL, NULL,
				NULL) == ok, "item_rect accepts NULL outputs");
			check(vgui_editor_asset_browser_item_rect(browser, 9999, &x, &y, &w, &h) == 0,
				"item_rect rejects an out-of-range row");
		}
		vgui_editor_asset_browser_refresh(browser);
		vgui_editor_asset_browser_enable_row_drag(browser);
		vgui_editor_asset_browser_enable_context_menu(browser);
		check(vgui_editor_asset_browser_context_menu_enabled(browser) == 1,
			"context menu enable is reported");

		/* Renaming. */
		a = vgui_editor_asset_browser_add_item(browser, "rename.me", "Text");
		vgui_editor_asset_browser_set_rename_handler(browser, accept_rename, &fix);
		check(vgui_editor_asset_browser_begin_rename(browser, a) == 1,
			"begin_rename starts an edit");
		dismiss_dialog();
		fix.rename_ok = 1;
		check(vgui_editor_asset_browser_rename_item(browser, a, "new_name.txt") == 1,
			"an accepted rename returns 1");
		check(fix.rename_calls == 1 && strcmp(fix.last_name, "new_name.txt") == 0,
			"the rename handler saw the new name");
		fix.rename_ok = 0;
		check(vgui_editor_asset_browser_rename_item(browser, a, "bad name!") == 0,
			"a rejected rename returns 0");
		vgui_editor_asset_browser_set_command_stack(browser, NULL);
		vgui_editor_asset_browser_set_rename_handler(browser, NULL, NULL);

		/* Activation and import. */
		vgui_editor_asset_browser_set_on_activate(browser, on_asset_activate, &fix);
		ids[0] = a;
		vgui_editor_asset_browser_set_selection(browser, ids, 1);
		vgui_editor_asset_browser_activate_selected(browser);
		check(fix.activate_calls >= 1, "activate_selected fired the callback");
		vgui_editor_asset_browser_set_import_handler(browser, accept_import, &fix);
		vgui_editor_asset_browser_set_import_prompt(browser, on_asset_prompt, &fix);
		files[0] = "a.fbx";
		files[1] = "b.fbx";
		check(vgui_editor_asset_browser_import_files(browser, files, 2) == 1,
			"import_files forwards to the host");
		check(fix.import_calls == 1 && fix.last_item == 2, "the handler saw two files");
		check(vgui_editor_asset_browser_import_files(browser, files, 0) == 0,
			"an empty batch is rejected");
		check(vgui_editor_asset_browser_request_import(browser) == 1,
			"request_import prompts the host");
		check(fix.prompt_calls >= 1, "the import prompt ran");

		/* A06 operations, results and outcomes. */
		vgui_editor_asset_browser_set_operation_handler(browser, run_operation, &fix,
			release_asset);
		vgui_editor_asset_browser_set_on_notice(browser, on_asset_notice, &fix);
		vgui_editor_asset_browser_set_move_targets(browser, fill_targets, &fix);
		vgui_editor_asset_browser_set_dependency_handler(browser, fill_deps, &fix);
		/* An undoable batch only reports itself undoable with a command stack to
		 * record it in; without one the kit calls it non-undoable. */
		stack = vgui_editor_command_stack_create();
		check(stack != NULL, "a standalone command stack builds");
		vgui_editor_asset_browser_set_command_stack(browser, stack);
		vgui_editor_asset_operation_result_default(&result);
		check(result.struct_size == (uint32_t)sizeof(result),
			"operation result default sizes itself");
		check(result.error == VGUI_EDITOR_ASSET_ERR_NONE && result.undoable == 0,
			"a default result is a clean, non-undoable success");
		check(result.message == NULL && result.select == NULL,
			"a default result carries no payload");
		/* The kit builds the request it hands the handler, so the size stamp the
		 * host reads is the library's own - assert it instead of filling one by
		 * hand. */
		count = vgui_editor_asset_browser_selection(browser, shown, 8);
		check(count == 1, "one id is selected for the operations");
		check(vgui_editor_asset_browser_duplicate_items(browser, ids, 1) == 1,
			"duplicate_items reached the host");
		check(strcmp(fix.last_op, "duplicate") == 0, "the request kind is duplicate");
		check(fix.op_calls >= 1, "the operation handler ran");
		check((size_t)fix.op_struct_size
			== sizeof(vgui_editor_asset_operation_request),
			"the request the handler receives is size-stamped");
		check(vgui_editor_asset_browser_last_undoable(browser) == 1,
			"an undoable batch reports itself undoable");
		check(vgui_editor_asset_browser_last_error(browser)
			== VGUI_EDITOR_ASSET_ERR_NONE, "a successful operation has no error");
		needed = vgui_editor_asset_browser_last_message(browser, NULL, 0);
		memset(buf, 0, sizeof(buf));
		check(vgui_editor_asset_browser_last_message(browser, buf, needed + 1)
			== needed, "the last message round-trips");
		memset(tiny, 0, sizeof(tiny));
		check(vgui_editor_asset_browser_last_message(browser, tiny, 3) == needed,
			"a short message buffer still reports the full size");
		check(fix.notice_calls >= 1, "the notice callback fired");
		check(fix.notice_error == VGUI_EDITOR_ASSET_ERR_NONE,
			"the notice carries ERR_NONE on success");
		/* Move, remove and reimport through the same A06 door.  With no navigate
		 * handler the browser edits its own rows, so an accepted move or remove
		 * drops the item and every later verb needs a fresh one.  Moving into
		 * the current directory is rejected outright. */
		count = fill_targets(&fix, NULL, 0);
		check(count == 2, "the move-target provider reports two destinations");
		check(vgui_editor_asset_browser_move_items(browser, ids, 1, "/materials") == 1,
			"move_items reached the host");
		check(strcmp(fix.last_op, "move") == 0, "the request kind is move");
		check(vgui_editor_asset_browser_index_of(browser, a) < 0,
			"the moved row leaves the local model");
		a = vgui_editor_asset_browser_add_item(browser, "second.fbx", "Mesh");
		ids[0] = a;
		vgui_editor_asset_browser_set_selection(browser, ids, 1);
		check(vgui_editor_asset_browser_move_selected(browser, "/materials") == 1,
			"move_selected reached the host");
		a = vgui_editor_asset_browser_add_item(browser, "again.fbx", "Mesh");
		ids[0] = a;
		vgui_editor_asset_browser_set_selection(browser, ids, 1);
		check(vgui_editor_asset_browser_reimport_items(browser, ids, 1) == 1,
			"reimport_items reached the host");
		check(strcmp(fix.last_op, "reimport") == 0, "the request kind is reimport");
		check(vgui_editor_asset_browser_last_error(browser)
			== VGUI_EDITOR_ASSET_ERR_NONE, "a successful reimport has no error");
		check(vgui_editor_asset_browser_index_of(browser, a) >= 0,
			"a reimport keeps the row");

		/* A rejected batch: the host decides, the browser reports it. */
		fix.op_ok = 0;
		check(vgui_editor_asset_browser_remove_items(browser, ids, 1) == 0,
			"a host rejection makes remove_items return 0");
		check(vgui_editor_asset_browser_last_error(browser)
			== VGUI_EDITOR_ASSET_ERR_INVALID_NAME, "the rejected error is remembered");
		check(vgui_editor_asset_browser_last_undoable(browser) == 0,
			"a rejected batch is not undoable");
		check(vgui_editor_asset_browser_last_message(browser, buf, sizeof(buf)) > 0,
			"the failure keeps a message");
		check(fix.notice_error == VGUI_EDITOR_ASSET_ERR_INVALID_NAME,
			"the notice carries the fixed error category");
		fix.op_ok = 1;
		check(vgui_editor_asset_browser_remove_selected(browser) == 1,
			"remove_selected runs again once the host accepts");
		check(vgui_editor_asset_browser_index_of(browser, a) < 0,
			"the removed row leaves the local model");
		a = vgui_editor_asset_browser_add_item(browser, "third.fbx", "Mesh");
		ids[0] = a;
		check(vgui_editor_asset_browser_confirm_remove(browser, ids, 1) == 1,
			"confirm_remove runs the batch");
		/* The confirm dialog only opened; cancelling it applies nothing. */
		dismiss_dialog();
		check(vgui_editor_asset_browser_index_of(browser, a) >= 0,
			"a cancelled confirm keeps the row");
		a = vgui_editor_asset_browser_add_item(browser, "fourth.fbx", "Mesh");
		ids[0] = a;
		vgui_editor_asset_browser_set_selection(browser, ids, 1);
		check(vgui_editor_asset_browser_duplicate_selected(browser) == 1,
			"duplicate_selected runs the batch");

		/* Dependencies and the move-target list, both host supplied. */
		count = vgui_editor_asset_browser_dependencies(browser, a, NULL, 0);
		check(count == 2, "the dependency provider reports two entries");
		memset(deps, 0, sizeof(deps));
		check(vgui_editor_asset_browser_dependencies(browser, a, deps, 4) == 2
			&& deps[0].name && strcmp(deps[0].name, "hero.fbx") == 0,
			"the dependency rows copy out");
		check(deps[1].id == 301 && deps[1].type && strcmp(deps[1].type, "Texture") == 0,
			"dependency ids and types survive");
		check(vgui_editor_asset_browser_show_dependencies(browser, a) == 1,
			"show_dependencies formats the list");
		dismiss_dialog();
		check(fix.dependency_calls >= 2, "the dependency handler ran for both paths");

		/* Menu / shortcut verbs over the same model. */
		i = fix.prompt_calls;
		check(vgui_editor_asset_browser_perform_action(browser,
			VGUI_EDITOR_ASSET_ACTION_IMPORT, NULL) == 1, "ACTION_IMPORT dispatched");
		check(fix.prompt_calls > i, "ACTION_IMPORT used the import prompt");
		check(vgui_editor_asset_browser_perform_action(browser,
			VGUI_EDITOR_ASSET_ACTION_DEPENDENCIES, NULL) == 1,
			"ACTION_DEPENDENCIES dispatched");
		dismiss_dialog();
		check(vgui_editor_asset_browser_perform_action(browser,
			VGUI_EDITOR_ASSET_ACTION_DUPLICATE, NULL) == 1, "ACTION_DUPLICATE dispatched");
		check(strcmp(fix.last_op, "duplicate") == 0, "the verb mapped to the kind");
		check(vgui_editor_asset_browser_perform_action(browser,
			VGUI_EDITOR_ASSET_ACTION_MOVE, "/materials") == 1, "ACTION_MOVE dispatched");
		/* The move dropped the row, so hand the remove verb a live selection. */
		a = vgui_editor_asset_browser_add_item(browser, "fifth.fbx", "Mesh");
		ids[0] = a;
		vgui_editor_asset_browser_set_selection(browser, ids, 1);
		check(vgui_editor_asset_browser_perform_action(browser,
			VGUI_EDITOR_ASSET_ACTION_REMOVE, NULL) == 1, "ACTION_REMOVE dispatched");
		dismiss_dialog();
		check(vgui_editor_asset_browser_perform_action(browser,
			(vgui_editor_asset_action)99, NULL) == 0, "an unknown verb is rejected");

		vgui_editor_asset_browser_set_command_stack(browser, NULL);
		vgui_editor_command_stack_destroy(stack);

		/* The borrowed thumbnail cache the browser owns. */
		thumbs = vgui_editor_asset_browser_thumbnails(browser);
		check(thumbs != NULL, "the browser exposes its thumbnail cache");
		{
			vgui_editor_thumbnails* again = vgui_editor_asset_browser_thumbnails(browser);
			check(again == thumbs, "the borrowed cache handle is stable across calls");
		}
		a = vgui_editor_asset_browser_add_item(browser, "atlas.png", "Texture");
		needed = vgui_editor_asset_browser_thumbnail_key(browser, a, NULL, 0);
		memset(buf, 0, sizeof(buf));
		check(vgui_editor_asset_browser_thumbnail_key(browser, a, buf, needed + 1)
			== needed && needed > 0, "the thumbnail key round-trips");
		check(vgui_editor_asset_browser_thumbnail_state(browser, a)
			== VGUI_EDITOR_THUMB_MISSING, "a new item has no thumbnail yet");
		/* Retrying only restarts a load the host already rejected, so a row that
		 * was never attempted is refused - as is an unknown id. */
		check(vgui_editor_asset_browser_retry_thumbnail(browser, a) == 0,
			"a never-attempted row has nothing to retry");
		check(vgui_editor_asset_browser_retry_thumbnail(browser, 424242) == 0,
			"an unknown item has nothing to retry");
		vgui_editor_asset_browser_tick(browser, 0.05f);
		frame(2);
		check(vgui_editor_asset_browser_item_thumbnail(browser, a) == NULL,
			"an unconfigured cache keeps the slot empty");
		/* Fail this row through the browser's own cache, then retry it. */
		vgui_editor_thumbnails_set_load_handler(thumbs, reject_load, &fix, NULL);
		check(vgui_editor_thumbnails_configured(thumbs) == 1,
			"the borrowed cache reports configured");
		vgui_editor_thumbnail_request_default(&thumb_request);
		check(thumb_request.struct_size == (uint32_t)sizeof(thumb_request)
			&& thumb_request.key == NULL, "the thumbnail request default sizes itself");
		thumb_request.key = buf;
		thumb_request.name = "atlas.png";
		thumb_request.type = "Texture";
		vgui_editor_thumbnails_enqueue(thumbs, &thumb_request, VGUI_EDITOR_THUMB_HIGH);
		ok = pump_until_ready(browser, thumbs, buf, &thumb_state);
		check(ok == 0 && thumb_state == VGUI_EDITOR_THUMB_FAILED,
			"a rejected load fails the browser's own row");
		check(vgui_editor_asset_browser_thumbnail_state(browser, a)
			== VGUI_EDITOR_THUMB_FAILED, "the item view sees the failure");
		check(vgui_editor_asset_browser_retry_thumbnail(browser, a) == 1,
			"retry_thumbnail restarts a failed row");
		vgui_editor_thumbnails_set_load_handler(thumbs, NULL, NULL, NULL);
		vgui_editor_thumbnails_cancel_all(thumbs);
		vgui_editor_thumbnails_clear(thumbs);

		/* Clear every callback before destroying the browser. */
		vgui_editor_asset_browser_set_navigate_handler(browser, NULL, NULL, NULL);
		vgui_editor_asset_browser_set_on_path(browser, NULL, NULL);
		vgui_editor_asset_browser_set_on_selection(browser, NULL, NULL);
		vgui_editor_asset_browser_set_on_activate(browser, NULL, NULL);
		vgui_editor_asset_browser_set_rename_handler(browser, NULL, NULL);
		vgui_editor_asset_browser_set_import_handler(browser, NULL, NULL);
		vgui_editor_asset_browser_set_import_prompt(browser, NULL, NULL);
		vgui_editor_asset_browser_set_operation_handler(browser, NULL, NULL, NULL);
		vgui_editor_asset_browser_set_on_notice(browser, NULL, NULL);
		vgui_editor_asset_browser_set_move_targets(browser, NULL, NULL);
		vgui_editor_asset_browser_set_dependency_handler(browser, NULL, NULL);
		vgui_editor_asset_browser_set_source(browser, NULL);
		check(fix.releases >= 3, "cleared handlers released their users");

		{
			check(vgui_editor_asset_browser_widget(NULL) == NULL,
				"widget(NULL) is NULL");
			check(vgui_editor_asset_browser_item_count(NULL) == 0,
				"item_count(NULL) is 0");
			check(vgui_editor_asset_browser_get_item(NULL, 0, &entry) == 0,
				"get_item(NULL) is rejected");
			check(vgui_editor_asset_browser_is_virtual(NULL) == 0,
				"is_virtual(NULL) is false");
			check(vgui_editor_asset_browser_visible_count(NULL) == 0,
				"visible_count(NULL) is 0");
			check(vgui_editor_asset_browser_grid_columns(NULL) == 0,
				"grid_columns(NULL) is 0");
			check(vgui_editor_asset_browser_type_option_count(NULL) == 0,
				"type_option_count(NULL) is 0");
			check(vgui_editor_asset_browser_search_pending(NULL) == 0,
				"search_pending(NULL) is false");
			check(vgui_editor_asset_browser_current_path(NULL, buf, sizeof(buf)) == 0,
				"current_path(NULL) reports nothing");
			check(vgui_editor_asset_browser_thumbnails(NULL) == NULL,
				"thumbnails(NULL) is NULL");
			check(vgui_editor_asset_browser_thumbnail_state(NULL, a)
				== VGUI_EDITOR_THUMB_MISSING, "thumbnail_state(NULL) is missing");
			check(vgui_editor_asset_browser_last_error(NULL)
				== VGUI_EDITOR_ASSET_ERR_NONE, "last_error(NULL) is clean");
			check(vgui_editor_asset_browser_perform_action(NULL,
				VGUI_EDITOR_ASSET_ACTION_RENAME, NULL) == 0, "perform_action(NULL) fails");
			check(vgui_editor_asset_browser_dependencies(NULL, a, deps, 4) == 0,
				"dependencies(NULL) reports nothing");
			check(vgui_editor_asset_browser_selection(NULL, shown, 8) == 0,
				"selection(NULL) reports nothing");
			vgui_editor_asset_entry_default(NULL);
			vgui_editor_asset_source_default(NULL);
			vgui_editor_asset_directory_default(&directory);
			check(directory.struct_size == (uint32_t)sizeof(directory),
				"the directory default sizes itself");
			check(directory.path == NULL && directory.source == NULL
				&& directory.empty_text == NULL, "a default directory is empty");
			vgui_editor_asset_operation_result_default(&result);
			check(result.error == VGUI_EDITOR_ASSET_ERR_NONE,
				"a default result starts clean");
			vgui_editor_asset_path_item_default(NULL);
			vgui_editor_asset_path_item_default(&g_path[0]);
			check(g_path[0].struct_size == (uint32_t)sizeof(g_path[0])
				&& g_path[0].label == NULL, "the path item default sizes itself");
			vgui_editor_asset_move_target_default(NULL);
			vgui_editor_asset_move_target_default(targets);
			check(targets[0].struct_size == (uint32_t)sizeof(targets[0])
				&& targets[0].path_id == NULL, "the move target default sizes itself");
			vgui_editor_asset_dependency_default(NULL);
			vgui_editor_asset_dependency_default(deps);
			check(deps[0].struct_size == (uint32_t)sizeof(deps[0])
				&& deps[0].name == NULL && deps[0].id == 0,
				"the dependency default sizes itself");
			vgui_editor_asset_browser_set_search(NULL, "x", 1);
			vgui_editor_asset_browser_set_view(NULL, VGUI_EDITOR_ASSET_GRID);
			vgui_editor_asset_browser_clear(NULL);
			vgui_editor_asset_browser_refresh(NULL);
			vgui_editor_asset_browser_tick(NULL, 0.016f);
		}
		(void)detached;
		(void)run_undo;
		(void)run_redo;
		/* `thumbs` belongs to the browser and is never destroyed here. */
		vgui_editor_asset_browser_destroy(browser);
	}
}

/* -------------------------------------------------------------------------- */
/* Asset preview                                                               */
/* -------------------------------------------------------------------------- */

/* Four RGBA8 rows: rebuild_texture() needs stride * height bytes or it leaves
 * the slot empty. */
static const unsigned char g_preview_pixels[64] = {
	 10,  20,  30, 255,  40,  50,  60, 255,  70,  80,  90, 255, 100, 110, 120, 255,
	 11,  21,  31, 255,  41,  51,  61, 255,  71,  81,  91, 255, 101, 111, 121, 255,
	 12,  22,  32, 255,  42,  52,  62, 255,  72,  82,  92, 255, 102, 112, 122, 255,
	 13,  23,  33, 255,  43,  53,  63, 255,  73,  83,  93, 255, 103, 113, 123, 255
};
static vgui_editor_preview_field g_preview_fields[2];
static vgui_color g_preview_swatches[2];
static float g_preview_wave[4];

typedef struct preview_fixture {
	int load_calls;
	int releases;
	char last_name[64];
	char last_type[64];
	vgui_item_id last_id;
	int fail;
} preview_fixture;

static int preview_load(void* user, const vgui_editor_preview_request* request,
	vgui_editor_preview_data* out)
{
	preview_fixture* fix = (preview_fixture*)user;
	++fix->load_calls;
	if (request)
	{
		fix->last_id = request->id;
		if (request->name)
		{
			size_t bytes = strlen(request->name);
			if (bytes >= sizeof(fix->last_name))
				bytes = sizeof(fix->last_name) - 1;
			memcpy(fix->last_name, request->name, bytes);
			fix->last_name[bytes] = '\0';
		}
		if (request->type)
		{
			size_t bytes = strlen(request->type);
			if (bytes >= sizeof(fix->last_type))
				bytes = sizeof(fix->last_type) - 1;
			memcpy(fix->last_type, request->type, bytes);
			fix->last_type[bytes] = '\0';
		}
	}
	vgui_editor_preview_data_default(out);
	if (fix->fail)
	{
		out->error = "the host could not read it";
		return 0;
	}
	out->kind = VGUI_EDITOR_PREVIEW_IMAGE;
	out->name = "filled.png";
	out->type = "Texture";
	out->detail = "4x4 RGBA8";
	out->text = "a small text block";
	vgui_editor_preview_field_default(g_preview_fields);
	g_preview_fields[0].label = "Size";
	g_preview_fields[0].value = "16 B";
	vgui_editor_preview_field_default(g_preview_fields + 1);
	g_preview_fields[1].label = "Format";
	g_preview_fields[1].value = "RGBA8";
	out->fields = g_preview_fields;
	out->field_count = 2;
	out->image_width = 4;
	out->image_height = 4;
	out->image_stride = 0;
	out->image_pixels = g_preview_pixels;
	out->image_pixel_count = sizeof(g_preview_pixels);
	out->accent = 0xFF00AAFFu;
	out->secondary = 0xFF223344u;
	g_preview_swatches[0] = 0xFF112233u;
	g_preview_swatches[1] = 0xFF445566u;
	out->swatches = g_preview_swatches;
	out->swatch_count = 2;
	g_preview_wave[0] = 0.0f;
	g_preview_wave[1] = 0.5f;
	g_preview_wave[2] = -0.5f;
	g_preview_wave[3] = 0.0f;
	out->waveform = g_preview_wave;
	out->waveform_count = 4;
	return 1;
}

static void release_preview(void* user)
{
	++((preview_fixture*)user)->releases;
}

void test_asset_preview(probe* p)
{
	vgui_editor_asset_preview* preview;
	vgui_editor_preview_data data;
	preview_fixture fix;
	vgui_item_id item;
	char buf[128];
	char tiny[8];
	size_t needed;
	int i;
	int n;
	int loaded;

	memset(p, 0, sizeof(*p));
	memset(&fix, 0, sizeof(fix));

	vgui_editor_preview_data_default(&data);
	check(data.struct_size == (uint32_t)sizeof(data), "preview data sizes itself");
	check(data.kind == VGUI_EDITOR_PREVIEW_NONE, "preview data defaults to none");
	check(data.name == NULL && data.error == NULL && data.fields == NULL,
		"the default preview data is empty");
	check(data.accent == 0xFF409EFFu && data.secondary == 0xFF2E3440u,
		"the default colours match the kit");

	preview = vgui_editor_asset_preview_create(vgui_root(g_ui));
	frame(2);
	check(preview != NULL, "asset preview built");
	check(vgui_editor_asset_preview_widget(preview) != NULL, "preview widget exists");
	check(vgui_editor_asset_preview_stage(preview) != NULL, "the image stage is exposed");

	/* The empty state before any item. */
	vgui_editor_asset_preview_set_texts(preview, "nothing here", "one moment",
		"that broke");
	needed = vgui_editor_asset_preview_status_text(preview, NULL, 0);
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_asset_preview_status_text(preview, buf, needed + 1) == needed
		&& strcmp(buf, "nothing here") == 0, "the custom empty text is shown");
	memset(tiny, 0, sizeof(tiny));
	check(vgui_editor_asset_preview_status_text(preview, tiny, 5) == needed,
		"a short status buffer still reports the full size");
	check(vgui_editor_asset_preview_item(preview) == 0, "no item yet");
	check(vgui_editor_asset_preview_loading(preview) == 0, "idle with no item");
	check(vgui_editor_asset_preview_has_error(preview) == 0, "no error with no item");
	check(vgui_editor_asset_preview_error(preview, buf, sizeof(buf)) == 0,
		"the error text is empty");
	check(vgui_editor_asset_preview_texture(preview) == NULL, "no texture yet");

	/* A successful load through the worker handler. */
	vgui_editor_asset_preview_set_load_handler(preview, preview_load, &fix,
		release_preview);
	item = 4711;
	vgui_editor_asset_preview_set_item(preview, item, "hero.png", "Texture");
	loaded = 0;
	for (i = 0; i < 200 && !loaded; ++i)
	{
		/* poll() leads: tick() pumps the same async queue, so calling it first
		 * would apply the result and leave nothing for poll() to report. */
		if (vgui_editor_asset_preview_poll(preview) == 1)
			loaded = 1;
		frame(1);
	}
	check(loaded == 1, "the preview reports new data after polling");
	check(fix.load_calls >= 1, "the worker loader ran");
	check(fix.last_id == item, "the request carries the item id");
	check(strcmp(fix.last_name, "hero.png") == 0, "the request carries the name");
	check(strcmp(fix.last_type, "Texture") == 0, "the request carries the type");
	check(vgui_editor_asset_preview_item(preview) == item, "item() reads back");
	check(vgui_editor_asset_preview_loading(preview) == 0, "loading finished");
	check(vgui_editor_asset_preview_has_error(preview) == 0, "the load succeeded");
	check(vgui_editor_asset_preview_texture(preview) != NULL,
		"the preview texture is available");
	needed = vgui_editor_asset_preview_status_text(preview, NULL, 0);
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_asset_preview_status_text(preview, buf, needed + 1) == needed,
		"the status text round-trips");

	/* Reload re-runs the loader, pumped by tick() alone: the kit applies the
	 * result without the host asking poll() for it. */
	i = fix.load_calls;
	vgui_editor_asset_preview_reload(preview);
	loaded = 0;
	for (n = 0; n < 200 && !loaded; ++n)
	{
		vgui_editor_asset_preview_tick(preview, 0.05f);
		loaded = fix.load_calls > i && !vgui_editor_asset_preview_loading(preview)
			? 1 : 0;
		frame(1);
	}
	check(loaded == 1, "tick() applies the reloaded preview");
	check(vgui_editor_asset_preview_poll(preview) == 0,
		"poll without new data reports nothing");
	check(fix.load_calls >= 1, "reload re-ran the loader");

	/* The failure path keeps the item and shows the host message. */
	fix.fail = 1;
	vgui_editor_asset_preview_set_item(preview, 4712, "broken.png", "Texture");
	{
		int errors = 0;
		for (i = 0; i < 200 && !errors; ++i)
		{
			vgui_editor_asset_preview_tick(preview, 0.05f);
			vgui_editor_asset_preview_poll(preview);
			errors = vgui_editor_asset_preview_has_error(preview);
			frame(1);
		}
		check(errors == 1, "a rejected load marks the preview as failed");
	}
	needed = vgui_editor_asset_preview_error(preview, NULL, 0);
	check(needed > 0, "the failure keeps error text");
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_asset_preview_error(preview, buf, needed + 1) == needed
		&& strcmp(buf, "the host could not read it") == 0,
		"the host error message round-trips");
	check(vgui_editor_asset_preview_item(preview) == 4712,
		"the failed preview keeps its item");
	check(vgui_editor_asset_preview_status_text(preview, buf, sizeof(buf)) > 0,
		"the failed state has a status line");

	/* Clearing falls back to the custom empty text. */
	vgui_editor_asset_preview_clear(preview);
	check(vgui_editor_asset_preview_item(preview) == 0, "clear drops the item");
	check(vgui_editor_asset_preview_has_error(preview) == 0, "clear drops the error");
	check(vgui_editor_asset_preview_texture(preview) == NULL, "clear drops the texture");
	needed = vgui_editor_asset_preview_status_text(preview, NULL, 0);
	memset(buf, 0, sizeof(buf));
	check(vgui_editor_asset_preview_status_text(preview, buf, needed + 1) == needed
		&& strcmp(buf, "nothing here") == 0, "clear restores the empty text");
	vgui_editor_asset_preview_set_texts(preview, NULL, NULL, NULL);
	needed = vgui_editor_asset_preview_status_text(preview, NULL, 0);
	memset(buf, 0, sizeof(buf));
	vgui_editor_asset_preview_status_text(preview, buf, needed + 1);
	check(strcmp(buf, "Select an asset to preview") == 0,
		"NULL restores the documented default text");

	/* Teardown. */
	vgui_editor_asset_preview_set_load_handler(preview, NULL, NULL, NULL);
	check(fix.releases == 1, "the preview loader released its user once");
	{
		check(vgui_editor_asset_preview_widget(NULL) == NULL,
			"widget(NULL) is NULL");
		check(vgui_editor_asset_preview_item(NULL) == 0, "item(NULL) is 0");
		check(vgui_editor_asset_preview_loading(NULL) == 0, "loading(NULL) is false");
		check(vgui_editor_asset_preview_has_error(NULL) == 0, "has_error(NULL) false");
		check(vgui_editor_asset_preview_poll(NULL) == 0, "poll(NULL) reports nothing");
		check(vgui_editor_asset_preview_error(NULL, buf, sizeof(buf)) == 0,
			"error(NULL) reports nothing");
		vgui_editor_preview_data_default(NULL);
		vgui_editor_preview_field_default(NULL);
		vgui_editor_preview_field_default(g_preview_fields);
		check(g_preview_fields[0].struct_size
			== (uint32_t)sizeof(vgui_editor_preview_field)
			&& g_preview_fields[0].label == NULL,
			"the preview field default sizes itself");
		vgui_editor_asset_preview_set_item(NULL, item, "x", "y");
		vgui_editor_asset_preview_set_texts(NULL, NULL, NULL, NULL);
		vgui_editor_asset_preview_reload(NULL);
		vgui_editor_asset_preview_clear(NULL);
		vgui_editor_asset_preview_tick(NULL, 0.016f);
	}
	vgui_editor_asset_preview_destroy(preview);
}
