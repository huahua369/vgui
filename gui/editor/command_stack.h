#pragma once
/*
 * command_stack.h - editor-wide undo/redo foundation.
 *
 * Concrete commands are supplied by the host application.  The editor kit only
 * owns execution order, transactions and the undo/redo stacks.
 */
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vgui_editor_kit {

	class command
	{
	public:
		virtual ~command() = default;
		virtual const char* name() const noexcept = 0;
		virtual void execute() = 0;
		virtual void undo() = 0;
	};

	/* Convenience command for callbacks.  The caller is responsible for keeping
	 * captured objects alive as long as the command remains in the stack. */
	class function_command final : public command
	{
	public:
		using callback = std::function<void()>;

		function_command(std::string name, callback execute, callback undo);

		const char* name() const noexcept override;
		void execute() override;
		void undo() override;

	private:
		std::string name_;
		callback execute_;
		callback undo_;
	};

	class command_group final : public command
	{
	public:
		explicit command_group(std::string name);

		const char* name() const noexcept override;
		void execute() override;
		void undo() override;

		void add(std::unique_ptr<command> child);
		bool empty() const noexcept;

	private:
		std::string name_;
		std::vector<std::unique_ptr<command>> children_;
	};

	class command_stack
	{
	public:
		command_stack() = default;
		command_stack(const command_stack&) = delete;
		command_stack& operator=(const command_stack&) = delete;

		/* Executes and records a command.  Commands must not throw. */
		void execute(std::unique_ptr<command> command);

		/* Records a command whose effect has already been applied.  Useful for
		 * property editors that update their model before creating the undo
		 * record.  Inside a transaction the command joins the current group. */
		void push(std::unique_ptr<command> command);

		bool undo();
		bool redo();
		void clear();

		bool can_undo() const noexcept;
		bool can_redo() const noexcept;
		std::size_t undo_count() const noexcept;
		std::size_t redo_count() const noexcept;
		const char* next_undo_name() const noexcept;
		const char* next_redo_name() const noexcept;

		/* Groups subsequent execute() calls into one undo record. */
		void begin_transaction(std::string name);
		bool commit_transaction();
		void rollback_transaction();
		bool in_transaction() const noexcept;

	private:
		std::vector<std::unique_ptr<command>> undo_;
		std::vector<std::unique_ptr<command>> redo_;
		std::vector<std::unique_ptr<command_group>> transactions_;
	};

} /* namespace vgui_editor_kit */
