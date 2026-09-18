#pragma once
#include <commands/ICommandStrategy.h>
#include <memory>

/// Visible echo (unit tests). Pass hidden=true for the production binary.
std::shared_ptr<ICommandStrategy> CreateEchoCommand(bool hidden = false);
