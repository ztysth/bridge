#include "bridge/core/content.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>
using namespace bridge;
TEST_CASE("single-file manifest and bounded resume geometry use 64-bit offsets") {
    FileManifest manifest{{1}, "file.bin", (5ULL << 30U) + 7, {}};
    REQUIRE(validate_manifest(manifest));
    REQUIRE(chunk_count(manifest.size) == 5121);
    REQUIRE(chunk_length(manifest.size, 5120) == 7);
    REQUIRE(resume_index(manifest.size, 5ULL << 30U) == 5120);
    REQUIRE(resume_index(manifest.size, manifest.size) == 5121);
    REQUIRE(chunk_count(0) == 0);
    REQUIRE(resume_index(0, 0) == 0);
    REQUIRE_FALSE(chunk_length(0, 0));
    REQUIRE_FALSE(chunk_length(manifest.size, 5121));
    REQUIRE_FALSE(resume_index(manifest.size, 1));
    REQUIRE_FALSE(resume_index(manifest.size, manifest.size + 1));
    manifest.size = maximum_file_size;
    REQUIRE(validate_manifest(manifest));
    REQUIRE(chunk_count(manifest.size) == 1048576);
    manifest.size++;
    REQUIRE_FALSE(validate_manifest(manifest));
    REQUIRE_FALSE(chunk_length(std::numeric_limits<std::uint64_t>::max(), 0));
    REQUIRE_FALSE(resume_index(std::numeric_limits<std::uint64_t>::max(), 0));
    manifest.size = 0;
    manifest.id = {};
    REQUIRE_FALSE(validate_manifest(manifest));
    manifest.id = {1};
    manifest.name = std::string(256, 'a');
    REQUIRE_FALSE(validate_manifest(manifest));
}
