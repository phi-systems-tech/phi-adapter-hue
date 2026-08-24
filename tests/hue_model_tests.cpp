// A Hue bridge's answers, and what they become.
//
// The second adapter suite, copied from phi-adapter-z2m's. Everything here is
// pure conversion - a bridge payload in, a v1 type out, or a channel write in
// and the body the bridge expects out - so none of it needs a bridge.

#include <phi/adapter/testing/check.h>

#include "hue_model.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

using namespace phicore::hue::ipc;
namespace v1 = phicore::adapter::v1;

namespace {

QJsonArray arrayOf(const char *text)
{
    return QJsonDocument::fromJson(QByteArray(text)).array();
}

phicore::adapter::sdk::ChannelInvokeRequest write(const char *channel, v1::ScalarValue value)
{
    phicore::adapter::sdk::ChannelInvokeRequest request;
    request.channelExternalId = channel;
    request.value = std::move(value);
    request.hasScalarValue = true;
    return request;
}

QJsonObject bodyOf(const QByteArray &payload)
{
    return QJsonDocument::fromJson(payload).object();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // --- what a channel write becomes -------------------------------------

    // Switching a lamp on is one field, and it is the field the bridge names.
    const QJsonObject on = bodyOf(buildLightCommandPayload(QStringLiteral("on"), write("on", true)));
    PHI_CHECK(on.value(QStringLiteral("on")).toObject().value(QStringLiteral("on")).toBool() == true);

    // Setting a brightness also switches the lamp on: a bridge that is told to
    // dim to 40 while off does nothing, and the operator meant something.
    const QJsonObject dim = bodyOf(buildLightCommandPayload(QStringLiteral("bri"), write("bri", 40.0)));
    PHI_CHECK(dim.value(QStringLiteral("dimming")).toObject()
                  .value(QStringLiteral("brightness")).toDouble() == 40.0);
    PHI_CHECK(dim.value(QStringLiteral("on")).toObject().value(QStringLiteral("on")).toBool() == true);

    // ...and dimming to nothing switches it off rather than leaving a lamp lit
    // at zero.
    const QJsonObject dark = bodyOf(buildLightCommandPayload(QStringLiteral("bri"), write("bri", 0.0)));
    PHI_CHECK(dark.value(QStringLiteral("on")).toObject().value(QStringLiteral("on")).toBool() == false);

    // A value the channel cannot carry is refused with a reason, not sent.
    QString error;
    phicore::adapter::sdk::ChannelInvokeRequest empty;
    empty.channelExternalId = "on";
    PHI_CHECK(buildLightCommandPayload(QStringLiteral("on"), empty, &error).isEmpty());
    PHI_CHECK(!error.isEmpty());

    // --- the colour conversion --------------------------------------------

    // Hue speaks CIE xy, phi speaks RGB, and a primary has to survive the round
    // trip or a light drifts every time anyone looks at it. Compared by hue
    // rather than by brightness: xy carries the colour and drops the intensity,
    // which is what `brightness01` puts back.
    const auto roundTrip = [](double r0, double g0, double b0, double tolerance, const char *what) {
        double x = 0.0;
        double y = 0.0;
        rgbToXy(r0, g0, b0, &x, &y);
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        xyToRgb(x, y, 1.0, &r, &g, &b);
        const double scale = std::max({r, g, b});
        const double sourceScale = std::max({r0, g0, b0});
        PHI_CHECK_MSG(scale > 0.0, "%s came back black", what);
        if (scale <= 0.0 || sourceScale <= 0.0)
            return;
        PHI_CHECK_MSG(std::abs(r / scale - r0 / sourceScale) < tolerance
                          && std::abs(g / scale - g0 / sourceScale) < tolerance
                          && std::abs(b / scale - b0 / sourceScale) < tolerance,
                      "%s came back as %f %f %f", what, r / scale, g / scale, b / scale);
    };
    roundTrip(1.0, 0.0, 0.0, 0.06, "red");
    roundTrip(0.0, 1.0, 0.0, 0.06, "green");
    roundTrip(0.0, 0.0, 1.0, 0.06, "blue");
    roundTrip(1.0, 1.0, 1.0, 0.06, "white");

    // A colour outside what a lamp can produce does not round-trip, and cannot:
    // xy is a triangle and the conversion clamps to it. What must hold is that
    // it stays a colour - in range, not black, and still recognisably itself.
    {
        double x = 0.0;
        double y = 0.0;
        rgbToXy(0.2, 0.4, 0.8, &x, &y);
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        xyToRgb(x, y, 1.0, &r, &g, &b);
        PHI_CHECK(r >= 0.0 && r <= 1.0 && g >= 0.0 && g <= 1.0 && b >= 0.0 && b <= 1.0);
        PHI_CHECK_MSG(b > g && g > r, "a blue came back as %f %f %f", r, g, b);
    }

    // --- what a bridge snapshot becomes -----------------------------------

    // The shape the v2 API answers with: a device that owns a light service,
    // and the light resource that service points at.
    const Snapshot snapshot = buildSnapshot(
        arrayOf(R"([{
            "id": "0b21b4b0-1111-2222-3333-444455556666",
            "metadata": {"name": "Reading lamp", "archetype": "table_shade"},
            "product_data": {"model_id": "LCT015", "manufacturer_name": "Signify"},
            "services": [{"rid": "aaaa1111-2222-3333-4444-555566667777", "rtype": "light"}]
        }])"),
        arrayOf(R"([{
            "id": "aaaa1111-2222-3333-4444-555566667777",
            "owner": {"rid": "0b21b4b0-1111-2222-3333-444455556666", "rtype": "device"},
            "on": {"on": true},
            "dimming": {"brightness": 62.5}
        }])"),
        {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {});

    PHI_CHECK_MSG(snapshot.devices.size() == 1, "expected one device, got %d",
                  int(snapshot.devices.size()));
    if (snapshot.devices.size() == 1) {
        const DeviceEntry &entry = *snapshot.devices.constBegin();
        PHI_CHECK(QString::fromStdString(std::string(entry.device.name)) == QStringLiteral("Reading lamp"));
        PHI_CHECK(entry.state.hasOn && entry.state.on);
        PHI_CHECK(entry.state.hasBrightness && entry.state.brightness == 62.5);
        bool hasOnChannel = false;
        bool hasBrightnessChannel = false;
        for (const v1::Channel &channel : entry.channels) {
            const QString id = QString::fromStdString(std::string(channel.externalId));
            if (id == QStringLiteral("on"))
                hasOnChannel = true;
            if (id == QStringLiteral("bri"))
                hasBrightnessChannel = true;
        }
        PHI_CHECK_MSG(hasOnChannel, "the lamp has no on channel");
        PHI_CHECK_MSG(hasBrightnessChannel, "the lamp has no brightness channel");
    }

    return phi::testing::report("hue_model_tests");
}
