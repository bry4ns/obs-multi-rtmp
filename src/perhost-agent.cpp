#include "perhost-agent.h"

#include "helpers.h"
#include "plugin-support.h"
#include "output-config.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cstring>

namespace {

constexpr auto kTargetPrefix = "perhost-native:";
constexpr auto kDefaultBackendUrl = "wss://adm.perhost.app";

QString valueOrEmpty(const QJsonObject& object, const char* key)
{
    return object.value(QLatin1String(key)).toString();
}

QUrl FindBrowserSourceUrl()
{
    QUrl result;
    obs_enum_sources([](void* data, obs_source_t* source) {
        auto* browserUrl = static_cast<QUrl*>(data);
        if (browserUrl->isValid() || std::strcmp(obs_source_get_id(source), "browser_source") != 0) {
            return !browserUrl->isValid();
        }

        obs_data_t* settings = obs_source_get_settings(source);
        const char* url = settings ? obs_data_get_string(settings, "url") : nullptr;
        QUrl candidate = QUrl::fromUserInput(QString::fromUtf8(url ? url : ""));
        if (settings) {
            obs_data_release(settings);
        }
        QUrlQuery query(candidate);
        if (candidate.isValid() && !query.queryItemValue("streamid").isEmpty()) {
            *browserUrl = candidate;
            return false;
        }
        return true;
    }, &result);
    return result;
}

}

NativeOutputManager::NativeOutputManager(QWidget* parent)
    : owner_(new QWidget(parent))
{
    owner_->setAttribute(Qt::WA_DontShowOnScreen);
}

NativeOutputManager::~NativeOutputManager()
{
    Clear();
    delete owner_;
}

QString NativeOutputManager::TargetId(const QString& destinationId)
{
    return QString::fromLatin1(kTargetPrefix) + destinationId;
}

void NativeOutputManager::Clear()
{
    for (auto* output : outputs_) {
        output->ForceStop();
        delete output;
    }
    outputs_.clear();
    enabled_.clear();
    names_.clear();
    order_.clear();

    auto& targets = GlobalMultiOutputConfig().targets;
    targets.remove_if([](const OutputTargetConfigPtr& target) {
        return target && target->id.rfind(kTargetPrefix, 0) == 0;
    });
}

void NativeOutputManager::BuildTarget(const QJsonObject& destination)
{
    const auto destinationId = valueOrEmpty(destination, "id");
    const auto name = valueOrEmpty(destination, "name");
    const auto server = valueOrEmpty(destination, "rtmpUrl");
    const auto streamKey = valueOrEmpty(destination, "streamKey");
    if (destinationId.isEmpty() || server.isEmpty() || streamKey.isEmpty()) {
        blog(LOG_WARNING, TAG "Ignoring incomplete native destination");
        return;
    }

    auto target = std::make_shared<OutputTargetConfig>();
    target->id = TargetId(destinationId).toStdString();
    target->name = name.toStdString();
    target->protocol = "RTMP";
    target->syncStart = false;
    target->syncStop = false;
    target->serviceParam = nlohmann::json{
        {"server", server.toStdString()},
        {"key", streamKey.toStdString()},
    };
    target->outputParam = nlohmann::json::object();
    target->videoConfig = std::string(OBS_STREAMING_ENC_PLACEHOLDER);
    target->audioConfig = std::string(OBS_STREAMING_ENC_PLACEHOLDER);

    GlobalMultiOutputConfig().targets.emplace_back(target);

    auto* widget = createPushWidget(target->id, owner_);
    outputs_.insert(destinationId, widget);
    enabled_.insert(destinationId, destination.value("enabled").toBool(false));
    names_.insert(destinationId, name);
    order_.append(destinationId);
}

void NativeOutputManager::ApplyConfig(const QJsonObject& config)
{
    lastConfig_ = config;
    const auto lock = config.value("lock").toObject();
    lockedResolution_ = valueOrEmpty(lock, "resolution");
    lockedFps_ = lock.value("fps").toInt();

    Clear();
    const auto destinations = config.value("destinations").toArray();
    for (const auto& value : destinations) {
        if (value.isObject()) {
            BuildTarget(value.toObject());
        }
    }
    configured_ = true;
}

void NativeOutputManager::StartAll()
{
    for (const auto& destinationId : order_) {
        if (enabled_.value(destinationId, false)) {
            if (auto* output = outputs_.value(destinationId)) {
                output->StartStreaming();
            }
        }
    }
}

void NativeOutputManager::StopAll()
{
    for (auto* output : outputs_) {
        output->ForceStop();
    }
}

void NativeOutputManager::SetEnabled(const QString& destinationId, bool enabled)
{
    if (!outputs_.contains(destinationId)) {
        return;
    }

    enabled_.insert(destinationId, enabled);
    if (auto* output = outputs_.value(destinationId)) {
        if (enabled) {
            output->StartStreaming();
        } else {
            output->ForceStop();
        }
    }
}

QJsonObject NativeOutputManager::Status() const
{
    QJsonArray destinations;
    bool streaming = false;
    for (const auto& destinationId : order_) {
        const auto* output = outputs_.value(destinationId);
        const bool active = output != nullptr && output->IsRunning();
        streaming = streaming || active;

        QJsonObject destination;
        destination.insert("id", destinationId);
        destination.insert("name", names_.value(destinationId));
        destination.insert("active", active);
        destination.insert("state", active ? "running" : (enabled_.value(destinationId) ? "stopped" : "disabled"));
        destinations.append(destination);
    }

    QJsonObject status;
    status.insert("type", "plugin_status");
    status.insert("configured", configured_);
    status.insert("streaming", streaming);
    status.insert("active", streaming);
    status.insert("resolution", lockedResolution_);
    status.insert("fps", lockedFps_);
    status.insert("destinations", destinations);
    return status;
}

void NativeOutputManager::ResetForProfileChange()
{
    if (!lastConfig_.isEmpty()) {
        ApplyConfig(lastConfig_);
    }
}

PerHostAgent::PerHostAgent(QWidget* parent)
    : QObject(parent)
    , outputs_(parent)
{
    reconnectTimer_.setInterval(3000);
    reconnectTimer_.setSingleShot(true);
    statusTimer_.setInterval(2000);

    connect(&socket_, &QWebSocket::connected, this, [this]() {
        reconnectTimer_.stop();
        SendHello();
        SendStatus();
    });
    connect(&socket_, &QWebSocket::textMessageReceived, this, [this](const QString& message) {
        HandleMessage(message);
    });
    connect(&socket_, &QWebSocket::disconnected, this, [this]() {
        if (!reconnectTimer_.isActive()) {
            reconnectTimer_.start();
        }
    });
    connect(&reconnectTimer_, &QTimer::timeout, this, &PerHostAgent::ConnectToBackend);
    connect(&statusTimer_, &QTimer::timeout, this, &PerHostAgent::SendStatus);

    statusTimer_.start();
    ConnectToBackend();
}

PerHostAgent::~PerHostAgent()
{
    socket_.close();
}

bool PerHostAgent::DiscoverBackend()
{
    const QUrl browserUrl = FindBrowserSourceUrl();
    if (!browserUrl.isValid()) {
        blog(LOG_WARNING, TAG "Native agent waiting for a Browser Source with streamid");
        return false;
    }

    const QUrlQuery browserQuery(browserUrl);
    streamId_ = browserQuery.queryItemValue("streamid");
    auto base = browserQuery.queryItemValue("backend");
    if (base.isEmpty()) {
        base = QString::fromLatin1(kDefaultBackendUrl);
    }
    if (base.startsWith("https://")) {
        base.replace(0, 8, "wss://");
    } else if (base.startsWith("http://")) {
        base.replace(0, 7, "ws://");
    } else if (!base.startsWith("ws://") && !base.startsWith("wss://")) {
        base.prepend("wss://");
    }
    while (base.endsWith('/')) {
        base.chop(1);
    }

    QUrl url(base + "/obs/" + QString::fromUtf8(QUrl::toPercentEncoding(streamId_)));
    QUrlQuery query;
    query.addQueryItem("agent", "perhost-plugin");
    url.setQuery(query);
    if (!url.isValid() || streamId_.isEmpty()) {
        blog(LOG_WARNING, TAG "Native agent could not resolve the backend URL from Browser Source");
        return false;
    }

    backendUrl_ = url;
    return true;
}

void PerHostAgent::ConnectToBackend()
{
    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        return;
    }
    if (!backendUrl_.isValid() && !DiscoverBackend()) {
        if (!reconnectTimer_.isActive()) {
            reconnectTimer_.start();
        }
        return;
    }
    socket_.open(backendUrl_);
}

void PerHostAgent::Send(const QJsonObject& message)
{
    if (socket_.state() != QAbstractSocket::ConnectedState) {
        return;
    }
    socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void PerHostAgent::SendHello()
{
    QJsonObject hello;
    hello.insert("type", "plugin_hello");
    hello.insert("streamId", streamId_);
    hello.insert("version", QString::fromLatin1(PLUGIN_VERSION));
    Send(hello);
}

void PerHostAgent::SendStatus()
{
    Send(outputs_.Status());
}

void PerHostAgent::HandleMessage(const QString& message)
{
    const auto document = QJsonDocument::fromJson(message.toUtf8());
    if (!document.isObject()) {
        return;
    }

    const auto object = document.object();
    const auto type = object.value("type").toString();
    if (type == "plugin_config") {
        outputs_.ApplyConfig(object);
        SendStatus();
        return;
    }

    if (type != "plugin_command") {
        return;
    }

    const auto action = object.value("action").toString();
    if (action == "multistream.start") {
        outputs_.StartAll();
    } else if (action == "multistream.stop") {
        outputs_.StopAll();
    } else if (action == "destination.set_enabled") {
        const auto params = object.value("params").toObject();
        outputs_.SetEnabled(params.value("destinationId").toString(), params.value("enabled").toBool());
    }
    SendStatus();
}

void PerHostAgent::OnOBSProfileChanged()
{
    outputs_.ResetForProfileChange();
    backendUrl_ = QUrl();
    streamId_.clear();
    socket_.close();
    SendStatus();
}
