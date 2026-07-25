#pragma once

#include "pch.h"
#include "push-widget.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QUrl>
#include <QTimer>
#include <QWebSocket>
#include <QStringList>

#include <memory>

class NativeOutputManager final {
public:
    explicit NativeOutputManager(QWidget* parent);
    ~NativeOutputManager();

    void ApplyConfig(const QJsonObject& config);
    void StartAll();
    void StopAll();
    void SetEnabled(const QString& destinationId, bool enabled);
    QJsonObject Status() const;
    void ResetForProfileChange();

private:
    static QString TargetId(const QString& destinationId);
    void Clear();
    void BuildTarget(const QJsonObject& destination);

    QWidget* owner_ = nullptr;
    QHash<QString, PushWidget*> outputs_;
    QHash<QString, bool> enabled_;
    QHash<QString, QString> names_;
    QStringList order_;
    QJsonObject lastConfig_;
    bool configured_ = false;
    QString lockedResolution_;
    int lockedFps_ = 0;
};

class PerHostAgent final : public QObject {
    Q_OBJECT

public:
    explicit PerHostAgent(QWidget* parent);
    ~PerHostAgent() override;

    void OnOBSProfileChanged();

private:
    void ConnectToBackend();
    void Send(const QJsonObject& message);
    void SendHello();
    void SendStatus();
    void HandleMessage(const QString& message);
    bool DiscoverBackend();

    QWebSocket socket_;
    QTimer reconnectTimer_;
    QTimer statusTimer_;
    NativeOutputManager outputs_;
    QString streamId_;
    QUrl backendUrl_;
};
