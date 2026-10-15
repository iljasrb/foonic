/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicsettingspage.h"

#include "subsonicclient.h"
#include "subsonicsettings.h"

#include <core/network/networkaccessmanager.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
namespace {
class SubsonicSettingsPageWidget : public SettingsPageWidget
{
    Q_OBJECT

public:
    SubsonicSettingsPageWidget(std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings);

    void load() override;
    void apply() override;
    void reset() override;

private:
    [[nodiscard]] SubsonicSettings uiSettings() const;
    void testConnection();
    void loadMusicFolders();
    void browseLibraryDir();
    void updateBitRateState();
    void setTestStatus(const QString& text, const QString& color = {});

    std::shared_ptr<NetworkAccessManager> m_network;
    SettingsManager* m_settings;

    QLineEdit* m_server;
    QLineEdit* m_username;
    QLineEdit* m_password;
    QCheckBox* m_ignoreSsl;
    QComboBox* m_musicFolder;
    QPushButton* m_refreshFolders;
    QLabel* m_testResult;
    QComboBox* m_format;
    QSpinBox* m_maxBitRate;
    QLineEdit* m_libraryDir;
    QSpinBox* m_streamBuffer;
    QSpinBox* m_remotePrebuffer;
    QCheckBox* m_seekFade;
    QSpinBox* m_seekFadeMs;
    QSpinBox* m_coverArtSize;
    QCheckBox* m_scrobble;
    QComboBox* m_authMode;
    QLineEdit* m_apiKey;
};
} // namespace

SubsonicSettingsPageWidget::SubsonicSettingsPageWidget(std::shared_ptr<NetworkAccessManager> network,
                                                       SettingsManager* settings)
    : m_network{std::move(network)}
    , m_settings{settings}
    , m_server{new QLineEdit{this}}
    , m_username{new QLineEdit{this}}
    , m_password{new QLineEdit{this}}
    , m_ignoreSsl{new QCheckBox{tr("Ignore SSL certificate errors"), this}}
    , m_musicFolder{new QComboBox{this}}
    , m_refreshFolders{new QPushButton{tr("Refresh"), this}}
    , m_testResult{new QLabel{this}}
    , m_format{new QComboBox{this}}
    , m_maxBitRate{new QSpinBox{this}}
    , m_libraryDir{new QLineEdit{this}}
    , m_streamBuffer{new QSpinBox{this}}
    , m_remotePrebuffer{new QSpinBox{this}}
    , m_seekFade{new QCheckBox{tr("Fade seek"), this}}
    , m_seekFadeMs{new QSpinBox{this}}
    , m_coverArtSize{new QSpinBox{this}}
    , m_scrobble{new QCheckBox{tr("Report now playing and play counts to the server"), this}}
    , m_authMode{new QComboBox{this}}
    , m_apiKey{new QLineEdit{this}}
{
    m_server->setPlaceholderText(u"https://music.example.com"_s);
    m_password->setEchoMode(QLineEdit::Password);
    m_ignoreSsl->setToolTip(
        tr("Accept self-signed or invalid TLS certificates. Only enable this for a server you trust."));

    auto* testButton = new QPushButton{tr("Test Connection"), this};

    m_musicFolder->setToolTip(tr("Limit browsing and searching to one server music folder."));
    m_refreshFolders->setToolTip(tr("Reload the music folder list from the server."));
    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(m_musicFolder, 1);
    folderRow->addWidget(m_refreshFolders);

    auto* connectionForm = new QFormLayout;
    m_authMode->addItem(tr("Token (recommended)"), u"token"_s);
    m_authMode->addItem(tr("Password (plain text)"), u"password"_s);
    m_authMode->addItem(tr("API key"), u"apiKey"_s);
    m_apiKey->setPlaceholderText(tr("Server API key"));
    connectionForm->addRow(tr("Server URL"), m_server);
    connectionForm->addRow(tr("Username"), m_username);
    connectionForm->addRow(tr("Password"), m_password);
    connectionForm->addRow(tr("Authentication"), m_authMode);
    connectionForm->addRow(tr("API key"), m_apiKey);
    connectionForm->addRow({}, m_ignoreSsl);
    connectionForm->addRow(tr("Music folder"), folderRow);
    connectionForm->addRow(tr("Cover art size"), m_coverArtSize);

    auto* testRow = new QHBoxLayout;
    testRow->addWidget(testButton);
    testRow->addWidget(m_testResult, 1);
    connectionForm->addRow(testRow);

    auto* connectionGroup = new QGroupBox{tr("Connection"), this};
    connectionGroup->setLayout(connectionForm);

    m_format->addItem(tr("Original (no transcoding)"), u"raw"_s);
    m_format->addItem(tr("MP3"), u"mp3"_s);
    m_format->addItem(tr("Opus"), u"opus"_s);
    m_format->addItem(tr("AAC"), u"aac"_s);

    m_maxBitRate->setRange(0, 320);
    m_maxBitRate->setSingleStep(32);
    m_maxBitRate->setSuffix(u" kbps"_s);
    m_maxBitRate->setSpecialValueText(tr("Unlimited"));

    m_streamBuffer->setRange(8, 1024);
    m_streamBuffer->setSuffix(u" MB"_s);
    m_streamBuffer->setToolTip(tr("In-memory buffer per remote stream. At least the track size keeps the whole "
                                  "track in memory, making seeks instant."));
    m_remotePrebuffer->setRange(100, 10000);
    m_remotePrebuffer->setSingleStep(100);
    m_remotePrebuffer->setSuffix(u" ms"_s);
    m_remotePrebuffer->setToolTip(
        tr("Audio buffered before playback resumes after a remote seek. Raise it on a slow network."));
    m_seekFade->setToolTip(tr("Fade out at the current position and fade in at the new one when seeking."));
    m_seekFadeMs->setRange(0, 2000);
    m_seekFadeMs->setSingleStep(50);
    m_seekFadeMs->setSuffix(u" ms"_s);
    m_coverArtSize->setRange(64, 2000);
    m_coverArtSize->setSingleStep(64);
    m_coverArtSize->setSuffix(u" px"_s);
    m_coverArtSize->setToolTip(tr("Cover art size requested from the server. Larger means sharper but heavier."));
    auto* fadeRow = new QHBoxLayout;
    fadeRow->addWidget(m_seekFade);
    fadeRow->addWidget(m_seekFadeMs);
    fadeRow->addStretch();
    auto updateFadeState = [this] { m_seekFadeMs->setEnabled(m_seekFade->isChecked()); };
    connect(m_seekFade, &QCheckBox::toggled, this, updateFadeState);
    updateFadeState();

    auto* playbackForm = new QFormLayout;
    playbackForm->addRow(tr("Stream format"), m_format);
    playbackForm->addRow(tr("Max bitrate"), m_maxBitRate);
    playbackForm->addRow(tr("Stream buffer"), m_streamBuffer);
    playbackForm->addRow(tr("Remote prebuffer"), m_remotePrebuffer);
    playbackForm->addRow(tr("Seek fade"), fadeRow);
    playbackForm->addRow({}, m_scrobble);

    auto* playbackGroup = new QGroupBox{tr("Playback"), this};
    playbackGroup->setLayout(playbackForm);

    auto* libraryBrowse = new QPushButton{tr("Browse..."), this};
    auto* libraryRow    = new QHBoxLayout;
    libraryRow->addWidget(m_libraryDir, 1);
    libraryRow->addWidget(libraryBrowse);

    auto* libraryHint = new QLabel{
        tr("Albums and songs downloaded here are saved as tagged files and added to the fooyin library."), this};
    libraryHint->setWordWrap(true);

    auto* libraryForm = new QFormLayout;
    libraryForm->addRow(tr("Library folder"), libraryRow);
    libraryForm->addRow({}, libraryHint);

    auto* libraryGroup = new QGroupBox{tr("Local Library"), this};
    libraryGroup->setLayout(libraryForm);

    auto* layout = new QVBoxLayout{this};
    layout->addWidget(connectionGroup);
    layout->addWidget(playbackGroup);
    layout->addWidget(libraryGroup);
    layout->addStretch();

    connect(testButton, &QPushButton::clicked, this, &SubsonicSettingsPageWidget::testConnection);
    connect(m_refreshFolders, &QPushButton::clicked, this, &SubsonicSettingsPageWidget::loadMusicFolders);
    connect(libraryBrowse, &QPushButton::clicked, this, &SubsonicSettingsPageWidget::browseLibraryDir);
    connect(m_format, &QComboBox::currentIndexChanged, this, &SubsonicSettingsPageWidget::updateBitRateState);
    auto updateAuthState = [this] {
        const bool apiKey = m_authMode->currentData().toString() == u"apiKey"_s;
        m_username->setEnabled(!apiKey);
        m_password->setEnabled(!apiKey);
        m_apiKey->setEnabled(apiKey);
    };
    connect(m_authMode, &QComboBox::currentIndexChanged, this, updateAuthState);

    updateBitRateState();
    updateAuthState();
}

SubsonicSettings SubsonicSettingsPageWidget::uiSettings() const
{
    SubsonicSettings settings;
    settings.server          = m_server->text().trimmed();
    settings.username        = m_username->text().trimmed();
    settings.password        = m_password->text();
    settings.authMode        = m_authMode->currentData().toString();
    settings.apiKey          = m_apiKey->text().trimmed();
    settings.ignoreSslErrors = m_ignoreSsl->isChecked();
    settings.musicFolderId   = m_musicFolder->currentData().toString();
    return settings;
}

void SubsonicSettingsPageWidget::load()
{
    const SubsonicSettings settings = loadSubsonicSettings(m_settings);

    m_server->setText(settings.server);
    m_username->setText(settings.username);
    m_password->setText(settings.password);
    m_ignoreSsl->setChecked(settings.ignoreSslErrors);
    m_apiKey->setText(settings.apiKey);

    const int authIndex = m_authMode->findData(settings.authMode);
    m_authMode->setCurrentIndex(authIndex >= 0 ? authIndex : 0);

    m_musicFolder->clear();
    m_musicFolder->addItem(tr("All folders"), QString{});
    if(!settings.musicFolderId.isEmpty()) {
        m_musicFolder->addItem(settings.musicFolderId, settings.musicFolderId);
        m_musicFolder->setCurrentIndex(1);
    }

    const int formatIndex = m_format->findData(settings.format);
    m_format->setCurrentIndex(formatIndex >= 0 ? formatIndex : 0);

    m_maxBitRate->setValue(settings.maxBitRate);
    m_libraryDir->setText(settings.libraryDir);
    m_streamBuffer->setValue(settings.streamBufferMb);
    m_remotePrebuffer->setValue(settings.remotePrebufferMs);
    m_seekFade->setChecked(settings.seekFadeEnabled);
    m_seekFadeMs->setValue(settings.seekFadeMs);
    m_coverArtSize->setValue(settings.coverArtSize);
    m_scrobble->setChecked(settings.scrobblePlays);
    setTestStatus({});
    updateBitRateState();

    if(settings.isValid()) {
        loadMusicFolders();
    }

    qCInfo(SUBSONIC) << "Loaded settings: format=" << settings.format << "maxBitRate=" << settings.maxBitRate
                     << "musicFolder=" << settings.musicFolderId << "ignoreSsl=" << settings.ignoreSslErrors;
}

void SubsonicSettingsPageWidget::apply()
{
    SubsonicSettings settings;
    settings.server       = m_server->text().trimmed();
    settings.username     = m_username->text().trimmed();
    settings.password     = m_password->text();
    settings.authMode     = m_authMode->currentData().toString();
    settings.apiKey       = m_apiKey->text().trimmed();
    settings.format       = m_format->currentData().toString();
    settings.maxBitRate   = (settings.format == "raw"_L1) ? 0 : m_maxBitRate->value();
    settings.libraryDir   = m_libraryDir->text().trimmed();
    settings.streamBufferMb         = m_streamBuffer->value();
    settings.remotePrebufferMs      = m_remotePrebuffer->value();
    settings.seekFadeEnabled        = m_seekFade->isChecked();
    settings.seekFadeMs             = m_seekFadeMs->value();
    settings.coverArtSize           = m_coverArtSize->value();
    settings.scrobblePlays          = m_scrobble->isChecked();
    m_settings->set(QStringLiteral("Engine/RemotePrebufferMs"), settings.remotePrebufferMs);
    settings.ignoreSslErrors        = m_ignoreSsl->isChecked();
    settings.musicFolderId          = m_musicFolder->currentData().toString();

    saveSubsonicSettings(m_settings, settings);
    // Persist immediately so the choice survives an unclean exit (the core only flushes every 5 minutes).
    m_settings->storeSettings();

    qCInfo(SUBSONIC) << "Applied settings: format=" << settings.format << "maxBitRate=" << settings.maxBitRate
                     << "musicFolder=" << settings.musicFolderId << "ignoreSsl=" << settings.ignoreSslErrors;
}

void SubsonicSettingsPageWidget::reset()
{
    m_server->clear();
    m_username->clear();
    m_password->clear();
    m_apiKey->clear();
    m_authMode->setCurrentIndex(0);
    m_ignoreSsl->setChecked(false);
    m_musicFolder->clear();
    m_musicFolder->addItem(tr("All folders"), QString{});
    m_format->setCurrentIndex(0);
    m_maxBitRate->setValue(0);
    m_libraryDir->setText(defaultLibraryDir());
    m_streamBuffer->setValue(64);
    m_remotePrebuffer->setValue(400);
    m_seekFade->setChecked(true);
    m_seekFadeMs->setValue(200);
    m_coverArtSize->setValue(300);
    m_scrobble->setChecked(true);
    setTestStatus({});
    updateBitRateState();
}

void SubsonicSettingsPageWidget::setTestStatus(const QString& text, const QString& color)
{
    m_testResult->setText(text);
    m_testResult->setStyleSheet(color.isEmpty() ? QString{} : u"color: %1;"_s.arg(color));
}

void SubsonicSettingsPageWidget::testConnection()
{
    const SubsonicSettings settings = uiSettings();
    if(!settings.isValid()) {
        setTestStatus(tr("Enter a server and username first."), u"#ef5350"_s);
        return;
    }

    setTestStatus(tr("Connecting..."));
    auto* client = new SubsonicClient{m_network, settings, this};
    client->ping([this, client](const QJsonObject& response, const QString& error) {
        client->deleteLater();
        if(!error.isEmpty()) {
            setTestStatus(tr("Failed: %1").arg(error), u"#ef5350"_s);
            return;
        }

        QStringList bits;
        if(const QString type = response.value(u"type"_s).toString(); !type.isEmpty()) {
            bits.push_back(type);
        }
        if(const QString version = response.value(u"serverVersion"_s).toString(); !version.isEmpty()) {
            bits.push_back(version);
        }
        QString text = tr("Connection OK");
        if(!bits.isEmpty()) {
            text += u" — "_s + bits.join(u' ');
        }
        setTestStatus(text, u"#4caf50"_s);
        loadMusicFolders();
    });
}

void SubsonicSettingsPageWidget::loadMusicFolders()
{
    const SubsonicSettings settings = uiSettings();
    if(!settings.isValid()) {
        return;
    }

    m_refreshFolders->setEnabled(false);
    auto* client           = new SubsonicClient{m_network, settings, this};
    const QString selected = m_musicFolder->currentData().toString();
    client->getMusicFolders([this, client, selected](const QJsonArray& folders, const QString& error) {
        client->deleteLater();
        m_refreshFolders->setEnabled(true);
        if(!error.isEmpty()) {
            return;
        }

        m_musicFolder->clear();
        m_musicFolder->addItem(tr("All folders"), QString{});
        for(const QJsonValue& value : folders) {
            const QJsonObject folder = value.toObject();
            m_musicFolder->addItem(folder.value(u"name"_s).toString(), folder.value(u"id"_s).toString());
        }
        const int index = m_musicFolder->findData(selected);
        m_musicFolder->setCurrentIndex(index >= 0 ? index : 0);
    });
}

void SubsonicSettingsPageWidget::browseLibraryDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Library folder"), m_libraryDir->text());
    if(!dir.isEmpty()) {
        m_libraryDir->setText(dir);
    }
}

void SubsonicSettingsPageWidget::updateBitRateState()
{
    m_maxBitRate->setEnabled(m_format->currentData().toString() != "raw"_L1);
}

SubsonicSettingsPage::SubsonicSettingsPage(std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings,
                                           QObject* parent)
    : SettingsPage{settings->settingsDialog(), parent}
{
    setId(SettingsPageId);
    setName(tr("Foonic"));
    setCategory({tr("Foonic")});
    setPosition(SettingsPagePosition::Last);
    setWidgetCreator([network = std::move(network), settings]() {
        return new SubsonicSettingsPageWidget{network, settings};
    });

    qCInfo(SUBSONIC) << "Registered Subsonic settings page";
}
} // namespace Fooyin::Subsonic

#include "subsonicsettingspage.moc"
