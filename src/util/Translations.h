#pragma once

namespace gitbolt::util {

/// Whether this build carries GitBolt's English plural forms: the root
/// CMakeLists.txt compiles translations/gitbolt_en.ts into gitbolt_util
/// when it finds Qt LinguistTools and Qt 6.7 or later (PLURALS_TS_FILE),
/// and defines GITBOLT_ENGLISH_PLURALS then. Without them the build
/// still works, and the %n texts keep their "(s)".
constexpr bool hasEnglishPlurals()
{
#ifdef GITBOLT_ENGLISH_PLURALS
    return true;
#else
    return false;
#endif
}

/// Installs GitBolt's English translation, which holds nothing but the
/// singular and plural form of each %n string. Qt picks a form for
/// tr("%n file(s)", nullptr, n) only from a loaded translation; with
/// none it puts n into the source text as it stands, "1 file(s)". With
/// this one it reads "1 file" and "2 files".
///
/// Loaded whatever the locale (LANG, LC_ALL, the system's language):
/// the UI is English in every one, so the plurals must be too. Call it
/// once a QCoreApplication exists, before any window is built; a second
/// call is a no-op. The translator lives as long as the application.
/// True when it is installed, false in a build without the forms
/// (hasEnglishPlurals()) or with no application.
bool installEnglishPlurals();

} // namespace gitbolt::util
