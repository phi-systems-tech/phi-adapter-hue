// adapters/api/adapterinterface.h
#pragma once

#include <QObject>
#include <QVariant>
#include <QLoggingCategory>
#include <QThread>
#include <QDateTime>
#include <QJsonObject>

#include "device.h"
#include "channel.h"
#include "room.h"
#include "group.h"
#include "types.h"
#include "scene.h"
#include "adapterconfig.h"

Q_DECLARE_LOGGING_CATEGORY(adapterLog)

namespace phicore {

class AdapterManager;

class AdapterInterface : public QObject
{
    Q_OBJECT
    friend class AdapterManager;

public:
    explicit AdapterInterface(QObject *parent = nullptr) : QObject(parent) {}
    ~AdapterInterface() override { qCDebug(adapterLog) << "~AdapterInterface() -" << m_info.id; }

signals:
    void connectionStateChanged(bool connected);
    void errorOccurred(const QString &msg, const QVariantList &params = {}, const QString &ctx = {});
    void fullSyncCompleted();

    void roomUpdated(const phicore::Room &room);
    void roomRemoved(const QString &roomId);
    void groupUpdated(const phicore::Group &group);
    void groupRemoved(const QString &groupId);

    void deviceUpdated(const phicore::Device &device, const phicore::ChannelList &channels);
    void deviceRemoved(const QString &deviceId);

    void channelUpdated(const QString &deviceId, const phicore::Channel &channel);
    void channelRemoved(const QString &deviceId, const QString &channelId);

    void scenesUpdated(const QList<phicore::Scene> &scenes);

    void channelStateUpdated(const QString &deviceId, const QString &channelId,
        const QVariant &value, qint64 ts = 0);

    void cmdResult(const phicore::CmdResponse &response);
    void actionResult(const phicore::ActionResponse &response);
    void adapterMetaUpdated(const QJsonObject &metaPatch);

    // automatically called from startAsync, don't use it - implement start(errorString) only
    void started(bool ok, const QString &errorString);

protected:
    // Initialize and start connections.
    //
    // Called from the adapter's own thread (via startAsync), after:
    //  - AdapterManager has set adapter metadata (m_info)
    //  - and moved the adapter to its dedicated QThread.
    //
    // Use adapter().host / ip / port / user / pw / appKey / meta for your config.
    virtual bool start(QString &errorString) = 0;

    // Stop connections (MQTT, HTTP, ...).
    virtual void stop() = 0;

    const Adapter &adapter() const noexcept { return m_info; }

protected slots:
    // Called when adapter config metadata changes.
    // Default behavior triggers a full sync.
    virtual void adapterConfigUpdated() { requestFullSync(); }

    // Trigger a full sync of devices/channels from the remote system.
    // must emit fullSyncCompleted() when finished
    virtual void requestFullSync() = 0;

    // Must emit a CmdResponse for every command, even if the adapter itself
    // does not support the requested operation.
    //
    // Semantics:
    //  - updateChannelState is responsible for executing a command on a device
    //    and reporting the outcome via cmdResult(CmdResponse).
    //  - It MUST NOT emit channelStateUpdated(...) directly as a shortcut for
    //    state changes caused by this command. Channel state updates should
    //    always come from the normal data flow (device notifications, polling,
    //    etc.) so ChannelRegistry can apply de-duplication and history
    //    handling consistently.
    virtual void updateChannelState(const QString &deviceExternalId,
            const QString &channelExternalId, const QVariant &value, CmdId cmdId) {
        CmdResponse cr {
            .id     = cmdId,
            .status = CmdStatus::NotImplemented,
            .error  = QStringLiteral("AdapterInterface not available"),
            .tsMs   = QDateTime::currentMSecsSinceEpoch()
        };
        emit cmdResult(cr);
    }

    // Optional: propagate user-facing device name changes back to the adapter.
    // Default implementation is a no-op. Adapters that support renaming should
    // override this to call the respective remote API.
    virtual void updateDeviceName(const QString &deviceId, const QString &name, CmdId cmdId) {
        Q_UNUSED(deviceId);
        Q_UNUSED(name);
        if (cmdId == 0)
            return;
        CmdResponse cr {
            .id     = cmdId,
            .status = CmdStatus::NotImplemented,
            .error  = QStringLiteral("Device rename not supported"),
            .tsMs   = QDateTime::currentMSecsSinceEpoch()
        };
        emit cmdResult(cr);
    }

    // Optional adapter-level actions (e.g. re-sync, diagnostics).
    // Default implementation reports NotImplemented.
    virtual void invokeAdapterAction(const QString &actionId, const QJsonObject &params, CmdId cmdId) {
        if (actionId == QLatin1String("settings")) {
            emit adapterMetaUpdated(params);
            if (cmdId == 0)
                return;
            ActionResponse cr {
                .id     = cmdId,
                .status = CmdStatus::Success,
                .error  = QString(),
                .tsMs   = QDateTime::currentMSecsSinceEpoch()
            };
            emit actionResult(cr);
            return;
        }
        Q_UNUSED(params);
        if (cmdId == 0)
            return;
        ActionResponse cr {
            .id     = cmdId,
            .status = CmdStatus::NotImplemented,
            .error  = QStringLiteral("AdapterInterface action not supported"),
            .tsMs   = QDateTime::currentMSecsSinceEpoch()
        };
        emit actionResult(cr);
    }

    virtual void invokeDeviceEffect(const QString &deviceExternalId,
                                    phicore::DeviceEffect effect,
                                    const QString &effectId,
                                    const QJsonObject &params,
                                    CmdId cmdId) {
        Q_UNUSED(deviceExternalId);
        Q_UNUSED(effect);
        Q_UNUSED(effectId);
        Q_UNUSED(params);
        if (cmdId == 0)
            return;
        CmdResponse cr {
            .id     = cmdId,
            .status = CmdStatus::NotImplemented,
            .error  = QStringLiteral("Device effect not supported"),
            .tsMs   = QDateTime::currentMSecsSinceEpoch()
        };
        emit cmdResult(cr);
    }

    virtual void invokeScene(const QString &sceneExternalId,
                             const QString &groupExternalId,
                             const QString &action,
                             CmdId cmdId) {
        Q_UNUSED(sceneExternalId);
        Q_UNUSED(groupExternalId);
        Q_UNUSED(action);
        if (cmdId == 0)
            return;
        CmdResponse cr {
            .id     = cmdId,
            .status = CmdStatus::NotImplemented,
            .error  = QStringLiteral("Scene invocation not supported"),
            .tsMs   = QDateTime::currentMSecsSinceEpoch()
        };
        emit cmdResult(cr);
    }

    // AdapterManager can provide static adapter config (mapping tables, etc.).
    // Adapters may override this and store the config as needed.
    virtual void updateStaticConfig(const QJsonObject &config) {
        Q_UNUSED(config);
    }

private slots:
    // Called from AdapterManager via queued connection
    void startAsync() {
        QString err;
        emit connectionStateChanged(false);
        const bool ok = start(err);
        emit started(ok, err);
    }
    // Set by AdapterManager before thread start
    void setAdapter(const phicore::Adapter &info) { m_info = info; }

private:
    Adapter m_info;
};

} // namespace phicore
