#include "bridge/security/hash.hpp"
#include <openssl/evp.h>
namespace bridge::security {
void Sha256::Deleter::operator()(evp_md_ctx_st* context) const { EVP_MD_CTX_free(context); }
Sha256::Sha256(std::unique_ptr<evp_md_ctx_st, Deleter> context) : context_(std::move(context)) {}
Result<Sha256> Sha256::create() {
    std::unique_ptr<evp_md_ctx_st, Deleter> context(EVP_MD_CTX_new());
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        return std::unexpected(Error{ErrorCode::hash_failed});
    return Sha256(std::move(context));
}
Result<void> Sha256::update(std::span<const std::uint8_t> bytes) {
    if (!context_ || EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) != 1)
        return std::unexpected(Error{ErrorCode::hash_failed});
    return {};
}
Result<Digest> Sha256::finish() {
    Digest digest{};
    unsigned int length = 0;
    if (!context_ || EVP_DigestFinal_ex(context_.get(), digest.data(), &length) != 1 ||
        length != digest.size())
        return std::unexpected(Error{ErrorCode::hash_failed});
    context_.reset();
    return digest;
}
Result<Digest> sha256(std::span<const std::uint8_t> bytes) {
    auto context = Sha256::create();
    if (!context)
        return std::unexpected(context.error());
    auto updated = context->update(bytes);
    if (!updated)
        return std::unexpected(updated.error());
    return context->finish();
}
} // namespace bridge::security
