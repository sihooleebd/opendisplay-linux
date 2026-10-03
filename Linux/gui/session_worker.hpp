#pragma once

#include "opendisplay/session.hpp"
#include "opendisplay/types.hpp"

#include <QObject>

#include <memory>

class QTimer;

namespace od::gui {

class SessionWorker final : public QObject {
    Q_OBJECT

public:
    explicit SessionWorker(QObject* parent = nullptr);

public slots:
    void start(od::Options options);
    void stop();

signals:
    void stateChanged(const QString& status, const QString& detail,
                      bool connected, bool busy);

private slots:
    void tick();

private:
    QTimer* timer_ = nullptr;
    std::unique_ptr<od::Session> session_;
    // Both start() and tick() can reach a portal request, which runs a nested
    // event loop and keeps delivering queued calls to this thread — stop()
    // among them. While one of those frames is live, stop() must defer.
    bool inSessionCall_ = false;
    bool stopRequested_ = false;
};

}  // namespace od::gui
