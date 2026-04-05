#pragma once
#include <QObject>

namespace gitbolt::services {

class SearchService : public QObject {
    Q_OBJECT
public:
    explicit SearchService(QObject* parent = nullptr);
};

} // namespace gitbolt::services
