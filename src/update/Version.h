#pragma once

#include <QString>
#include <QStringView>

#include <compare>
#include <optional>

namespace gitbolt::update {

/// A release version, x.y.z, compared part by part as numbers: 0.9.10
/// is newer than 0.9.9, which a string compare gets wrong. Only plain
/// releases: a tag with a suffix (-rc1, +build) is no version the
/// updater offers, so parse() refuses it.
struct Version {
    int majorPart = 0;
    int minorPart = 0;
    int patchPart = 0;

    /// "1.2.3" or "v1.2.3", nothing else: no suffix, no spaces, no
    /// leading zeros ("01"), at most 6 digits a part. nullopt otherwise.
    static std::optional<Version> parse(QStringView text);
    /// "1.2.3".
    QString toString() const;

    friend auto operator<=>(const Version&, const Version&) = default;
    friend bool operator==(const Version&, const Version&) = default;
};

} // namespace gitbolt::update
