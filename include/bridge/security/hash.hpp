#pragma once
#include "bridge/core/content.hpp"
#include <memory>
struct evp_md_ctx_st;
namespace bridge::security {
class Sha256 {
  public:
    static Result<Sha256> create();
    Sha256(Sha256&&) noexcept = default;
    Sha256& operator=(Sha256&&) noexcept = default;
    Result<void> update(std::span<const std::uint8_t> bytes);
    Result<Digest> finish();

  private:
    struct Deleter {
        void operator()(evp_md_ctx_st* context) const;
    };
    explicit Sha256(std::unique_ptr<evp_md_ctx_st, Deleter> context);
    std::unique_ptr<evp_md_ctx_st, Deleter> context_;
};
Result<Digest> sha256(std::span<const std::uint8_t> bytes);
} // namespace bridge::security
