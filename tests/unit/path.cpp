#include "bridge/io/path.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
TEST_CASE("portable relative ASCII subset rejects cross-platform escapes") {
    using bridge::io::validate_relative_path;
    for (auto path : {"file.bin", "folder/sub-file.txt", "empty", "COM10.txt"})
        REQUIRE(validate_relative_path(path));
    for (auto path :
         {"",        "/root", "../file",        "a/../b",       "./file",  "a//b",    "a/",
          "C:/file", "a\\b",  "//server/share", "file:stream",  "NUL",     "aux.txt", "Com1.bin",
          "LPT9",    "file.", "file ",          "a/<b>",        "a?b",     "a|b",     "a*b",
          "a\"b",    "a\nb",  "a\tb",           "\xC3\xA9.txt", "\xC0\xAF"})
        REQUIRE_FALSE(validate_relative_path(path));
    REQUIRE_FALSE(validate_relative_path(std::string(256, 'x')));
    REQUIRE_FALSE(validate_relative_path(std::string(4097, 'x')));
    REQUIRE_FALSE(validate_relative_path(std::string("a\0b", 3)));
}
