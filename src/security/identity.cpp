#include "bridge/security/identity.hpp"
#include <QCryptographicHash>
#include <array>
#include <cstring>
#include <memory>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
namespace bridge::security {
namespace {
template <typename T, auto Free> using Owned = std::unique_ptr<T, decltype(Free)>;
const auto invalid = std::unexpected(Error{ErrorCode::identity_failed});
Result<QByteArray> pem_bytes(BIO* bio) {
    char* data = nullptr; // Borrowed view into the owned BIO.
    const auto size = BIO_get_mem_data(bio, &data);
    if (size <= 0 || size > 16384 || data == nullptr)
        return invalid;
    return QByteArray(data, static_cast<qsizetype>(size));
}
} // namespace
Result<Identity> make_identity() {
    Owned<EVP_PKEY, EVP_PKEY_free> key(EVP_EC_gen("prime256v1"), EVP_PKEY_free);
    Owned<X509, X509_free> cert(X509_new(), X509_free);
    if (!key || !cert || X509_set_version(cert.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1 ||
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 300) == nullptr ||
        X509_set_pubkey(cert.get(), key.get()) != 1)
        return invalid;
    auto* name = X509_get_subject_name(cert.get()); // Borrowed from cert.
    const auto* cn = reinterpret_cast<const unsigned char*>("bridge.local");
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, cn, -1, -1, 0) != 1 ||
        X509_set_issuer_name(cert.get(), name) != 1)
        return invalid;
    const std::array<std::pair<int, const char*>, 4> extensions{
        {{NID_basic_constraints, "critical,CA:FALSE"},
         {NID_key_usage, "critical,digitalSignature"},
         {NID_ext_key_usage, "serverAuth,clientAuth"},
         {NID_subject_alt_name, "DNS:bridge.local"}}};
    X509V3_CTX context{};
    X509V3_set_ctx(&context, cert.get(), cert.get(), nullptr, nullptr, 0);
    for (const auto& [nid, value] : extensions) {
        Owned<X509_EXTENSION, X509_EXTENSION_free> extension(
            X509V3_EXT_conf_nid(nullptr, &context, nid, value), X509_EXTENSION_free);
        if (!extension || X509_add_ext(cert.get(), extension.get(), -1) != 1)
            return invalid;
    }
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0)
        return invalid;
    Owned<BIO, BIO_free> cert_bio(BIO_new(BIO_s_mem()), BIO_free);
    Owned<BIO, BIO_free> key_bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!cert_bio || !key_bio || PEM_write_bio_X509(cert_bio.get(), cert.get()) != 1 ||
        PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) !=
            1)
        return invalid;
    auto cert_pem = pem_bytes(cert_bio.get());
    auto key_pem = pem_bytes(key_bio.get());
    if (!cert_pem || !key_pem)
        return invalid;
    Identity identity{QSslCertificate(*cert_pem), QSslKey(*key_pem, QSsl::Ec)};
    key_pem->fill(0);
    if (identity.certificate.isNull() || identity.key.isNull())
        return invalid;
    return identity;
}
Result<void> validate_peer(const QSslCertificate& certificate) {
    const auto der = certificate.toDer();
    if (certificate.isNull() || der.isEmpty() || der.size() > 16384)
        return invalid;
    const auto* cursor = reinterpret_cast<const unsigned char*>(der.constData());
    const auto* end = cursor + der.size();
    Owned<X509, X509_free> cert(d2i_X509(nullptr, &cursor, static_cast<long>(der.size())),
                                X509_free);
    if (!cert || cursor != end)
        return invalid;
    Owned<EVP_PKEY, EVP_PKEY_free> key(X509_get_pubkey(cert.get()), EVP_PKEY_free);
    std::array<char, 80> group{};
    std::size_t group_length = 0;
    const auto before = X509_cmp_current_time(X509_get0_notBefore(cert.get()));
    const auto after = X509_cmp_current_time(X509_get0_notAfter(cert.get()));
    if (!key || EVP_PKEY_is_a(key.get(), "EC") != 1 ||
        EVP_PKEY_get_group_name(key.get(), group.data(), group.size(), &group_length) != 1 ||
        std::strcmp(group.data(), "prime256v1") != 0 || X509_verify(cert.get(), key.get()) != 1 ||
        before >= 0 || after <= 0 ||
        X509_check_host(cert.get(), "bridge.local", 12, 0, nullptr) != 1)
        return invalid;
    return {};
}
Result<QString> pairing_fingerprint(const QSslCertificate& client, const QSslCertificate& server) {
    if (!validate_peer(client) || !validate_peer(server))
        return invalid;
    auto input = QByteArray("bridge pairing v1");
    input.append(client.digest(QCryptographicHash::Sha256));
    input.append(server.digest(QCryptographicHash::Sha256));
    return QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}
} // namespace bridge::security
