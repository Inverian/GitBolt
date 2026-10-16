#pragma once

#include <QByteArray>
#include <QString>

namespace gitbolt::git {

/// Set in the environment of every git GitProcess starts, and so of
/// the askpass git or ssh starts: GitBolt itself, which main() then
/// runs as the askpass. ssh passes no flags, so an env marker is the
/// only reliable switch.
inline constexpr char kAskpassModeEnv[] = "GITBOLT_ASKPASS_MODE";
/// Ask the user (runAskpass in main.cpp).
inline constexpr char kAskpassAsk[] = "1";
/// Answer no at once, without a window: a git command nobody is
/// waiting on, the periodic auto-fetch (GitProcess::forbidPrompts).
inline constexpr char kAskpassRefuse[] = "refuse";

enum class AskpassKind {
    /// A password or passphrase: a masked field.
    Secret,
    /// A user name: a field that shows what is typed.
    Text,
    /// ssh's yes-or-no questions: Yes / No buttons, answered "yes" or
    /// "no".
    YesNo,
};

/// What the askpass shows for `prompt`. `sshHint` is the
/// SSH_ASKPASS_PROMPT ssh sets: "confirm" for a yes-or-no question
/// (a ControlMaster's "Allow shared connection…?"). It sets none for
/// a new host key's "Are you sure you want to continue connecting
/// (yes/no/[fingerprint])?" ("(yes/no)?" before OpenSSH 8.0), so the
/// text decides that one: shown as a secret, it had the user type
/// "yes" blind. Typing the fingerprint instead is not offered.
inline AskpassKind askpassKind(const QString& prompt, const QByteArray& sshHint)
{
    if (sshHint == "confirm" || prompt.contains(QStringLiteral("(yes/no")))
        return AskpassKind::YesNo;
    // "Username for 'https://…'"; everything else (Password for…,
    // Enter passphrase for key…) is a secret.
    if (prompt.contains(QStringLiteral("username"), Qt::CaseInsensitive))
        return AskpassKind::Text;
    return AskpassKind::Secret;
}

} // namespace gitbolt::git
