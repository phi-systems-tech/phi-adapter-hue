#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include "hue_http.h"
#include "hue_probe.h"
#include "hue_schema.h"
#include "hue_sidecar.h"
#include "phi/adapter/sdk/qt/instance_execution_backend_qt.h"
#include "phi/adapter/sdk/sidecar.h"

namespace {

namespace sdk = phicore::adapter::sdk;
namespace v1 = phicore::adapter::v1;
using phicore::hue::ipc::ConnectionSettings;
using phicore::hue::ipc::HttpClient;

std::atomic_bool g_running{true};

void handleSignal(int)
{
    g_running.store(false);
}

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

QJsonObject parseJsonObject(const std::string &json)
{
    const QByteArray bytes = QByteArray::fromStdString(json).trimmed();
    if (bytes.isEmpty())
        return {};

    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject())
        return {};
    return doc.object();
}

ConnectionSettings settingsFromAdapter(const v1::Adapter &adapter)
{
    ConnectionSettings settings;
    settings.host = QString::fromStdString(adapter.host).trimmed();
    settings.ip = QString::fromStdString(adapter.ip).trimmed();
    settings.port = static_cast<int>(adapter.port);
    settings.appKey = QString::fromStdString(adapter.token).trimmed();

    const QJsonObject meta = parseJsonObject(adapter.metaJson);
    if (meta.contains(QStringLiteral("host")))
        settings.host = meta.value(QStringLiteral("host")).toString().trimmed();
    if (meta.contains(QStringLiteral("ip")))
        settings.ip = meta.value(QStringLiteral("ip")).toString().trimmed();
    if (meta.contains(QStringLiteral("port")))
        settings.port = meta.value(QStringLiteral("port")).toInt(settings.port);
    if (meta.contains(QStringLiteral("appKey")))
        settings.appKey = meta.value(QStringLiteral("appKey")).toString().trimmed();

    if (meta.contains(QStringLiteral("useTls"))) {
        settings.useTls = meta.value(QStringLiteral("useTls")).toBool(true);
    } else {
        const bool flagTls = v1::hasFlag(adapter.flags, v1::AdapterFlag::UseTls);
        if (flagTls)
            settings.useTls = true;
        else if (settings.port > 0)
            settings.useTls = (settings.port == 443);
        else
            settings.useTls = true;
    }

    if (settings.port <= 0)
        settings.port = settings.useTls ? 443 : 80;

    return settings;
}

void applyProbeParams(const QJsonObject &params, ConnectionSettings *settings)
{
    if (!settings)
        return;
    if (params.contains(QStringLiteral("host")))
        settings->host = params.value(QStringLiteral("host")).toString().trimmed();
    if (params.contains(QStringLiteral("ip")))
        settings->ip = params.value(QStringLiteral("ip")).toString().trimmed();
    if (params.contains(QStringLiteral("port")))
        settings->port = params.value(QStringLiteral("port")).toInt(settings->port);
    if (params.contains(QStringLiteral("useTls")))
        settings->useTls = params.value(QStringLiteral("useTls")).toBool(settings->useTls);
    if (params.contains(QStringLiteral("appKey")))
        settings->appKey = params.value(QStringLiteral("appKey")).toString().trimmed();
}

class HueFactory final : public sdk::AdapterFactory
{
protected:
    void onBootstrap(const sdk::BootstrapRequest &request) override
    {
        m_factorySettings = settingsFromAdapter(request.adapter);
    }

    void onFactoryConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        m_factorySettings = settingsFromAdapter(request.adapter);
    }

    void onFactoryActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        submitFactoryActionResult(handleFactoryAction(request), "factory.action.invoke");
    }

    v1::Utf8String pluginType() const override
    {
        return phicore::hue::ipc::kPluginType;
    }

    v1::Utf8String displayName() const override
    {
        return phicore::hue::ipc::displayName();
    }

    v1::Utf8String description() const override
    {
        return phicore::hue::ipc::description();
    }

    v1::Utf8String iconSvg() const override
    {
        return phicore::hue::ipc::iconSvg();
    }

    v1::Utf8String apiVersion() const override
    {
        return "1.0.0";
    }

    int timeoutMs() const override
    {
        return 10000;
    }

    int maxInstances() const override
    {
        return 0;
    }

    v1::AdapterCapabilities capabilities() const override
    {
        return phicore::hue::ipc::capabilities();
    }

    v1::JsonText configSchemaJson() const override
    {
        return phicore::hue::ipc::configSchemaJson();
    }

    std::unique_ptr<sdk::InstanceExecutionBackend> createInstanceExecutionBackend(
        const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        return sdk::qt::createInstanceExecutionBackend();
    }

    std::unique_ptr<sdk::AdapterInstance> createInstance(const sdk::ExternalId &externalId) override
    {
        std::cerr << "create hue instance externalId=" << externalId << '\n';
        return std::make_unique<phicore::hue::ipc::HueAdapterInstance>();
    }

private:
    v1::ActionResponse handleFactoryAction(const sdk::AdapterActionInvokeRequest &request)
    {
        v1::ActionResponse response;
        response.id = request.cmdId;
        response.tsMs = nowMs();

        if (request.actionId != "probe") {
            response.status = v1::CmdStatus::NotSupported;
            response.error = "Unsupported factory action";
            return response;
        }

        ConnectionSettings settings = m_factorySettings;
        if (!request.paramsJson.empty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(request.paramsJson));
            if (doc.isObject())
                applyProbeParams(doc.object(), &settings);
        }

        const phicore::hue::ipc::ProbeResult probe = phicore::hue::ipc::runProbe(m_http, settings, 10000);
        if (!probe.ok) {
            response.status = v1::CmdStatus::Failure;
            response.error = probe.error.toStdString();
            response.resultType = v1::ActionResultType::None;
            return response;
        }

        if (!probe.metaPatch.isEmpty()) {
            const QByteArray patch = QJsonDocument(probe.metaPatch).toJson(QJsonDocument::Compact);
            v1::Utf8String patchError;
            if (!sendAdapterMetaUpdated(patch.toStdString(), &patchError))
                std::cerr << "hue-ipc failed to send adapterMetaUpdated(probe): " << patchError << '\n';
        }

        response.status = v1::CmdStatus::Success;
        if (!probe.appKey.isEmpty()) {
            response.resultType = v1::ActionResultType::String;
            response.resultValue = probe.appKey.toStdString();
        } else if (!probe.message.isEmpty()) {
            response.resultType = v1::ActionResultType::String;
            response.resultValue = probe.message.toStdString();
        } else {
            response.resultType = v1::ActionResultType::None;
        }
        return response;
    }

    void submitFactoryActionResult(v1::ActionResponse response, const char *context)
    {
        v1::Utf8String error;
        if (!sendResult(response, &error))
            std::cerr << "failed to send " << context << " result: " << error << '\n';
    }

    QNetworkAccessManager m_probeNetwork;
    HttpClient m_http{&m_probeNetwork};
    ConnectionSettings m_factorySettings;
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    const char *envSocketPath = std::getenv("PHI_ADAPTER_SOCKET_PATH");
    const v1::Utf8String socketPath = (argc > 1)
        ? argv[1]
        : (envSocketPath ? envSocketPath : v1::Utf8String("/tmp/phi-adapter-hue-ipc.sock"));

    std::cerr << "starting phi_adapter_hue_ipc for pluginType=" << phicore::hue::ipc::kPluginType
              << " socket=" << socketPath << '\n';

    HueFactory factory;
    sdk::SidecarHost host(socketPath, factory);

    v1::Utf8String error;
    if (!host.start(&error)) {
        std::cerr << "failed to start sidecar host: " << error << '\n';
        return 1;
    }

    constexpr std::chrono::milliseconds kPollTimeout{16};

    QTimer hostPollTimer;
    QObject::connect(&hostPollTimer, &QTimer::timeout, [&]() {
        if (!g_running.load(std::memory_order_relaxed)) {
            app.quit();
            return;
        }

        if (!host.pollOnce(kPollTimeout, &error)) {
            std::cerr << "poll failed: " << error << '\n';
        }
    });
    hostPollTimer.start(16);

    int execResult = app.exec();
    hostPollTimer.stop();

    host.stop();
    std::cerr << "stopping phi_adapter_hue_ipc" << '\n';
    return execResult;
}
