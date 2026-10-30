#pragma once
/*
 * Internal tokenizer contract shared by the editor and other editor-kit
 * surfaces.  tokenize_code_line() lives in code_editor.cpp; the declaration is
 * all a consumer of fenced-code highlighting needs.
 */
#include <vgui/editor/code_editor.h>

#include <string>
#include <vector>

namespace vgui_editor_kit {

	void tokenize_code_line(code_language language, const std::string& text,
		int lex_in, std::vector<code_span>& spans, int* lex_out);

} // namespace vgui_editor_kit
