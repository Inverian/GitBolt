#pragma once

class QLockFile;

namespace gitbolt::util {

/// Clears a leftover lock file that QLockFile's own PID check can never
/// prove stale: one truncated by a crash mid-write, or naming another
/// machine ID (a home directory shared between machines, or moved to a
/// new one). With stale time 0, as main.cpp's instance lock and the
/// telemetry queue's lock use, tryLock() would fail on such a file for
/// good. Call it after tryLock() failed with LockFailedError, then try
/// again; true when the file was removed.
///
/// Safe against a live owner: its flock (Unix) or open handle
/// (Windows) dies with it, and removeStaleLockFile() fails while one
/// is held. The age check stays clear of the instant between a new
/// owner creating the file and locking it, so a lock written less than
/// two seconds ago is left alone. Serialised through "<lock>.rmlock"
/// exactly as QLockFile serialises its own stale removal: two
/// processes removing at once could otherwise each unlink a lock the
/// other had just re-created, and both would hold it.
bool removeUnprovableStaleLock(QLockFile& lock);

} // namespace gitbolt::util
