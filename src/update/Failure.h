#pragma once

namespace gitbolt::update {

/// Why an update step failed, as a fixed code: what usage statistics
/// and error reports carry (never the message, which may hold paths).
/// telemetry::updateReason keeps its own copy of the names, which a test
/// pins to these. None stays last, so a test can walk them all.
enum class Failure {
    Other,
    // The check and the download
    Refused,             ///< not asked for: a URL, name or size GitBolt won't take
    NetworkError,        ///< no answer, or cut off (offline)
    HttpError,           ///< an HTTP status other than 200
    RateLimited,         ///< GitHub's rate limit
    BadResponse,         ///< not a release, or too large
    SizeMismatch,
    ChecksumMissing,     ///< SHA256SUMS.txt has no line for it
    ChecksumConflict,    ///< SHA256SUMS.txt and the API's digest differ
    ChecksumMismatch,
    DiskError,
    // Installer::prepare
    NotInstallable,      ///< this copy can't install updates
    WrongFile,           ///< not this package's asset
    Busy,
    AttachFailed,        ///< macOS: hdiutil attach
    BadBundle,           ///< macOS: not GitBolt's app, or not this version
    NeedsNewerSystem,    ///< the release's minimum macOS or Windows is above this one
    AptRefused,          ///< deb: apt-get -s says it can't be installed here
    SignatureInvalid,    ///< macOS: codesign --verify
    CopyFailed,          ///< macOS: ditto
    // Installer::launchHelper
    NotReady,
    NoPkexec,
    HelperStartFailed,
    ElevationFailed,     ///< Windows' UAC or pkexec couldn't elevate
    PermissionDeclined,  ///< UAC's No, or pkexec's dialog dismissed
    // The helper's install-result (installResultFailure)
    QuitTimeout,         ///< GitBolt didn't quit in time (gave-up)
    NewCopyMissing,
    MoveAsideFailed,
    MoveIntoPlaceFailed,
    RollbackFailed,
    AptFailed,
    InUse,               ///< Windows: another GitBolt ran from the install folder
    InstallerNotStarted,
    InstallerExit,
    WrongVersion,
    ElevatedError,
    ElevatedTimeout,
    ElevatedNoResult,
    NoResult,            ///< the helper never reported back
    None,
};

/// The code's snake_case name, e.g. "checksum_mismatch".
constexpr const char* failureName(Failure failure) {
    switch (failure) {
    case Failure::Other: return "other";
    case Failure::Refused: return "refused";
    case Failure::NetworkError: return "network_error";
    case Failure::HttpError: return "http_error";
    case Failure::RateLimited: return "rate_limited";
    case Failure::BadResponse: return "bad_response";
    case Failure::SizeMismatch: return "size_mismatch";
    case Failure::ChecksumMissing: return "checksum_missing";
    case Failure::ChecksumConflict: return "checksum_conflict";
    case Failure::ChecksumMismatch: return "checksum_mismatch";
    case Failure::DiskError: return "disk_error";
    case Failure::NotInstallable: return "not_installable";
    case Failure::WrongFile: return "wrong_file";
    case Failure::Busy: return "busy";
    case Failure::AttachFailed: return "attach_failed";
    case Failure::BadBundle: return "bad_bundle";
    case Failure::NeedsNewerSystem: return "needs_newer_system";
    case Failure::AptRefused: return "apt_refused";
    case Failure::SignatureInvalid: return "signature_invalid";
    case Failure::CopyFailed: return "copy_failed";
    case Failure::NotReady: return "not_ready";
    case Failure::NoPkexec: return "no_pkexec";
    case Failure::HelperStartFailed: return "helper_start_failed";
    case Failure::ElevationFailed: return "elevation_failed";
    case Failure::PermissionDeclined: return "permission_declined";
    case Failure::QuitTimeout: return "quit_timeout";
    case Failure::NewCopyMissing: return "new_copy_missing";
    case Failure::MoveAsideFailed: return "move_aside_failed";
    case Failure::MoveIntoPlaceFailed: return "move_into_place_failed";
    case Failure::RollbackFailed: return "rollback_failed";
    case Failure::AptFailed: return "apt_failed";
    case Failure::InUse: return "in_use";
    case Failure::InstallerNotStarted: return "installer_not_started";
    case Failure::InstallerExit: return "installer_exit";
    case Failure::WrongVersion: return "wrong_version";
    case Failure::ElevatedError: return "elevated_error";
    case Failure::ElevatedTimeout: return "elevated_timeout";
    case Failure::ElevatedNoResult: return "elevated_no_result";
    case Failure::NoResult: return "no_result";
    case Failure::None: return "none";
    }
    return "other";
}

} // namespace gitbolt::update
