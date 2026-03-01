#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

#include <QCoreApplication>
#include <QEventLoop>

#include "hue_schema.h"
#include "hue_sidecar.h"
#include "phi/adapter/sdk/sidecar.h"

namespace {

namespace sdk = phicore::adapter::sdk;
namespace v1 = phicore::adapter::v1;

std::atomic_bool g_running{true};

void handleSignal(int)
{
    g_running.store(false);
}

class HueFactory final : public sdk::AdapterFactory
{
public:
    v1::Utf8String pluginType() const override
    {
        return phicore::hue::ipc::kPluginType;
    }

    std::unique_ptr<sdk::AdapterSidecar> create() const override
    {
        return std::make_unique<phicore::hue::ipc::HueSidecar>();
    }
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

    while (g_running.load()) {
        if (!host.pollOnce(std::chrono::milliseconds(250), &error)) {
            std::cerr << "poll failed: " << error << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        if (auto *adapter = dynamic_cast<phicore::hue::ipc::HueSidecar *>(host.adapter()))
            adapter->tick();

        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    host.stop();
    std::cerr << "stopping phi_adapter_hue_ipc" << '\n';
    return 0;
}
