#pragma once

#include "widgets/CommitFilterProxy.h"

#include <QDialog>

class QCheckBox;
class QDateEdit;
class QLineEdit;

namespace gitbolt::dialogs {

/// Multi-criterion filter for the revision grid. Captures
/// message / author / SHA / date-range criteria and converts them
/// into a CommitFilterProxy::Criteria struct. The host applies
/// the result to the live proxy on the revision graph.
///
/// Each criterion has a "use this" checkbox to make "I do want
/// this empty" intentional rather than ambiguous: a blank input
/// with the box ticked still filters out everything that doesn't
/// match an empty substring (i.e. matches everything), so the
/// checkbox is what makes the field opt-in. This avoids the
/// surprise of clearing one field accidentally and unfiltering
/// the whole log.
class AdvancedFilterDialog : public QDialog {
    Q_OBJECT
public:
    explicit AdvancedFilterDialog(QWidget* parent = nullptr);

    /// Pre-populate the dialog with the proxy's current criteria.
    /// Useful when re-opening the dialog so users see what they
    /// previously set.
    void setCriteria(const widgets::CommitFilterProxy::Criteria& c);
    widgets::CommitFilterProxy::Criteria criteria() const;

private:
    void setupUi();

    QCheckBox* msgEnabled_    = nullptr;
    QLineEdit* msgEdit_       = nullptr;
    QCheckBox* authorEnabled_ = nullptr;
    QLineEdit* authorEdit_    = nullptr;
    QCheckBox* shaEnabled_    = nullptr;
    QLineEdit* shaEdit_       = nullptr;
    QCheckBox* fromEnabled_   = nullptr;
    QDateEdit* fromEdit_      = nullptr;
    QCheckBox* toEnabled_     = nullptr;
    QDateEdit* toEdit_        = nullptr;
};

} // namespace gitbolt::dialogs
