#pragma once
#include "bridge/core/error.hpp"
#include <string_view>
namespace bridge::io {
// Lexical subset only; this is not an OS destination sandbox.
Result<void> validate_relative_path(std::string_view path);
} // namespace bridge::io
