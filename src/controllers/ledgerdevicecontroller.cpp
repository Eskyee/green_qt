#include "device.h"
#include "ga.h"
#include "json.h"
#include "ledgerdevicecontroller.h"
#include "network.h"
#include "networkmanager.h"
#include "util.h"
#include "wallet.h"
#include "walletmanager.h"


LedgerDeviceController::LedgerDeviceController(QObject* parent) //Device* device, Network* network)
    : QObject(parent)
//    , m_device(device)
//    , m_network(network)
{
}

void LedgerDeviceController::setDevice(Device *device)
{
    if (m_device == device) return;
    m_device = device;
    emit deviceChanged(m_device);

    if (m_device) {
        QTimer::singleShot(1000, this, &LedgerDeviceController::initialize);
    }
}

Network* LedgerDeviceController::networkFromAppName(const QString& app_name)
{
    QString id;
    if (app_name == "Bitcoin") id = "mainnet";
    if (app_name == "Bitcoin Test") id = "testnet";
    if (app_name == "Liquid") id = "liquid";
    return id.isEmpty() ? nullptr : NetworkManager::instance()->network(id);
}

void LedgerDeviceController::initialize()
{
    auto cmd = new GetAppNameCommand(m_device);
    connect(cmd, &Command::finished, [this, cmd] {
        m_network = LedgerDeviceController::networkFromAppName(cmd->m_name);
        emit networkChanged(m_network);
        if (!m_network) {
            Q_ASSERT(cmd->m_name.indexOf("OLOS") >= 0);
            QTimer::singleShot(1000, this, &LedgerDeviceController::initialize);
            return;
        }

        const auto name = m_device->name().toLocal8Bit();
        hw_device = Json::fromObject({{
            "device", QJsonObject({
                { "name", name.constData() },
                { "supports_arbitrary_scripts", true },
                { "supports_low_r", false },
                { "supports_liquid", 1 }
            })
        }});

        m_wallet = new Wallet();
        m_wallet->setNetwork(m_network);

        login();
    });
    connect(cmd, &Command::error, [this] {
        QTimer::singleShot(1000, this, &LedgerDeviceController::initialize);
    });
    cmd->exec();
}

void LedgerDeviceController::login()
{
    auto log_level = QString::fromLocal8Bit(qgetenv("GREEN_GDK_LOG_LEVEL"));
    if (log_level.isEmpty()) log_level = "info";

    QJsonObject params{
        { "name", m_network->id() },
        { "log_level", log_level },
        { "use_tor", false },
    };

    m_wallet->createSession();
    GA_connect(m_wallet->m_session, Json::fromObject(params));

    // TODO: use RegisterUserHandler
    GA_register_user(m_wallet->m_session, hw_device, "", &m_register_handler);

    QJsonObject result = GA::auth_handler_get_result(m_register_handler);
    QString status = result.value("status").toString();

    Q_ASSERT(result.value("status").toString() == "resolve_code");
    Q_ASSERT(result.value("action").toString() == "get_xpubs");

    m_paths = result.value("required_data").toObject().value("paths").toArray();

    auto batch = new CommandBatch;
    for (auto path : m_paths) {
        auto cmd = new GetWalletPublicKeyCommand(m_device, m_network, ParsePath(path));
        connect(cmd, &Command::finished, [this, cmd] {
            m_xpubs.append(cmd->m_xpub);
        });
        batch->add(cmd);
    }
    connect(batch, &Command::finished, [this] {
        Q_ASSERT(m_xpubs.size() == m_paths.size());
        QJsonObject code= {{ "xpubs", m_xpubs }};
        auto _code = QJsonDocument(code).toJson();
        GA_auth_handler_resolve_code(m_register_handler, _code.constData());
        GA_auth_handler_call(m_register_handler);

        m_paths = QJsonArray();
        m_xpubs = QJsonArray();

        login2();
    });
    batch->exec();
}

void LedgerDeviceController::login2()
{
    // TODO: use LoginHandler
    int err = GA_login(m_wallet->m_session, hw_device, "", "", &m_login_handler);
    Q_ASSERT(err == GA_OK);

    auto result = GA::auth_handler_get_result(m_login_handler);

    Q_ASSERT(result.value("status").toString() == "resolve_code");
    Q_ASSERT(result.value("action").toString() == "get_xpubs");

    m_paths = result.value("required_data").toObject().value("paths").toArray();

    for (auto path : m_paths) {
        auto cmd = new GetWalletPublicKeyCommand(m_device, m_network, ParsePath(path));
        connect(cmd, &Command::finished, this, [this, cmd] {
            m_xpubs.append(cmd->m_xpub);

            if (m_xpubs.size() == m_paths.size()) {
                QJsonObject code= {{ "xpubs", m_xpubs }};
                auto _code = QJsonDocument(code).toJson();
                GA_auth_handler_resolve_code(m_login_handler, _code.constData());
                GA::auth_handler_get_result(m_login_handler);
                GA_auth_handler_call(m_login_handler);
                auto result = GA::auth_handler_get_result(m_login_handler);
                auto required_data = result.value("required_data").toObject();
                QByteArray message = required_data.value("message").toString().toLocal8Bit();
                QVector<uint32_t> path = ParsePath(required_data.value("path"));
                auto prepare = new SignMessageCommand(m_device, path, message);
                connect(prepare, &Command::finished, [this] {
                    auto sign = new SignMessageCommand(m_device);
                    connect(sign, &Command::finished, [this, sign] {
                        QJsonObject code = {{ "signature", QString::fromLocal8Bit(sign->signature.toHex()) }};

                        auto _code = QJsonDocument(code).toJson();
                        GA_auth_handler_resolve_code(m_login_handler, _code.constData());
                        GA::auth_handler_get_result(m_login_handler);
                        GA_auth_handler_call(m_login_handler);
                        auto result = GA::auth_handler_get_result(m_login_handler);

                        Q_ASSERT(result.value("status").toString() == "resolve_code");
                        Q_ASSERT(result.value("action").toString() == "get_xpubs");

                        m_paths = result.value("required_data").toObject().value("paths").toArray();
                        m_xpubs = QJsonArray();

                        m_progress = qreal(2) / qreal(m_paths.size() + 2);
                        emit progressChanged(true);

                        for (auto path : m_paths) {
                            auto cmd = new GetWalletPublicKeyCommand(m_device, m_network, ParsePath(path));
                            connect(cmd, &Command::finished, [this, cmd] {
                                m_xpubs.append(cmd->m_xpub);

                                m_progress = qreal(m_xpubs.size() + 2) / qreal(m_paths.size() + 2);
                                emit progressChanged(true);

                                if (m_xpubs.size() == m_paths.size()) {
                                    QJsonObject code= {{ "xpubs", m_xpubs }};
                                    auto _code = QJsonDocument(code).toJson();
                                    GA_auth_handler_resolve_code(m_login_handler, _code.constData());
                                    GA::auth_handler_get_result(m_login_handler);
                                    GA_auth_handler_call(m_login_handler);
                                    GA::auth_handler_get_result(m_login_handler);

                                    m_wallet->m_device = m_device;
                                    m_wallet->setSession();
                                    WalletManager::instance()->addWallet(m_wallet);

                                    m_progress = 2;
                                    emit progressChanged(m_progress);

                                    auto w = m_wallet;
                                    connect(m_device, &QObject::destroyed, [w] {
                                        WalletManager::instance()->removeWallet(w);
                                        delete w;
                                    });
                                }
                            });
                            cmd->exec();
                        }
                    });
                    sign->exec();
                });
                prepare->exec();
            }
        });
        cmd->exec();
    }
}