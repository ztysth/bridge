#include <cstdio>
#include <memory>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <string_view>

namespace {
template <typename T, auto Free> using Owned = std::unique_ptr<T, decltype(Free)>;
enum class Mode { tls13, reject_legacy };

bool probe(Mode mode) {
    Owned<EVP_PKEY, EVP_PKEY_free> key(EVP_EC_gen("prime256v1"), EVP_PKEY_free);
    Owned<X509, X509_free> certificate(X509_new(), X509_free);
    if (!key || !certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 300) == nullptr ||
        X509_set_pubkey(certificate.get(), key.get()) != 1)
        return false;
    auto* name = X509_get_subject_name(certificate.get()); // Borrowed from certificate.
    const auto* cn = reinterpret_cast<const unsigned char*>("bridge.local");
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, cn, -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), name) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0)
        return false;

    Owned<SSL_CTX, SSL_CTX_free> client_context(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    Owned<SSL_CTX, SSL_CTX_free> server_context(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    if (!client_context || !server_context)
        return false;
    for (auto* context : {client_context.get(), server_context.get()}) {
        if (SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_max_proto_version(context, TLS1_3_VERSION) != 1 ||
            SSL_CTX_use_certificate(context, certificate.get()) != 1 ||
            SSL_CTX_use_PrivateKey(context, key.get()) != 1 ||
            X509_STORE_add_cert(SSL_CTX_get_cert_store(context), certificate.get()) != 1)
            return false;
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    }
    if (mode == Mode::reject_legacy &&
        (SSL_CTX_set_min_proto_version(server_context.get(), TLS1_2_VERSION) != 1 ||
         SSL_CTX_set_max_proto_version(server_context.get(), TLS1_2_VERSION) != 1))
        return false;
    Owned<SSL, SSL_free> client(SSL_new(client_context.get()), SSL_free);
    Owned<SSL, SSL_free> server(SSL_new(server_context.get()), SSL_free);
    if (!client || !server)
        return false;
    BIO* client_raw = nullptr;
    BIO* server_raw = nullptr;
    const auto paired = BIO_new_bio_pair(&client_raw, 16384, &server_raw, 16384);
    Owned<BIO, BIO_free> client_bio(client_raw, BIO_free), server_bio(server_raw, BIO_free);
    if (paired != 1)
        return false;
    // SSL_set_bio assumes ownership of each paired BIO, including both directions.
    auto* client_stream = client_bio.release();
    auto* server_stream = server_bio.release();
    SSL_set_bio(client.get(), client_stream, client_stream);
    SSL_set_bio(server.get(), server_stream, server_stream);
    SSL_set_connect_state(client.get());
    SSL_set_accept_state(server.get());
    for (unsigned step = 0; step < 64; ++step) {
        for (auto* endpoint : {client.get(), server.get()}) {
            const auto result = SSL_do_handshake(endpoint);
            if (result == 1)
                continue;
            const auto error = SSL_get_error(endpoint, result);
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
                return mode == Mode::reject_legacy && error == SSL_ERROR_SSL &&
                       ERR_GET_REASON(ERR_peek_last_error()) == SSL_R_UNSUPPORTED_PROTOCOL;
        }
        if (SSL_is_init_finished(client.get()) && SSL_is_init_finished(server.get()))
            return mode == Mode::tls13 && SSL_version(client.get()) == TLS1_3_VERSION &&
                   SSL_version(server.get()) == TLS1_3_VERSION &&
                   SSL_get_verify_result(client.get()) == X509_V_OK &&
                   SSL_get_verify_result(server.get()) == X509_V_OK &&
                   SSL_get0_peer_certificate(client.get()) != nullptr &&
                   SSL_get0_peer_certificate(server.get()) != nullptr;
    }
    return false;
}
} // namespace

int main(int argc, char** argv) {
    Mode mode = Mode::tls13;
    if (argc == 2 && std::string_view(argv[1]) == "--reject-legacy")
        mode = Mode::reject_legacy;
    else if (argc != 1)
        return 2;
    std::puts("Independent OpenSSL TLS probe started");
    std::fflush(stdout);
    if (!probe(mode)) {
        ERR_print_errors_fp(stderr);
        return 1;
    }
    std::puts("Independent OpenSSL TLS probe passed");
    return 0;
}
