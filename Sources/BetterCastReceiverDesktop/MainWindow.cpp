#include "MainWindow.h"
#include "VideoDecoder.h"
#include "VideoRenderer.h"
#include "VideoWindow.h"
#include "NetworkListener.h"
#include "InputHandler.h"
#include "InputEvent.h"
#include "ServiceDiscovery.h"
#include "AudioDecoder.h"
#include "AudioPlayer.h"
#include "AdbHelper.h"
#include "ServiceDiscovery.h"
#ifdef ENABLE_SENDER
#include "sender/SenderController.h"
#include "sender/VirtualDisplayVDD.h"
#endif

// Sidebar chrome, theming and the rounded logo are part of the shared UI, not
// sender mode. These sat inside the ENABLE_SENDER guard above purely because
// they were added next to the sender includes, which broke the Linux receiver
// build for several commits — Windows CI passes ENABLE_SENDER=ON and never
// noticed. Keep them out here.
#include "Icons.h"
#include "Theme.h"
#include "DisplayArrangement.h"
#include <QSettings>
#include <QPainter>
#include <QPainterPath>
#include <QStyleHints>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QScreen>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QNetworkInterface>
#include <QUrl>
#include <QKeyEvent>
#include <QMouseEvent>
#include <thread>


// ─── Sidebar section header helper ──────────────────────────────────────────────

static QListWidgetItem* addSidebarSection(QListWidget* list, const QString& title) {
    auto* item = new QListWidgetItem(title);
    item->setFlags(Qt::ItemIsEnabled); // not selectable
    item->setData(Qt::UserRole, -1);
    QFont f = item->font();
    f.setPointSize(9);
    f.setBold(true);
    item->setFont(f);
    item->setForeground(QColor("#777"));
    // Add extra spacing above sections (except the first)
    if (list->count() > 0) {
        item->setSizeHint(QSize(0, 32));
    }
    list->addItem(item);
    return item;
}

static QListWidgetItem* addSidebarItem(QListWidget* list, const QString& glyph,
                                        const QString& title, int pageIndex) {
    // The glyph goes on as an icon, not into the text: list text draws in the UI
    // font, which has no private-use glyphs and renders them as empty boxes.
    auto* item = new QListWidgetItem(title);
    item->setIcon(Icons::icon(glyph));
    item->setData(Qt::UserRole, pageIndex);
    item->setSizeHint(QSize(0, 34));
    list->addItem(item);
    return item;
}

// ─── Card widget helper ─────────────────────────────────────────────────────────

static QGroupBox* makeCard(const QString& title) {
    auto* card = new QGroupBox(title);
    return card;
}

// ─── Constructor ────────────────────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("BetterCast");
    setMinimumSize(800, 500);

    // Crash detection: check if previous session exited cleanly
    QString crashMarker = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/running.lock";
    if (QFile::exists(crashMarker)) {
        // Previous session crashed — offer to report
        QTimer::singleShot(500, this, [this]() {
            auto* dialog = new QMessageBox(this);
            dialog->setIcon(QMessageBox::Warning);
            dialog->setWindowTitle("BetterCast crashed last time");
            dialog->setText("BetterCast didn't exit cleanly last time. Would you like to report this issue on GitHub?");
            dialog->setStandardButtons(QMessageBox::Yes | QMessageBox::No);
            dialog->setDefaultButton(QMessageBox::Yes);
            if (dialog->exec() == QMessageBox::Yes) {
                onReportIssue();
            }
        });
    }
    // Write crash marker (removed on clean exit)
    QDir().mkpath(QFileInfo(crashMarker).absolutePath());
    QFile marker(crashMarker);
    marker.open(QIODevice::WriteOnly);
    marker.close();

    // Create core components
    m_decoder = new VideoDecoder(this);
    m_renderer = new VideoRenderer();
    m_network = new NetworkListener(this);
    m_inputHandler = new InputHandler(this);
    m_discovery = new ServiceDiscovery(this);
    m_audioDecoder = new AudioDecoder(this);
    m_audioPlayer = new AudioPlayer(this);
    m_adbHelper = new AdbHelper(this);
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(3000);
    connect(m_reconnectTimer, &QTimer::timeout, this, &MainWindow::attemptAdbReconnect);

#ifdef ENABLE_SENDER
    m_sender = new SenderController(this);
    connect(m_sender, &SenderController::statusChanged, this, [this](const QString& status) {
        if (m_senderStatusLabel) m_senderStatusLabel->setText(status);
        LogManager::instance().log("Sender: " + status);
    });
    connect(m_sender, &SenderController::error, this, [this](const QString& msg) {
        if (m_senderStatusLabel) m_senderStatusLabel->setText("Error: " + msg);
        LogManager::instance().log("Sender error: " + msg);
    });
    connect(m_sender, &SenderController::connected, this, [this](const QString& host) {
        LogManager::instance().log(QString("Sender: Connected and streaming to %1 (%2)")
                                       .arg(host, m_sender->displayForReceiver(host)));
        // Mark the device row connected so the sidebar shows it in green.
        const int idx = indexOfDevice(host);
        if (idx >= 0) { m_devices[idx].connected = true; rebuildSidebar(); }
    });
    connect(m_sender, &SenderController::stopped, this, [this](const QString& host) {
        const int idx = indexOfDevice(host);
        if (idx >= 0) { m_devices[idx].connected = false; rebuildSidebar(); }
    });
    // Several receivers can stream at once, so the buttons reflect the total
    // rather than a single on/off state.
    connect(m_sender, &SenderController::sessionsChanged, this, [this]() {
        const int n = m_sender->sessionCount();
        if (m_arrangement) {
            QStringList live;
            for (const QString& host : m_sender->activeReceivers()) {
                const QString d = m_sender->displayForReceiver(host);
                if (!d.isEmpty()) live << d;
            }
            m_arrangement->setActiveDisplays(live);
        }
        if (m_stopSendBtn) m_stopSendBtn->setEnabled(n > 0);
        if (m_sendBtn) m_sendBtn->setEnabled(true);          // more receivers may be added
        if (m_sendHostEdit) m_sendHostEdit->setEnabled(true);
        if (m_senderStatusLabel && n > 0) {
            m_senderStatusLabel->setText(
                QString("Streaming to %1 receiver%2").arg(n).arg(n == 1 ? "" : "s"));
        }
    });

    // mDNS browsing for receiver discovery
    connect(m_discovery, &ServiceDiscovery::serviceFound,
            this, &MainWindow::onReceiverDiscovered);
#endif

    // Wire up core components
    m_network->setup(m_decoder, m_renderer, m_audioDecoder);

    connect(m_audioDecoder, &AudioDecoder::pcmDecoded,
            m_audioPlayer, &AudioPlayer::onPcmDecoded);

    connect(m_decoder, &VideoDecoder::frameDecoded,
            m_renderer, &VideoRenderer::onFrameDecoded);
    connect(m_decoder, &VideoDecoder::dimensionsChanged,
            m_renderer, [this](int w, int h) {
                m_inputHandler->setContentSize(QSize(w, h));
            });
    connect(m_decoder, &VideoDecoder::keyframeNeeded,
            this, [this]() {
                // Request IDR from sender on decode errors (throttled to every 2s)
                static QDateTime lastRequest;
                if (lastRequest.msecsTo(QDateTime::currentDateTime()) > 2000) {
                    LogManager::instance().log("Decoder: Requesting keyframe from sender (decode error recovery)");
                    m_network->sendInputEvent(InputEvent(InputEventType::Command, 0, 0, kIDRRequestKeyCode));
                    lastRequest = QDateTime::currentDateTime();
                }
            });

    m_inputHandler->attach(m_renderer);
    connect(m_inputHandler, &InputHandler::inputEvent,
            m_network, &NetworkListener::sendInputEvent);

    connect(m_adbHelper, &AdbHelper::statusChanged,
            this, &MainWindow::onStatusChanged);

    connect(m_network, &NetworkListener::connectionEstablished,
            this, &MainWindow::onConnectionEstablished);
    connect(m_network, &NetworkListener::connectionLost,
            this, &MainWindow::onConnectionLost);
    connect(m_network, &NetworkListener::statusChanged,
            this, &MainWindow::onStatusChanged);

    // Create video window (separate window, like Mac app)
    m_videoWindow = new VideoWindow(m_renderer, m_inputHandler, this);
    connect(m_videoWindow, &VideoWindow::windowClosed, this, [this]() {
        LogManager::instance().log("Video window closed by user");
    });

    connect(m_renderer, &VideoRenderer::videoSizeChanged,
            this, &MainWindow::onVideoSizeChanged);

    // LogManager
    connect(&LogManager::instance(), &LogManager::logAdded,
            this, &MainWindow::onLogAdded);

    setupUi();

    // Start services
    m_network->start();
    uint16_t actualPort = m_network->actualTcpPort();
    m_discovery->startAdvertising(actualPort);
#ifdef ENABLE_SENDER
    m_discovery->startBrowsing();
#endif
    LogManager::instance().log(QString("BetterCast started — listening on port %1").arg(actualPort));
#ifdef _WIN32
    QByteArray fwStatus = qgetenv("BETTERCAST_FW_STATUS");
    if (fwStatus == "ok") {
        LogManager::instance().log("Firewall: Rules added (mDNS + TCP)");
    } else if (fwStatus == "failed") {
        LogManager::instance().log("Firewall: Rules NOT added — run as Administrator once for auto-discovery");
    } else {
        LogManager::instance().log("Firewall: Rules already exist");
    }
#endif

    // Default window size (landscape 16:9, 70% screen)
    QScreen* screen = QApplication::primaryScreen();
    if (screen) {
        QRect available = screen->availableGeometry();
        int w = static_cast<int>(available.width() * 0.7);
        int h = w * 9 / 16;
        int x = available.x() + (available.width() - w) / 2;
        int y = available.y() + (available.height() - h) / 2;
        setGeometry(x, y, w, h);
    }
}

MainWindow::~MainWindow() {
    m_discovery->stopAdvertising();
    // Clean exit — remove crash marker
    QString crashMarker = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/running.lock";
    QFile::remove(crashMarker);
}

// ─── UI Setup ───────────────────────────────────────────────────────────────────

// Occasional nudge towards the donation page.
//
// Honour system: there is no way for the app to learn that someone paid — Whop
// does not tell it — so "I've already donated" is taken at face value and
// silences it for good. Deliberately NOT shown on every launch: a nag that
// appears each time is the fastest route to one-star reviews. Every fifth
// launch, and never before the fifth, so a first impression is never a request
// for money.
void MainWindow::maybeShowSupportPrompt() {
    QSettings settings("BetterCast", "BetterCast");
    if (settings.value("support/dismissedForever", false).toBool()) return;

    const int launches = settings.value("support/launchCount", 0).toInt() + 1;
    settings.setValue("support/launchCount", launches);
    if (launches < 5 || launches % 5 != 0) return;

    QMessageBox box(this);
    box.setWindowTitle("Support BetterCast");
    box.setIconPixmap(Icons::rounded(QPixmap(":/appicon.png"), 56));
    box.setText("<b>Enjoying BetterCast?</b>");
    box.setInformativeText(
        "BetterCast is free and open source, built in my spare time.\n\n"
        "If it is useful to you, a donation helps keep it going.");

    auto* donate = box.addButton("Buy me a coffee", QMessageBox::AcceptRole);
    auto* later = box.addButton("Maybe later", QMessageBox::RejectRole);
    auto* never = box.addButton("I've already donated", QMessageBox::DestructiveRole);
    box.setDefaultButton(later);
    box.exec();

    if (box.clickedButton() == donate) {
        QDesktopServices::openUrl(QUrl("https://whop.com/bettercast/bettercast-donate/"));
        // Opening the page is treated as done — pestering someone who just
        // followed the link is the worst possible follow-up.
        settings.setValue("support/dismissedForever", true);
    } else if (box.clickedButton() == never) {
        settings.setValue("support/dismissedForever", true);
    }
}

void MainWindow::applyTheme() {
    const Theme::Palette p = Theme::activePalette();
    // Palette first: the stylesheet and many inline styles reference
    // palette(window-text) / palette(mid), so they resolve against this.
    qApp->setPalette(Theme::qtPalette(p));
    setStyleSheet(Theme::stylesheet(p));
    Theme::applyWindowBackdrop(this, p);
}

void MainWindow::setupUi() {
    applyTheme();

    // Follow the OS light/dark setting live, so flipping Windows' app theme
    // does not require restarting BetterCast.
    if (auto* hints = QGuiApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this,
                [this](Qt::ColorScheme) { applyTheme(); });
    }

    m_splitter = new QSplitter(Qt::Horizontal, this);
    setCentralWidget(m_splitter);

    // Sidebar, with a footer pinned beneath it (macOS uses safeAreaInset for
    // the same effect).
    m_sidebarList = new QListWidget();
    m_sidebarList->setFocusPolicy(Qt::NoFocus);
    m_sidebarList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* sidebarPanel = new QWidget();
    sidebarPanel->setFixedWidth(230);
    auto* sidebarLayout = new QVBoxLayout(sidebarPanel);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);
    sidebarLayout->addWidget(m_sidebarList, 1);

    auto* footer = new QWidget();
    auto* footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(16, 10, 16, 12);
    footerLayout->setSpacing(6);

    auto* donateBtn = new QPushButton("  Support BetterCast");
    donateBtn->setIcon(Icons::icon(Icons::heart(), QColor("#e0568a")));
    donateBtn->setCursor(Qt::PointingHandCursor);
    donateBtn->setToolTip("Open the BetterCast donation page on Whop");
    donateBtn->setStyleSheet(
        "QPushButton { background: transparent; border: 1px solid palette(mid); "
        "border-radius: 8px; padding: 7px 10px; font-size: 12px; }"
        "QPushButton:hover { background: rgba(224, 86, 138, 0.12); }");
    connect(donateBtn, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl("https://whop.com/bettercast/bettercast-donate/"));
    });
    footerLayout->addWidget(donateBtn);

    auto* madeWith = new QLabel(QString::fromUtf8("Made with \xE2\x99\xA5 by Stephen Lovino"));
    madeWith->setAlignment(Qt::AlignCenter);
    madeWith->setStyleSheet("font-size: 11px; color: palette(mid);");
    footerLayout->addWidget(madeWith);

    sidebarLayout->addWidget(footer);

    // Detail stack
    m_stack = new QStackedWidget();

    // Build pages — order matters for page indices
    setupOverviewPage();
#ifdef ENABLE_SENDER
    setupSendPage();
    setupDevicePage();
#endif
    setupReceivePage();
    setupSettingsPage();
    setupLogsPage();

    // Build sidebar
    setupSidebar();

    // Assemble splitter
    m_splitter->addWidget(sidebarPanel);
    m_splitter->addWidget(m_stack);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setCollapsible(0, false);
    m_splitter->setCollapsible(1, false);

    // Connect sidebar selection
    connect(m_sidebarList, &QListWidget::currentRowChanged,
            this, &MainWindow::onSidebarSelectionChanged);

    // Select Overview by default
    selectSidebarItem(m_pageOverview);

    // Deferred so it appears over a drawn window rather than mid-construction.
    QTimer::singleShot(1200, this, [this]() { maybeShowSupportPrompt(); });
}

void MainWindow::setupSidebar() {
    rebuildSidebar();
}

int MainWindow::indexOfDevice(const QString& name) const {
    for (int i = 0; i < m_devices.size(); i++) {
        if (m_devices[i].name == name) return i;
    }
    return -1;
}

// Device-centric sidebar, mirroring the macOS SidebarView: discovered devices
// are rows under "Devices" rather than being buried in a dropdown on a Send
// page. Rebuilt from scratch whenever the device list changes, so it has to
// preserve the current selection itself.
void MainWindow::rebuildSidebar() {
    if (!m_sidebarList) return;

    const QString keepDevice = m_selectedDeviceName;
    const int keepPage = m_sidebarList->currentItem()
                             ? m_sidebarList->currentItem()->data(Qt::UserRole).toInt()
                             : m_pageOverview;

    QSignalBlocker block(m_sidebarList);   // repopulating must not fire selection
    m_sidebarList->clear();

    addSidebarSection(m_sidebarList, "DEVICES");
    addSidebarItem(m_sidebarList, Icons::overview(), "Overview", m_pageOverview);

#ifdef ENABLE_SENDER
    if (m_devices.isEmpty()) {
        auto* searching = new QListWidgetItem("     Searching\xE2\x80\xA6");
        searching->setFlags(Qt::ItemIsEnabled);
        searching->setData(Qt::UserRole, -1);
        searching->setForeground(QColor("#777"));
        searching->setSizeHint(QSize(0, 30));
        m_sidebarList->addItem(searching);
    } else {
        for (const auto& dev : m_devices) {
            auto* item = new QListWidgetItem(dev.name);
            item->setIcon(Icons::icon(Icons::forDeviceName(dev.name),
                                      dev.connected ? QColor("#4caf50") : QColor("#c8c8c8")));
            item->setData(Qt::UserRole, m_pageDevice);
            item->setData(Qt::UserRole + 1, dev.name);
            item->setSizeHint(QSize(0, 42));
            item->setToolTip(QString("%1\n%2:%3\n%4")
                                 .arg(dev.name, dev.host)
                                 .arg(dev.port)
                                 .arg(dev.connected ? "Connected" : "Available"));
            if (dev.connected) item->setForeground(QColor("#4caf50"));
            m_sidebarList->addItem(item);
        }
    }

    addSidebarSection(m_sidebarList, "SEND");
    addSidebarItem(m_sidebarList, Icons::send(), "Send Screen", m_pageSend);
#endif

    addSidebarSection(m_sidebarList, "RECEIVE");
    addSidebarItem(m_sidebarList, Icons::receive(), "Receive Screen", m_pageReceive);

    addSidebarSection(m_sidebarList, "");
    addSidebarItem(m_sidebarList, Icons::settings(), "Settings", m_pageSettings);
    addSidebarItem(m_sidebarList, Icons::logs(), "Logs", m_pageLogs);

    // Restore selection: prefer the same device, else the same fixed page.
    for (int i = 0; i < m_sidebarList->count(); i++) {
        auto* item = m_sidebarList->item(i);
        if (!(item->flags() & Qt::ItemIsSelectable)) continue;
        const QString dev = item->data(Qt::UserRole + 1).toString();
        if (!keepDevice.isEmpty() ? dev == keepDevice
                                  : (dev.isEmpty() && item->data(Qt::UserRole).toInt() == keepPage)) {
            m_sidebarList->setCurrentItem(item);
            return;
        }
    }
    selectSidebarItem(keepPage >= 0 ? keepPage : m_pageOverview);
}

// ─── Overview Page ──────────────────────────────────────────────────────────────

void MainWindow::setupOverviewPage() {
    auto* page = new QWidget();
    auto* scroll = new QScrollArea();
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);

    auto* layout = new QVBoxLayout(page);
    layout->setAlignment(Qt::AlignCenter);
    layout->setContentsMargins(40, 40, 40, 40);
    layout->setSpacing(14);

    // App icon
    auto* iconLabel = new QLabel();
    QPixmap appIcon(":/appicon.png");
    if (!appIcon.isNull()) {
        iconLabel->setPixmap(Icons::rounded(appIcon, 80));
    }
    iconLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(iconLabel);

    // Title
    auto* title = new QLabel("BetterCast");
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: palette(window-text);");
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    auto* subtitle = new QLabel("Turn any device into a wireless extended display");
    subtitle->setStyleSheet("font-size: 14px; color: palette(mid);");
    subtitle->setAlignment(Qt::AlignCenter);
    layout->addWidget(subtitle);

    layout->addSpacing(24);

    // Getting started steps
    auto* stepsCard = makeCard("Getting Started");
    auto* stepsLayout = new QVBoxLayout(stepsCard);
    stepsLayout->setSpacing(16);

    auto addStep = [&](int num, const QString& title, const QString& desc) {
        auto* row = new QHBoxLayout();
        row->setSpacing(12);

        auto* numLabel = new QLabel(QString::number(num));
        numLabel->setFixedSize(28, 28);
        numLabel->setAlignment(Qt::AlignCenter);
        numLabel->setStyleSheet(
            "background-color: #0078D4; color: #ffffff; font-weight: bold; "
            "font-size: 13px; border-radius: 14px;");
        row->addWidget(numLabel);

        auto* textLayout = new QVBoxLayout();
        textLayout->setSpacing(2);
        auto* titleLabel = new QLabel(title);
        titleLabel->setStyleSheet("font-size: 14px; font-weight: bold; color: palette(window-text);");
        textLayout->addWidget(titleLabel);
        auto* descLabel = new QLabel(desc);
        descLabel->setStyleSheet("font-size: 12px; color: palette(mid);");
        descLabel->setWordWrap(true);
        textLayout->addWidget(descLabel);
        row->addLayout(textLayout, 1);

        stepsLayout->addLayout(row);
    };

    addStep(1, "Download the Receiver",
            "Install BetterCast Receiver on your iPad, Android, Windows, Linux, or Mac.");
    addStep(2, "Connect to the Same Network",
            "Make sure both devices are on the same Wi-Fi network.");
    addStep(3, "Open the Receiver App",
            "Your device will appear automatically, or use Manual IP to connect.");

    layout->addWidget(stepsCard);

    layout->addSpacing(12);

    // Display arrangement — the macOS overview's "Displays" card. Shows the
    // real screen on its own until virtual displays are attached, so it is
    // useful even with nothing connected.
    auto* displaysCard = makeCard("Displays");
    auto* displaysLayout = new QVBoxLayout(displaysCard);
    displaysLayout->setSpacing(8);

    m_arrangement = new DisplayArrangement();
    displaysLayout->addWidget(m_arrangement);

    auto* arrangeHint = new QLabel(
        "Green marks a display being streamed. Drag displays around in Windows "
        "Display Settings to change the arrangement.");
    arrangeHint->setWordWrap(true);
    arrangeHint->setStyleSheet("font-size: 11px; color: palette(mid);");
    displaysLayout->addWidget(arrangeHint);

    layout->addWidget(displaysCard);
    layout->addSpacing(12);

    // Status
    m_overviewStatusLabel = new QLabel("Searching for devices on your network...");
    m_overviewStatusLabel->setStyleSheet("font-size: 12px; color: palette(mid);");
    m_overviewStatusLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_overviewStatusLabel);

    // Local IP
    m_overviewIpLabel = new QLabel();
    m_overviewIpLabel->setStyleSheet("font-size: 12px; color: palette(mid);");
    m_overviewIpLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_overviewIpLabel);

    layout->addStretch();

    m_pageOverview = m_stack->addWidget(scroll);
    updateLocalIpDisplay();
}

// ─── Send Screen Page (ENABLE_SENDER) ───────────────────────────────────────────

#ifdef ENABLE_SENDER
void MainWindow::setupSendPage() {
    auto* page = new QWidget();
    auto* scroll = new QScrollArea();
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(40, 30, 40, 30);
    layout->setSpacing(16);

    auto* pageTitle = new QLabel("Send Screen");
    pageTitle->setStyleSheet("font-size: 22px; font-weight: bold; color: palette(window-text);");
    layout->addWidget(pageTitle);

    auto* pageDesc = new QLabel("Stream your screen to a BetterCast receiver on another device.");
    pageDesc->setStyleSheet("font-size: 13px; color: palette(mid);");
    pageDesc->setWordWrap(true);
    layout->addWidget(pageDesc);

    layout->addSpacing(8);

    // ─── Virtual Display card ──────────────────────────────────────────
    auto* vddCard = makeCard("Virtual Display (Extend Screen)");
    auto* vddLayout = new QVBoxLayout(vddCard);
    vddLayout->setSpacing(10);

    // VDD status
    m_vddStatusLabel = new QLabel();
    bool vddInstalled = m_sender && m_sender->vdd() && m_sender->vdd()->isVddInstalled();
    if (vddInstalled) {
        m_vddStatusLabel->setText("Virtual Display Driver detected");
        m_vddStatusLabel->setStyleSheet("font-size: 13px; color: #4caf50;");
    } else {
        m_vddStatusLabel->setText(
            "Virtual Display Driver not found — install from "
            "github.com/itsmikethetech/Virtual-Display-Driver");
        m_vddStatusLabel->setStyleSheet("font-size: 12px; color: #ff9800;");
    }
    m_vddStatusLabel->setWordWrap(true);
    vddLayout->addWidget(m_vddStatusLabel);

    // Re-check button (shown when VDD not detected)
    m_recheckVddBtn = new QPushButton("Re-check VDD Installation");
    m_recheckVddBtn->setVisible(!vddInstalled);
    m_recheckVddBtn->setStyleSheet(
        "QPushButton { background-color: #333; color: #4da6ff; padding: 6px 14px; "
        "border-radius: 5px; font-size: 12px; border: 1px solid #4da6ff; }"
        "QPushButton:hover { background-color: #1a3a5c; }");
    connect(m_recheckVddBtn, &QPushButton::clicked, this, [this]() {
        if (!m_sender || !m_sender->vdd()) return;
        m_sender->vdd()->refreshInstallStatus();
        bool found = m_sender->vdd()->isVddInstalled();
        if (found) {
            m_vddStatusLabel->setText("Virtual Display Driver detected");
            m_vddStatusLabel->setStyleSheet("font-size: 13px; color: #4caf50;");
            m_recheckVddBtn->setVisible(false);
            m_vddResolutionCombo->setEnabled(true);
            m_createVddBtn->setEnabled(true);
            if (m_topologyCombo) m_topologyCombo->setEnabled(true);
            if (m_applyTopologyBtn) m_applyTopologyBtn->setEnabled(true);
        } else {
            m_vddStatusLabel->setText(
                "Still not detected — make sure VDD is installed and try restarting the app");
            m_vddStatusLabel->setStyleSheet("font-size: 12px; color: #ff9800;");
        }
    });
    vddLayout->addWidget(m_recheckVddBtn);

    // Resolution picker
    auto* resRow = new QHBoxLayout();
    auto* resLabel = new QLabel("Resolution:");
    resLabel->setStyleSheet("font-size: 13px; color: palette(mid);");
    resRow->addWidget(resLabel);

    m_vddResolutionCombo = new QComboBox();
    m_vddResolutionCombo->addItem("1920 x 1080 @ 60Hz", QVariant::fromValue(QSize(1920, 1080)));
    m_vddResolutionCombo->addItem("2560 x 1440 @ 60Hz", QVariant::fromValue(QSize(2560, 1440)));
    m_vddResolutionCombo->addItem("3840 x 2160 @ 60Hz", QVariant::fromValue(QSize(3840, 2160)));
    m_vddResolutionCombo->addItem("1920 x 1200 @ 60Hz", QVariant::fromValue(QSize(1920, 1200)));
    m_vddResolutionCombo->addItem("2560 x 1600 @ 60Hz", QVariant::fromValue(QSize(2560, 1600)));
    m_vddResolutionCombo->addItem("1280 x 720 @ 60Hz",  QVariant::fromValue(QSize(1280, 720)));
    m_vddResolutionCombo->addItem("1024 x 768 @ 60Hz",  QVariant::fromValue(QSize(1024, 768)));
    m_vddResolutionCombo->setFixedWidth(240);
    m_vddResolutionCombo->setEnabled(vddInstalled);
    resRow->addWidget(m_vddResolutionCombo);
    resRow->addStretch();
    vddLayout->addLayout(resRow);

    // Create / Remove buttons
    auto* vddBtnRow = new QHBoxLayout();
    vddBtnRow->setSpacing(10);

    m_createVddBtn = new QPushButton("Create Virtual Display");
    m_createVddBtn->setEnabled(vddInstalled);
    m_createVddBtn->setStyleSheet(
        "QPushButton { background-color: #4caf50; color: #ffffff; font-weight: bold; "
        "padding: 8px 18px; border-radius: 6px; border: none; }"
        "QPushButton:hover { background-color: #66bb6a; }"
        "QPushButton:disabled { background-color: #2a2a2a; color: palette(mid); }");
    connect(m_createVddBtn, &QPushButton::clicked, this, &MainWindow::onCreateVirtualDisplay);
    vddBtnRow->addWidget(m_createVddBtn);

    m_removeVddBtn = new QPushButton("Remove");
    m_removeVddBtn->setToolTip("Remove all virtual displays. Displays created by the "
                               "installer are device nodes, so this asks for "
                               "administrator approval once.");
    m_removeVddBtn->setEnabled(false);
    m_removeVddBtn->setStyleSheet(
        "QPushButton { background-color: #333; color: palette(mid); padding: 8px 18px; "
        "border-radius: 6px; font-size: 13px; border: 1px solid #555; }"
        "QPushButton:hover { background-color: #444; }"
        "QPushButton:disabled { background-color: #2a2a2a; color: palette(mid); }");
    connect(m_removeVddBtn, &QPushButton::clicked, this, &MainWindow::onRemoveVirtualDisplay);
    vddBtnRow->addWidget(m_removeVddBtn);

    // The four Win+P projection modes, so users need not leave the app.
    m_topologyCombo = new QComboBox();
    m_topologyCombo->addItem("Extend",
        static_cast<int>(VirtualDisplayVDD::Topology::Extend));
    m_topologyCombo->addItem("Duplicate",
        static_cast<int>(VirtualDisplayVDD::Topology::Duplicate));
    m_topologyCombo->addItem("PC screen only",
        static_cast<int>(VirtualDisplayVDD::Topology::InternalOnly));
    m_topologyCombo->addItem("Second screen only",
        static_cast<int>(VirtualDisplayVDD::Topology::ExternalOnly));
    m_topologyCombo->setToolTip("Same options as Win+P, applied without opening "
                                "Display Settings");
    m_topologyCombo->setEnabled(vddInstalled);
    vddBtnRow->addWidget(m_topologyCombo);

    m_applyTopologyBtn = new QPushButton("Apply");
    m_applyTopologyBtn->setEnabled(vddInstalled);
    m_applyTopologyBtn->setStyleSheet(
        "QPushButton { background-color: #333; color: palette(mid); padding: 8px 18px; "
        "border-radius: 6px; font-size: 13px; border: 1px solid #555; }"
        "QPushButton:hover { background-color: #444; }"
        "QPushButton:disabled { background-color: #2a2a2a; color: palette(mid); }");
    connect(m_applyTopologyBtn, &QPushButton::clicked, this, &MainWindow::onExtendDisplays);
    vddBtnRow->addWidget(m_applyTopologyBtn);

    vddBtnRow->addStretch();
    vddLayout->addLayout(vddBtnRow);

    // Adding a display reconfigures the desktop, so a flicker is expected
    // rather than a fault. Saying so up front avoids it reading as a crash.
    auto* flickerNote = new QLabel(
        "Adding a virtual display briefly reconfigures your desktop — expect your "
        "screens to flicker or go black for a second. Existing streams may need "
        "restarting afterwards.");
    flickerNote->setWordWrap(true);
    flickerNote->setStyleSheet("font-size: 11px; color: palette(mid);");
    vddLayout->addWidget(flickerNote);

    layout->addWidget(vddCard);

    // ─── Monitor Selection card ────────────────────────────────────────
    auto* monCard = makeCard("Monitor to Stream");
    auto* monLayout = new QVBoxLayout(monCard);
    monLayout->setSpacing(10);

    auto* monDesc = new QLabel("Select which display to capture and stream:");
    monDesc->setStyleSheet("font-size: 12px; color: palette(mid);");
    monLayout->addWidget(monDesc);

    auto* monRow = new QHBoxLayout();
    m_monitorCombo = new QComboBox();
    m_monitorCombo->setMinimumWidth(300);
    connect(m_monitorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onMonitorSelected);
    monRow->addWidget(m_monitorCombo);

    auto* refreshBtn = new QPushButton("Refresh");
    refreshBtn->setStyleSheet(
        "QPushButton { background-color: #333; color: palette(mid); padding: 6px 14px; "
        "border-radius: 6px; font-size: 12px; border: 1px solid #555; }"
        "QPushButton:hover { background-color: #444; }");
    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefreshMonitors);
    monRow->addWidget(refreshBtn);

    monRow->addStretch();
    monLayout->addLayout(monRow);

    layout->addWidget(monCard);

    // Populate monitor list
    onRefreshMonitors();

    // Enable Remove button if virtual displays already exist from previous sessions
    if (m_removeVddBtn && m_monitorCombo) {
        for (int i = 0; i < m_monitorCombo->count(); i++) {
            QVariantMap data = m_monitorCombo->itemData(i).toMap();
            if (data.value("virtual", false).toBool()) {
                m_removeVddBtn->setEnabled(true);
                break;
            }
        }
    }

    // ─── Connection card ───────────────────────────────────────────────
    auto* connCard = makeCard("Target Receiver");
    auto* connLayout = new QVBoxLayout(connCard);
    connLayout->setSpacing(12);

    // Discovered receivers dropdown
    auto* discLabel = new QLabel("Discovered Receivers:");
    discLabel->setStyleSheet("font-size: 13px; color: palette(mid);");
    connLayout->addWidget(discLabel);

    m_receiverCombo = new QComboBox();
    m_receiverCombo->addItem("Searching for receivers...");
    m_receiverCombo->setEnabled(false);
    connect(m_receiverCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onReceiverSelected);
    connLayout->addWidget(m_receiverCombo);

    connLayout->addSpacing(4);

    auto* orLabel = new QLabel(QString::fromUtf8("\xe2\x80\x94 or enter IP manually \xe2\x80\x94"));
    orLabel->setStyleSheet("font-size: 11px; color: palette(mid);");
    orLabel->setAlignment(Qt::AlignCenter);
    connLayout->addWidget(orLabel);

    auto* hostRow = new QHBoxLayout();
    auto* hostLabel = new QLabel("Receiver IP:");
    hostLabel->setStyleSheet("font-size: 13px; color: palette(mid);");
    hostRow->addWidget(hostLabel);
    m_sendHostEdit = new QLineEdit();
    m_sendHostEdit->setPlaceholderText("e.g. 192.168.1.50");
    m_sendHostEdit->setFixedWidth(200);
    hostRow->addWidget(m_sendHostEdit);
    hostRow->addStretch();
    connLayout->addLayout(hostRow);

    layout->addWidget(connCard);

    // ─── Quality card ──────────────────────────────────────────────────
    auto* qualCard = makeCard("Stream Quality");
    auto* qualLayout = new QVBoxLayout(qualCard);
    qualLayout->setSpacing(10);

    auto* fpsRow = new QHBoxLayout();
    auto* fpsLabel = new QLabel("Frame Rate:");
    fpsLabel->setStyleSheet("font-size: 13px; color: palette(mid);");
    fpsRow->addWidget(fpsLabel);
    m_fpsSpinBox = new QSpinBox();
    m_fpsSpinBox->setRange(15, 120);
    m_fpsSpinBox->setValue(60);
    m_fpsSpinBox->setSuffix(" FPS");
    m_fpsSpinBox->setFixedWidth(100);
    fpsRow->addWidget(m_fpsSpinBox);
    fpsRow->addStretch();
    qualLayout->addLayout(fpsRow);

    auto* brRow = new QHBoxLayout();
    auto* brLabel = new QLabel("Bitrate:");
    brLabel->setStyleSheet("font-size: 13px; color: palette(mid);");
    brRow->addWidget(brLabel);
    m_bitrateSpinBox = new QSpinBox();
    m_bitrateSpinBox->setRange(2, 100);
    m_bitrateSpinBox->setValue(20);
    m_bitrateSpinBox->setSuffix(" Mbps");
    m_bitrateSpinBox->setFixedWidth(100);
    brRow->addWidget(m_bitrateSpinBox);
    brRow->addStretch();
    qualLayout->addLayout(brRow);

    layout->addWidget(qualCard);

    // ─── Action buttons ────────────────────────────────────────────────
    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(12);

    m_sendBtn = new QPushButton("Send Screen");
    m_sendBtn->setStyleSheet(
        "QPushButton { background-color: #0078D4; color: #ffffff; font-weight: bold; "
        "font-size: 14px; padding: 10px 24px; border-radius: 8px; border: none; }"
        "QPushButton:hover { background-color: #1a8ae8; }"
        "QPushButton:disabled { background-color: #2a2a2a; color: palette(mid); }");
    connect(m_sendBtn, &QPushButton::clicked, this, &MainWindow::onSendScreenClicked);
    btnRow->addWidget(m_sendBtn);

    m_stopSendBtn = new QPushButton("Stop");
    m_stopSendBtn->setEnabled(false);
    m_stopSendBtn->setStyleSheet(
        "QPushButton { background-color: #d32f2f; color: #ffffff; font-weight: bold; "
        "font-size: 14px; padding: 10px 24px; border-radius: 8px; border: none; }"
        "QPushButton:hover { background-color: #e53935; }"
        "QPushButton:disabled { background-color: #2a2a2a; color: palette(mid); }");
    connect(m_stopSendBtn, &QPushButton::clicked, this, &MainWindow::onStopSendingClicked);
    btnRow->addWidget(m_stopSendBtn);

    btnRow->addStretch();
    layout->addLayout(btnRow);

    // Status
    m_senderStatusLabel = new QLabel("Select a monitor and receiver to start streaming");
    m_senderStatusLabel->setStyleSheet("font-size: 12px; color: palette(mid);");
    m_senderStatusLabel->setWordWrap(true);
    layout->addWidget(m_senderStatusLabel);

    layout->addStretch();

    m_pageSend = m_stack->addWidget(scroll);
}
#endif

// ─── Receive Page ───────────────────────────────────────────────────────────────

void MainWindow::setupReceivePage() {
    auto* page = new QWidget();
    auto* scroll = new QScrollArea();
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(40, 30, 40, 30);
    layout->setSpacing(16);

    auto* pageTitle = new QLabel("Receive Screen");
    pageTitle->setStyleSheet("font-size: 22px; font-weight: bold; color: palette(window-text);");
    layout->addWidget(pageTitle);

    // Listening status card (prominent, like Mac's Start Listening)
    auto* listenCard = makeCard("Listening for Senders");
    auto* listenLayout = new QVBoxLayout(listenCard);
    listenLayout->setSpacing(10);

    // Status indicator
    m_recvStatusLabel = new QLabel("Listening on port 51820");
    m_recvStatusLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: #4da6ff;");
    listenLayout->addWidget(m_recvStatusLabel);

    m_recvIpLabel = new QLabel();
    m_recvIpLabel->setStyleSheet("font-size: 13px; color: palette(mid);");
    m_recvIpLabel->setWordWrap(true);
    listenLayout->addWidget(m_recvIpLabel);

    auto* instrLabel = new QLabel(
        "This device is ready to receive. On the sender device:\n"
        "  1. Open BetterCast and go to Send Screen\n"
        "  2. This device should appear automatically\n"
        "  3. Or enter this device's IP address manually");
    instrLabel->setStyleSheet("color: palette(mid); font-size: 12px;");
    instrLabel->setWordWrap(true);
    listenLayout->addWidget(instrLabel);

    layout->addWidget(listenCard);

    // Manual connect card (secondary)
    auto* manualCard = makeCard("Connect to a Sender (Manual)");
    auto* manualLayout = new QVBoxLayout(manualCard);
    manualLayout->setSpacing(10);

    auto* manualDesc = new QLabel("Connect to a sender that isn't auto-discovered:");
    manualDesc->setStyleSheet("font-size: 12px; color: palette(mid);");
    manualLayout->addWidget(manualDesc);

    auto* connRow = new QHBoxLayout();
    connRow->setSpacing(8);

    m_hostEdit = new QLineEdit();
    m_hostEdit->setPlaceholderText("Sender IP address");
    m_hostEdit->setFixedWidth(180);
    connRow->addWidget(m_hostEdit);

    m_portEdit = new QLineEdit("51820");
    m_portEdit->setPlaceholderText("Port");
    m_portEdit->setFixedWidth(80);
    connRow->addWidget(m_portEdit);

    m_connectBtn = new QPushButton("Connect");
    m_connectBtn->setStyleSheet(
        "QPushButton { background-color: #0078D4; color: #ffffff; font-weight: bold; "
        "padding: 8px 20px; border-radius: 6px; border: none; }"
        "QPushButton:hover { background-color: #1a8ae8; }"
        "QPushButton:disabled { background-color: #2a2a2a; color: palette(mid); }");
    connect(m_connectBtn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connRow->addWidget(m_connectBtn);

    connRow->addStretch();
    manualLayout->addLayout(connRow);

    layout->addWidget(manualCard);

    // ADB card
    auto* adbCard = makeCard("Android (ADB)");
    auto* adbLayout = new QVBoxLayout(adbCard);
    adbLayout->setSpacing(10);

    m_adbBtn = new QPushButton("Connect to Android (ADB)");
    m_adbBtn->setStyleSheet(
        "QPushButton { background-color: #3ddc84; color: black; font-weight: bold; "
        "padding: 10px 20px; border-radius: 8px; font-size: 14px; border: none; }"
        "QPushButton:hover { background-color: #50e898; }"
        "QPushButton:disabled { background-color: #2a2a2a; color: palette(mid); }");
    connect(m_adbBtn, &QPushButton::clicked, this, &MainWindow::onAdbConnectClicked);
    adbLayout->addWidget(m_adbBtn);

    m_adbHelpLabel = new QLabel(
        "To mirror your Android screen:\n"
        "1. Enable Developer Options (tap Build Number 7x in Settings > About)\n"
        "2. Enable USB Debugging in Developer Options\n"
        "3. Connect Android to this computer via USB\n"
        "4. Open BetterCast on Android and tap \"Start Casting\"\n"
        "5. Click the button above to connect");
    m_adbHelpLabel->setStyleSheet("color: palette(mid); font-size: 11px;");
    m_adbHelpLabel->setWordWrap(true);
    adbLayout->addWidget(m_adbHelpLabel);

    layout->addWidget(adbCard);

    layout->addStretch();

    m_pageReceive = m_stack->addWidget(scroll);
}

// ─── Settings Page ──────────────────────────────────────────────────────────────

void MainWindow::setupSettingsPage() {
    auto* page = new QWidget();
    auto* scroll = new QScrollArea();
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(40, 30, 40, 30);
    layout->setSpacing(16);

    auto* pageTitle = new QLabel("Settings");
    pageTitle->setStyleSheet("font-size: 22px; font-weight: 600;");
    layout->addWidget(pageTitle);

    // Appearance
    auto* themeCard = makeCard("Appearance");
    auto* themeLayout = new QVBoxLayout(themeCard);
    themeLayout->setSpacing(10);

    auto* themeRow = new QHBoxLayout();
    auto* themeLabel = new QLabel("Theme:");
    themeLabel->setStyleSheet("font-size: 13px;");
    themeRow->addWidget(themeLabel);

    m_themeCombo = new QComboBox();
    m_themeCombo->addItem("Follow system", static_cast<int>(Theme::Mode::System));
    m_themeCombo->addItem("Light", static_cast<int>(Theme::Mode::Light));
    m_themeCombo->addItem("Dark", static_cast<int>(Theme::Mode::Dark));
    m_themeCombo->setCurrentIndex(static_cast<int>(Theme::savedMode()));
    themeRow->addWidget(m_themeCombo);
    themeRow->addStretch();
    themeLayout->addLayout(themeRow);

    connect(m_themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                Theme::setSavedMode(static_cast<Theme::Mode>(idx));
                applyTheme();
            });

    auto* themeNote = new QLabel(
        "\"Follow system\" tracks the Windows light/dark setting and updates live.");
    themeNote->setWordWrap(true);
    themeNote->setStyleSheet("font-size: 11px; color: palette(mid);");
    themeLayout->addWidget(themeNote);

    layout->addWidget(themeCard);

    // About card
    auto* aboutCard = makeCard("About");
    auto* aboutLayout = new QVBoxLayout(aboutCard);
    aboutLayout->setSpacing(8);

    m_versionLabel = new QLabel(QString("BetterCast v%1")
        .arg(QApplication::applicationVersion()));
    m_versionLabel->setStyleSheet("font-size: 14px; font-weight: bold; color: palette(window-text);");
    aboutLayout->addWidget(m_versionLabel);

    auto* descLabel = new QLabel(
        "Turn any device into a wireless extended display. "
        "Works with iPad, Android, Windows, Linux, and Mac receivers.");
    descLabel->setStyleSheet("font-size: 12px; color: palette(mid);");
    descLabel->setWordWrap(true);
    aboutLayout->addWidget(descLabel);

    layout->addWidget(aboutCard);

    // Connection card
    auto* connCard = makeCard("Connection");
    auto* connLayout = new QVBoxLayout(connCard);
    connLayout->setSpacing(10);

    auto* portInfo = new QLabel("Listening on port 51820 (TCP)");
    portInfo->setStyleSheet("font-size: 13px; color: palette(mid);");
    connLayout->addWidget(portInfo);

    auto* ipInfo = new QLabel();
    QStringList ips;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags().testFlag(QNetworkInterface::IsUp) &&
            iface.flags().testFlag(QNetworkInterface::IsRunning) &&
            !iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            for (const auto& entry : iface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    ips.append(entry.ip().toString());
                }
            }
        }
    }
    ipInfo->setText(ips.isEmpty() ? "No network detected"
                                  : "Local IPs: " + ips.join(", "));
    ipInfo->setStyleSheet("font-size: 12px; color: palette(mid);");
    ipInfo->setWordWrap(true);
    connLayout->addWidget(ipInfo);

    layout->addWidget(connCard);

    // Changelog card
    auto* changeCard = makeCard("What's New");
    auto* changeLayout = new QVBoxLayout(changeCard);
    changeLayout->setSpacing(10);

    struct ChangeEntry {
        QString version, date;
        QStringList items;
    };
    QVector<ChangeEntry> changelog = {
        {"v8", "2026-03-30", {
            "Unified sender + receiver in a single app",
            "Apple Music-style sidebar with tinted selection",
            "Windows sender with sidebar UI",
            "In-app update checker via GitHub Releases",
        }},
        {"v7", "2026-03-23", {
            "Android ADB wireless auto-reconnect",
            "Orientation fix for rotated displays",
        }},
        {"v6", "2026-03-19", {
            "Windows sender Phase 1",
            "DMG signing improvements",
        }},
    };

    for (const auto& entry : changelog) {
        auto* verLabel = new QLabel(QString("%1  —  %2").arg(entry.version, entry.date));
        verLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: palette(mid);");
        changeLayout->addWidget(verLabel);

        for (const auto& item : entry.items) {
            auto* bulletLabel = new QLabel(QString("  \xE2\x80\xA2  %1").arg(item));
            bulletLabel->setStyleSheet("font-size: 11px; color: palette(mid);");
            changeLayout->addWidget(bulletLabel);
        }

        changeLayout->addSpacing(4);
    }

    layout->addWidget(changeCard);

    layout->addStretch();

    m_pageSettings = m_stack->addWidget(scroll);
}

// ─── Logs Page ──────────────────────────────────────────────────────────────────

void MainWindow::setupLogsPage() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(10);

    // Title row with buttons
    auto* titleRow = new QHBoxLayout();

    auto* pageTitle = new QLabel("Logs");
    pageTitle->setStyleSheet("font-size: 22px; font-weight: bold; color: palette(window-text);");
    titleRow->addWidget(pageTitle);

    titleRow->addStretch();

    auto* reportBtn = new QPushButton("Report Issue");
    reportBtn->setStyleSheet(
        "QPushButton { background-color: #333; color: palette(mid); padding: 6px 14px; "
        "border-radius: 6px; font-size: 12px; border: 1px solid #555; }"
        "QPushButton:hover { background-color: #444; }");
    connect(reportBtn, &QPushButton::clicked, this, &MainWindow::onReportIssue);
    titleRow->addWidget(reportBtn);

    auto* copyBtn = new QPushButton("Copy");
    copyBtn->setStyleSheet(
        "QPushButton { background-color: #333; color: palette(mid); padding: 6px 14px; "
        "border-radius: 6px; font-size: 12px; border: 1px solid #555; }"
        "QPushButton:hover { background-color: #444; }");
    connect(copyBtn, &QPushButton::clicked, this, &MainWindow::onCopyLogs);
    titleRow->addWidget(copyBtn);

    auto* clearBtn = new QPushButton("Clear");
    clearBtn->setStyleSheet(
        "QPushButton { background-color: #333; color: palette(mid); padding: 6px 14px; "
        "border-radius: 6px; font-size: 12px; border: 1px solid #555; }"
        "QPushButton:hover { background-color: #444; }");
    connect(clearBtn, &QPushButton::clicked, this, &MainWindow::onClearLogs);
    titleRow->addWidget(clearBtn);

    layout->addLayout(titleRow);

    // Log viewer
    m_logViewer = new QTextEdit();
    m_logViewer->setReadOnly(true);
    m_logViewer->setPlaceholderText("No log entries yet...");
    layout->addWidget(m_logViewer);

    m_pageLogs = m_stack->addWidget(page);
}

// ─── Sidebar Selection ──────────────────────────────────────────────────────────

void MainWindow::onSidebarSelectionChanged(int row) {
    auto* item = m_sidebarList->item(row);
    if (!item) return;

    int page = item->data(Qt::UserRole).toInt();
    if (page < 0) {
        // Section header — skip to next selectable item
        if (row + 1 < m_sidebarList->count()) {
            m_sidebarList->setCurrentRow(row + 1);
        }
        return;
    }

    // Device rows carry the device name; everything else is a fixed page.
    const QString deviceName = item->data(Qt::UserRole + 1).toString();
    m_selectedDeviceName = deviceName;
#ifdef ENABLE_SENDER
    if (!deviceName.isEmpty()) {
        onDeviceRowSelected(deviceName);
        return;
    }
#endif

    m_stack->setCurrentIndex(page);
}

void MainWindow::selectSidebarItem(int pageIndex) {
    for (int i = 0; i < m_sidebarList->count(); i++) {
        auto* item = m_sidebarList->item(i);
        if (item && item->data(Qt::UserRole).toInt() == pageIndex) {
            m_sidebarList->setCurrentRow(i);
            return;
        }
    }
}

// ─── Connection Handlers ────────────────────────────────────────────────────────

void MainWindow::onConnectClicked() {
    bool ok = false;
    uint16_t port = m_portEdit->text().toUShort(&ok);
    if (!ok) port = 51820;

    m_network->connectTo(m_hostEdit->text(), port);
    m_connectBtn->setEnabled(false);
    m_recvStatusLabel->setText("Connecting...");
    m_recvStatusLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: #4da6ff;");
    LogManager::instance().log("Connecting to " + m_hostEdit->text() + ":" + QString::number(port));
}

void MainWindow::onAdbConnectClicked() {
    m_adbBtn->setEnabled(false);
    m_adbBtn->setText("Setting up ADB...");
    m_recvStatusLabel->setText("Looking for Android device...");
    m_recvStatusLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: #4da6ff;");
    LogManager::instance().log("Starting ADB setup...");

    std::thread([this]() {
        bool success = m_adbHelper->setupForward(51820);
        uint16_t localPort = m_adbHelper->lastLocalPort();
        QMetaObject::invokeMethod(this, [this, success, localPort]() {
            m_adbBtn->setEnabled(true);
            m_adbBtn->setText("Connect to Android (ADB)");

            if (success) {
                m_recvStatusLabel->setText("ADB tunnel ready — connecting...");
                LogManager::instance().log(QString("ADB tunnel established, connecting to localhost:%1...").arg(localPort));
                m_network->connectTo("localhost", localPort);
            }
        });
    }).detach();
}

void MainWindow::onConnectionEstablished() {
    m_connectBtn->setEnabled(true);
    m_reconnectTimer->stop();
    LogManager::instance().log("Connection established — streaming video");

    // Open the video in a separate window
    if (m_videoWindow) {
        m_videoWindow->showForVideo();
    }

    m_recvStatusLabel->setText("Connected — video window opened");
    m_recvStatusLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: #4caf50;");

    // Reset reconnect counter only after video actually starts flowing
    // (delayed so brief connect-then-disconnect during reconnect doesn't reset it)
    QTimer::singleShot(3000, this, [this]() {
        // Only reset if we're still connected (not in a reconnect cycle)
        if (!m_network->clients().isEmpty()) {
            m_reconnectAttempts = 0;
        }
    });

    if (m_adbHelper->wasAdbConnection() && !m_wirelessAdbEnabled) {
        // Delay wireless ADB by 5 seconds — adb tcpip 5555 temporarily kills
        // the USB connection (and our forward tunnel). Give the stream time to
        // start before we switch to wireless mode.
        QTimer::singleShot(5000, this, [this]() {
            if (m_wirelessAdbEnabled) return; // already done
            m_wirelessAdbEnabled = true;
            LogManager::instance().log("Enabling wireless ADB (USB can be disconnected after)...");
            std::thread([this]() {
                m_adbHelper->enableWirelessAdb();
            }).detach();
        });
    }
}

void MainWindow::onConnectionLost() {
    m_connectBtn->setEnabled(true);

    if (m_adbHelper->wasAdbConnection()) {
        // Don't reset m_reconnectAttempts here — if the reconnect itself
        // succeeds briefly then disconnects, we'd loop forever.
        // The counter only resets after a sustained connection (3s in onConnectionEstablished).
        m_recvStatusLabel->setText("Connection lost — reconnecting in 2s...");
        m_recvStatusLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: orange;");
        LogManager::instance().log("Connection lost — will reconnect via ADB in 2s...");
        // Delay before first reconnect to avoid rapid cycling
        QTimer::singleShot(2000, this, [this]() {
            attemptAdbReconnect();
            m_reconnectTimer->start();
        });
    } else {
        // Close video window when non-ADB connection is lost
        if (m_videoWindow && m_videoWindow->isVisible()) {
            m_videoWindow->close();
        }
        m_recvStatusLabel->setText("Connection lost — still listening on port 51820");
        m_recvStatusLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: orange;");
        LogManager::instance().log("Connection lost");
    }
}

void MainWindow::onStatusChanged(const QString& status) {
    m_recvStatusLabel->setText(status);
    LogManager::instance().log(status);
}

void MainWindow::onVideoSizeChanged(QSize size) {
    if (size.width() > 0 && size.height() > 0 && m_videoWindow) {
        LogManager::instance().log(QString("Video size: %1x%2").arg(size.width()).arg(size.height()));
        m_videoWindow->resizeToFitVideo(size.width(), size.height());
    }
}

void MainWindow::attemptAdbReconnect() {
    m_reconnectAttempts++;

    if (m_reconnectAttempts > 15) {
        m_reconnectTimer->stop();
        m_recvStatusLabel->setText("Auto-reconnect failed. Click 'Connect to Android (ADB)' to retry.");
        m_recvStatusLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: #d32f2f;");
        LogManager::instance().log("ADB auto-reconnect failed after 15 attempts");
        return;
    }

    m_recvStatusLabel->setText(QString("Reconnecting via ADB... (attempt %1)").arg(m_reconnectAttempts));
    LogManager::instance().log(QString("ADB reconnect attempt %1").arg(m_reconnectAttempts));

    std::thread([this]() {
        bool success = m_adbHelper->setupForward(51820);
        uint16_t localPort = m_adbHelper->lastLocalPort();
        QMetaObject::invokeMethod(this, [this, success, localPort]() {
            if (success) {
                m_reconnectTimer->stop();
                m_recvStatusLabel->setText("ADB tunnel restored — connecting...");
                LogManager::instance().log("ADB tunnel restored, reconnecting...");
                m_network->connectTo("localhost", localPort);
            }
        });
    }).detach();
}

// ─── Sender Slots ───────────────────────────────────────────────────────────────

#ifdef ENABLE_SENDER
void MainWindow::onSendScreenClicked() {
    QString host = m_sendHostEdit->text().trimmed();
    if (host.isEmpty()) {
        m_senderStatusLabel->setText("Enter a receiver IP address first");
        m_senderStatusLabel->setStyleSheet("font-size: 12px; color: #d32f2f;");
        return;
    }

    // Apply selected monitor to sender controller
    if (m_monitorCombo && m_monitorCombo->currentIndex() >= 0) {
        QVariantMap monData = m_monitorCombo->currentData().toMap();
        int adapterIdx = monData.value("adapter", 0).toInt();
        int outputIdx = monData.value("output", 0).toInt();
        QString displayName = monData.value("displayName").toString();
        m_sender->setMonitorIndex(adapterIdx, outputIdx);
        m_sender->setDisplayName(displayName);
        LogManager::instance().log(QString("Capturing monitor: %1 (adapter %2, output %3)")
                                       .arg(m_monitorCombo->currentText())
                                       .arg(adapterIdx).arg(outputIdx));
    }

    if (m_sender->isSendingTo(host)) {
        m_senderStatusLabel->setText("Already streaming to " + host);
        m_senderStatusLabel->setStyleSheet("font-size: 12px; color: #ff9800;");
        return;
    }

    m_stopSendBtn->setEnabled(true);
    m_senderStatusLabel->setText("Starting sender...");
    m_senderStatusLabel->setStyleSheet("font-size: 12px; color: #4da6ff;");

    // Prefer the target device's own settings; the spinboxes here are defaults
    // for devices that have not been customised.
    int fps = m_fpsSpinBox->value();
    int bitrate = m_bitrateSpinBox->value();
    for (const auto& dev : m_devices) {
        if (dev.host == host && dev.settingsCustomised) {
            fps = dev.fps;
            bitrate = dev.bitrateMbps;
            break;
        }
    }
    LogManager::instance().log(QString("Starting sender to %1 at %2 FPS, %3 Mbps")
                                   .arg(host).arg(fps).arg(bitrate));

    // Empty display name lets the controller claim a virtual display this
    // receiver does not already share with another session.
    const QString chosenDisplay = m_monitorCombo && m_monitorCombo->currentIndex() >= 0
        ? m_monitorCombo->currentData().toMap().value("displayName").toString()
        : QString();
    m_sender->startSending(host, m_selectedReceiverPort, fps, bitrate, chosenDisplay);
}

void MainWindow::onCreateVirtualDisplay() {
    if (!m_sender || !m_sender->vdd()) return;

    QSize res = m_vddResolutionCombo->currentData().toSize();
    int w = res.width(), h = res.height();
    if (w <= 0 || h <= 0) { w = 1920; h = 1080; }
    // Windows brings an extended virtual display up at the driver's 800x600
    // default, so tell the VDD what to raise it to once it is attached.
    m_sender->vdd()->setPreferredResolution(w, h);

    m_createVddBtn->setEnabled(false);
    m_vddStatusLabel->setText("Creating virtual display...");
    m_vddStatusLabel->setStyleSheet("font-size: 12px; color: #4da6ff;");
    LogManager::instance().log(QString("Creating virtual display %1x%2...").arg(w).arg(h));

    // Run in background thread to avoid blocking UI
    std::thread([this, w, h]() {
        bool ok = m_sender->vdd()->createVirtualDisplay(w, h, 60);
        QMetaObject::invokeMethod(this, [this, ok, w, h]() {
            m_createVddBtn->setEnabled(true);
            if (ok) {
                m_removeVddBtn->setEnabled(true);
                m_vddStatusLabel->setText(QString("Virtual display active: %1x%2").arg(w).arg(h));
                m_vddStatusLabel->setStyleSheet("font-size: 13px; color: #4caf50;");
                LogManager::instance().log("Virtual display created successfully");
                // Refresh monitor list to include the new display
                onRefreshMonitors();
                // Auto-select the virtual display
                for (int i = 0; i < m_monitorCombo->count(); i++) {
                    QVariantMap data = m_monitorCombo->itemData(i).toMap();
                    if (data.value("virtual", false).toBool()) {
                        m_monitorCombo->setCurrentIndex(i);
                        break;
                    }
                }
            } else {
                m_vddStatusLabel->setText("Failed to create virtual display — check logs");
                m_vddStatusLabel->setStyleSheet("font-size: 12px; color: #d32f2f;");
            }
        });
    }).detach();
}

void MainWindow::onRemoveVirtualDisplay() {
    if (!m_sender || !m_sender->vdd()) return;

    m_removeVddBtn->setEnabled(false);
    LogManager::instance().log("Removing virtual display...");

    std::thread([this]() {
        bool ok = m_sender->vdd()->removeAllVirtualDisplays();
        QMetaObject::invokeMethod(this, [this, ok]() {
            if (ok) {
                m_vddStatusLabel->setText("Virtual display removed");
                m_vddStatusLabel->setStyleSheet("font-size: 12px; color: palette(mid);");
                LogManager::instance().log("Virtual display removed");
            } else {
                m_removeVddBtn->setEnabled(true);
                LogManager::instance().log("Failed to remove virtual display");
            }
            onRefreshMonitors();
        });
    }).detach();
}

// ─── Per-Device Detail Page ─────────────────────────────────────────────────────

// Mirrors the macOS DiscoveredDeviceView: a page scoped to one receiver, showing
// how to connect to it and what will be streamed, instead of a single Send form
// shared by every device.
void MainWindow::setupDevicePage() {
    auto* page = new QWidget();
    auto* scroll = new QScrollArea();
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(16);

    m_deviceTitleLabel = new QLabel();
    m_deviceTitleLabel->setStyleSheet("font-size: 22px; font-weight: 600; color: palette(window-text);");
    layout->addWidget(m_deviceTitleLabel);

    m_deviceSubtitleLabel = new QLabel();
    m_deviceSubtitleLabel->setStyleSheet("font-size: 13px; color: palette(mid);");
    layout->addWidget(m_deviceSubtitleLabel);

    // Body is rebuilt per device; keep it in its own container so repopulating
    // never disturbs the title above it.
    m_devicePageBody = new QWidget();
    auto* bodyLayout = new QVBoxLayout(m_devicePageBody);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(16);
    layout->addWidget(m_devicePageBody);

    layout->addStretch();
    m_pageDevice = m_stack->addWidget(scroll);
}

void MainWindow::populateDevicePage(const DeviceEntry& device) {
    if (!m_devicePageBody) return;

    m_deviceTitleLabel->setText(device.name);
    m_deviceSubtitleLabel->setText(
        QString("%1:%2  \xE2\x80\xA2  %3")
            .arg(device.host).arg(device.port)
            .arg(device.connected ? "Connected" : "Available"));
    m_deviceSubtitleLabel->setStyleSheet(
        device.connected ? "font-size: 13px; color: #4caf50;"
                         : "font-size: 13px; color: palette(mid);");

    // Clear the previous device's body.
    QLayout* bodyLayout = m_devicePageBody->layout();
    while (QLayoutItem* child = bodyLayout->takeAt(0)) {
        if (child->widget()) child->widget()->deleteLater();
        delete child;
    }

    auto* card = makeCard("Stream to this device");
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setSpacing(10);

    auto* hint = new QLabel(
        "Streams the monitor selected on the Send Screen page. Pick a "
        "[Virtual] display there to extend your desktop rather than mirror it.");
    hint->setWordWrap(true);
    hint->setStyleSheet("font-size: 12px; color: palette(mid);");
    cardLayout->addWidget(hint);

    auto* btnRow = new QHBoxLayout();
    const bool streaming = m_sender && m_sender->isSendingTo(device.host);

    if (streaming) {
        auto* activeLabel = new QLabel(
            QString("Streaming %1 to this device")
                .arg(m_sender->displayForReceiver(device.host)));
        activeLabel->setStyleSheet("font-size: 12px; color: #4caf50;");
        cardLayout->addWidget(activeLabel);

        auto* stopBtn = new QPushButton("Stop Streaming Here");
        stopBtn->setStyleSheet(
            "QPushButton { background-color: #d32f2f; color: #ffffff; font-weight: bold; "
            "padding: 9px 20px; border-radius: 6px; border: none; }"
            "QPushButton:hover { background-color: #e34a4a; }");
        connect(stopBtn, &QPushButton::clicked, this, [this, device]() {
            m_sender->stopSending(device.host);   // other receivers keep streaming
            onDeviceRowSelected(device.name);     // refresh this page
        });
        btnRow->addWidget(stopBtn);
    } else {
        auto* sendBtn = new QPushButton("Send Screen Here");
        sendBtn->setIcon(Icons::icon(Icons::send(), QColor("white")));
        sendBtn->setStyleSheet(
            "QPushButton { background-color: #0078D4; color: #ffffff; font-weight: bold; "
            "padding: 9px 20px; border-radius: 6px; border: none; }"
            "QPushButton:hover { background-color: #1a88e0; }");
        connect(sendBtn, &QPushButton::clicked, this, [this, device]() {
            if (m_sendHostEdit) m_sendHostEdit->setText(device.host);
            m_selectedReceiverPort = device.port;
            // This device's own settings, not the Send page's globals. Empty
            // display: the controller claims one not already in use, so a second
            // receiver gets its own screen rather than mirroring the first.
            const int idx = indexOfDevice(device.name);
            const int fps = idx >= 0 ? m_devices[idx].fps : device.fps;
            const int bitrate = idx >= 0 ? m_devices[idx].bitrateMbps : device.bitrateMbps;
            m_sender->startSending(device.host, device.port, fps, bitrate, QString());
            onDeviceRowSelected(device.name);
        });
        btnRow->addWidget(sendBtn);
    }

    auto* configureBtn = new QPushButton("Configure\xE2\x80\xA6");
    connect(configureBtn, &QPushButton::clicked, this, [this, device]() {
        if (m_sendHostEdit) m_sendHostEdit->setText(device.host);
        m_selectedReceiverPort = device.port;
        selectSidebarItem(m_pageSend);
    });
    btnRow->addWidget(configureBtn);
    btnRow->addStretch();
    cardLayout->addLayout(btnRow);

    bodyLayout->addWidget(card);

    // Per-device stream quality. Mirrors the macOS DiscoveredDeviceView, where
    // resolution and quality live on the device rather than on the app.
    auto* qualityCard = makeCard("Stream quality for this device");
    auto* qLayout = new QVBoxLayout(qualityCard);
    qLayout->setSpacing(10);

    const int deviceIdx = indexOfDevice(device.name);

    auto* fpsRow = new QHBoxLayout();
    auto* fpsLabel = new QLabel("Frame Rate:");
    fpsLabel->setStyleSheet("font-size: 13px; color: palette(mid);");
    fpsRow->addWidget(fpsLabel);
    auto* fpsSpin = new QSpinBox();
    fpsSpin->setRange(15, 120);
    fpsSpin->setSingleStep(5);
    fpsSpin->setSuffix(" FPS");
    fpsSpin->setValue(device.fps);
    fpsRow->addWidget(fpsSpin);
    fpsRow->addStretch();
    qLayout->addLayout(fpsRow);

    auto* brRow = new QHBoxLayout();
    auto* brLabel = new QLabel("Bitrate:");
    brLabel->setStyleSheet("font-size: 13px; color: palette(mid);");
    brRow->addWidget(brLabel);
    auto* brSpin = new QSpinBox();
    brSpin->setRange(2, 100);
    brSpin->setSuffix(" Mbps");
    brSpin->setValue(device.bitrateMbps);
    brRow->addWidget(brSpin);
    brRow->addStretch();
    qLayout->addLayout(brRow);

    // Changes apply to the next stream to this device; restarting a live
    // session mid-stream would drop the picture on the receiver.
    if (deviceIdx >= 0) {
        connect(fpsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this, deviceIdx](int v) {
                    if (deviceIdx >= m_devices.size()) return;
                    m_devices[deviceIdx].fps = v;
                    m_devices[deviceIdx].settingsCustomised = true;
                });
        connect(brSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this, deviceIdx](int v) {
                    if (deviceIdx >= m_devices.size()) return;
                    m_devices[deviceIdx].bitrateMbps = v;
                    m_devices[deviceIdx].settingsCustomised = true;
                });
    }

    if (streaming) {
        auto* note = new QLabel("Changes take effect the next time you start "
                                "streaming to this device.");
        note->setWordWrap(true);
        note->setStyleSheet("font-size: 11px; color: palette(mid);");
        qLayout->addWidget(note);
    }

    bodyLayout->addWidget(qualityCard);

    // Receiving the other way round: this device sends, we display it.
    auto* recvCard = makeCard("Receive from this device");
    auto* recvLayout = new QVBoxLayout(recvCard);
    recvLayout->setSpacing(10);

    auto* recvHint = new QLabel(
        QString("BetterCast is listening on port 51820 and is advertised to %1 over "
                "mDNS. Streaming is started by whichever device is sending, so pick "
                "this PC in BetterCast on %1 to extend its desktop here.")
            .arg(device.name));
    recvHint->setWordWrap(true);
    recvHint->setStyleSheet("font-size: 12px; color: palette(mid);");
    recvLayout->addWidget(recvHint);

    auto* recvRow = new QHBoxLayout();
    auto* recvBtn = new QPushButton("Open Receive Screen");
    recvBtn->setIcon(Icons::icon(Icons::receive()));
    connect(recvBtn, &QPushButton::clicked, this, [this]() {
        selectSidebarItem(m_pageReceive);
    });
    recvRow->addWidget(recvBtn);
    recvRow->addStretch();
    recvLayout->addLayout(recvRow);

    bodyLayout->addWidget(recvCard);
}

void MainWindow::onDeviceRowSelected(const QString& deviceName) {
    const int idx = indexOfDevice(deviceName);
    if (idx < 0 || m_pageDevice < 0) return;
    populateDevicePage(m_devices[idx]);
    m_stack->setCurrentIndex(m_pageDevice);
}

void MainWindow::onExtendDisplays() {
    if (!m_sender || !m_sender->vdd() || !m_topologyCombo) return;

    const auto mode = static_cast<VirtualDisplayVDD::Topology>(
        m_topologyCombo->currentData().toInt());
    const QString modeName = m_topologyCombo->currentText();

    m_applyTopologyBtn->setEnabled(false);
    m_vddStatusLabel->setText("Applying " + modeName + "...");
    m_vddStatusLabel->setStyleSheet("font-size: 12px; color: #4da6ff;");
    LogManager::instance().log("Applying display mode: " + modeName);

    // SetDisplayConfig blocks while the mode change settles — keep it off the UI thread.
    std::thread([this, mode, modeName]() {
        auto* vdd = m_sender->vdd();
        // Extend goes through ensureExtendedTopology so it also verifies the
        // result and repositions the virtual display; the other modes are a
        // straight projection change.
        const bool ok = (mode == VirtualDisplayVDD::Topology::Extend)
                            ? vdd->ensureExtendedTopology()
                            : vdd->applyTopology(mode);

        QMetaObject::invokeMethod(this, [this, ok, modeName]() {
            m_applyTopologyBtn->setEnabled(true);
            if (ok) {
                m_vddStatusLabel->setText("Display mode: " + modeName);
                m_vddStatusLabel->setStyleSheet("font-size: 13px; color: #4caf50;");
            } else {
                m_vddStatusLabel->setText("Could not apply " + modeName + " — see logs");
                m_vddStatusLabel->setStyleSheet("font-size: 12px; color: #d32f2f;");
            }
            onRefreshMonitors();
        });
    }).detach();
}

void MainWindow::onRefreshMonitors() {
    if (!m_monitorCombo || !m_sender || !m_sender->vdd()) return;

    m_monitorCombo->clear();

    auto monitors = m_sender->vdd()->enumerateMonitors();
    if (monitors.isEmpty()) {
        m_monitorCombo->addItem("No monitors detected (adapter 0, output 0)");
        QVariantMap defaultData;
        defaultData["adapter"] = 0;
        defaultData["output"] = 0;
        defaultData["virtual"] = false;
        m_monitorCombo->setItemData(0, defaultData);
        return;
    }

    for (const auto& mon : monitors) {
        // A detached display reports 0x0 and has no framebuffer. Saying so is
        // far more useful than showing "0x0" and letting capture fail later.
        const bool detached = (mon.width <= 0 || mon.height <= 0);
        QString label = detached
            ? QString("%1  not attached  (%2)").arg(mon.name, mon.adapterName)
            : QString("%1  %2x%3  (%4)").arg(mon.name)
                  .arg(mon.width).arg(mon.height).arg(mon.adapterName);
        if (mon.isVirtual) {
            label += "  [Virtual]";
        }

        QVariantMap data;
        data["adapter"] = mon.adapterIndex;
        data["output"] = mon.outputIndex;
        data["virtual"] = mon.isVirtual;
        data["attached"] = !detached;
        data["displayName"] = mon.name;
        m_monitorCombo->addItem(label, data);
    }

    // Default to a virtual display when one exists, preferring one that is
    // actually attached. The combo used to land on index 0 — the primary — so
    // anyone with pre-existing virtual displays silently streamed their own
    // main screen and it looked like mirroring.
    int firstVirtual = -1;
    int firstAttachedVirtual = -1;
    int virtualCount = 0;
    int detachedCount = 0;
    for (int i = 0; i < m_monitorCombo->count(); i++) {
        const QVariantMap d = m_monitorCombo->itemData(i).toMap();
        if (!d.value("virtual", false).toBool()) continue;
        virtualCount++;
        if (firstVirtual < 0) firstVirtual = i;
        if (d.value("attached", true).toBool()) {
            if (firstAttachedVirtual < 0) firstAttachedVirtual = i;
        } else {
            detachedCount++;
        }
    }
    const int preferred = firstAttachedVirtual >= 0 ? firstAttachedVirtual : firstVirtual;
    if (preferred >= 0) m_monitorCombo->setCurrentIndex(preferred);
    if (detachedCount > 0) {
        LogManager::instance().log(
            QString("Note: %1 virtual display(s) are not attached to the desktop and "
                    "have nothing to capture. BetterCast will attach one automatically "
                    "when it needs it; use Remove to clear leftovers.").arg(detachedCount));
    }

    // Remove is meaningful whenever any virtual display is present, including
    // ones left behind by earlier installs.
    if (m_removeVddBtn) m_removeVddBtn->setEnabled(virtualCount > 0);

    LogManager::instance().log(QString("Found %1 monitor(s), %2 virtual")
                                   .arg(monitors.size()).arg(virtualCount));
    if (virtualCount > 1) {
        LogManager::instance().log(
            QString("Note: %1 virtual displays present. Extras are usually left over "
                    "from repeated installs — use Remove to clear them.").arg(virtualCount));
    }
}

void MainWindow::onMonitorSelected(int index) {
    if (!m_monitorCombo || index < 0) return;
    QVariantMap data = m_monitorCombo->itemData(index).toMap();
    LogManager::instance().log(QString("Selected monitor: %1 (adapter %2, output %3)")
                                   .arg(m_monitorCombo->currentText())
                                   .arg(data.value("adapter", 0).toInt())
                                   .arg(data.value("output", 0).toInt()));
}

void MainWindow::onStopSendingClicked() {
    m_sender->stopAll();
    m_senderStatusLabel->setText("Sender stopped");
    m_senderStatusLabel->setStyleSheet("font-size: 12px; color: palette(mid);");
    m_fpsSpinBox->setEnabled(true);
    m_bitrateSpinBox->setEnabled(true);
    LogManager::instance().log("Sender stopped");
}

void MainWindow::onReceiverDiscovered(const DiscoveredService& service) {
    if (!m_receiverCombo) return;

    // Remove the "Searching..." placeholder on first discovery
    if (m_receiverCombo->count() == 1 && !m_receiverCombo->isEnabled()) {
        m_receiverCombo->clear();
        m_receiverCombo->setEnabled(true);
        m_receiverCombo->addItem("Select a receiver...");
    }

    // All BetterCast receivers listen on port 51820. mDNS may report a different port
    // (e.g., from the P2P/AWDL listener) which is unreachable from Windows/Linux.
    // Always use the standard port for reliability.
    uint16_t port = 51820;

    // Check if already in the list
    QString entry = QString("%1  (%2:%3)").arg(service.name, service.host).arg(port);
    for (int i = 0; i < m_receiverCombo->count(); i++) {
        QVariantMap existing = m_receiverCombo->itemData(i).toMap();
        if (existing.value("host").toString() == service.host) {
            m_receiverCombo->setItemText(i, entry);
            QVariantMap updated;
            updated["host"] = service.host;
            updated["port"] = port;
            m_receiverCombo->setItemData(i, updated);
            return;
        }
    }

    QVariantMap data;
    data["host"] = service.host;
    data["port"] = port;
    m_receiverCombo->addItem(entry, data);

    // Keep the sidebar's device list in step — this is what turns the sidebar
    // device-centric rather than leaving receivers hidden in a dropdown.
    const int existingIdx = indexOfDevice(service.name);
    if (existingIdx >= 0) {
        m_devices[existingIdx].host = service.host;
        m_devices[existingIdx].port = port;
    } else {
        DeviceEntry entry;
        entry.name = service.name;
        entry.host = service.host;
        entry.port = port;
        // Seed from the Send page, then this device owns its own settings.
        entry.fps = m_fpsSpinBox ? m_fpsSpinBox->value() : 60;
        entry.bitrateMbps = m_bitrateSpinBox ? m_bitrateSpinBox->value() : 20;
        m_devices.append(entry);
    }
    rebuildSidebar();
    if (service.port != port) {
        LogManager::instance().log(QString("Discovered receiver: %1 at %2 (mDNS reported port %3, using standard port %4)")
                                       .arg(service.name, service.host).arg(service.port).arg(port));
    } else {
        LogManager::instance().log(QString("Discovered receiver: %1 at %2:%3")
                                       .arg(service.name, service.host).arg(port));
    }
}

void MainWindow::onReceiverSelected(int index) {
    if (!m_receiverCombo || !m_sendHostEdit) return;
    QVariantMap data = m_receiverCombo->itemData(index).toMap();
    QString host = data.value("host").toString();
    if (!host.isEmpty()) {
        m_sendHostEdit->setText(host);
        m_selectedReceiverPort = static_cast<uint16_t>(data.value("port", 51820).toUInt());
    }
}
#endif

// ─── Log Slots ──────────────────────────────────────────────────────────────────

void MainWindow::onLogAdded(const QString& entry) {
    if (m_logViewer) {
        m_logViewer->append(entry);
    }
}

void MainWindow::onCopyLogs() {
    QApplication::clipboard()->setText(
        LogManager::instance().entries().join("\n"));
    LogManager::instance().log("Logs copied to clipboard");
}

void MainWindow::onClearLogs() {
    LogManager::instance().clear();
    if (m_logViewer) m_logViewer->clear();
}

void MainWindow::onReportIssue() {
    QString sysInfo = QString("Platform: %1, BetterCast %2")
        .arg(
#ifdef _WIN32
            "Windows"
#elif __linux__
            "Linux"
#else
            "Unknown"
#endif
        )
        .arg(QApplication::applicationVersion());

    QStringList recentLogs = LogManager::instance().entries();
    if (recentLogs.size() > 30) {
        recentLogs = recentLogs.mid(recentLogs.size() - 30);
    }

    QString body = QString(
        "**Describe the issue:**\n\n\n"
        "**Steps to reproduce:**\n1. \n\n"
        "**Expected behavior:**\n\n\n"
        "**System info:** %1\n\n"
        "<details><summary>Recent Logs</summary>\n\n```\n%2\n```\n\n</details>"
    ).arg(sysInfo, recentLogs.join("\n"));

    QString url = QString("https://github.com/StephenLovino/BetterCast/issues/new?title=%1&body=%2")
        .arg(QString("Bug: ").toUtf8().toPercentEncoding(),
             body.toUtf8().toPercentEncoding());

    QDesktopServices::openUrl(QUrl(url));
    LogManager::instance().log("Opened GitHub issue form");
}

// Key/mouse events for video are handled by VideoWindow

// ─── Local IP Display ───────────────────────────────────────────────────────────

void MainWindow::updateLocalIpDisplay() {
    QStringList ips;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags().testFlag(QNetworkInterface::IsUp) &&
            iface.flags().testFlag(QNetworkInterface::IsRunning) &&
            !iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            for (const auto& entry : iface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    ips.append(entry.ip().toString());
                }
            }
        }
    }

    QString text = ips.isEmpty()
        ? "No network detected"
        : "This device: " + ips.join(" / ") + " : 51820";

    if (m_overviewIpLabel) m_overviewIpLabel->setText(text);
    if (m_recvIpLabel) m_recvIpLabel->setText(text);
}
