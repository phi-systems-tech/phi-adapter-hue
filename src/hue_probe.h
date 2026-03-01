#pragma once

#include <QJsonObject>
#include <QString>

#include "hue_http.h"

namespace phicore::hue::ipc {

struct ProbeResult {
    bool ok = false;
    QString error;
    QString message;
    QString appKey;
    QJsonObject metaPatch;
};

ProbeResult runProbe(HttpClient &http,
                     const ConnectionSettings &settings,
                     int timeoutMs = 10000);

} // namespace phicore::hue::ipc
