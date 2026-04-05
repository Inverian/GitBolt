#pragma once
#include <QObject>

namespace gitbolt::services {

class RepoManager : public QObject {
    Q_OBJECT
public:
    explicit RepoManager(QObject* parent = nullptr);
};

} // namespace gitbolt::services
