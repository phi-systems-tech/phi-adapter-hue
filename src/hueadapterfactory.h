// adapters/plugins/hue/hueadapterfactory.h
#include "adapterfactory.h"
#include "discoveryquery.h"

namespace phicore::adapter {

class HueAdapterFactory : public AdapterFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PHI_ADAPTER_FACTORY_IID)
    Q_INTERFACES(phicore::adapter::AdapterFactory)

public:
    HueAdapterFactory(QObject *parent = nullptr) : AdapterFactory(parent) {}

    QString pluginType()  const override { return QStringLiteral("hue"); }
    QString displayName() const override { return QStringLiteral("Philips Hue"); }
    QString apiVersion()  const override { return QStringLiteral("1.0.0"); }
    QString description() const override { return QStringLiteral("Provides devices for Philips HUE bridge"); }
    QByteArray icon() const override;

    AdapterCapabilities            capabilities() const override;
    discovery::DiscoveryList       discover() const override;
    discovery::DiscoveryQueryList  discoveryQueries() const override;
    AdapterConfigSchema            configSchema(const Adapter &info) const override;
    ActionResponse invokeFactoryAction(const QString &actionId, Adapter &infoInOut,
        const QJsonObject &params) const override;
    AdapterInterface              *create(QObject *parent = nullptr) override;
};

} // namespace phicore::adapter
