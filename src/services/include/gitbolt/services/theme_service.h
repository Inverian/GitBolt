#pragma once
#include <QObject>

namespace gitbolt::services {

class ThemeService : public QObject {
    Q_OBJECT
public:
    explicit ThemeService(QObject* parent = nullptr);
};

} // namespace gitbolt::services
