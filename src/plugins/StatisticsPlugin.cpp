#include "plugins/StatisticsPlugin.h"
#include "git/Repository.h"

#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <unordered_map>

namespace gitbolt::plugins {

// =========================================================================
// Helper: chart widgets painted via QPainter
// =========================================================================

namespace {

/// A simple bar chart widget.
class BarChartWidget : public QWidget {
public:
    struct Bar {
        QString label;
        int value = 0;
    };

    explicit BarChartWidget(QVector<Bar> bars, QWidget* parent = nullptr)
        : QWidget(parent), bars_(std::move(bars))
    {
        setMinimumHeight(static_cast<int>(bars_.size()) * 28 + 20);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (bars_.isEmpty())
            return;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        int maxVal = 0;
        for (const auto& b : bars_)
            maxVal = std::max(maxVal, b.value);
        if (maxVal == 0)
            maxVal = 1;

        const int labelWidth = 140;
        const int barHeight = 20;
        const int spacing = 6;
        const int maxBarWidth = width() - labelWidth - 60;

        QColor barColor = palette().color(QPalette::Highlight);
        QColor textColor = palette().color(QPalette::WindowText);

        int y = 10;
        for (const auto& b : bars_) {
            p.setPen(textColor);
            p.drawText(QRect(4, y, labelWidth - 8, barHeight), Qt::AlignRight | Qt::AlignVCenter,
                       b.label);

            int bw = static_cast<int>(static_cast<double>(b.value) / maxVal * maxBarWidth);
            p.setBrush(barColor);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(labelWidth, y, std::max(bw, 2), barHeight, 3, 3);

            p.setPen(textColor);
            p.drawText(QRect(labelWidth + bw + 4, y, 50, barHeight),
                       Qt::AlignLeft | Qt::AlignVCenter, QString::number(b.value));

            y += barHeight + spacing;
        }
    }

private:
    QVector<Bar> bars_;
};

/// A simple line chart widget (commits over time by month).
class LineChartWidget : public QWidget {
public:
    struct Point {
        QString label;
        int value = 0;
    };

    explicit LineChartWidget(QVector<Point> points, QWidget* parent = nullptr)
        : QWidget(parent), points_(std::move(points))
    {
        setMinimumHeight(200);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (points_.size() < 2)
            return;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const int leftMargin = 50;
        const int bottomMargin = 40;
        const int topMargin = 10;
        const int rightMargin = 10;
        const int chartW = width() - leftMargin - rightMargin;
        const int chartH = height() - topMargin - bottomMargin;

        int maxVal = 0;
        for (const auto& pt : points_)
            maxVal = std::max(maxVal, pt.value);
        if (maxVal == 0)
            maxVal = 1;

        QColor lineColor = palette().color(QPalette::Highlight);
        QColor axisColor = palette().color(QPalette::Mid);
        QColor textColor = palette().color(QPalette::WindowText);

        // Axes
        p.setPen(axisColor);
        p.drawLine(leftMargin, topMargin, leftMargin, topMargin + chartH);
        p.drawLine(leftMargin, topMargin + chartH, leftMargin + chartW, topMargin + chartH);

        // Y-axis labels (5 ticks)
        p.setPen(textColor);
        for (int i = 0; i <= 4; ++i) {
            int yVal = maxVal * i / 4;
            int yPos = topMargin + chartH - static_cast<int>(static_cast<double>(i) / 4 * chartH);
            p.drawText(QRect(0, yPos - 8, leftMargin - 6, 16),
                       Qt::AlignRight | Qt::AlignVCenter, QString::number(yVal));
        }

        // Build polyline
        QVector<QPointF> poly;
        double step = static_cast<double>(chartW) / (points_.size() - 1);
        for (int i = 0; i < points_.size(); ++i) {
            double x = leftMargin + i * step;
            double y = topMargin + chartH
                       - static_cast<double>(points_[i].value) / maxVal * chartH;
            poly.append(QPointF(x, y));
        }

        p.setPen(QPen(lineColor, 2));
        p.drawPolyline(poly.data(), poly.size());

        // Draw dots
        p.setBrush(lineColor);
        p.setPen(Qt::NoPen);
        for (const auto& pt : poly)
            p.drawEllipse(pt, 3, 3);

        // X-axis labels (show every Nth)
        p.setPen(textColor);
        int labelStep = std::max(1, static_cast<int>(points_.size() / 8));
        for (int i = 0; i < static_cast<int>(points_.size()); i += labelStep) {
            double x = leftMargin + i * step;
            p.save();
            p.translate(x, topMargin + chartH + 4);
            p.rotate(45);
            p.drawText(0, 0, points_[i].label);
            p.restore();
        }
    }

private:
    QVector<Point> points_;
};

} // anonymous namespace

// =========================================================================
// StatisticsPlugin
// =========================================================================

StatisticsPlugin::StatisticsPlugin(QObject* parent)
    : QObject(parent)
    , action_(new QAction(tr("Repository Statistics"), this))
{
    connect(action_, &QAction::triggered, this, &StatisticsPlugin::showStatistics);
}

QString StatisticsPlugin::name() const
{
    return QStringLiteral("Statistics");
}

QString StatisticsPlugin::description() const
{
    return tr("Shows commit statistics for the current repository.");
}

QString StatisticsPlugin::version() const
{
    return QStringLiteral("1.0.0");
}

bool StatisticsPlugin::initialize(PluginContext* context)
{
    ctx_ = context;
    if (ctx_)
        ctx_->registerMenuItem(QStringLiteral("Plugins"), action_);
    return true;
}

void StatisticsPlugin::shutdown()
{
    ctx_ = nullptr;
}

QList<QAction*> StatisticsPlugin::menuActions()
{
    return {action_};
}

// ---------------------------------------------------------------------------
// Statistics computation & dialog
// ---------------------------------------------------------------------------

void StatisticsPlugin::showStatistics()
{
    if (!ctx_ || !ctx_->activeRepository()) {
        if (ctx_)
            ctx_->showNotification(tr("No repository is open."));
        return;
    }

    auto* repo = ctx_->activeRepository();

    // Walk the entire commit log
    auto walkResult = repo->createRevWalk();
    if (!walkResult) {
        if (ctx_)
            ctx_->showNotification(tr("Failed to create revision walk."));
        return;
    }

    auto& walk = walkResult.value();
    walk.pushHead();
    walk.setSorting(git::SortOrder::Time);

    auto commitsResult = walk.all();
    if (!commitsResult) {
        if (ctx_)
            ctx_->showNotification(tr("Failed to read commits."));
        return;
    }

    const auto& commits = commitsResult.value();

    // Aggregate data
    std::unordered_map<std::string, int> commitsPerAuthor;
    std::map<std::string, int> commitsPerMonth; // YYYY-MM -> count
    std::unordered_map<std::string, int> fileChangeCounts;
    int totalCommits = static_cast<int>(commits.size());

    for (const auto& c : commits) {
        commitsPerAuthor[c.author.name]++;

        auto tp = c.author.when;
        auto tt = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &tt);
#else
        gmtime_r(&tt, &tm);
#endif
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04d-%02d", tm.tm_year + 1900, tm.tm_mon + 1);
        commitsPerMonth[std::string(buf)]++;
    }

    // Compute file change counts from diffs of the most recent N commits
    // (walking all can be slow; sample up to 500)
    int sampleSize = std::min(totalCommits, 500);
    for (int i = 0; i < sampleSize; ++i) {
        auto diffResult = repo->diffCommit(commits[i].id);
        if (!diffResult)
            continue;
        for (const auto& file : diffResult->files)
            fileChangeCounts[file.path()]++;
    }

    // Sort authors by count descending, take top 15
    QVector<BarChartWidget::Bar> authorBars;
    {
        std::vector<std::pair<std::string, int>> sorted(commitsPerAuthor.begin(),
                                                         commitsPerAuthor.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        int limit = std::min(static_cast<int>(sorted.size()), 15);
        for (int i = 0; i < limit; ++i)
            authorBars.append({QString::fromStdString(sorted[i].first), sorted[i].second});
    }

    // Commits over time
    QVector<LineChartWidget::Point> timePoints;
    for (const auto& [month, count] : commitsPerMonth)
        timePoints.append({QString::fromStdString(month), count});

    // Top changed files (top 20)
    QVector<std::pair<QString, int>> topFiles;
    {
        std::vector<std::pair<std::string, int>> sorted(fileChangeCounts.begin(),
                                                         fileChangeCounts.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        int limit = std::min(static_cast<int>(sorted.size()), 20);
        for (int i = 0; i < limit; ++i)
            topFiles.append({QString::fromStdString(sorted[i].first), sorted[i].second});
    }

    // Branch and tag counts
    int branchCount = 0;
    int tagCount = 0;
    if (auto br = repo->allBranches())
        branchCount = static_cast<int>(br->size());
    if (auto tg = repo->tags())
        tagCount = static_cast<int>(tg->size());

    // -- Build dialog --
    QWidget* parentWidget = QApplication::activeWindow();
    auto* dlg = new QDialog(parentWidget);
    dlg->setWindowTitle(tr("Repository Statistics"));
    dlg->resize(700, 520);
    dlg->setAttribute(Qt::WA_DeleteOnClose);

    auto* mainLayout = new QVBoxLayout(dlg);

    // Summary row
    auto* summaryRow = new QHBoxLayout;
    auto addStat = [&](const QString& label, int value) {
        auto* box = new QVBoxLayout;
        auto* valLabel = new QLabel(QString::number(value), dlg);
        valLabel->setAlignment(Qt::AlignCenter);
        valLabel->setStyleSheet(QStringLiteral("font-size: 22pt; font-weight: bold;"));
        box->addWidget(valLabel);
        auto* nameLabel = new QLabel(label, dlg);
        nameLabel->setAlignment(Qt::AlignCenter);
        nameLabel->setStyleSheet(QStringLiteral("color: gray;"));
        box->addWidget(nameLabel);
        summaryRow->addLayout(box);
    };
    addStat(tr("Total Commits"), totalCommits);
    addStat(tr("Authors"), static_cast<int>(commitsPerAuthor.size()));
    addStat(tr("Branches"), branchCount);
    addStat(tr("Tags"), tagCount);
    mainLayout->addLayout(summaryRow);
    mainLayout->addSpacing(12);

    // Tabs
    auto* tabs = new QTabWidget(dlg);

    // Authors tab
    {
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        auto* chart = new BarChartWidget(authorBars);
        scroll->setWidget(chart);
        tabs->addTab(scroll, tr("Commits per Author"));
    }

    // Timeline tab
    {
        auto* chart = new LineChartWidget(timePoints);
        tabs->addTab(chart, tr("Commits over Time"));
    }

    // Top files tab
    {
        auto* table = new QTableWidget(topFiles.size(), 2, dlg);
        table->setHorizontalHeaderLabels({tr("File"), tr("Changes")});
        table->horizontalHeader()->setStretchLastSection(true);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        for (int i = 0; i < topFiles.size(); ++i) {
            table->setItem(i, 0, new QTableWidgetItem(topFiles[i].first));
            auto* countItem = new QTableWidgetItem(QString::number(topFiles[i].second));
            countItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            table->setItem(i, 1, countItem);
        }
        tabs->addTab(table, tr("Top Changed Files"));
    }

    mainLayout->addWidget(tabs, 1);
    dlg->show();
}

} // namespace gitbolt::plugins
