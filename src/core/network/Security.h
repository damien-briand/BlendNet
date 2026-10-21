#pragma once

#include "network/NetworkTypes.h"

#include <optional>

struct x509_st;

namespace blendnet::network {
struct ServerIdentity {
    std::filesystem::path certificateFile;
    std::filesystem::path privateKeyFile;
    std::filesystem::path tokenFile;
    std::string token;
};
// RNF-S1/S2 : identité persistante, clé/token privés et 32 octets d'entropie pour le token.
NetworkError ensureServerIdentity(const std::filesystem::path& directory, ServerIdentity& identity);
std::optional<std::string> randomToken();
std::optional<std::string> randomSessionId();
std::optional<std::string> certificateFingerprint(x509_st* certificate);
bool sameToken(std::string_view first, std::string_view second);
bool sameFingerprint(std::string_view first, std::string_view second);
NetworkError persistPin(const std::filesystem::path& settingsPath, const KnownSource& source,
                        const std::string& fingerprint);
} // namespace blendnet::network
