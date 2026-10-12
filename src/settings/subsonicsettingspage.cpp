/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicsettingspage.h"

#include "subsonicclient.h"
#include "subsonicdownloadmanager.h"
#include "subsonicsettings.h"

#include <core/network/networkaccessmanager.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
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
    SubsonicSettingsPageWidget(std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings,
                               SubsonicDownloadManager* downloads);

    void load() override;
    void apply() override;
    void reset() override;

private:
    [[nodiscard]] SubsonicSettings uiSettings() const;
    void testConnection();
    void loadMusicFolders();
    void browseDownloadDir();
    void browseLibraryDir();
    void clearCache();
    void updateBitRateState();
    void updateModeState();
    void updateCacheInfo();
    void setTestStatus(const QString& text, const QString& color = {});

    std::shared_ptr<NetworkAccessManager> m_network;
    SettingsManager* m_settings;
    SubsonicDownloadManager* m_downloads;

    QLineEdit* m_server;
    QLineEdit* m_username;
    QLineEdit* m_password;
    QCheckBox* m_ignoreSsl;
    QComboBox* m_musicFolder;
    QPushButton* m_refreshFolders;
    QLabel* m_testResult;
    QComboBox* m_mode;
    QComboBox* m_format;
    QSpinBox* m_maxBitRate;
    QLineEdit* m_downloadDir;
    QLineEdit* m_libraryDir;
    QCheckBox* m_streamWhileDownloading;
    QSpinBox* m_prefetch;
    QSpinBox* m_cacheLimit;
    QLabel* m_cacheInfo;
};
} // namespace

SubsonicSettingsPageWidget::SubsonicSettingsPageWidget(std::shared_ptr<NetworkAccessManager> network,
                                                       SettingsManager* settings,
                                                       SubsonicDownloadManager* downloads)
    : m_network{std::move(network)}
    , m_settings{settings}
    , m_downloads{downloads}
    , m_server{new QLineEdit{this}}
    , m_username{new QLineEdit{this}}
    , m_password{new QLineEdit{this}}
    , m_ignoreSsl{new QCheckBox{tr("Ignore SSL certificate errors"), this}}
    , m_musicFolder{new QComboBox{this}}
    , m_refreshFolders{new QPushButton{tr("Refresh"), this}}
    , m_testResult{new QLabel{this}}
    , m_mode{new QComboBox{this}}
    , m_format{new QComboBox{this}}
    , m_maxBitRate{new QSpinBox{this}}
    , m_downloadDir{new QLineEdit{this}}
    , m_libraryDir{new QLineEdit{this}}
    , m_streamWhileDownloading{new QCheckBox{tr("Stream while downloading"), this}}
    , m_prefetch{new QSpinBox{this}}
    , m_cacheLimit{new QSpinBox{this}}
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
    connectionForm->addRow(tr("Server URL"), m_server);
    connectionForm->addRow(tr("Username"), m_username);
    connectionForm->addRow(tr("Password"), m_password);
    connectionForm->addRow({}, m_ignoreSsl);
    connectionForm->addRow(tr("Music folder"), folderRow);

    auto* testRow = new QHBoxLayout;
    testRow->addWidget(testButton);
    testRow->addWidget(m_testResult, 1);
    connectionForm->addRow(testRow);

    auto* connectionGroup = new QGroupBox{tr("Connection"), this};
    connectionGroup->setLayout(connectionForm);

    m_mode->addItem(tr("Download first (seekable)"), static_cast<int>(PlaybackMode::DownloadFirst));
    m_mode->addItem(tr("Stream (play immediately) — testing"), static_cast<int>(PlaybackMode::Stream));
    m_mode->setToolTip(tr("Download-first saves each track before playing, which enables seeking and avoids "
                          "servers/proxies that cut long-lived streams. Streaming is experimental."));

    m_format->addItem(tr("Original (no transcoding)"), u"raw"_s);
    m_format->addItem(tr("MP3"), u"mp3"_s);
    m_format->addItem(tr("Opus"), u"opus"_s);
    m_format->addItem(tr("AAC"), u"aac"_s);

    m_maxBitRate->setRange(0, 320);
    m_maxBitRate->setSingleStep(32);
    m_maxBitRate->setSuffix(u" kbps"_s);
    m_maxBitRate->setSpecialValueText(tr("Unlimited"));

    m_streamWhileDownloading->setToolTip(
        tr("Play right away and switch to the cached file at the same position. Disable to wait for the "
           "download before playing."));

    m_prefetch->setRange(0, 5);
    m_prefetch->setSuffix(tr(" tracks"));
    m_prefetch->setSpecialValueText(tr("None"));
    m_prefetch->setToolTip(tr("Download this many upcoming tracks ahead of playback."));

    auto* playbackForm = new QFormLayout;
    playbackForm->addRow(tr("Playback mode"), m_mode);
    playbackForm->addRow({}, m_streamWhileDownloading);
    playbackForm->addRow(tr("Prefetch ahead"), m_prefetch);
    playbackForm->addRow(tr("Stream format"), m_format);
    playbackForm->addRow(tr("Max bitrate"), m_maxBitRate);

    auto* playbackGroup = new QGroupBox{tr("Playback"), this};
    playbackGroup->setLayout(playbackForm);

    auto* browseButton = new QPushButton{tr("Browse..."), this};
    auto* clearButton  = new QPushButton{tr("Clear Cache"), this};

    auto* dirRow = new QHBoxLayout;
    dirRow->addWidget(m_downloadDir, 1);
    dirRow->addWidget(browseButton);
    dirRow->addWidget(clearButton);

    m_cacheLimit->setRange(0, 102400);
    m_cacheLimit->setSingleStep(256);
    m_cacheLimit->setSuffix(u" MB"_s);
    m_cacheLimit->setSpecialValueText(tr("Unlimited"));
    m_cacheLimit->setToolTip(tr("Oldest cached files are removed when the cache grows past this size."));

    m_cacheInfo = new QLabel{this};
    m_cacheInfo->setWordWrap(true);

    auto* cacheForm = new QFormLayout;
    cacheForm->addRow(tr("Download folder"), dirRow);
    cacheForm->addRow(tr("Cache size limit"), m_cacheLimit);
    cacheForm->addRow(tr("Cache"), m_cacheInfo);

    auto* cacheGroup = new QGroupBox{tr("Download Cache"), this};
    cacheGroup->setLayout(cacheForm);

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
    layout->addWidget(cacheGroup);
    layout->addWidget(libraryGroup);
    layout->addStretch();

    connect(testButton, &QPushButton::clicked, this, &SubsonicSettingsPageWidget::testConnection);
    connect(m_refreshFolders, &QPushButton::clicked, this, &SubsonicSettingsPageWidget::loadMusicFolders);
    connect(browseButton, &QPushButton::clicked, this, &SubsonicSettingsPageWidget::browseDownloadDir);
    connect(libraryBrowse, &QPushButton::clicked, this, &SubsonicSettingsPageWidget::browseLibraryDir);
    connect(clearButton, &QPushButton::clicked, this, &SubsonicSettingsPageWidget::clearCache);
    connect(m_format, &QComboBox::currentIndexChanged, this, &SubsonicSettingsPageWidget::updateBitRateState);
    connect(m_mode, &QComboBox::currentIndexChanged, this, &SubsonicSettingsPageWidget::updateModeState);
    connect(m_cacheLimit, &QSpinBox::valueChanged, this, &SubsonicSettingsPageWidget::updateCacheInfo);

    if(m_downloads) {
        connect(m_downloads, &SubsonicDownloadManager::downloadProgressChanged, this,
                &SubsonicSettingsPageWidget::updateCacheInfo);
        connect(m_downloads, &SubsonicDownloadManager::cacheStateChanged, this,
                [this](const QString&) { updateCacheInfo(); });
    }

    updateBitRateState();
    updateModeState();
    updateCacheInfo();
}

SubsonicSettings SubsonicSettingsPageWidget::uiSettings() const
{
    SubsonicSettings settings;
    settings.server          = m_server->text().trimmed();
    settings.username        = m_username->text().trimmed();
    settings.password        = m_password->text();
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

    m_musicFolder->clear();
    m_musicFolder->addItem(tr("All folders"), QString{});
    if(!settings.musicFolderId.isEmpty()) {
        m_musicFolder->addItem(settings.musicFolderId, settings.musicFolderId);
        m_musicFolder->setCurrentIndex(1);
    }

    const int modeIndex = m_mode->findData(static_cast<int>(settings.playbackMode));
    m_mode->setCurrentIndex(modeIndex >= 0 ? modeIndex : 0);

    const int formatIndex = m_format->findData(settings.format);
    m_format->setCurrentIndex(formatIndex >= 0 ? formatIndex : 0);

    m_maxBitRate->setValue(settings.maxBitRate);
    m_downloadDir->setText(settings.downloadDir);
    m_libraryDir->setText(settings.libraryDir);
    m_streamWhileDownloading->setChecked(settings.streamWhileDownloading);
    m_prefetch->setValue(settings.prefetchCount);
    m_cacheLimit->setValue(settings.cacheLimitMb);
    setTestStatus({});
    updateBitRateState();
    updateModeState();
    updateCacheInfo();

    if(settings.isValid()) {
        loadMusicFolders();
    }

    qCInfo(SUBSONIC) << "Loaded settings: mode="
                     << (settings.playbackMode == PlaybackMode::DownloadFirst ? "download-first" : "stream")
                     << "format=" << settings.format << "maxBitRate=" << settings.maxBitRate
                     << "downloadDir=" << settings.downloadDir << "musicFolder=" << settings.musicFolderId
                     << "ignoreSsl=" << settings.ignoreSslErrors;
}

void SubsonicSettingsPageWidget::apply()
{
    SubsonicSettings settings;
    settings.server       = m_server->text().trimmed();
    settings.username     = m_username->text().trimmed();
    settings.password     = m_password->text();
    settings.playbackMode = static_cast<PlaybackMode>(m_mode->currentData().toInt());
    settings.format       = m_format->currentData().toString();
    settings.maxBitRate   = (settings.format == "raw"_L1) ? 0 : m_maxBitRate->value();
    settings.downloadDir  = m_downloadDir->text().trimmed();
    settings.libraryDir   = m_libraryDir->text().trimmed();
    settings.streamWhileDownloading = m_streamWhileDownloading->isChecked();
    settings.prefetchCount          = m_prefetch->value();
    settings.cacheLimitMb           = m_cacheLimit->value();
    settings.ignoreSslErrors        = m_ignoreSsl->isChecked();
    settings.musicFolderId          = m_musicFolder->currentData().toString();

    saveSubsonicSettings(m_settings, settings);
    if(m_downloads) {
        m_downloads->applyPlaybackMode();
    }
    // Persist immediately so the choice survives an unclean exit (the core only flushes every 5 minutes).
    m_settings->storeSettings();

    qCInfo(SUBSONIC) << "Applied settings: mode="
                     << (settings.playbackMode == PlaybackMode::DownloadFirst ? "download-first" : "stream")
                     << "format=" << settings.format << "maxBitRate=" << settings.maxBitRate
                     << "downloadDir=" << settings.downloadDir << "musicFolder=" << settings.musicFolderId
                     << "ignoreSsl=" << settings.ignoreSslErrors;
}

void SubsonicSettingsPageWidget::reset()
{
    m_server->clear();
    m_username->clear();
    m_password->clear();
    m_ignoreSsl->setChecked(false);
    m_musicFolder->clear();
    m_musicFolder->addItem(tr("All folders"), QString{});
    m_mode->setCurrentIndex(0);
    m_format->setCurrentIndex(0);
    m_maxBitRate->setValue(0);
    m_downloadDir->setText(defaultDownloadDir());
    m_libraryDir->setText(defaultLibraryDir());
    m_streamWhileDownloading->setChecked(true);
    m_prefetch->setValue(1);
    m_cacheLimit->setValue(0);
    setTestStatus({});
    updateBitRateState();
    updateModeState();
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

void SubsonicSettingsPageWidget::browseDownloadDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Download folder"), m_downloadDir->text());
    if(!dir.isEmpty()) {
        m_downloadDir->setText(dir);
    }
}

void SubsonicSettingsPageWidget::browseLibraryDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Library folder"), m_libraryDir->text());
    if(!dir.isEmpty()) {
        m_libraryDir->setText(dir);
    }
}

void SubsonicSettingsPageWidget::clearCache()
{
    const auto answer = QMessageBox::question(this, tr("Foosonic"), tr("Delete all cached Subsonic downloads?"),
                                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if(answer != QMessageBox::Yes) {
        return;
    }

    if(m_downloads) {
        m_downloads->clearCache();
    }
    else {
        const QString dir = m_downloadDir->text().trimmed();
        if(!dir.isEmpty()) {
            QDir{dir}.removeRecursively();
        }
    }
    updateCacheInfo();
}

void SubsonicSettingsPageWidget::updateBitRateState()
{
    m_maxBitRate->setEnabled(m_format->currentData().toString() != "raw"_L1);
}

void SubsonicSettingsPageWidget::updateModeState()
{
    m_streamWhileDownloading->setEnabled(
        static_cast<PlaybackMode>(m_mode->currentData().toInt()) == PlaybackMode::DownloadFirst);
}

void SubsonicSettingsPageWidget::updateCacheInfo()
{
    if(!m_cacheInfo) {
        return;
    }

    if(!m_downloads) {
        m_cacheInfo->setText(tr("Not available"));
        return;
    }

    const qint64 bytes = m_downloads->cacheSizeBytes();
    const QString size = bytes > 0 ? formatBytes(bytes) : u"0 B"_s;
    QString info       = tr("%1 file(s), %2").arg(m_downloads->cachedCount()).arg(size);

    const int limitMb = m_cacheLimit->value();
    if(limitMb > 0) {
        const qint64 limit = static_cast<qint64>(limitMb) * 1024 * 1024;
        const int percent  = static_cast<int>(bytes * 100 / limit);
        info += tr(" of %1 MB (%2%)").arg(limitMb).arg(percent);
    }

    const QString active = m_downloads->activeDownloadName();
    if(!active.isEmpty()) {
        info += u"\n"_s + tr("Downloading: %1 (%2%)").arg(active).arg(m_downloads->activeDownloadProgress());
    }

    m_cacheInfo->setText(info);
}

SubsonicSettingsPage::SubsonicSettingsPage(std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings,
                                           SubsonicDownloadManager* downloads, QObject* parent)
    : SettingsPage{settings->settingsDialog(), parent}
{
    setId(SettingsPageId);
    setName(tr("Foosonic"));
    setCategory({tr("Foosonic")});
    setPosition(SettingsPagePosition::Last);
    setWidgetCreator([network = std::move(network), settings, downloads]() {
        return new SubsonicSettingsPageWidget{network, settings, downloads};
    });

    qCInfo(SUBSONIC) << "Registered Subsonic settings page";
}
} // namespace Fooyin::Subsonic

#include "subsonicsettingspage.moc"
