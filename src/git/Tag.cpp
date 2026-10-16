#include "git/Tag.h"

#include <array>

namespace gitbolt::git {

std::string_view tagMessageWithoutSignature(std::string_view message) {
    // The markers git's gpg-interface.c knows: OpenPGP (both armor
    // headers), X.509 (gpgsm) and SSH.
    static constexpr std::array<std::string_view, 4> kSignatureStarts = {
        "-----BEGIN PGP SIGNATURE-----",
        "-----BEGIN PGP MESSAGE-----",
        "-----BEGIN SIGNED MESSAGE-----",
        "-----BEGIN SSH SIGNATURE-----",
    };
    size_t end = message.size();
    size_t lineStart = 0;
    while (lineStart < message.size()) {
        const std::string_view line = message.substr(lineStart);
        for (std::string_view start : kSignatureStarts) {
            if (line.starts_with(start)) {
                end = lineStart;
                break;
            }
        }
        const size_t eol = message.find('\n', lineStart);
        if (eol == std::string_view::npos)
            break;
        lineStart = eol + 1;
    }
    return message.substr(0, end);
}

} // namespace gitbolt::git
