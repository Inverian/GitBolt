#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gitbolt::git {

/// Incremental progress report for a clone, emitted by
/// `GitProcess::clone()` (parsed from `git clone --progress`). A
/// clone has two separate metric sets — the fetch/indexing phase
/// (objects, bytes, deltas) and the checkout phase (files written) —
/// and this struct flattens both into one shape that the caller can
/// render into a single progress UI without caring which phase is
/// active.
struct CloneProgress {
    enum class Phase {
        Receiving,   ///< downloading pack objects from the remote
        Resolving,   ///< indexing / resolving deltas after fetch
        CheckingOut  ///< writing files to the working directory
    };
    Phase    phase = Phase::Receiving;

    // Fetch-phase counters (valid in Receiving / Resolving phases).
    uint32_t receivedObjects = 0;
    uint32_t indexedObjects  = 0;
    uint32_t totalObjects    = 0;
    uint32_t indexedDeltas   = 0;
    uint32_t totalDeltas     = 0;
    uint64_t receivedBytes   = 0;

    // Checkout-phase counters (valid in CheckingOut phase).
    uint32_t completedSteps  = 0;
    uint32_t totalSteps      = 0;
};

/// Callback type invoked from the clone's worker thread. It may be
/// called many times per second — if the receiver lives on a
/// different thread (e.g. the GUI thread), the lambda must marshal
/// the update safely via QMetaObject::invokeMethod or similar.
using CloneProgressCallback = std::function<void(const CloneProgress&)>;

/// Turns the stderr of `git clone --progress` into CloneProgress
/// updates, incrementally: feed() takes whatever a pipe read returned,
/// so a record may arrive split across any number of chunks.
///
/// git ends an in-place progress update with '\r' and a finished line
/// with '\n'; text relayed from the server's sideband is prefixed with
/// "remote: " and padded with spaces (or an ANSI clear-to-EOL when git
/// thinks stderr is a terminal). Recognised titles map onto phases:
///
///   Cloning into '…'                   Receiving, nothing yet
///   remote: Counting/Compressing/…     Receiving, nothing yet (the
///                                      server is still preparing)
///   Receiving / Unpacking objects      Receiving (objects, bytes)
///   Resolving deltas                   Resolving
///   Updating files / Checking out      CheckingOut
///   files / Filtering content
///
/// Every other line that has the progress shape ("<title>: 45% (n/m)"
/// or "<title>: n, done.") is dropped, so localized titles never leak
/// into the error text. Lines that are not progress at all ("fatal:
/// …", "ERROR: Permission denied (publickey).") are kept — the last
/// few of them are what messages() returns when git fails.
class CloneProgressParser {
public:
    /// Parse every record `chunk` completes; the trailing partial
    /// record (if any) is held until a later feed() or finish().
    std::vector<CloneProgress> feed(std::string_view chunk);

    /// End of output: parse the held partial record, if any.
    std::vector<CloneProgress> finish();

    /// The last few non-progress lines, oldest first, joined by '\n'.
    /// Empty when git printed nothing but progress.
    std::string messages() const;

    /// git said "Clone succeeded, but checkout failed": the fetch
    /// completed and only writing the work tree failed (a path the
    /// file system rejects, a smudge filter that errored…), so git
    /// kept the repository on purpose. Its advice on finishing the
    /// checkout is the tail of messages().
    bool checkoutFailed() const { return checkoutFailed_; }

private:
    void parseRecord(std::string_view record, std::vector<CloneProgress>& out);

    std::string pending_;              // unterminated tail of the stream
    CloneProgress state_;              // cumulative; each update is a copy
    std::deque<std::string> messages_; // bounded to kMaxMessages
    bool checkoutFailed_ = false;
};

} // namespace gitbolt::git
