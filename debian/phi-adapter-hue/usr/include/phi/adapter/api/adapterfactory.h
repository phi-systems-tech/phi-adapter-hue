// adapters/api/adapterfactory.h
#pragma once

#include <QObject>
#include <QList>
#include <QJsonObject>

#include "adapterconfig.h"
#include "discovery.h"
#include "discoveryquery.h"

#define PHI_ADAPTER_FACTORY_IID "tech.phi-systems.phi-core.AdapterFactory/1.0"

namespace phicore {

class AdapterInterface;

class AdapterFactory : public QObject
{
    Q_OBJECT

public:
    AdapterFactory(QObject *parent = nullptr) : QObject(parent) {}
    ~AdapterFactory() override = default;

    // ---------------------------------------------------------------------
    // Static plugin infos (for UI, logs, ...)
    // ---------------------------------------------------------------------
    virtual QString pluginType()  const = 0;   // "hue", "z2m", "matter"
    virtual QString displayName() const = 0;  // "Philips Hue"
    virtual QString apiVersion()  const { return QStringLiteral("1.0.0"); }
    virtual QString description() const { return QStringLiteral("n/a"); }
    virtual QByteArray icon()     const { return QByteArray(); }
    virtual QByteArray image()    const { return QByteArray(); }
    virtual int deviceTimeout()   const { return static_cast<int>(5000); }
    virtual int maxInstances() const { return 5; }
    virtual QString loggingCategory() const { return QStringLiteral("phi-core.adapters.%1").arg(pluginType()); }

    // ---------------------------------------------------------------------
    // Capabilities / requirements
    // ---------------------------------------------------------------------
    // UI can use this to show "what is needed" before schema is fetched.
    virtual AdapterCapabilities capabilities() const {
        AdapterCapabilities caps;
        caps.required          = AdapterRequirement::None;
        caps.optional          = AdapterRequirement::None;
        return caps;
    }

    // ---------------------------------------------------------------------
    // Discovery
    // ---------------------------------------------------------------------
    // Discover possible adapter instances on the network / system if "Manual"
    // is chosen for discoveryQueries()
    //
    // Each entry should contain at least:
    //  - pluginType
    //  - discoveredId (stable instance id, e.g. bridge id / MAC)
    //  - label (default display name)
    //
    // Optional prefilled fields:
    //  - hostname / ip / port
    //  - meta[...] (additional hints for configSchema)
    virtual discovery::DiscoveryList discover() const { return {}; }

    // let core discover via mDNS, ssdp, ..., but give hints
    virtual discovery::DiscoveryQueryList discoveryQueries() const { return {}; }
    // optional blocking probe usually from a worker thread
    virtual bool verifyCandidate(Adapter &io, QString &err) const { return false; }

    // ---------------------------------------------------------------------
    // Configuration schema
    // ---------------------------------------------------------------------
    // Return configuration schema for a given candidate.
    //
    // 'info' contains pluginType + externalId and any discovery metadata
    // (e.g. host, default name, meta fields).
    virtual AdapterConfigSchema configSchema(const Adapter &info) const {
        Q_UNUSED(info);
        return {};
    }

    // Standard connection test for UI "Test connection" actions.
    // Default behavior delegates to the factory "probe" action.
    virtual ActionResponse invokeTestConnection(Adapter &infoInOut) const {
        return invokeFactoryAction(QStringLiteral("probe"), infoInOut, QJsonObject());
    }

    // ---------------------------------------------------------------------
    // Factory-level actions (e.g. probe/pairing) invoked before an adapter
    // instance exists. Default implementation reports "unsupported".
    virtual ActionResponse invokeFactoryAction(const QString &actionId,
                                               Adapter &infoInOut,
                                               const QJsonObject &params) const
    {
        Q_UNUSED(actionId);
        Q_UNUSED(infoInOut);
        Q_UNUSED(params);
        ActionResponse resp;
        resp.status = CmdStatus::NotImplemented;
        resp.error = QStringLiteral("Factory action not supported");
        return resp;
    }

    // ---------------------------------------------------------------------
    // AdapterInterface instance creation
    // ---------------------------------------------------------------------
    // Create the actual adapter instance with the given parent.
    //
    // AdapterManager will:
    //  - call factory->create(...)
    //  - call adapter->setAdapter(info) (friend)
    //  - move the adapter to its dedicated QThread
    //  - trigger startAsync() in that thread
    virtual AdapterInterface* create(QObject *parent = nullptr) = 0;
};

} // namespace phicore

Q_DECLARE_INTERFACE(phicore::AdapterFactory, PHI_ADAPTER_FACTORY_IID)
