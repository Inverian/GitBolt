#pragma once

#include "git/Remote.h"

#include <QDialog>
#include <vector>

class QPushButton;
class QTableWidget;

namespace gitbolt::dialogs {

/// Modeless dialog for managing the set of remotes on the open
/// repository. Shows name + URL in a two-column table and exposes
/// Add / Edit URL / Remove. The host wires the buttons to
/// Repository::addRemote / removeRemote and refreshes the list.
///
/// Edit URL is implemented as remove + add against the same name —
/// libgit2 doesn't expose a "set URL" verb on Repository today and
/// the round-trip is harmless: refspecs and push URLs default back
/// to git's standard layout, which is what the dialog is showing
/// anyway.
class RemotesDialog : public QDialog {
    Q_OBJECT
public:
    explicit RemotesDialog(QWidget* parent = nullptr);

    /// Replace the displayed table with a fresh remote list. Called
    /// by the host on open and after every mutation.
    void setRemotes(const std::vector<gitbolt::git::RemoteInfo>& remotes);

signals:
    /// Add a brand-new remote with the given name and fetch URL.
    void addRequested(const QString& name, const QString& url);

    /// Replace the URL of the named remote. Implemented host-side
    /// as a remove + add since Repository has no setUrl verb.
    void editUrlRequested(const QString& name, const QString& newUrl);

    /// Remove the named remote.
    void removeRequested(const QString& name);

private:
    void setupUi();
    void onSelectionChanged();
    /// Currently-selected remote name, or empty if nothing is
    /// selected. Used by Edit URL and Remove.
    QString selectedName() const;
    /// Currently-selected URL, or empty. Pre-populated into the
    /// Edit URL prompt so the user is editing the existing value.
    QString selectedUrl() const;

    QTableWidget* table_ = nullptr;
    QPushButton* addBtn_    = nullptr;
    QPushButton* editBtn_   = nullptr;
    QPushButton* removeBtn_ = nullptr;
    QPushButton* closeBtn_  = nullptr;
};

} // namespace gitbolt::dialogs
