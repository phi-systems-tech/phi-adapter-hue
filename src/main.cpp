// Process entry point for the Hue sidecar: the adapter factory and the SDK's
// own main loop. The bridge runtime lives in hue_instance; the conversion in
// hue_model; the settings and the probe in hue_settings and hue_probe.

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "phi/adapter/net/http_client.h"
#include "phi/adapter/sdk/loop_execution_backend.h"
#include "phi/adapter/sdk/sidecar.h"
#include "phi/runtime/loop.h"

#include "hue_instance.h"
#include "hue_json.h"
#include "hue_probe.h"
#include "hue_schema.h"
#include "hue_settings.h"

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;
namespace net = phicore::adapter::net;

using namespace phicore::hue::ipc;

namespace {

class HueFactory final : public sdk::AdapterFactory
{
protected:
    void onBootstrap(const sdk::BootstrapRequest &request) override
    {
        m_settings = settingsFromAdapter(request.adapter, parseObject(request.adapter.metaJson));
    }

    void onFactoryConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        m_settings = settingsFromAdapter(request.adapter, parseObject(request.adapter.metaJson));
    }

    // The probe is an HTTPS round trip with a ten second budget; on the host
    // poll thread it froze IPC for every instance of this sidecar. A loop
    // backend, because the HTTP client needs somewhere to watch a descriptor.
    std::unique_ptr<sdk::InstanceExecutionBackend> createFactoryExecutionBackend() override
    {
        return sdk::createLoopExecutionBackend("hue-factory");
    }

    std::unique_ptr<sdk::InstanceExecutionBackend> createInstanceExecutionBackend(
        const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        return sdk::createLoopExecutionBackend("hue-instance");
    }

    v1::Utf8String pluginType() const override { return kPluginType; }
    v1::Utf8String displayName() const override { return phicore::hue::ipc::displayName(); }
    v1::Utf8String description() const override { return phicore::hue::ipc::description(); }
    v1::Utf8String iconSvg() const override { return phicore::hue::ipc::iconSvg(); }
    v1::Utf8String apiVersion() const override { return "1.0.0"; }
    int timeoutMs() const override { return 10000; }
    int maxInstances() const override { return 0; }

    v1::AdapterCapabilities capabilities() const override
    {
        return phicore::hue::ipc::capabilities();
    }

    std::optional<v1::AdapterConfigSchema> configSchema() const override
    {
        return phicore::hue::ipc::configSchema();
    }

    std::unique_ptr<sdk::AdapterInstance> createInstance(const sdk::ExternalId &externalId) override
    {
        std::cerr << "create hue instance externalId=" << externalId << '\n';
        return makeInstance();
    }

    void onFactoryActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        if (request.actionId != "probe") {
            answer(request.cmdId, v1::CmdStatus::NotSupported, "Unsupported factory action");
            return;
        }
        ConnectionSettings settings = m_settings;
        applyProbeParams(parseObject(request.paramsJson), settings);
        if (settings.caFile.empty())
            settings.caFile = bundledCaFile();

        if (!m_http) {
            phi::runtime::Loop *loop = phi::runtime::Loop::current();
            if (loop == nullptr) {
                answer(request.cmdId, v1::CmdStatus::Failure, "No loop on the factory thread");
                return;
            }
            m_http.emplace(*loop);
        }
        std::cerr << "hue-ipc probe " << settings.baseUrl()
                  << " keySet=" << (settings.appKey.empty() ? "false" : "true") << '\n';

        const v1::CmdId cmdId = request.cmdId;
        runProbe(*m_http, settings, [this, cmdId](ProbeOutcome outcome) {
            v1::ActionResponse response;
            response.id = cmdId;
            response.tsMs = 0;
            if (!outcome.ok) {
                response.status = v1::CmdStatus::Failure;
                response.error = outcome.error;
                response.resultType = v1::ActionResultType::None;
                send(response);
                return;
            }
            // Factory-scope meta updates are not part of the contract; what
            // pairing produced travels back as form values on the answer.
            response.formValues = std::move(outcome.formValues);
            response.status = v1::CmdStatus::Success;
            response.resultType = v1::ActionResultType::String;
            response.resultValue = outcome.appKey.empty() ? outcome.message : outcome.appKey;
            send(response);
        });
    }

    /// The client belongs to the factory backend's loop; this is the last
    /// callback that still runs on it.
    void onFactoryStopping() override { m_http.reset(); }

private:
    void answer(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error)
    {
        v1::ActionResponse response;
        response.id = cmdId;
        response.status = status;
        response.error = error;
        response.resultType = v1::ActionResultType::None;
        send(response);
    }

    void send(const v1::ActionResponse &response)
    {
        v1::Utf8String error;
        if (!sendResult(response, &error))
            std::cerr << "failed to send factory.action.invoke result: " << error << '\n';
    }

    std::optional<net::HttpClient> m_http;
    ConnectionSettings m_settings;
};

} // namespace

int main(int argc, char **argv)
{
    const char *envSocketPath = std::getenv("PHI_ADAPTER_SOCKET_PATH");
    const v1::Utf8String socketPath = (argc > 1)
        ? argv[1]
        : (envSocketPath ? envSocketPath : v1::Utf8String("/tmp/phi-adapter-hue-ipc.sock"));

    std::cerr << "starting phi_adapter_hue_ipc for pluginType=" << kPluginType
              << " socket=" << socketPath << '\n';

    HueFactory factory;
    sdk::SidecarHost host(socketPath, factory);
    return sdk::runSidecarMain(host);
}
