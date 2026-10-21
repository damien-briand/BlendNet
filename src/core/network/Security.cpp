#include "network/Security.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <sstream>

namespace blendnet::network {
namespace {
std::string hex(const unsigned char* data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(digits[data[index] >> 4]);
        result.push_back(digits[data[index] & 15]);
    }
    return result;
}
bool writeFile(const std::filesystem::path& path, std::string_view text, bool secret) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
#ifndef _WIN32
    if (secret) {
        std::error_code error;
        std::filesystem::permissions(
            path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, error);
        if (error)
            return false;
    }
#else
    (void)secret; // ACL héritées du dossier utilisateur sous Windows.
#endif
    output << text;
    output.close();
    return static_cast<bool>(output);
}
bool createCertificate(const std::filesystem::path& certPath,
                       const std::filesystem::path& keyPath) {
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) <= 0)
        return false;
    EVP_PKEY* rawKey = nullptr;
    if (EVP_PKEY_keygen(context.get(), &rawKey) <= 0)
        return false;
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
    const std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    std::array<unsigned char, 16> serial{};
    if (!cert || RAND_bytes(serial.data(), serial.size()) != 1)
        return false;
    serial[0] &= 0x7f;
    const std::unique_ptr<BIGNUM, decltype(&BN_free)> number(
        BN_bin2bn(serial.data(), serial.size(), nullptr), BN_free);
    const std::unique_ptr<ASN1_INTEGER, decltype(&ASN1_INTEGER_free)> asnSerial(
        number ? BN_to_ASN1_INTEGER(number.get(), nullptr) : nullptr, ASN1_INTEGER_free);
    if (!asnSerial || X509_set_version(cert.get(), 2) != 1 ||
        X509_set_serialNumber(cert.get(), asnSerial.get()) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(cert.get()), -300) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert.get()), 365L * 24 * 60 * 60) ||
        X509_set_pubkey(cert.get(), key.get()) != 1)
        return false;
    auto* name = X509_get_subject_name(cert.get());
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("BlendNet"), -1, -1,
                                   0) != 1 ||
        X509_set_issuer_name(cert.get(), name) != 1 ||
        X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0)
        return false;
    const std::unique_ptr<BIO, decltype(&BIO_free)> certBio(BIO_new(BIO_s_mem()), BIO_free);
    const std::unique_ptr<BIO, decltype(&BIO_free)> keyBio(BIO_new(BIO_s_mem()), BIO_free);
    if (!certBio || !keyBio || PEM_write_bio_X509(certBio.get(), cert.get()) != 1 ||
        PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) !=
            1)
        return false;
    char* certData = nullptr;
    char* keyData = nullptr;
    const auto certSize = BIO_get_mem_data(certBio.get(), &certData);
    const auto keySize = BIO_get_mem_data(keyBio.get(), &keyData);
    return certSize > 0 && keySize > 0 &&
           writeFile(keyPath, std::string_view(keyData, static_cast<std::size_t>(keySize)), true) &&
           writeFile(certPath, std::string_view(certData, static_cast<std::size_t>(certSize)),
                     false);
}
} // namespace

std::optional<std::string> randomToken() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1)
        return std::nullopt;
    return hex(bytes.data(), bytes.size());
}
std::optional<std::string> randomSessionId() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1)
        return std::nullopt;
    bytes[6] = (bytes[6] & 15) | 0x40;
    bytes[8] = (bytes[8] & 63) | 0x80;
    const auto value = hex(bytes.data(), bytes.size());
    return value.substr(0, 8) + "-" + value.substr(8, 4) + "-" + value.substr(12, 4) + "-" +
           value.substr(16, 4) + "-" + value.substr(20);
}
std::optional<std::string> certificateFingerprint(X509* certificate) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (!certificate || X509_digest(certificate, EVP_sha256(), bytes.data(), &size) != 1 ||
        size != 32)
        return std::nullopt;
    return hex(bytes.data(), size);
}
bool sameToken(std::string_view first, std::string_view second) {
    std::array<unsigned char, 32> a{}, b{};
    unsigned int aSize = 0, bSize = 0;
    return EVP_Digest(first.data(), first.size(), a.data(), &aSize, EVP_sha256(), nullptr) == 1 &&
           EVP_Digest(second.data(), second.size(), b.data(), &bSize, EVP_sha256(), nullptr) == 1 &&
           aSize == 32 && bSize == 32 && CRYPTO_memcmp(a.data(), b.data(), 32) == 0;
}
bool sameFingerprint(std::string_view first, std::string_view second) {
    if (first.size() != 64 || second.size() != 64)
        return false;
    const auto lower = [](char c) { return c >= 'A' && c <= 'F' ? char(c - 'A' + 'a') : c; };
    return std::equal(first.begin(), first.end(), second.begin(),
                      [lower](char a, char b) { return lower(a) == lower(b); });
}
NetworkError ensureServerIdentity(const std::filesystem::path& directory,
                                  ServerIdentity& identity) {
    try {
        std::filesystem::create_directories(directory);
        ServerIdentity result{directory / "certificate.pem",
                              directory / "private-key.pem",
                              directory / "pairing-token.txt",
                              {}};
        const bool certExists = std::filesystem::exists(result.certificateFile);
        const bool keyExists = std::filesystem::exists(result.privateKeyFile);
        if (certExists != keyExists ||
            (!certExists && !createCertificate(result.certificateFile, result.privateKeyFile)))
            return NetworkError::IdentityFailed;
        if (std::filesystem::exists(result.tokenFile)) {
            std::ifstream input(result.tokenFile);
            std::ostringstream contents;
            contents << input.rdbuf();
            result.token = contents.str();
            if (!input || result.token.size() != 64 ||
                result.token.find_first_not_of("0123456789abcdef") != std::string::npos)
                return NetworkError::IdentityFailed;
        } else {
            const auto token = randomToken();
            if (!token || !writeFile(result.tokenFile, *token, true))
                return NetworkError::IdentityFailed;
            result.token = *token;
        }
        identity = std::move(result);
        return NetworkError::None;
    } catch (const std::filesystem::filesystem_error&) {
        return NetworkError::IdentityFailed;
    }
}
NetworkError persistPin(const std::filesystem::path& path, const KnownSource& source,
                        const std::string& fingerprint) {
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    AppSettings settings;
    const auto loaded = AppSettings::load(path, settings);
    if (loaded != SettingsError::None && loaded != SettingsError::NotFound)
        return NetworkError::PinPersistenceFailed;
    auto found = std::find_if(settings.knownSources.begin(), settings.knownSources.end(),
                              [&source](const KnownSource& entry) {
                                  return entry.host == source.host && entry.port == source.port;
                              });
    if (found != settings.knownSources.end()) {
        if (found->certificateSha256 && !sameFingerprint(*found->certificateSha256, fingerprint))
            return NetworkError::CertificateMismatch;
        found->certificateSha256 = fingerprint;
        found->token = source.token;
    } else {
        auto entry = source;
        entry.certificateSha256 = fingerprint;
        settings.knownSources.push_back(std::move(entry));
    }
    return settings.save(path) == SettingsError::None ? NetworkError::None
                                                      : NetworkError::PinPersistenceFailed;
}
} // namespace blendnet::network
