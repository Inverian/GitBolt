#pragma once
#include <QObject>

namespace gitbolt::conf {

class ThemeService : public QObject {
    Q_OBJECT
public:
    explicit ThemeService(QObject* parent = nullptr);
};

} // namespace gitbolt::conf
