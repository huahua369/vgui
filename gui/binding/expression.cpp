// gui/binding/expression.cpp - string expression bindings (Phase 4).
//
// A self-contained tokenizer + recursive-descent parser + evaluator that turns
// an expression string into a read-only computed binding.  Everything is built
// on the public vgui_bind_cb() / vgui_binding_add_dependency() API, so the core
// vgui_binding struct and the flush/writeback paths stay untouched.  Pure core:
// no SDL, no OS headers, CORE_ONLY safe.

#include <vgui/binding/expression.h>
#include <vgui/vgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	// ---------------------------------------------------------------------
	// Runtime value: a number or a string.  Coercion helpers mirror the DSL
	// contract documented in expression.h (numbers parse from strings, strings
	// format from numbers with %.6g).
	// ---------------------------------------------------------------------
	struct Value
	{
		bool is_str = false;
		double num = 0.0;
		std::string str;

		static Value number(double v) { Value x; x.num = v; return x; }
		static Value text(std::string s) { Value x; x.is_str = true; x.str = std::move(s); return x; }
	};

	static double value_num(const Value& v)
	{
		if (!v.is_str) return v.num;
		return std::strtod(v.str.c_str(), nullptr);
	}

	static std::string value_str(const Value& v)
	{
		if (v.is_str) return v.str;
		char buf[64];
		std::snprintf(buf, sizeof buf, "%.6g", v.num);
		return std::string(buf);
	}

	static bool value_truthy(const Value& v)
	{
		return v.is_str ? !v.str.empty() : (v.num != 0.0);
	}

	// ---------------------------------------------------------------------
	// Variable descriptor: field (model + offset) or getter callback.  Dep
	// model is the field's model, or the getter's dep_model (may be NULL).
	// ---------------------------------------------------------------------
	struct Variable
	{
		std::string name;
		int value_type = VGUI_BINDV_FLOAT;
		void* model = nullptr;
		size_t offset = 0;
		vgui_expr_get_fn get = nullptr;
		void* user = nullptr;
		void* dep_model = nullptr;

		Value read() const
		{
			if (get)
			{
				switch (value_type)
				{
				case VGUI_BINDV_FLOAT:
				{
					float out = 0;
					get(user, value_type, &out);
					return Value::number(out);
				}
				case VGUI_BINDV_INT:
				{
					int out = 0;
					get(user, value_type, &out);
					return Value::number(out);
				}
				case VGUI_BINDV_TEXT:
				{
					const char* out = nullptr;
					get(user, value_type, &out);
					return Value::text(out ? out : "");
				}
				default: return Value::number(0);
				}
			}

			if (!model) return value_type == VGUI_BINDV_TEXT ? Value::text("") : Value::number(0);
			const char* base = static_cast<const char*>(model) + offset;
			switch (value_type)
			{
			case VGUI_BINDV_FLOAT: return Value::number(*reinterpret_cast<const float*>(base));
			case VGUI_BINDV_INT:   return Value::number(*reinterpret_cast<const int*>(base));
			case VGUI_BINDV_TEXT:  return Value::text(*reinterpret_cast<const std::string*>(base));
			default: return Value::number(0);
			}
		}
	};

	using VarPtr = std::shared_ptr<Variable>;

	// ---------------------------------------------------------------------
	// AST
	// ---------------------------------------------------------------------
	struct Node
	{
		enum class Kind { Num, Str, Var, Unary, Binary, Call };
		Kind kind = Kind::Num;
		double num = 0.0;
		std::string text;                 // string literal / operator / function name
		VarPtr var;                        // Kind::Var
		std::vector<std::unique_ptr<Node>> kids;
	};

	using NodePtr = std::unique_ptr<Node>;

	static NodePtr make_num(double v)
	{
		NodePtr n(new Node());
		n->kind = Node::Kind::Num;
		n->num = v;
		return n;
	}

	static NodePtr make_str(std::string s)
	{
		NodePtr n(new Node());
		n->kind = Node::Kind::Str;
		n->text = std::move(s);
		return n;
	}

	// ---------------------------------------------------------------------
	// Tokenizer
	// ---------------------------------------------------------------------
	struct Token
	{
		enum class Kind { End, Num, Str, Ident, Op, LParen, RParen, Comma, Error };
		Kind kind = Kind::End;
		std::string text;
		double num = 0.0;
		int pos = 0;
	};

	static std::vector<Token> tokenize(const char* src, std::string& err)
	{
		std::vector<Token> out;
		if (!src) { err = "null expression"; return out; }
		const char* p = src;
		while (*p)
		{
			if (std::isspace(static_cast<unsigned char>(*p))) { ++p; continue; }
			int pos = (int)(p - src);
			if (std::isdigit(static_cast<unsigned char>(*p)) ||
				(*p == '.' && std::isdigit(static_cast<unsigned char>(p[1]))))
			{
				char* end = nullptr;
				double v = std::strtod(p, &end);
				if (end == p) { err = "bad number at " + std::to_string(pos); return out; }
				Token t; t.kind = Token::Kind::Num; t.num = v; t.pos = pos;
				out.push_back(t);
				p = end;
				continue;
			}
			if (*p == '\'' || *p == '"')
			{
				char quote = *p++;
				std::string s;
				bool closed = false;
				while (*p && *p != quote)
				{
					if (*p == '\\' && p[1])
					{
						++p;
						switch (*p)
						{
						case 'n': s.push_back('\n'); break;
						case 't': s.push_back('\t'); break;
						case 'r': s.push_back('\r'); break;
						default: s.push_back(*p); break;
						}
						++p;
					}
					else s.push_back(*p++);
				}
				if (*p == quote) { ++p; closed = true; }
				if (!closed) { err = "unterminated string at " + std::to_string(pos); return out; }
				Token t; t.kind = Token::Kind::Str; t.text = std::move(s); t.pos = pos;
				out.push_back(t);
				continue;
			}
			if (std::isalpha(static_cast<unsigned char>(*p)) || *p == '_')
			{
				const char* s = p;
				while (std::isalnum(static_cast<unsigned char>(*p)) || *p == '_') ++p;
				Token t; t.kind = Token::Kind::Ident;
				t.text.assign(s, p); t.pos = pos;
				out.push_back(t);
				continue;
			}
			// two-char operators first
			if ((*p == '=' && p[1] == '=') || (*p == '!' && p[1] == '=') ||
				(*p == '<' && p[1] == '=') || (*p == '>' && p[1] == '=') ||
				(*p == '&' && p[1] == '&') || (*p == '|' && p[1] == '|'))
			{
				Token t; t.kind = Token::Kind::Op;
				t.text.assign(p, 2); t.pos = pos;
				out.push_back(t);
				p += 2;
				continue;
			}
			if (std::strchr("+-*/%<>!", *p))
			{
				Token t; t.kind = Token::Kind::Op;
				t.text.assign(1, *p); t.pos = pos;
				out.push_back(t);
				++p;
				continue;
			}
			if (*p == '(') { Token t; t.kind = Token::Kind::LParen; t.pos = pos; out.push_back(t); ++p; continue; }
			if (*p == ')') { Token t; t.kind = Token::Kind::RParen; t.pos = pos; out.push_back(t); ++p; continue; }
			if (*p == ',') { Token t; t.kind = Token::Kind::Comma; t.pos = pos; out.push_back(t); ++p; continue; }
			err = std::string("unexpected character '") + *p + "' at " + std::to_string(pos);
			return out;
		}
		Token end; end.kind = Token::Kind::End; end.pos = (int)(p - src);
		out.push_back(end);
		return out;
	}

	// ---------------------------------------------------------------------
	// Parser (recursive descent).  Collects referenced variables and validates
	// names/functions at compile time.
	// ---------------------------------------------------------------------
	class Parser
	{
	public:
		Parser(std::vector<Token> toks,
			const std::unordered_map<std::string, VarPtr>& vars,
			std::string& err)
			: toks_(std::move(toks)), vars_(vars), err_(err) {}

		NodePtr parse()
		{
			NodePtr n = parse_or();
			if (!n) return nullptr;
			if (peek().kind != Token::Kind::End)
			{
				fail(std::string("unexpected token '") + peek().text + "'");
				return nullptr;
			}
			return n;
		}

		const std::vector<VarPtr>& refs() const { return refs_; }

	private:
		const Token& peek() const { return toks_[pos_]; }
		bool is_op(const char* op) const
		{
			return peek().kind == Token::Kind::Op && peek().text == op;
		}
		bool take_op(const char* op)
		{
			if (is_op(op)) { ++pos_; return true; }
			return false;
		}
		void fail(std::string msg)
		{
			if (err_.empty()) err_ = std::move(msg);
		}
		void add_ref(const VarPtr& v)
		{
			if (!v) return;
			for (const auto& r : refs_) if (r == v) return;
			refs_.push_back(v);
		}

		NodePtr parse_or()
		{
			NodePtr lhs = parse_and();
			if (!lhs) return nullptr;
			while (take_op("||"))
			{
				NodePtr rhs = parse_and();
				if (!rhs) return nullptr;
				lhs = binary("||", std::move(lhs), std::move(rhs));
			}
			return lhs;
		}
		NodePtr parse_and()
		{
			NodePtr lhs = parse_eq();
			if (!lhs) return nullptr;
			while (take_op("&&"))
			{
				NodePtr rhs = parse_eq();
				if (!rhs) return nullptr;
				lhs = binary("&&", std::move(lhs), std::move(rhs));
			}
			return lhs;
		}
		NodePtr parse_eq()
		{
			NodePtr lhs = parse_rel();
			if (!lhs) return nullptr;
			for (;;)
			{
				const char* op = is_op("==") ? "==" : (is_op("!=") ? "!=" : nullptr);
				if (!op) break;
				++pos_;
				NodePtr rhs = parse_rel();
				if (!rhs) return nullptr;
				lhs = binary(op, std::move(lhs), std::move(rhs));
			}
			return lhs;
		}
		NodePtr parse_rel()
		{
			NodePtr lhs = parse_add();
			if (!lhs) return nullptr;
			for (;;)
			{
				const char* op = nullptr;
				if (is_op("<")) op = "<";
				else if (is_op("<=")) op = "<=";
				else if (is_op(">")) op = ">";
				else if (is_op(">=")) op = ">=";
				if (!op) break;
				++pos_;
				NodePtr rhs = parse_add();
				if (!rhs) return nullptr;
				lhs = binary(op, std::move(lhs), std::move(rhs));
			}
			return lhs;
		}
		NodePtr parse_add()
		{
			NodePtr lhs = parse_mul();
			if (!lhs) return nullptr;
			for (;;)
			{
				const char* op = is_op("+") ? "+" : (is_op("-") ? "-" : nullptr);
				if (!op) break;
				++pos_;
				NodePtr rhs = parse_mul();
				if (!rhs) return nullptr;
				lhs = binary(op, std::move(lhs), std::move(rhs));
			}
			return lhs;
		}
		NodePtr parse_mul()
		{
			NodePtr lhs = parse_unary();
			if (!lhs) return nullptr;
			for (;;)
			{
				const char* op = nullptr;
				if (is_op("*")) op = "*";
				else if (is_op("/")) op = "/";
				else if (is_op("%")) op = "%";
				if (!op) break;
				++pos_;
				NodePtr rhs = parse_unary();
				if (!rhs) return nullptr;
				lhs = binary(op, std::move(lhs), std::move(rhs));
			}
			return lhs;
		}
		NodePtr parse_unary()
		{
			if (take_op("!") || take_op("-"))
			{
				// take_op above consumed; recover which by looking at previous token
				const std::string& op = toks_[pos_ - 1].text;
				NodePtr child = parse_unary();
				if (!child) return nullptr;
				NodePtr n(new Node());
				n->kind = Node::Kind::Unary;
				n->text = op;
				n->kids.push_back(std::move(child));
				return n;
			}
			return parse_primary();
		}
		NodePtr parse_primary()
		{
			const Token& t = peek();
			if (t.kind == Token::Kind::Num)
			{
				++pos_;
				return make_num(t.num);
			}
			if (t.kind == Token::Kind::Str)
			{
				++pos_;
				return make_str(t.text);
			}
			if (t.kind == Token::Kind::LParen)
			{
				++pos_;
				NodePtr n = parse_or();
				if (!n) return nullptr;
				if (peek().kind != Token::Kind::RParen)
				{
					fail("missing ')'");
					return nullptr;
				}
				++pos_;
				return n;
			}
			if (t.kind == Token::Kind::Ident)
			{
				++pos_;
				if (peek().kind == Token::Kind::LParen)
				{
					++pos_;
					NodePtr n(new Node());
					n->kind = Node::Kind::Call;
					n->text = t.text;
					if (peek().kind != Token::Kind::RParen)
					{
						for (;;)
						{
							NodePtr arg = parse_or();
							if (!arg) return nullptr;
							n->kids.push_back(std::move(arg));
							if (peek().kind == Token::Kind::Comma) { ++pos_; continue; }
							break;
						}
					}
					if (peek().kind != Token::Kind::RParen)
					{
						fail("missing ')' in call to " + t.text);
						return nullptr;
					}
					++pos_;
					if (!is_known_function(t.text))
					{
						fail("unknown function '" + t.text + "'");
						return nullptr;
					}
					return n;
				}
				auto it = vars_.find(t.text);
				if (it == vars_.end())
				{
					fail("unknown variable '" + t.text + "'");
					return nullptr;
				}
				NodePtr n(new Node());
				n->kind = Node::Kind::Var;
				n->var = it->second;
				add_ref(it->second);
				return n;
			}
			fail("expected expression");
			return nullptr;
		}

		static bool is_known_function(const std::string& name)
		{
			static const char* names[] = {
				"min", "max", "abs", "round", "floor", "ceil", "clamp",
				"len", "upper", "lower"
			};
			for (const char* n : names) if (name == n) return true;
			return false;
		}

		static NodePtr binary(std::string op, NodePtr lhs, NodePtr rhs)
		{
			NodePtr n(new Node());
			n->kind = Node::Kind::Binary;
			n->text = std::move(op);
			n->kids.push_back(std::move(lhs));
			n->kids.push_back(std::move(rhs));
			return n;
		}

		std::vector<Token> toks_;
		const std::unordered_map<std::string, VarPtr>& vars_;
		std::string& err_;
		std::size_t pos_ = 0;
		std::vector<VarPtr> refs_;
	};

	// ---------------------------------------------------------------------
	// Evaluator
	// ---------------------------------------------------------------------
	static Value eval_node(const Node* n)
	{
		switch (n->kind)
		{
		case Node::Kind::Num: return Value::number(n->num);
		case Node::Kind::Str: return Value::text(n->text);
		case Node::Kind::Var: return n->var->read();
		case Node::Kind::Unary:
		{
			Value a = eval_node(n->kids[0].get());
			if (n->text == "!") return Value::number(value_truthy(a) ? 0.0 : 1.0);
			return Value::number(-value_num(a)); // '-'
		}
		case Node::Kind::Binary:
		{
			const std::string& op = n->text;
			Value a = eval_node(n->kids[0].get());
			Value b = eval_node(n->kids[1].get());
			if (op == "&&") return Value::number(value_truthy(a) && value_truthy(b) ? 1.0 : 0.0);
			if (op == "||") return Value::number(value_truthy(a) || value_truthy(b) ? 1.0 : 0.0);
			if (op == "==" || op == "!=")
			{
				bool eq;
				if (a.is_str && b.is_str) eq = (a.str == b.str);
				else eq = (value_num(a) == value_num(b));
				return Value::number((op == "==") == eq ? 1.0 : 0.0);
			}
			if (op == "<" || op == "<=" || op == ">" || op == ">=")
			{
				bool cmp;
				if (a.is_str && b.is_str)
				{
					int r = a.str.compare(b.str);
					cmp = op == "<" ? r < 0 : op == "<=" ? r <= 0 : op == ">" ? r > 0 : r >= 0;
				}
				else
				{
					double x = value_num(a), y = value_num(b);
					cmp = op == "<" ? x < y : op == "<=" ? x <= y : op == ">" ? x > y : x >= y;
				}
				return Value::number(cmp ? 1.0 : 0.0);
			}
			if (op == "+")
			{
				if (a.is_str || b.is_str)
					return Value::text(value_str(a) + value_str(b));
				return Value::number(value_num(a) + value_num(b));
			}
			if (op == "-") return Value::number(value_num(a) - value_num(b));
			if (op == "*") return Value::number(value_num(a) * value_num(b));
			if (op == "/")
			{
				double d = value_num(b);
				return Value::number(d == 0.0 ? 0.0 : value_num(a) / d);
			}
			if (op == "%")
			{
				int d = (int)value_num(b);
				return Value::number(d == 0 ? 0 : (int)value_num(a) % d);
			}
			return Value::number(0);
		}
		case Node::Kind::Call:
		{
			const std::string& fn = n->text;
			std::vector<Value> args;
			args.reserve(n->kids.size());
			for (const auto& k : n->kids) args.push_back(eval_node(k.get()));

			if (fn == "len")
				return Value::number(args.empty() ? 0 : (double)value_str(args[0]).size());
			if (fn == "upper")
			{
				std::string s = args.empty() ? "" : value_str(args[0]);
				for (char& c : s) c = (char)std::toupper(static_cast<unsigned char>(c));
				return Value::text(std::move(s));
			}
			if (fn == "lower")
			{
				std::string s = args.empty() ? "" : value_str(args[0]);
				for (char& c : s) c = (char)std::tolower(static_cast<unsigned char>(c));
				return Value::text(std::move(s));
			}
			if (fn == "abs")
				return Value::number(std::fabs(args.empty() ? 0 : value_num(args[0])));
			if (fn == "round")
				return Value::number(std::round(args.empty() ? 0 : value_num(args[0])));
			if (fn == "floor")
				return Value::number(std::floor(args.empty() ? 0 : value_num(args[0])));
			if (fn == "ceil")
				return Value::number(std::ceil(args.empty() ? 0 : value_num(args[0])));
			if (fn == "min" || fn == "max")
			{
				if (args.empty()) return Value::number(0);
				double best = value_num(args[0]);
				for (std::size_t i = 1; i < args.size(); ++i)
				{
					double v = value_num(args[i]);
					best = (fn == "min") ? std::min(best, v) : std::max(best, v);
				}
				return Value::number(best);
			}
			if (fn == "clamp")
			{
				if (args.empty()) return Value::number(0);
				double v = value_num(args[0]);
				double lo = args.size() > 1 ? value_num(args[1]) : 0;
				double hi = args.size() > 2 ? value_num(args[2]) : v;
				return Value::number(std::max(lo, std::min(hi, v)));
			}
			return Value::number(0);
		}
		}
		return Value::number(0);
	}

	// ---------------------------------------------------------------------
	// Compiled expression (owned by the scope; bound as vgui_bind_cb user data)
	// ---------------------------------------------------------------------
	struct CompiledExpr
	{
		NodePtr root;
		std::vector<VarPtr> refs;
		std::string text_scratch;   // stable storage for TEXT get-callback result
		vgui_context* ctx = nullptr;
	};

	static void expr_get_fn(void* user, vgui_bind_kind kind, void* out)
	{
		CompiledExpr* e = static_cast<CompiledExpr*>(user);
		if (!e || !e->root || !out) return;
		Value v = eval_node(e->root.get());
		switch (kind)
		{
		case VGUI_BIND_VALUE:
		case VGUI_BIND_PROGRESS:
			*static_cast<float*>(out) = (float)value_num(v);
			break;
		case VGUI_BIND_CHECKED:
		case VGUI_BIND_VISIBLE:
		case VGUI_BIND_ENABLED:
			*static_cast<int*>(out) = value_truthy(v) ? 1 : 0;
			break;
		case VGUI_BIND_SELECTION:
			*static_cast<int*>(out) = (int)value_num(v);
			break;
		case VGUI_BIND_TEXT:
			e->text_scratch = value_str(v);
			*static_cast<const char**>(out) = e->text_scratch.c_str();
			break;
		}
	}

	// ---------------------------------------------------------------------
	// Scope
	// ---------------------------------------------------------------------
	struct Scope
	{
		vgui_context* ctx = nullptr;
		std::unordered_map<std::string, VarPtr> vars;
		std::vector<std::unique_ptr<CompiledExpr>> compiled;
		std::string last_error;
	};

} // namespace

struct vgui_expr_scope : Scope {};

// -------------------------------------------------------------------------
// Public C API
// -------------------------------------------------------------------------
extern "C" {

vgui_expr_scope* vgui_expr_scope_create(vgui_context* ctx)
{
	Scope* s = new Scope();
	s->ctx = ctx;
	return static_cast<vgui_expr_scope*>(s);
}

void vgui_expr_scope_destroy(vgui_expr_scope* scope)
{
	delete scope;
}

void vgui_expr_scope_add_field(vgui_expr_scope* scope, const char* name,
	void* model, size_t offset, int value_type)
{
	if (!scope || !name || !*name || !model) return;
	VarPtr v(new Variable());
	v->name = name;
	v->value_type = value_type;
	v->model = model;
	v->offset = offset;
	v->dep_model = model;
	scope->vars[name] = std::move(v);
}

void vgui_expr_scope_add_getter(vgui_expr_scope* scope, const char* name,
	int value_type, vgui_expr_get_fn get, void* user, void* dep_model)
{
	if (!scope || !name || !*name || !get) return;
	VarPtr v(new Variable());
	v->name = name;
	v->value_type = value_type;
	v->get = get;
	v->user = user;
	v->dep_model = dep_model;
	scope->vars[name] = std::move(v);
}

vgui_binding* vgui_bind_expr(vgui_context* ctx, vgui_widget* widget,
	vgui_bind_kind kind, vgui_expr_scope* scope, const char* expression,
	char* errbuf, size_t errbuf_size)
{
	if (errbuf && errbuf_size) errbuf[0] = '\0';
	if (!ctx || !widget || !scope || !expression)
	{
		if (errbuf && errbuf_size)
			std::snprintf(errbuf, errbuf_size, "invalid arguments");
		scope->last_error = "invalid arguments";
		return nullptr;
	}

	scope->last_error.clear();
	std::string err;
	std::vector<Token> toks = tokenize(expression, err);
	if (!err.empty())
	{
		scope->last_error = err;
		if (errbuf && errbuf_size) std::snprintf(errbuf, errbuf_size, "%s", err.c_str());
		return nullptr;
	}

	Parser parser(std::move(toks), scope->vars, err);
	NodePtr root = parser.parse();
	if (!root)
	{
		if (err.empty()) err = "compile failed";
		scope->last_error = err;
		if (errbuf && errbuf_size) std::snprintf(errbuf, errbuf_size, "%s", err.c_str());
		return nullptr;
	}

	std::unique_ptr<CompiledExpr> ce(new CompiledExpr());
	ce->root = std::move(root);
	ce->refs = parser.refs();
	ce->ctx = ctx;

	vgui_binding* b = vgui_bind_cb(ctx, widget, kind, ce.get(), expr_get_fn, nullptr);
	if (!b)
	{
		scope->last_error = "vgui_bind_cb failed";
		if (errbuf && errbuf_size) std::snprintf(errbuf, errbuf_size, "vgui_bind_cb failed");
		return nullptr;
	}

	// Automatic dependency analysis: register each referenced variable's model.
	for (const VarPtr& v : ce->refs)
		if (v->dep_model)
			vgui_binding_add_dependency(b, v->dep_model);

	// The scope owns the compiled expression for the binding's lifetime.
	scope->compiled.push_back(std::move(ce));
	return b;
}

const char* vgui_expr_scope_last_error(const vgui_expr_scope* scope)
{
	return scope ? scope->last_error.c_str() : "";
}

int vgui_expr_scope_var_count(const vgui_expr_scope* scope)
{
	return scope ? (int)scope->vars.size() : 0;
}

} // extern "C"
