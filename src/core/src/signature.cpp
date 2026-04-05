#include "gitbolt/core/signature.h"
#include <git2.h>

namespace gitbolt::core {

Signature Signature::fromGit(const git_signature* sig) {
    Signature result;
    if (sig) {
        result.name = sig->name ? sig->name : "";
        result.email = sig->email ? sig->email : "";
        result.when = std::chrono::system_clock::from_time_t(sig->when.time);
        result.offset_minutes = sig->when.offset;
    }
    return result;
}

} // namespace gitbolt::core
