#pragma once

#include <QComboBox>
#include <QDateEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

namespace gitbolt::widgets {

class SearchBarWidget : public QWidget {
    Q_OBJECT
public:
    enum class SearchType {
        Message,
        Author,
        DateRange,
        FileContent,   // pickaxe (-S)
        FilePath,
    };

    explicit SearchBarWidget(QWidget* parent = nullptr);

    SearchType currentSearchType() const;
    QString queryText() const;
    QDate fromDate() const;
    QDate toDate() const;

signals:
    void searchRequested(const QString& query, const QString& type);
    void searchDateRangeRequested(const QDate& from, const QDate& to);

private slots:
    void onSearchTypeChanged(int index);
    void onSearchClicked();
    void onReturnPressed();

private:
    void setupUI();
    void updateDateVisibility();
    void executeSearch();

    QComboBox* typeCombo_ = nullptr;
    QLineEdit* queryInput_ = nullptr;
    QDateEdit* fromDateEdit_ = nullptr;
    QDateEdit* toDateEdit_ = nullptr;
    QPushButton* searchBtn_ = nullptr;
};

} // namespace gitbolt::widgets
