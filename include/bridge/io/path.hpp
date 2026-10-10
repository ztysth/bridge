#pragma once
#include "bridge/core/error.hpp"
#include <string>
#include <string_view>
namespace bridge::io {
// Strict UTF-8 and portable lexical rules; not an OS destination sandbox.
Result<void> validate_relative_path(std::string_view path);
// Bounded NFC + full case-fold key for collision checks only. Never a disk path.
Result<std::string> path_collision_key(std::string_view path);
} // namespace bridge::io
