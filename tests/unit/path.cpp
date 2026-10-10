#include "bridge/io/path.hpp"
#include "../utf8.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
TEST_CASE("portable UTF-8 names accept Chinese spaces dot prefixes and underscores") {
    using bridge::io::validate_relative_path;
    for (auto path : {u8"\u4e2d\u6587\u6587\u4ef6\u5939/\u8d44\u6599.txt", u8".mincraft/_hello",
                      u8"you have to/run this", u8"caf\u00e9.txt", u8"cafe\u0301.txt",
                      u8"\u05e9\u05dc\u05d5\u05dd.txt", u8"\U0001f469\u200d\U0001f4bb.txt"})
        REQUIRE(validate_relative_path(bridge::test::utf8(path)));
    REQUIRE(validate_relative_path(std::string(255, 'x')));
    std::u8string longest;
    for (int i = 0; i < 85; ++i)
        longest += u8"\u4e2d";
    REQUIRE(validate_relative_path(bridge::test::utf8(longest)));
}
TEST_CASE("portable relative UTF-8 subset rejects cross-platform escapes") {
    using bridge::io::validate_relative_path;
    for (auto path : {"file.bin", "folder/sub-file.txt", "empty", "COM10.txt"})
        REQUIRE(validate_relative_path(path));
    for (auto path :
         {"",        "/root", "../file",        "a/../b",      "./file",  "a//b",    "a/",
          "C:/file", "a\\b",  "//server/share", "file:stream", "NUL",     "aux.txt", "Com1.bin",
          "LPT9",    "file.", "file ",          "a/<b>",       "a?b",     "a|b",     "a*b",
          "a\"b",    "a\nb",  "a\tb",           "\xC0\xAF",    "CON .txt"})
        REQUIRE_FALSE(validate_relative_path(path));
    REQUIRE_FALSE(validate_relative_path(std::string(256, 'x')));
    REQUIRE_FALSE(validate_relative_path(std::string(4097, 'x')));
    REQUIRE_FALSE(validate_relative_path(std::string("a\0b", 3)));
}
TEST_CASE("portable names reject malformed Unicode controls and Windows device aliases") {
    using bridge::io::validate_relative_path;
    for (auto path : {"\x80", "\xc0\x80", "\xe4\xb8", "\xed\xa0\x80", "\xf4\x90\x80\x80",
                      "\xf5\x80\x80\x80", "\xff", "a\x7f"})
        REQUIRE_FALSE(validate_relative_path(path));
    for (auto path : {u8"a\u0085b", u8"a\u2028b", u8"a\u2029b", u8"a\u202eb", u8"a\u2066b",
                      u8"a\u061cb", u8"COM\u00b9.txt", u8"lpt\u00b2.bin", u8"COM\u00b3"})
        REQUIRE_FALSE(validate_relative_path(bridge::test::utf8(path)));
}
TEST_CASE("collision keys fold Unicode and canonical equivalents without compatibility mapping") {
    using bridge::io::path_collision_key;
    for (auto pair : {std::pair{u8"CAF\u00c9/file", u8"cafe\u0301/FILE"},
                      std::pair{u8"Stra\u00dfe", u8"STRASSE"}, std::pair{u8"\u03a3", u8"\u03c2"}}) {
        auto left = path_collision_key(bridge::test::utf8(pair.first));
        auto right = path_collision_key(bridge::test::utf8(pair.second));
        REQUIRE(left);
        REQUIRE(right);
        REQUIRE(*left == *right);
    }
    const auto lower = path_collision_key(".mincraft/_hello");
    const auto upper = path_collision_key(".MINCRAFT/_HELLO");
    REQUIRE(lower);
    REQUIRE(upper);
    REQUIRE(*lower == *upper);
    const auto wide = path_collision_key(bridge::test::utf8(u8"\uff21"));
    const auto ascii = path_collision_key("A");
    REQUIRE(wide);
    REQUIRE(ascii);
    REQUIRE(*wide != *ascii);
    REQUIRE_FALSE(path_collision_key("../escape"));
    // Valid input bytes cannot force an unbounded normalization workspace/key.
    std::string expanded;
    for (int component = 0; component < 16; ++component) {
        if (component != 0)
            expanded += '/';
        for (int i = 0; i < 127; ++i)
            expanded += bridge::test::utf8(u8"\u0390");
    }
    REQUIRE(bridge::io::validate_relative_path(expanded));
    REQUIRE_FALSE(path_collision_key(expanded));
}
