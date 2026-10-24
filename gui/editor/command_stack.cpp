#include "command_stack.h"

#include <utility>

namespace vgui_editor_kit {

	function_command::function_command(std::string name, callback execute, callback undo)
		: name_(std::move(name)), execute_(std::move(execute)), undo_(std::move(undo))
	{
	}

	const char* function_command::name() const noexcept
	{
		return name_.c_str();
	}

	void function_command::execute()
	{
		if (execute_)
			execute_();
	}

	void function_command::undo()
	{
		if (undo_)
			undo_();
	}

	command_group::command_group(std::string name)
		: name_(std::move(name))
	{
	}

	const char* command_group::name() const noexcept
	{
		return name_.c_str();
	}

	void command_group::execute()
	{
		for (auto& child : children_)
			child->execute();
	}

	void command_group::undo()
	{
		for (auto it = children_.rbegin(); it != children_.rend(); ++it)
			(*it)->undo();
	}

	void command_group::add(std::unique_ptr<command> child)
	{
		if (child)
			children_.push_back(std::move(child));
	}

	bool command_group::empty() const noexcept
	{
		return children_.empty();
	}

	void command_stack::execute(std::unique_ptr<command> command)
	{
		if (!command)
			return;

		command->execute();
		if (!transactions_.empty())
		{
			transactions_.back()->add(std::move(command));
			return;
		}

		undo_.push_back(std::move(command));
		redo_.clear();
	}

	void command_stack::push(std::unique_ptr<command> command)
	{
		if (!command)
			return;
		if (!transactions_.empty())
		{
			transactions_.back()->add(std::move(command));
			return;
		}
		undo_.push_back(std::move(command));
		redo_.clear();
	}

	bool command_stack::undo()
	{
		if (undo_.empty())
			return false;

		auto command = std::move(undo_.back());
		undo_.pop_back();
		command->undo();
		redo_.push_back(std::move(command));
		return true;
	}

	bool command_stack::redo()
	{
		if (redo_.empty())
			return false;

		auto command = std::move(redo_.back());
		redo_.pop_back();
		command->execute();
		undo_.push_back(std::move(command));
		return true;
	}

	void command_stack::clear()
	{
		undo_.clear();
		redo_.clear();
		transactions_.clear();
	}

	bool command_stack::can_undo() const noexcept
	{
		return !undo_.empty();
	}

	bool command_stack::can_redo() const noexcept
	{
		return !redo_.empty();
	}

	std::size_t command_stack::undo_count() const noexcept
	{
		return undo_.size();
	}

	std::size_t command_stack::redo_count() const noexcept
	{
		return redo_.size();
	}

	const char* command_stack::next_undo_name() const noexcept
	{
		return undo_.empty() ? nullptr : undo_.back()->name();
	}

	const char* command_stack::next_redo_name() const noexcept
	{
		return redo_.empty() ? nullptr : redo_.back()->name();
	}

	void command_stack::begin_transaction(std::string name)
	{
		transactions_.push_back(std::make_unique<command_group>(std::move(name)));
	}

	bool command_stack::commit_transaction()
	{
		if (transactions_.empty())
			return false;

		auto group = std::move(transactions_.back());
		transactions_.pop_back();
		if (!group || group->empty())
			return false;

		if (!transactions_.empty())
		{
			transactions_.back()->add(std::move(group));
			return true;
		}

		undo_.push_back(std::move(group));
		redo_.clear();
		return true;
	}

	void command_stack::rollback_transaction()
	{
		if (transactions_.empty())
			return;

		auto group = std::move(transactions_.back());
		transactions_.pop_back();
		if (group)
			group->undo();
	}

	bool command_stack::in_transaction() const noexcept
	{
		return !transactions_.empty();
	}

} /* namespace vgui_editor_kit */
