#pragma once
/*
 * expression.h - string expression bindings with automatic dependency analysis.
 *
 * This is the optional Phase 4 layer on top of vgui_bind_cb().  It lets a host
 * describe a computed widget value as one expression string instead of writing
 * a get callback and registering dependencies by hand:
 *
 *   vgui_expr_scope* scope = vgui_expr_scope_create(ctx);
 *   vgui_expr_scope_add_field(scope, "price", &order, VGUI_OFFSET(Order, price), VGUI_BINDV_FLOAT);
 *   vgui_expr_scope_add_field(scope, "qty",   &order, VGUI_OFFSET(Order, qty),   VGUI_BINDV_INT);
 *   vgui_binding* b = vgui_bind_expr(ctx, label, VGUI_BIND_TEXT, scope,
 *                                    "price * qty", errbuf, sizeof errbuf);
 *
 * The expression is parsed once into an AST.  Every variable it references is
 * resolved in the scope and its model is registered as a binding dependency,
 * so a later vgui_model_mark_dirty() on that model re-evaluates the expression
 * on the next vgui_update.  Idle frames do no work and issue no set_* calls.
 *
 * Grammar (first version):
 *   literals    decimal numbers ("1", "2.5"), single/double quoted strings
 *   variables   [A-Za-z_][A-Za-z0-9_]*  (resolved in the scope at compile time)
 *   operators   + - * / %   == != < <= > >=   && || !   ( )
 *               '+' concatenates when either operand is a string
 *   functions   min max abs round floor ceil clamp   (numeric)
 *               len upper lower                     (string)
 *   precedence  unary ! -  >  * / %  >  + -  >  comparison  >  &&  >  ||
 *
 * Results are coerced to the binding kind: VALUE/PROGRESS -> float,
 * CHECKED/VISIBLE/ENABLED -> int (non-zero true), TEXT -> decimal string.
 * Expression bindings are read-only: they install no setter, so user edits
 * never write back and the edit journal never records them.
 *
 * Lifetime: a scope owns every compiled expression created from it, and the
 * AST holds shared references to the variables it uses.  The scope MUST
 * outlive all bindings created from it (and the context that flushes them) --
 * the same borrowed-pointer convention as vgui::vector_rows.  Destroy the
 * context/unbind first, then the scope.
 *
 * CORE_ONLY safe: pure core, no SDL / OS dependency.
 */
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>

#include <vgui/vgui.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vgui_expr_scope vgui_expr_scope;

/* Custom variable getter.  Writes the variable's current value through out
 * according to value_type (vgui_bind_value_type):
 *   VGUI_BINDV_FLOAT -> float*, VGUI_BINDV_INT -> int*,
 *   VGUI_BINDV_TEXT  -> const char** (host keeps the string alive; it is copied
 *                       during evaluation). */
typedef void (*vgui_expr_get_fn)(void* user, int value_type, void* out);

VGUI_API vgui_expr_scope* vgui_expr_scope_create(vgui_context* ctx);
VGUI_API void vgui_expr_scope_destroy(vgui_expr_scope* scope);

/* Variable source 1: a struct field at (model, offset), typed by value_type. */
VGUI_API void vgui_expr_scope_add_field(vgui_expr_scope* scope, const char* name,
                                        void* model, size_t offset, int value_type);
/* Variable source 2: a getter callback.  dep_model (may be NULL) is the model
 * whose version changes retrigger evaluation; NULL means the host must call
 * vgui_binding_invalidate() to push updates. */
VGUI_API void vgui_expr_scope_add_getter(vgui_expr_scope* scope, const char* name,
                                         int value_type, vgui_expr_get_fn get,
                                         void* user, void* dep_model);

/* Compile + bind.  Returns NULL on a compile error (bad syntax, unknown
 * variable/function), copying a human-readable message into errbuf when given.
 * On success the AST and its dependencies are owned by the scope. */
VGUI_API vgui_binding* vgui_bind_expr(vgui_context* ctx, vgui_widget* widget,
                                      vgui_bind_kind kind, vgui_expr_scope* scope,
                                      const char* expression,
                                      char* errbuf, size_t errbuf_size);

/* Diagnostics: last compile error (valid until the next bind_expr) and the
 * number of registered variables. */
VGUI_API const char* vgui_expr_scope_last_error(const vgui_expr_scope* scope);
VGUI_API int vgui_expr_scope_var_count(const vgui_expr_scope* scope);

#ifdef __cplusplus
} /* extern "C" */
#endif

#ifdef __cplusplus

namespace vgui
{
	/* RAII owner of an expression scope.  Move-only.  Must be declared before
	 * the context/widgets it binds and destroyed after them. */
	class expr_scope
	{
	public:
		explicit expr_scope(vgui_context* ctx)
			: scope_(vgui_expr_scope_create(ctx)), ctx_(ctx) {}
		~expr_scope() { if (scope_) vgui_expr_scope_destroy(scope_); }

		expr_scope(const expr_scope&) = delete;
		expr_scope& operator=(const expr_scope&) = delete;
		expr_scope(expr_scope&& o) noexcept : scope_(o.scope_), ctx_(o.ctx_)
		{
			o.scope_ = nullptr;
			o.ctx_ = nullptr;
		}
		expr_scope& operator=(expr_scope&& o) noexcept
		{
			if (this != &o)
			{
				if (scope_) vgui_expr_scope_destroy(scope_);
				scope_ = o.scope_;
				ctx_ = o.ctx_;
				o.scope_ = nullptr;
				o.ctx_ = nullptr;
			}
			return *this;
		}

		vgui_expr_scope* native() const { return scope_; }

		/* Register a model field; value_type is deduced from the member type. */
		template <class M, class T>
		expr_scope& field(const char* name, M* model, T M::*member)
		{
			vgui_expr_scope_add_field(scope_, name, model,
				detail::member_offset(member), value_type_of<T>());
			return *this;
		}

		expr_scope& getter(const char* name, int value_type,
			vgui_expr_get_fn get, void* user, void* dep_model = nullptr)
		{
			vgui_expr_scope_add_getter(scope_, name, value_type, get, user, dep_model);
			return *this;
		}

		vgui_binding* bind(vgui_widget* widget, vgui_bind_kind kind,
			const char* expression, std::string* error = nullptr)
		{
			char buf[256];
			buf[0] = '\0';
			vgui_binding* b = vgui_bind_expr(ctx_, widget, kind, scope_,
				expression, buf, sizeof buf);
			if (error) *error = buf;
			return b;
		}

		int var_count() const { return vgui_expr_scope_var_count(scope_); }
		const char* last_error() const { return vgui_expr_scope_last_error(scope_); }

	private:
		template <class T>
		static int value_type_of()
		{
			if constexpr (std::is_same_v<T, std::string>)
				return VGUI_BINDV_TEXT;
			else if constexpr (std::is_floating_point_v<T>)
				return VGUI_BINDV_FLOAT;
			else
				return VGUI_BINDV_INT;
		}

		vgui_expr_scope* scope_;
		vgui_context* ctx_;
	};

} /* namespace vgui */

#endif /* __cplusplus */
