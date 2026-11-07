#pragma once
/*
 * editor_c_api.h - shared plumbing for the pure-C editor facade declared in
 * <vgui/vgui_editor.h>.
 *
 * Every C handle is a small struct that wraps one C++ component plus the
 * bookkeeping the C contract needs (borrowed-string scratch, callback
 * pointers, optional user release).  The wrapper either owns the component or
 * borrows it from a container such as the editor shell.  Keep this header
 * internal to the library.
 */
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <vgui/vgui_editor.h>

#include <vgui/editor/command_registry.h>
#include <vgui/editor/command_stack.h>
#include <vgui/editor/drag_bridge.h>
#include <vgui/editor/editor_context.h>
#include <vgui/editor/editor_layout.h>
#include <vgui/editor/editor_types.h>

namespace vgeditor {

	/* Thread-local message behind vgui_editor_last_error(). */
	void set_error(const std::string& text);
	std::string& error_storage();

	/* Copies text into the caller's buffer, truncating but always terminating.
	 * Returns the length the full text would need. */
	size_t copy_string(char* out, size_t cap, const char* text);

	/* NULL / empty input is a empty string in the kit's value domain. */
	std::string to_text(const char* text);

	/* A NULL array yields an empty list; NULL entries become empty strings. */
	std::vector<std::string> to_text_list(const char* const* items, size_t count);

	/* Base of every C handle.  The handle either owns its C++ object - created
	 * through vgui_editor_*_create() - or borrows one from a container such as
	 * the editor shell.  impl is a reference, so the adapters read both cases
	 * with the same `handle->impl.member()` syntax.  Destroying a borrowed
	 * handle must leave the object alone: the source owns it.
	 *
	 * The virtual destructor lets a container that handed out borrowed wrappers
	 * (the shell caches one per panel) release them without knowing the concrete
	 * handle layout, which stays private to the adapter that owns it. */
	struct c_object_base {
		c_object_base() = default;
		c_object_base(const c_object_base&) = delete;
		c_object_base& operator=(const c_object_base&) = delete;
		virtual ~c_object_base();
	};

	template <typename T>
	class c_object : public c_object_base {
	public:
		T& impl;

		explicit c_object(T* target, bool owns = true)
			: owned_(owns ? target : nullptr), impl(*target)
		{
		}

		bool owns_object() const noexcept { return owned_ != nullptr; }

	private:
		std::unique_ptr<T> owned_;
	};

	/* Wraps an already-constructed object that the new handle takes ownership of.
	 * Returns NULL when either allocation fails, and never leaks the object. */
	template <typename Handle, typename T>
	Handle* own_object(T* target)
	{
		if (!target)
			return nullptr;
		std::unique_ptr<T> guard(target);
		Handle* handle = new (std::nothrow) Handle(guard.get(), true);
		if (!handle)
			return nullptr;
		guard.release();
		return handle;
	}

	/* Wraps an object owned elsewhere (a shell panel, a container's child). */
	template <typename Handle, typename T>
	Handle* borrow_object(T& target)
	{
		return new (std::nothrow) Handle(&target, false);
	}

	/* Result of a host string callback.  ok == false means the callback reported
	 * VGUI_EDITOR_NO_VALUE (or is absent). */
	struct text_result {
		bool ok = false;
		std::string value;
	};

	/* Runs a host string callback with a probe buffer and, when the callback
	 * reports that the value did not fit, one exact-size retry pass - so long
	 * values are never silently truncated. */
	template <typename Call>
	text_result read_text(Call&& call)
	{
		text_result out;
		char small[64];
		size_t need = call(small, sizeof(small));
		if (need == VGUI_EDITOR_NO_VALUE)
			return out;
		if (need + 1 <= sizeof(small)) {
			out.ok = true;
			out.value.assign(small);
			return out;
		}
		std::vector<char> buffer(need + 1);
		need = call(buffer.data(), buffer.size());
		if (need == VGUI_EDITOR_NO_VALUE)
			return out;
		out.ok = true;
		out.value.assign(buffer.data());
		return out;
	}

	/* Reads a caller struct that may be shorter than the current version: only
	 * the fields the caller knows about are taken, the rest stay at their
	 * default (zero) value. */
	template <typename T>
	T take_desc(const T* src)
	{
		T out = {};
		if (!src)
			return out;
		std::size_t bytes = sizeof(out);
		if (src->struct_size && src->struct_size < bytes)
			bytes = src->struct_size;
		std::memcpy(&out, src, bytes);
		return out;
	}

	/* Keeps a C user pointer alive for as long as a callback that captured it
	 * exists, and releases it once the last copy is gone. */
	struct user_owner {
		void* user = nullptr;
		vgui_editor_release_fn release = nullptr;
		~user_owner();
		user_owner() = default;
		user_owner(const user_owner&) = delete;
		user_owner& operator=(const user_owner&) = delete;
	};

	using user_owner_ptr = std::shared_ptr<user_owner>;

	user_owner_ptr make_owner(void* user, vgui_editor_release_fn release);

	/* One C callback slot: NULL fn means "not installed". */
	template <typename Fn>
	struct callback_slot {
		Fn fn = nullptr;
		void* user = nullptr;
		user_owner_ptr owner;

		void set(Fn next_fn, void* next_user, vgui_editor_release_fn release)
		{
			owner = make_owner(next_user, release);
			user = owner->user;
			fn = next_fn;
		}

		void clear()
		{
			fn = nullptr;
			user = nullptr;
			owner.reset();
		}
	};

	/* Undoable edit supplied by a C host.  execute() propagates the C return
	 * value so a rejected edit is not recorded. */
	class c_command final : public vgui_editor_kit::command
	{
	public:
		c_command(std::string name, vgui_editor_execute_fn execute,
			vgui_editor_undo_fn undo, user_owner_ptr owner);

		const char* name() const noexcept override;
		bool execute() override;
		void undo() override;

	private:
		std::string name_;
		vgui_editor_execute_fn execute_ = nullptr;
		vgui_editor_undo_fn undo_ = nullptr;
		user_owner_ptr owner_;
	};

	/* Converts between the kit's drag payload value type and the C struct.  The
	 * C view borrows the kit strings, so it is only valid while the payload is
	 * alive (i.e. for the duration of one callback). */
	void fill_c_payload(vgui_editor_drag_payload* out,
		const vgui_editor_kit::drag_payload& payload,
		std::vector<const char*>& file_ptrs);
	vgui_editor_kit::drag_payload from_c_payload(
		const vgui_editor_drag_payload* payload);

	/* C drop_target description turned into a kit target. */
	class c_drop_target final : public vgui_editor_kit::drop_target
	{
	public:
		vgui_editor_drop_target_desc desc;
		void* user = nullptr;

		bool can_accept(const vgui_editor_kit::drag_payload& payload) const override;
		void hover(const vgui_editor_kit::drag_payload& payload, vgui_widget* target,
			float x, float y, vgui_drop_hint* hint) override;
		bool drop(const vgui_editor_kit::drag_payload& payload, float x,
			float y) override;
	};

	/* Command registry helpers shared by the registry handle and the shell. */
	std::string shortcut_text(const vgui_editor_kit::command_shortcut& shortcut);
	vgui_editor_kit::command_shortcut parse_shortcut(const char* text);

	/* Handle internals shared between facade translation units: a component that
	 * needs the C++ object behind another handle goes through these accessors
	 * instead of duplicating the handle layout. */
	vgui_editor_kit::command_registry* registry_impl(vgui_editor_command_registry*);
	vgui_editor_kit::command_stack* stack_impl(vgui_editor_command_stack*);
	vgui_editor_kit::editor_context* context_impl(vgui_editor_context*);

} /* namespace vgeditor */
