#include "widgets/SearchBarWidget.h"

#include <QHBoxLayout>
#include <QLabel>

namespace gitbolt::widgets {

SearchBarWidget::SearchBarWidget(QWidget* parent)
    : QWidget(parent)
    , typeCombo_(new QComboBox(this))
    , queryInput_(new QLineEdit(this))
    , fromDateEdit_(new QDateEdit(this))
    , toDateEdit_(new QDateEdit(this))
    , searchBtn_(new QPushButton(tr("Search"), this))
{
    setupUI();
}

void SearchBarWidget::setupUI() {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);

    // Search type combo
    typeCombo_->addItem(tr("Message"), static_cast<int>(SearchType::Message));
    typeCombo_->addItem(tr("Author"), static_cast<int>(SearchType::Author));
    typeCombo_->addItem(tr("Date Range"), static_cast<int>(SearchType::DateRange));
    typeCombo_->addItem(tr("File Content"), static_cast<int>(SearchType::FileContent));
    typeCombo_->addItem(tr("File Path"), static_cast<int>(SearchType::FilePath));
    typeCombo_->setToolTip(tr("Select search type"));
    layout->addWidget(typeCombo_);

    // Query text input
    queryInput_->setPlaceholderText(tr("Search..."));
    queryInput_->setClearButtonEnabled(true);
    queryInput_->setMinimumWidth(200);
    layout->addWidget(queryInput_, 1);

    // Date range pickers (hidden by default)
    auto* fromLabel = new QLabel(tr("From:"), this);
    fromLabel->setObjectName(QStringLiteral("fromLabel"));
    layout->addWidget(fromLabel);

    fromDateEdit_->setCalendarPopup(true);
    fromDateEdit_->setDate(QDate::currentDate().addMonths(-1));
    fromDateEdit_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    fromDateEdit_->setObjectName(QStringLiteral("fromDateEdit"));
    layout->addWidget(fromDateEdit_);

    auto* toLabel = new QLabel(tr("To:"), this);
    toLabel->setObjectName(QStringLiteral("toLabel"));
    layout->addWidget(toLabel);

    toDateEdit_->setCalendarPopup(true);
    toDateEdit_->setDate(QDate::currentDate());
    toDateEdit_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    toDateEdit_->setObjectName(QStringLiteral("toDateEdit"));
    layout->addWidget(toDateEdit_);

    // Search button
    searchBtn_->setIcon(QIcon::fromTheme(QStringLiteral("edit-find")));
    searchBtn_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    layout->addWidget(searchBtn_);

    // Initially hide date pickers
    updateDateVisibility();

    // Connections
    connect(typeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SearchBarWidget::onSearchTypeChanged);
    connect(searchBtn_, &QPushButton::clicked,
            this, &SearchBarWidget::onSearchClicked);
    connect(queryInput_, &QLineEdit::returnPressed,
            this, &SearchBarWidget::onReturnPressed);
}

SearchBarWidget::SearchType SearchBarWidget::currentSearchType() const {
    return static_cast<SearchType>(typeCombo_->currentData().toInt());
}

QString SearchBarWidget::queryText() const {
    return queryInput_->text().trimmed();
}

QDate SearchBarWidget::fromDate() const {
    return fromDateEdit_->date();
}

QDate SearchBarWidget::toDate() const {
    return toDateEdit_->date();
}

void SearchBarWidget::onSearchTypeChanged(int /*index*/) {
    updateDateVisibility();
}

void SearchBarWidget::updateDateVisibility() {
    bool isDateRange = (currentSearchType() == SearchType::DateRange);

    // Show/hide date widgets
    fromDateEdit_->setVisible(isDateRange);
    toDateEdit_->setVisible(isDateRange);

    // Also show/hide the labels
    auto* fromLabel = findChild<QLabel*>(QStringLiteral("fromLabel"));
    auto* toLabel = findChild<QLabel*>(QStringLiteral("toLabel"));
    if (fromLabel) fromLabel->setVisible(isDateRange);
    if (toLabel) toLabel->setVisible(isDateRange);

    // Show/hide the text input (hidden for date range)
    queryInput_->setVisible(!isDateRange);

    // Update placeholder text based on type
    switch (currentSearchType()) {
    case SearchType::Message:
        queryInput_->setPlaceholderText(tr("Search commit messages..."));
        break;
    case SearchType::Author:
        queryInput_->setPlaceholderText(tr("Search by author name or email..."));
        break;
    case SearchType::FileContent:
        queryInput_->setPlaceholderText(tr("Search file content (pickaxe)..."));
        break;
    case SearchType::FilePath:
        queryInput_->setPlaceholderText(tr("Search by file path..."));
        break;
    default:
        break;
    }
}

void SearchBarWidget::onSearchClicked() {
    executeSearch();
}

void SearchBarWidget::onReturnPressed() {
    executeSearch();
}

void SearchBarWidget::executeSearch() {
    SearchType type = currentSearchType();

    if (type == SearchType::DateRange) {
        emit searchDateRangeRequested(fromDateEdit_->date(), toDateEdit_->date());
        return;
    }

    QString query = queryInput_->text().trimmed();
    if (query.isEmpty())
        return;

    QString typeStr;
    switch (type) {
    case SearchType::Message:     typeStr = QStringLiteral("message");     break;
    case SearchType::Author:      typeStr = QStringLiteral("author");      break;
    case SearchType::FileContent: typeStr = QStringLiteral("filecontent"); break;
    case SearchType::FilePath:    typeStr = QStringLiteral("filepath");    break;
    default:                      typeStr = QStringLiteral("message");     break;
    }

    emit searchRequested(query, typeStr);
}

} // namespace gitbolt::widgets
