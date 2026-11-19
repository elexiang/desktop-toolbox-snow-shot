// SPDX-License-Identifier: GPL-3.0-or-later
#include "toolboxcontroller.h"
#include "toolboxpaths.h"
#include "activitylog.h"
#include "appfeedback.h"
#include "offlinetranslator.h"
#include "taskbardock.h"
#include "qrgenerator.h"
#include "librarywindows.h"
#include "quickpaste.h"
#ifndef SNOW_TOOLBOX_TEST_NO_STORAGE
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/configurationschema.h"
#endif
#include <QtWidgets>
#include <QTranslator>
#include <QJsonArray>
#include <algorithm>
#include <optional>
#include <vector>
#ifdef Q_OS_WIN
#include <winevt.h>
#endif

namespace toolbox {
namespace {
constexpr QSize homeMinimumSize(820, 360);

QString themeColor() {
    const auto key = QSettings(settingsPath(), QSettings::IniFormat)
                         .value(QStringLiteral("themeColor"), QStringLiteral("blue")).toString();
    const QMap<QString, QString> colors{{QStringLiteral("blue"), QStringLiteral("#3976e9")},
                                       {QStringLiteral("teal"), QStringLiteral("#087f8c")},
                                       {QStringLiteral("green"), QStringLiteral("#258049")},
                                       {QStringLiteral("purple"), QStringLiteral("#8052cd")},
                                       {QStringLiteral("rose"), QStringLiteral("#be3970")},
                                       {QStringLiteral("orange"), QStringLiteral("#b85c12")}};
    return colors.value(key, QStringLiteral("#3976e9"));
}
QString style() {
    return QStringLiteral(
        "QWidget{font-family:'Microsoft YaHei UI';font-size:10pt;color:#263342;}"
        "QWidget#ToolboxHome{background:#f4f6fa;} "
        "QFrame#Box,QWidget#QuotaCard,QWidget#QuotaDetail{background:#fff;border:1px solid "
        "#dce2eb;border-radius:12px;}"
        "QLabel{background:transparent;border:0;} "
        "QLabel#Title{font-size:23pt;font-weight:600;color:#142234;}"
        "QPushButton{border:1px solid #d8dfea;border-radius:7px;padding:6px "
        "12px;background:#fff;color:#334155;}"
        "QPushButton:hover{background:%2;border-color:%1;} "
        "QPushButton:disabled{color:#96a0b0;background:#edf0f5;}"
        "QTabWidget::pane{border:0;} QTabBar::tab{padding:10px 24px;border:0;color:#778396;}"
        "QTabBar::tab:selected{color:%1;border-bottom:2px solid %1;}"
        "QTabBar#NoteCategories::tab,QTabBar#QuickCategories::tab{padding:6px 14px;}"
        "QToolButton#ClipboardOptions,QToolButton#NoteOptions{border:1px solid #d8dfea;"
        "border-radius:7px;padding:6px 10px;background:#fff;color:#334155;}"
        "QToolButton#ClipboardOptions:hover,QToolButton#NoteOptions:hover{border-color:%1;}"
        "QToolButton#ClipboardOptions::menu-indicator,QToolButton#NoteOptions::menu-indicator"
        "{image:none;}"
        "QCheckBox{spacing:8px;padding:7px 0;} "
        "QLineEdit,QKeySequenceEdit,QComboBox{background:white;border:1px solid "
        "#d8dfea;border-radius:6px;padding:7px;}"
        "QGroupBox{border:1px solid #dce2eb;border-radius:9px;margin-top:14px;padding:16px 12px "
        "10px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:12px;padding:0 5px;} "
        "QMenu{background:white;padding:6px;} QMenu::item{padding:8px 20px;} "
        "QMenu::item:selected{background:%2;}"
        "QLineEdit:focus,QKeySequenceEdit:focus,QComboBox:focus{border-color:%1;}")
        .arg(themeColor(), QColor(themeColor()).lighter(185).name());
}
QLabel* label(const QString& text, QBoxLayout* layout) {
    auto* result = new QLabel(text);
    result->setWordWrap(true);
    layout->addWidget(result);
    return result;
}
QPushButton* button(const QString& text, QBoxLayout* layout, const std::function<void()>& action) {
    auto* result = new QPushButton(text);
    layout->addWidget(result);
    QObject::connect(result, &QPushButton::clicked, result, action);
    return result;
}
QFrame* box(QBoxLayout* layout) {
    auto* result = new QFrame;
    result->setObjectName(QStringLiteral("Box"));
    layout->addWidget(result);
    return result;
}
QPoint visiblePosition(QPoint position, QSize size) {
    QScreen* screen =
        QGuiApplication::screenAt(position + QPoint(size.width() / 2, size.height() / 2));
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const auto area = screen->availableGeometry();
    return {std::clamp(position.x(), area.left(),
                       std::max(area.left(), area.right() - size.width() + 1)),
            std::clamp(position.y(), area.top(),
                       std::max(area.top(), area.bottom() - size.height() + 1))};
}

class ToolboxHome final : public QWidget {
#ifdef Q_OS_WIN
  protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override {
        const auto* event = static_cast<const MSG*>(message);
        if (event->message == WM_ENTERSIZEMOVE)
            m_moveStart = pos();
        if (event->message == WM_EXITSIZEMOVE) {
            if (m_moveStart && pos() != *m_moveStart && !isMaximized() && !isMinimized())
                QSettings(settingsPath(), QSettings::IniFormat)
                    .setValue(QStringLiteral("homePosition"), pos());
            m_moveStart.reset();
        }
        return QWidget::nativeEvent(eventType, message, result);
    }

  private:
    std::optional<QPoint> m_moveStart;
#endif
};
} // namespace
ToolboxController::ToolboxController(ToolboxActions actions, QObject* parent)
    : QObject(parent), m_actions(std::move(actions)), m_quota(this), m_memory(this) {
    qApp->setApplicationDisplayName(tr("Longniang Toolbox"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/toolbox/dragon/app.ico")));
    qApp->setQuitOnLastWindowClosed(false);
    qApp->installEventFilter(this);
    m_library = new LocalLibrary(dataPath() + QStringLiteral("/library"), this);
    m_clipboardTracker = new ClipboardTracker(m_library, this);
    connect(m_clipboardTracker, &ClipboardTracker::captureDiagnostic, this,
            [this](const QString& stage) {
                appendLog(QStringLiteral("[clipboard.history] pid=%1 stage=%2")
                              .arg(QCoreApplication::applicationPid()).arg(stage));
            });
    m_quickPaste = new QuickPaste(this);
    connect(m_quickPaste, &QuickPaste::requested, this, &ToolboxController::showQuickPicker);
    connect(m_quickPaste, &QuickPaste::finished, this, [this](bool sent, const QString& message) {
        if (!sent)
            AppFeedback::show(QStringLiteral("快速粘贴"), message);
    });
    buildHome();
    m_menu.setStyleSheet(style());
    m_menu.addAction(tr("Open toolbox"), this, &ToolboxController::showHome);
    m_menu.addAction(tr("Capture region"), this, &ToolboxController::capture);
    m_menu.addAction(tr("Restore pinned images"), this, [this] {
        if (enabled(QStringLiteral("pin")))
            m_actions.restorePins();
    });
    m_menu.addAction(tr("Refresh quota"), &m_quota, &QuotaService::refresh);
    m_menu.addAction(QStringLiteral("快速剪贴板"), this, [this] { showQuickPicker(false); })
        ->setProperty("requiresModule", QStringLiteral("clipboardHistory"));
    m_menu.addAction(QStringLiteral("快速笔记"), this, [this] { showQuickPicker(true); })
        ->setProperty("requiresModule", QStringLiteral("localNotes"));
    m_menu.addSeparator();
    m_menu.addAction(tr("Quit toolbox"), qApp, &QApplication::quit);
    m_tray.setIcon(QApplication::windowIcon());
    m_tray.setParent(this);
    m_tray.setObjectName(QStringLiteral("ToolboxTray"));
    m_tray.setToolTip(tr("Longniang Toolbox"));
    m_tray.setContextMenu(&m_menu);
    m_tray.show();
    connect(&m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger)
                    showHome();
            });
    m_card = new QuotaCard;
    m_card->setObjectName(QStringLiteral("QuotaCard"));
    m_card->setWindowTitle(tr("Codex quota card"));
    m_card->resize(160, 30);
    m_card->installEventFilter(this);
    QSettings settings(settingsPath(), QSettings::IniFormat);
    const auto area = QGuiApplication::primaryScreen()->availableGeometry();
    m_card->move(visiblePosition(
        settings.value(QStringLiteral("cardPosition"), area.bottomRight() - QPoint(285, 210))
            .toPoint(),
        m_card->size()));
    m_taskbarDocked =
        settings.value(QStringLiteral("cardStyle")).toString() == QStringLiteral("taskbar");
    m_detail = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    m_detail->setObjectName(QStringLiteral("QuotaDetail"));
    m_detail->setStyleSheet(style());
    m_detail->setAttribute(Qt::WA_ShowWithoutActivating);
    m_detail->setFixedWidth(390);
    auto* detailLayout = new QVBoxLayout(m_detail);
    detailLayout->setContentsMargins(19, 18, 19, 18);
    detailLayout->setSpacing(13);
    m_detailLabel = label({}, detailLayout);
    m_detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* cleanup = new QHBoxLayout;
    detailLayout->addLayout(cleanup);
    auto* regular = button(tr("Regular cleanup"), cleanup, [this] { clean(false); });
    regular->setObjectName(QStringLiteral("CleanupButton"));
    auto* deep = button(tr("Deep cleanup…"), cleanup, [this] { clean(true); });
    deep->setObjectName(QStringLiteral("CleanupButton"));
    auto* openToolbox = button(tr("Open toolbox"), detailLayout, [this] { showHome(); });
    openToolbox->setObjectName(QStringLiteral("OpenToolbox"));
    openToolbox->setIcon(QIcon(QStringLiteral(":/toolbox/dragon/app.ico")));
    openToolbox->setIconSize(QSize(22, 22));
    const auto reposition = [this] {
        positionCard();
        m_detail->hide();
    };
    for (auto* screen : QGuiApplication::screens())
        connect(screen, &QScreen::availableGeometryChanged, this, reposition);
    connect(qApp, &QGuiApplication::screenAdded, this, reposition);
    connect(qApp, &QGuiApplication::screenRemoved, this, reposition);
    m_taskbarTracking.setInterval(750);
    m_taskbarTracking.setParent(this);
    m_taskbarTracking.setObjectName(QStringLiteral("TaskbarTracking"));
    m_taskbarEvents = new TaskbarEvents(this, [this] { positionCard(); });
    connect(&m_taskbarTracking, &QTimer::timeout, this, &ToolboxController::positionCard);
    m_hover.setSingleShot(true);
    m_hover.setInterval(300);
    connect(&m_hover, &QTimer::timeout, this, &ToolboxController::showDetail);
    m_leave.setInterval(100);
    connect(&m_leave, &QTimer::timeout, this, [this] {
        const auto point = QCursor::pos();
        if (m_card->geometry().adjusted(-8, -8, 8, 8).contains(point) ||
            m_detail->geometry().adjusted(-8, -8, 8, 8).contains(point))
            return;
        m_detail->hide();
        m_leave.stop();
    });
    connect(&m_quota, &QuotaService::changed, this, [this] {
        const auto status = m_quota.status();
        const auto key = status + m_quota.updatedAt().toString(Qt::ISODateWithMs);
        if (key != m_lastQuotaLog) {
            appendLog(tr("Codex quota: %1").arg(status));
            m_lastQuotaLog = key;
        }
        refreshUi();
    });
    connect(&m_memory, &MemoryService::changed, this, &ToolboxController::refreshUi);
    connect(&m_memory, &MemoryService::cleaned, this, [this](const QString& result) {
        m_status->setText(result);
        appendLog(result);
        AppFeedback::show(tr("Longniang Toolbox"), result);
        refreshUi();
    });
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (!m_taskbarDocked)
            QSettings(settingsPath(), QSettings::IniFormat)
                .setValue(QStringLiteral("cardPosition"), m_card->pos());
        m_quota.shutdown();
        OfflineTranslator::instance().stop();
    });
    const auto startupError = applySettings(false);
    if (!startupError.isEmpty()) {
        appendLog(startupError);
        QTimer::singleShot(0, this, [startupError] { AppFeedback::show(QStringLiteral("快捷键设置"), startupError); });
    }
    cleanupLogs();
    m_logCleanup.setInterval(60 * 60 * 1000);
    connect(&m_logCleanup, &QTimer::timeout, this, &ToolboxController::cleanupLogs);
    m_logCleanup.start();
    appendLog(tr("Toolbox started"));
#ifdef Q_OS_WIN
#ifdef SNOW_TOOLBOX_TEST_NO_STORAGE
    if (qEnvironmentVariableIsSet("SNOW_TOOLBOX_TEST_POPUP_RECORD"))
#endif
    {
        m_popupPoll.setInterval(30000);
        connect(&m_popupPoll, &QTimer::timeout, this, &ToolboxController::pollWindowsPopups);
        m_popupPoll.start();
        QTimer::singleShot(0, this, &ToolboxController::pollWindowsPopups);
    }
#endif
    if (!settings.value(QStringLiteral("hasStarted"), false).toBool()) {
        settings.setValue(QStringLiteral("hasStarted"), true);
        QTimer::singleShot(0, this, &ToolboxController::showHome);
    }
}
ToolboxController::~ToolboxController() {
    m_taskbarEvents->setEnabled(false);
    setShortcutEditing(false);
    if (m_contextProcess.state() != QProcess::NotRunning) {
        if (!m_contextProcess.waitForFinished(1000)) {
            m_contextProcess.kill();
            m_contextProcess.waitForFinished(1000);
        }
    }
    delete m_detail;
    delete m_qrWindow;
    delete m_libraryWindow;
    delete m_card;
    delete m_home;
}
void ToolboxController::showLibrary(bool notes) {
    if (!enabled(notes ? QStringLiteral("localNotes") : QStringLiteral("clipboardHistory")) ||
        m_capture || m_quickPaste->busy())
        return;
    if (!m_libraryWindow || !m_libraryWindow->isVisible() || !m_libraryWindow->isActiveWindow())
        m_quickPaste->rememberTarget();
    if (!m_libraryWindow) {
        m_libraryWindow = new LibraryWindow(m_library, m_clipboardTracker, m_home);
        m_libraryWindow->setFeatures(enabled(QStringLiteral("clipboardHistory")),
                                     enabled(QStringLiteral("localNotes")));
        m_libraryWindow->setStyleSheet(style());
        connect(m_libraryWindow, &LibraryWindow::pasteRequested, this,
                [this](const QString& text, const QImage& image, const QString& html) {
                    if (m_libraryWindow->notes()->flush())
                        m_quickPaste->paste(text, image, m_libraryWindow, html);
                });
        connect(m_libraryWindow, &LibraryWindow::canceled, this,
                [this] { m_quickPaste->cancel(m_libraryWindow); });
    }
    m_libraryWindow->selectPage(notes);
    if (m_libraryWindow->isMinimized())
        m_libraryWindow->setWindowState(m_libraryWindow->windowState() & ~Qt::WindowMinimized);
    m_libraryWindow->show();
    m_libraryWindow->raise();
    m_libraryWindow->activateWindow();
}
void ToolboxController::showNotes() {
    showLibrary(true);
}
void ToolboxController::showClipboard() {
    showLibrary(false);
}
void ToolboxController::showQuickPicker(bool notes) {
    if (m_capture || m_quickPaste->busy())
        return;
    if (m_libraryWindow && m_libraryWindow->isVisible() && !m_libraryWindow->isMinimized() &&
        m_libraryWindow->isActiveWindow() && m_libraryWindow->notesMode() == notes) {
        if (m_libraryWindow->notes()->flush())
            m_quickPaste->cancel(m_libraryWindow);
        return;
    }
    showLibrary(notes);
}
void ToolboxController::buildHome() {
    m_home = new ToolboxHome;
    m_home->setObjectName(QStringLiteral("ToolboxHome"));
    m_home->setWindowTitle(tr("Longniang Toolbox"));
    m_home->resize(900, 660);
    m_home->setMinimumSize(homeMinimumSize);
    m_home->setStyleSheet(style());
    m_home->installEventFilter(this);
    auto* layout = new QVBoxLayout(m_home);
    layout->setContentsMargins(28, 22, 28, 22);
    layout->setSpacing(13);
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(10);
    auto* avatar = new QLabel;
    avatar->setObjectName(QStringLiteral("DragonAccent"));
    avatar->setFixedSize(36, 36);
    avatar->setPixmap(QPixmap(QStringLiteral(":/toolbox/dragon/avatar.png"))
                          .scaled(36, 36, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    titleRow->addWidget(avatar);
    label(tr("Longniang Toolbox"), titleRow)->setObjectName(QStringLiteral("Title"));
    layout->addLayout(titleRow);
    auto* tabs = new QTabWidget;
    layout->addWidget(tabs, 1);
    auto* overview = new QWidget;
    overview->setObjectName(QStringLiteral("OverviewPage"));
    auto* body = new QVBoxLayout(overview);
    body->setContentsMargins(0, 16, 0, 0);
    body->setSpacing(14);
    auto* overviewScroll = new QScrollArea;
    overviewScroll->setFrameShape(QFrame::NoFrame);
    overviewScroll->setWidgetResizable(true);
    overviewScroll->setWidget(overview);
    body->setSizeConstraint(QLayout::SetMinimumSize);
    tabs->addTab(overviewScroll, tr("Tools"));
    auto* quotaBox = new QGroupBox(tr("Codex"));
    quotaBox->setObjectName(QStringLiteral("CodexContextBox"));
    body->addWidget(quotaBox);
    auto* quotaLayout = new QVBoxLayout(quotaBox);
    quotaLayout->setContentsMargins(20, 17, 20, 17);
    m_quotaLabel = label({}, quotaLayout);
    m_quotaLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_quotaLabel->setObjectName(QStringLiteral("QuotaValues"));
    m_quotaLabel->setMinimumHeight(85);
    m_quotaLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    auto* quotaFooter = new QHBoxLayout;
    quotaFooter->setSpacing(12);
    quotaLayout->addLayout(quotaFooter);
    m_quotaStatus = label({}, quotaFooter);
    m_quotaStatus->setObjectName(QStringLiteral("QuotaStatus"));
    m_quotaStatus->setWordWrap(false);
    m_quotaStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    quotaFooter->setStretchFactor(m_quotaStatus, 1);
    m_quotaUpdated = label({}, quotaFooter);
    m_quotaUpdated->setObjectName(QStringLiteral("QuotaUpdated"));
    m_quotaUpdated->setWordWrap(false);
    auto* refresh = button(tr("Manually refresh data"), quotaFooter, [this] {
        appendLog(tr("Manual quota refresh requested"));
        m_quota.refresh();
    });
    refresh->setObjectName(QStringLiteral("ManualQuotaRefresh"));
    auto* captureRow = new QHBoxLayout;
    body->addLayout(captureRow);
    auto* captureButton = button(tr("Capture region"), captureRow, [this] { capture(); });
    captureButton->setObjectName(QStringLiteral("CaptureRegion"));
    captureButton->setProperty("requiresCapture", true);
    auto* paste = button(tr("Pin clipboard image"), captureRow, [this] {
        if (enabled(QStringLiteral("pin")))
            m_actions.pastePin();
    });
    paste->setObjectName(QStringLiteral("PinButton"));
    auto* restore = button(tr("Restore pinned images"), captureRow, [this] {
        if (enabled(QStringLiteral("pin")))
            m_actions.restorePins();
    });
    restore->setObjectName(QStringLiteral("PinButton"));
    button(tr("Text and translation panel"), captureRow, [this] {
        if (m_textWindow) {
            m_textWindow->show();
            m_textWindow->raise();
            m_textWindow->activateWindow();
        } else
            presentText({});
    });
    auto* qrButton = button(tr("Generate QR code"), captureRow, [this] {
        if (!m_qrWindow) {
            m_qrWindow = new QrGeneratorWindow(m_home);
            m_qrWindow->setStyleSheet(style());
        }
        m_qrWindow->show();
        m_qrWindow->raise();
        m_qrWindow->activateWindow();
    });
    qrButton->setObjectName(QStringLiteral("OpenQrGenerator"));
    qrButton->setProperty("requiresModule", QStringLiteral("qrGenerator"));
    auto* clipboardButton = button(QStringLiteral("剪贴板历史"), captureRow, [this] { showClipboard(); });
    clipboardButton->setObjectName(QStringLiteral("OpenClipboardHistory"));
    clipboardButton->setProperty("requiresModule", QStringLiteral("clipboardHistory"));
    auto* notesButton = button(tr("Local notes"), captureRow, [this] { showNotes(); });
    notesButton->setObjectName(QStringLiteral("OpenLocalNotes"));
    notesButton->setProperty("requiresModule", QStringLiteral("localNotes"));
    for (int i = 0; i < captureRow->count(); ++i)
        captureRow->itemAt(i)->widget()->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    captureRow->addStretch();
    auto* memoryBox = box(body);
    auto* memoryLayout = new QVBoxLayout(memoryBox);
    memoryLayout->setContentsMargins(20, 17, 20, 17);
    m_memoryLabel = label({}, memoryLayout);
    m_memoryLabel->setObjectName(QStringLiteral("MemoryValues"));
    m_memoryLabel->setMinimumHeight(62);
    auto* memoryRow = new QHBoxLayout;
    memoryLayout->addLayout(memoryRow);
    for (bool deep : {false, true}) {
        auto* b = button(deep ? tr("Deep cleanup…") : tr("Regular cleanup"), memoryRow,
                         [this, deep] { clean(deep); });
        b->setObjectName(QStringLiteral("CleanupButton"));
    }
    label(tr("Manual cleanup only. Deep cleanup requests Windows authorization and can affect "
             "system caches. It does not change the page file"),
          memoryLayout);
    m_status = new QLabel(m_home);
    m_status->hide();
    m_status->setObjectName(QStringLiteral("ToolboxStatus"));
    auto* logPage = new QWidget;
    logPage->setObjectName(QStringLiteral("LogPage"));
    auto* logLayout = new QVBoxLayout(logPage);
    logLayout->setContentsMargins(0, 16, 0, 0);
    auto* logRow = new QHBoxLayout;
    logLayout->addLayout(logRow);
    label(tr("Recent activity"), logRow);
    logRow->addStretch();
    label(tr("Keep logs for"), logRow);
    auto* retention = new QComboBox;
    retention->setObjectName(QStringLiteral("LogRetention"));
    for (int days : {7, 14, 30, 90, 0})
        retention->addItem(days ? tr("%1 days").arg(days) : tr("Do not auto-clean"), days);
    const int savedDays = QSettings(settingsPath(), QSettings::IniFormat)
                              .value(QStringLiteral("logRetentionDays"), 30)
                              .toInt();
    retention->setCurrentIndex(std::max(0, retention->findData(savedDays)));
    logRow->addWidget(retention);
    connect(retention, &QComboBox::currentIndexChanged, this, [this, retention] {
        QSettings(settingsPath(), QSettings::IniFormat)
            .setValue(QStringLiteral("logRetentionDays"), retention->currentData());
        cleanupLogs();
    });
    m_log = new QPlainTextEdit;
    m_log->setObjectName(QStringLiteral("ActivityLog"));
    m_log->setReadOnly(true);
    m_log->setPlainText(recentActivity(dataPath() + QStringLiteral("/activity.log")));
    logLayout->addWidget(m_log, 1);
    tabs->addTab(logPage, tr("Logs"));
    auto* settingsPage = new QWidget;
    settingsPage->setObjectName(QStringLiteral("SettingsPage"));
    auto* settingsLayout = new QVBoxLayout(settingsPage);
    settingsLayout->setContentsMargins(0, 12, 0, 0);
    auto* settingsScroll = new QScrollArea;
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setWidget(settingsPage);
    settingsLayout->setSizeConstraint(QLayout::SetMinimumSize);
    tabs->addTab(settingsScroll, tr("Settings"));
    auto* startupRow = new QHBoxLayout;
    settingsLayout->addLayout(startupRow);
    auto* autoStart = new QCheckBox(tr("Start with Windows"));
    autoStart->setObjectName(QStringLiteral("ToolboxAutoStart"));
    autoStart->setChecked(m_actions.autoStartEnabled());
    startupRow->addWidget(autoStart);
    auto* clipboardRecording = new QCheckBox(QStringLiteral("记录剪贴板文字和图片（本地持久化）"));
    clipboardRecording->setObjectName(QStringLiteral("ClipboardRecordingSetting"));
    clipboardRecording->setChecked(m_clipboardTracker->recording());
    connect(clipboardRecording, &QCheckBox::toggled, m_clipboardTracker, &ClipboardTracker::setRecording);
    connect(m_clipboardTracker, &ClipboardTracker::stateChanged, clipboardRecording, [this, clipboardRecording] {
        const QSignalBlocker blocker(clipboardRecording);
        clipboardRecording->setChecked(m_clipboardTracker->recording());
    });
    startupRow->addWidget(clipboardRecording);
    startupRow->addStretch();
    auto* modules = new QGroupBox(tr("Enabled features"));
    auto* moduleGrid = new QGridLayout(modules);
    settingsLayout->addWidget(modules);
    const QList<QPair<QString, QString>> names{
        {QStringLiteral("quota"), tr("Codex quota")},
        {QStringLiteral("card"), tr("Desktop quota card")},
        {QStringLiteral("capture"), tr("Screenshot shortcut")},
        {QStringLiteral("annotation"), tr("Annotation tools")},
        {QStringLiteral("color"), tr("Color picker")},
        {QStringLiteral("pin"), tr("Pinned images")},
        {QStringLiteral("ocr"), tr("Text recognition")},
        {QStringLiteral("barcode"), tr("QR codes and barcodes")},
        {QStringLiteral("translation"), tr("Offline translation")},
        {QStringLiteral("memory"), tr("Memory information")},
        {QStringLiteral("cleanup"), tr("Memory cleanup")},
        {QStringLiteral("clipboardHistory"),
         QCoreApplication::translate("ToolboxLibrary", "Clipboard history")},
        {QStringLiteral("localNotes"), tr("Local notes")},
        {QStringLiteral("qrGenerator"), tr("Generate QR code")}};
    QSettings settings(settingsPath(), QSettings::IniFormat);
    constexpr int moduleColumns = 4;
    for (int column = 0; column < moduleColumns; ++column)
        moduleGrid->setColumnStretch(column, 1);
    int index = 0;
    QHash<QString, QCheckBox*> checks;
    for (const auto& item : names) {
        auto* check = new QCheckBox(item.second);
        check->setProperty("module", item.first);
        check->setChecked(enabled(item.first));
        moduleGrid->addWidget(check, index / moduleColumns, index % moduleColumns);
        ++index;
        checks.insert(item.first, check);
    }
    auto* form = new QFormLayout;
    settingsLayout->addLayout(form);
    auto* theme = new QComboBox;
    theme->setObjectName(QStringLiteral("ToolboxThemeColor"));
    theme->setFixedWidth(220);
    const QList<QPair<QString, QString>> themes{
        {QStringLiteral("blue"), tr("Blue (default)")},
        {QStringLiteral("teal"), tr("Teal")},
        {QStringLiteral("green"), tr("Green")},
        {QStringLiteral("purple"), tr("Purple")},
        {QStringLiteral("rose"), tr("Rose")},
        {QStringLiteral("orange"), tr("Orange")}};
    for (const auto& entry : themes) {
        theme->addItem(entry.second, entry.first);
    }
    theme->setCurrentIndex(std::max(0, theme->findData(
        settings.value(QStringLiteral("themeColor"), QStringLiteral("blue")))));
    form->addRow(tr("Theme color"), theme);
    connect(theme, &QComboBox::currentIndexChanged, this, [this, theme] {
        QSettings(settingsPath(), QSettings::IniFormat)
            .setValue(QStringLiteral("themeColor"), theme->currentData());
        applyTheme();
    });
    auto* key = new QKeySequenceEdit(
        QKeySequence(settings.value(QStringLiteral("captureShortcut"), QStringLiteral("Ctrl+Alt+1"))
                         .toString()));
    key->setMaximumSequenceLength(1);
    key->setFixedWidth(220);
    connect(qApp, &QApplication::focusChanged, key, [this](QWidget*, QWidget* focused) {
        bool editing = false;
        for (auto* editor : m_home->findChildren<QKeySequenceEdit*>())
            editing |= focused && (focused == editor || editor->isAncestorOf(focused));
        setShortcutEditing(editing);
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, key,
            [this](Qt::ApplicationState state) {
                auto* focused = QApplication::focusWidget();
                bool editing = false;
                for (auto* editor : m_home->findChildren<QKeySequenceEdit*>())
                    editing |= focused && (focused == editor || editor->isAncestorOf(focused));
                setShortcutEditing(state == Qt::ApplicationActive && editing);
            });
    auto* shortcutRow = new QHBoxLayout;
    shortcutRow->addWidget(key);
    button(tr("Screenshot session shortcuts…"), shortcutRow, [this] {
        m_actions.openCaptureSettings();
    })->setObjectName(QStringLiteral("ScreenshotSessionShortcuts"));
    shortcutRow->addStretch();
    key->setObjectName(QStringLiteral("CaptureShortcut"));
    form->addRow(tr("Screenshot shortcut"), shortcutRow);
    auto* quickClipboard = new QKeySequenceEdit(QKeySequence(settings.value(QStringLiteral("quickClipboardShortcut"), QStringLiteral("Ctrl+Alt+V")).toString()));
    auto* quickNotes = new QKeySequenceEdit(QKeySequence(settings.value(QStringLiteral("quickNotesShortcut"), QStringLiteral("Ctrl+Alt+N")).toString()));
    quickClipboard->setObjectName(QStringLiteral("QuickClipboardShortcut"));
    quickNotes->setObjectName(QStringLiteral("QuickNotesShortcut"));
    for (auto* editor : {quickClipboard, quickNotes}) {
        editor->setMaximumSequenceLength(1);
        editor->setFixedWidth(220);
        editor->setToolTip(QStringLiteral("在原输入框内按此快捷键呼出，搜索后回车粘贴；清空可禁用该快捷键"));
    }
    auto* quickRow = new QHBoxLayout;
    quickRow->addWidget(quickClipboard);
    auto* quickNotesLabel = new QLabel(QStringLiteral("快速笔记"));
    quickNotesLabel->setBuddy(quickNotes);
    quickRow->addSpacing(12);
    quickRow->addWidget(quickNotesLabel);
    quickRow->addWidget(quickNotes);
    quickRow->addStretch();
    form->addRow(QStringLiteral("快速剪贴板"), quickRow);
    auto* cardStyle = new QButtonGroup(settingsPage);
    cardStyle->setObjectName(QStringLiteral("QuotaCardStyle"));
    auto* styleRow = new QHBoxLayout;
    auto* compactStyle = new QRadioButton(tr("Compact capsule"));
    auto* stackedStyle = new QRadioButton(tr("Four-row pebble"));
    auto* taskbarStyle = new QRadioButton(tr("Taskbar (left of tray)"));
    taskbarStyle->setToolTip(tr("Fix the capsule beside the Windows taskbar tray. It may overlap "
                                "taskbar buttons when space is limited"));
#ifndef Q_OS_WIN
    taskbarStyle->setEnabled(false);
#endif
    cardStyle->addButton(compactStyle, 0);
    cardStyle->addButton(stackedStyle, 1);
    cardStyle->addButton(taskbarStyle, 2);
    styleRow->addWidget(compactStyle);
    styleRow->addWidget(stackedStyle);
    styleRow->addWidget(taskbarStyle);
    styleRow->addStretch();
    const auto selectedStyle = settings.value(QStringLiteral("cardStyle")).toString();
    cardStyle
        ->button(selectedStyle == QStringLiteral("taskbar")   ? 2
                 : selectedStyle == QStringLiteral("stacked") ? 1
                                                              : 0)
        ->setChecked(true);
    form->addRow(tr("Desktop card style"), styleRow);
    auto* codexRow = new QHBoxLayout;
    auto* executable = new QLineEdit(
        settings.value(QStringLiteral("codexExecutable"), QuotaService::discover()).toString());
    codexRow->addWidget(executable);
    button(tr("Browse…"), codexRow, [this, executable] {
        const auto path = QFileDialog::getOpenFileName(m_home, tr("Select Codex CLI"), {},
                                                       QStringLiteral("codex.exe (codex.exe)"));
        if (!path.isEmpty())
            executable->setText(path);
    });
    form->addRow(tr("Codex CLI"), codexRow);
    label(tr("Closing this panel keeps the card and tray active. Disable a feature to stop its "
             "background activity. Translation may use internal OCR when the OCR button is hidden"),
          settingsLayout);
    button(tr("Save and apply"), settingsLayout,
           [this, checks, key, quickClipboard, quickNotes, executable, cardStyle, autoStart] {
               for (auto* editor : {key, quickClipboard, quickNotes})
                   editor->clearFocus();
               setShortcutEditing(false);
               const bool clipboardEnabled = checks.value(QStringLiteral("clipboardHistory"))->isChecked();
               const bool notesEnabled = checks.value(QStringLiteral("localNotes"))->isChecked();
               if (!notesEnabled && m_libraryWindow && !m_libraryWindow->notes()->flush()) {
                   showNotes();
                   return;
               }
               const auto error = m_actions.setAutoStart(autoStart->isChecked());
               if (!error.isEmpty()) {
                   autoStart->setChecked(m_actions.autoStartEnabled());
                   m_status->setText(error);
                   QMessageBox::warning(m_home, tr("Start with Windows"), error);
                   return;
               }
               if (QGuiApplication::platformName() == QStringLiteral("windows")) {
                   const auto quickError = m_quickPaste->configure(
                       clipboardEnabled ? quickClipboard->keySequence() : QKeySequence{},
                       notesEnabled ? quickNotes->keySequence() : QKeySequence{});
                   if (!quickError.isEmpty()) {
                       QMessageBox::warning(m_home, QStringLiteral("快速呼出快捷键"), quickError);
                       return;
                   }
               }
               QSettings values(settingsPath(), QSettings::IniFormat);
               values.setValue(QStringLiteral("cardStyle"),
                               cardStyle->checkedId() == 2   ? QStringLiteral("taskbar")
                               : cardStyle->checkedId() == 1 ? QStringLiteral("stacked")
                                                             : QStringLiteral("compact"));
               for (auto it = checks.cbegin(); it != checks.cend(); ++it)
                   values.setValue(it.key(), it.value()->isChecked());
               values.setValue(QStringLiteral("captureShortcut"),
                               key->keySequence().toString(QKeySequence::PortableText));
               values.setValue(QStringLiteral("quickClipboardShortcut"), quickClipboard->keySequence().toString(QKeySequence::PortableText));
               values.setValue(QStringLiteral("quickNotesShortcut"), quickNotes->keySequence().toString(QKeySequence::PortableText));
               values.setValue(QStringLiteral("codexExecutable"), executable->text().trimmed());
               values.sync();
               m_actions.cancelCapture();
               const auto shortcutError = applySettings();
               m_status->setText(shortcutError.isEmpty() ? tr("Settings saved and applied")
                                                         : shortcutError);
               appendLog(m_status->text());
               AppFeedback::show(tr("Settings"), m_status->text());
           })
        ->setObjectName(QStringLiteral("SaveToolboxSettings"));
    auto* contextRow = new QHBoxLayout;
    quotaLayout->addLayout(contextRow);
    m_contextToggle = new QCheckBox(tr("Enable 1M context"));
    m_contextToggle->setObjectName(QStringLiteral("CodexContextToggle"));
    m_contextToggle->setEnabled(false);
    contextRow->addWidget(m_contextToggle);
    m_contextHelp = new QToolButton;
    m_contextHelp->setText(QStringLiteral("?"));
    m_contextHelp->setObjectName(QStringLiteral("CodexContextHelp"));
    m_contextHelp->setAccessibleName(tr("About 1M context"));
    contextRow->addWidget(m_contextHelp);
    setContextStatus(tr("Read the current Codex configuration before changing it"));
    connect(m_contextHelp, &QToolButton::clicked, this, [this] {
        QMessageBox::information(m_home, tr("About 1M context"), m_contextHelp->toolTip());
    });
    m_contextRefresh = button(tr("Refresh context setting"), contextRow,
                              [this] { runContext(QStringLiteral("read")); });
    m_contextDefault = button(tr("Restore model defaults"), contextRow,
                              [this] { runContext(QStringLiteral("off")); });
    m_contextDefault->setObjectName(QStringLiteral("CodexContextDefault"));
    m_contextDefault->setEnabled(false);
    contextRow->addStretch();
    connect(m_contextToggle, &QCheckBox::clicked, this, [this](bool checked) {
        runContext(checked ? QStringLiteral("on") : QStringLiteral("off"));
    });
    m_contextTimeout.setSingleShot(true);
    m_contextTimeout.setInterval(10000);
    connect(&m_contextTimeout, &QTimer::timeout, this, [this] { m_contextProcess.kill(); });
    connect(&m_contextProcess, &QProcess::finished, this, &ToolboxController::finishContext);
    connect(&m_contextProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    finishContext();
            });
    connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 0)
            runContext(QStringLiteral("read"));
    });
    body->setAlignment(Qt::AlignTop);
    settingsLayout->setAlignment(Qt::AlignTop);
}
void ToolboxController::appendLog(const QString& message) {
    if (message.isEmpty())
        return;
    const auto line = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) +
                      QStringLiteral("  ") + message.simplified();
    const auto path = dataPath() + QStringLiteral("/activity.log");
    QFile file(path);
    if (file.size() > 256 * 1024 && file.open(QIODevice::ReadOnly)) {
        auto tail = file.readAll().right(128 * 1024);
        tail = tail.mid(tail.indexOf('\n') + 1);
        file.close();
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(tail);
            file.close();
        }
    }
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        file.write((line + QLatin1Char('\n')).toUtf8());
        file.close();
    }
    if (m_log)
        m_log->setPlainText(recentActivity(path));
}
void ToolboxController::reportClipboardCopy(quint64 generation, const QString& stage, int attempts,
                                            quint32 code) {
    appendLog(QStringLiteral("[screenshot.clipboard] pid=%1 operation=%2 stage=%3 attempts=%4 code=0x%5")
                  .arg(QCoreApplication::applicationPid())
                  .arg(generation)
                  .arg(stage)
                  .arg(attempts)
                  .arg(code, 8, 16, QLatin1Char('0')));
}

void ToolboxController::pollWindowsPopups() {
#ifdef Q_OS_WIN
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto lookback = m_popupPollAt ? std::clamp<qint64>(now - m_popupPollAt + 5000, 10000, 86400000)
                                       : 86400000;
    const auto query = QStringLiteral(
        "*[System[Provider[@Name='Application Popup'] and EventID=26 and "
        "TimeCreated[timediff(@SystemTime) <= %1]]]").arg(lookback);
    const auto events = EvtQuery(nullptr, L"System", reinterpret_cast<LPCWSTR>(query.utf16()),
                                 EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!events) {
        const auto error = GetLastError();
        if (error != m_popupReadError)
            appendLog(QStringLiteral("[Windows.ApplicationPopup] query_failed code=%1").arg(error));
        m_popupReadError = error;
        return;
    }
    m_popupReadError = 0;
    m_popupPollAt = now;
    const auto executable = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    QStringList messages;
    for (int index = 0; index < 128; ++index) {
        EVT_HANDLE event = nullptr;
        DWORD returned = 0;
        if (!EvtNext(events, 1, &event, 0, 0, &returned))
            break;
        DWORD bytes = 0, properties = 0;
        if (!EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &bytes, &properties) &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes <= 128 * 1024) {
            std::vector<wchar_t> buffer((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t));
            if (EvtRender(nullptr, event, EvtRenderEventXml, bytes, buffer.data(), &bytes,
                          &properties)) {
                const auto message = windowsPopupDiagnostic(QString::fromWCharArray(buffer.data()),
                                                            executable);
                if (!message.isEmpty())
                    messages.prepend(message);
            }
        }
        EvtClose(event);
    }
    EvtClose(events);
    if (messages.isEmpty())
        return;
    auto lines = activityLines(dataPath() + QStringLiteral("/activity.log"));
    for (const auto& message : messages) {
        if (std::none_of(lines.cbegin(), lines.cend(),
                         [&message](const QString& line) { return line.endsWith(message); }))
            appendLog(message);
    }
#endif
}

void ToolboxController::cleanupLogs() {
    const auto path = dataPath() + QStringLiteral("/activity.log");
    const int days = QSettings(settingsPath(), QSettings::IniFormat)
                         .value(QStringLiteral("logRetentionDays"), 30)
                         .toInt();
    if (!pruneActivity(path, days, QDateTime::currentDateTime())) {
        m_status->setText(tr("Unable to clean old logs. Check write access to the data folder"));
        return;
    }
    if (m_log)
        m_log->setPlainText(recentActivity(path));
}
void ToolboxController::setContextStatus(const QString& text) {
    const auto help =
        tr("On: 1,000,000 tokens, compact at 900,000. Off: remove both overrides and use "
           "the model default. Restart the Codex session to apply. Model support is required");
    m_contextHelp->setToolTip(help + QStringLiteral("\n\n") + text);
    m_contextHelp->setAccessibleDescription(m_contextHelp->toolTip());
}
void ToolboxController::runContext(const QString& action) {
    if (m_contextProcess.state() != QProcess::NotRunning)
        return;
    m_contextToggle->setEnabled(false);
    m_contextRefresh->setEnabled(false);
    m_contextDefault->setEnabled(false);
    m_contextWriting = action != QStringLiteral("read");
    setContextStatus(tr("Reading or updating the Codex context setting…"));
    const auto runtime =
        qEnvironmentVariable("SNOW_TOOLBOX_RUNTIME",
                             QCoreApplication::applicationDirPath() + QStringLiteral("/runtime"));
    QStringList arguments{runtime + QStringLiteral("/codex_context.py"), action};
    if (m_contextWriting)
        arguments.append(m_contextRevision);
    m_contextTimeout.start();
    m_contextProcess.start(runtime + QStringLiteral("/python/python.exe"), arguments);
}
void ToolboxController::finishContext() {
    m_contextTimeout.stop();
    const auto result = QJsonDocument::fromJson(m_contextProcess.readAllStandardOutput()).object();
    m_contextProcess.readAllStandardError();
    m_contextRefresh->setEnabled(true);
    m_contextRevision = result.value(QStringLiteral("revision")).toString();
    const auto error = result.value(QStringLiteral("error")).toString();
    if (m_contextProcess.exitStatus() != QProcess::NormalExit || m_contextProcess.exitCode() != 0 ||
        m_contextRevision.isEmpty() || !error.isEmpty()) {
        m_contextRevision.clear();
        m_contextToggle->setChecked(false);
        const auto message =
            error == QStringLiteral("changed")
                ? tr("The configuration changed elsewhere. Refresh and try again")
                : tr("Unable to read or update the Codex configuration. Check access and TOML "
                     "syntax; unsupported layouts are left unchanged");
        setContextStatus(message);
        m_status->setText(message);
        appendLog(message);
        if (m_contextWriting)
            AppFeedback::show(tr("Codex"), message);
        return;
    }
    const auto state = result.value(QStringLiteral("state")).toString();
    m_contextToggle->setChecked(state == QStringLiteral("1m"));
    m_contextToggle->setEnabled(true);
    m_contextDefault->setEnabled(state != QStringLiteral("default"));
    QString text =
        state == QStringLiteral("1m") ? tr("1M context enabled")
        : state == QStringLiteral("default")
            ? tr("Using the model default context")
            : tr("Custom context override detected; enable 1M or restore model defaults");
    text += QStringLiteral("\n") + result.value(QStringLiteral("path")).toString();
    const auto profile = result.value(QStringLiteral("profile")).toString();
    if (!profile.isEmpty())
        text += QStringLiteral("\n") + tr("Selected profile: %1").arg(profile);
    const auto backup = result.value(QStringLiteral("backup")).toString();
    if (!backup.isEmpty())
        text += QStringLiteral("\n") + tr("Backup: %1").arg(backup);
    if (m_contextWriting)
        text += QStringLiteral("\n") + tr("Saved. Restart the Codex session to apply");
    setContextStatus(text);
    if (m_contextWriting) {
        m_status->setText(text.section(QLatin1Char('\n'), 0, 0) + QStringLiteral(" · ") +
                          tr("Saved. Restart the Codex session to apply"));
        AppFeedback::show(tr("Codex"), m_status->text());
        appendLog(tr("Codex context: %1")
                      .arg(m_contextToggle->isChecked() ? tr("1M context enabled")
                                                        : tr("Using the model default context")));
    }
}
void ToolboxController::applyTheme() {
    const auto sheet = style();
    m_home->setStyleSheet(sheet);
    m_menu.setStyleSheet(sheet);
    if (m_detail)
        m_detail->setStyleSheet(sheet);
    if (m_textWindow)
        m_textWindow->setStyleSheet(sheet);
    for (auto* window : {static_cast<QWidget*>(m_libraryWindow.data()), m_qrWindow.data()})
        if (window)
            window->setStyleSheet(sheet);
    auto colors = m_home->palette();
    colors.setColor(QPalette::Highlight, QColor(themeColor()));
    colors.setColor(QPalette::HighlightedText, Qt::white);
    m_home->setPalette(colors);
    refreshUi();
}
QString ToolboxController::applySettings(bool updateShortcut) {
    applyTheme();
    const auto shortcutError =
        configureCapture()
            ? (updateShortcut ? m_actions.applyCaptureShortcut() : QString())
            : tr("Unable to apply the screenshot shortcut. Check the shortcut and try again");
    m_quota.setEnabled(enabled(QStringLiteral("quota")));
    m_memory.setEnabled(enabled(QStringLiteral("memory")));
    const bool clipboardEnabled = enabled(QStringLiteral("clipboardHistory"));
    const bool notesEnabled = enabled(QStringLiteral("localNotes"));
    m_clipboardTracker->setHistoryEnabled(clipboardEnabled);
    m_home->findChild<QCheckBox*>(QStringLiteral("ClipboardRecordingSetting"))
        ->setEnabled(clipboardEnabled);
    if (m_libraryWindow)
        m_libraryWindow->setFeatures(clipboardEnabled, notesEnabled);
    if (m_qrWindow && !enabled(QStringLiteral("qrGenerator")))
        m_qrWindow->close();
    const auto updateModuleEntry = [](auto* entry) {
        const auto module = entry->property("requiresModule").toString();
        if (!module.isEmpty())
            entry->setEnabled(enabled(module));
    };
    for (auto* button : m_home->findChildren<QPushButton*>())
        updateModuleEntry(button);
    for (auto* action : m_menu.actions())
        updateModuleEntry(action);
    if (!enabled(QStringLiteral("translation")))
        OfflineTranslator::instance().stop();
    const bool pinsEnabled = enabled(QStringLiteral("pin"));
    if (m_pinsEnabled != pinsEnabled) {
        m_actions.setPinsEnabled(pinsEnabled);
        m_pinsEnabled = pinsEnabled;
    }
    m_card->setVisible(!m_capture && enabled(QStringLiteral("card")));
    if (!m_card->isVisible()) {
        m_detail->hide();
        m_hover.stop();
        m_leave.stop();
    }
    refreshUi();
    if (m_home->findChild<QTabWidget*>()->currentIndex() == 0)
        runContext(QStringLiteral("read"));
    QString quickError;
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        QSettings settings(settingsPath(), QSettings::IniFormat);
        quickError = m_quickPaste->configure(
            clipboardEnabled
                ? QKeySequence(settings.value(QStringLiteral("quickClipboardShortcut"),
                                              QStringLiteral("Ctrl+Alt+V")).toString())
                : QKeySequence{},
            notesEnabled
                ? QKeySequence(settings.value(QStringLiteral("quickNotesShortcut"),
                                              QStringLiteral("Ctrl+Alt+N")).toString())
                : QKeySequence{});
    }
    return shortcutError.isEmpty() ? quickError : quickError.isEmpty() ? shortcutError : shortcutError + QLatin1Char('\n') + quickError;
}
void ToolboxController::setShortcutEditing(bool editing) {
    if (m_shortcutEditing == editing)
        return;
    m_shortcutEditing = editing;
    m_actions.setShortcutEditing(editing);
    const auto error = m_quickPaste ? m_quickPaste->setSuspended(editing) : QString();
    if (!error.isEmpty())
        AppFeedback::show(QStringLiteral("快速呼出快捷键"), error);
}
void ToolboxController::showHome() {
    m_quota.refreshOnInteraction();
    m_hover.stop();
    m_leave.stop();
    m_detail->hide();
    m_home->setWindowState(m_home->windowState() & ~Qt::WindowMinimized);
    m_home->show();
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        // A hidden STARTUPINFO can suppress the first native ShowWindow even though Qt marks
        // the widget visible. Explicitly show it again so every entry point can recover.
        const auto handle = reinterpret_cast<HWND>(m_home->winId());
        if (!IsWindowVisible(handle))
            ShowWindow(handle, m_home->isMaximized() ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        SetForegroundWindow(handle);
    }
#endif
    if (!m_home->isMaximized()) {
        QSettings settings(settingsPath(), QSettings::IniFormat);
        const auto savedPosition = settings.value(QStringLiteral("homePosition"));
        auto* screen = savedPosition.isValid()
                           ? QGuiApplication::screenAt(savedPosition.toPoint())
                           : nullptr;
        const bool restorePosition = screen != nullptr;
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        const auto area = screen->availableGeometry();
        const auto frameExtra = m_home->frameSize() - m_home->size();
        const auto availableSize = (area.size() - frameExtra).expandedTo(QSize(1, 1));
        m_home->setMinimumSize(homeMinimumSize.boundedTo(availableSize));
        m_home->resize(m_home->size().boundedTo(availableSize));
        auto frame = m_home->frameGeometry();
        frame.moveCenter(area.center());
        if (restorePosition) {
            const auto position = savedPosition.toPoint();
            frame.moveTopLeft({std::clamp(position.x(), area.left(),
                                          std::max(area.left(), area.right() - frame.width() + 1)),
                               std::clamp(position.y(), area.top(),
                                          std::max(area.top(), area.bottom() - frame.height() + 1))});
        }
        m_home->move(frame.topLeft());
    }
    m_home->raise();
    m_home->activateWindow();
    refreshUi();
}
void ToolboxController::reportShortcutFailure() {
    const auto message =
        tr("The screenshot shortcut could not be registered. Choose another shortcut in Settings");
    m_status->setText(message);
    appendLog(message);
    AppFeedback::notify(&m_tray, tr("Screenshot shortcut"), message, QSystemTrayIcon::Warning);
}
void ToolboxController::presentText(const QString& text) {
    if (m_textWindow)
        m_textWindow->close();
    auto* window = new QWidget(nullptr, Qt::Window | Qt::WindowStaysOnTopHint);
    m_textWindow = window;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setObjectName(QStringLiteral("TextTranslationWindow"));
    window->setWindowTitle(tr("Longniang Toolbox · text and translation"));
    window->setStyleSheet(style());
    window->resize(880, 510);
    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    label(tr("Original text → offline translation"), layout);
    auto* editors = new QHBoxLayout;
    layout->addLayout(editors, 1);
    auto* source = new QPlainTextEdit;
    source->setObjectName(QStringLiteral("TextSource"));
    source->setPlaceholderText(tr("Type or paste text here"));
    auto* target = new QPlainTextEdit;
    target->setObjectName(QStringLiteral("TextTranslationResult"));
    target->setReadOnly(true);
    editors->addWidget(source);
    editors->addWidget(target);
    auto* state = label({}, layout);
    auto* row = new QHBoxLayout;
    row->setSpacing(12);
    layout->addLayout(row);
    auto* translateColumn = new QVBoxLayout;
    auto* copyColumn = new QVBoxLayout;
    row->addLayout(translateColumn, 1);
    row->addLayout(copyColumn, 1);
    auto* direction = new QComboBox;
    direction->setObjectName(QStringLiteral("TranslationDirection"));
    direction->addItem(tr("Auto Chinese ↔ English"), QString());
    direction->addItem(tr("English → Chinese"), QStringLiteral("zh"));
    direction->addItem(tr("Chinese → English"), QStringLiteral("en"));
    direction->setMinimumHeight(32);
    translateColumn->addWidget(direction);
    auto request = std::make_shared<quint64>(0);
    auto generation = std::make_shared<quint64>(0);
    const auto translate = [this, window, source, target, state, direction, request, generation] {
        OfflineTranslator::instance().cancel(*request);
        ++*generation;
        if (!enabled(QStringLiteral("translation"))) {
            target->clear();
            state->setText(tr("Offline translation is disabled in Settings"));
            return;
        }
        if (source->toPlainText().trimmed().isEmpty()) {
            target->clear();
            state->clear();
            return;
        }
        const auto current = *generation;
        state->setText(tr("Translating locally…"));
        appendLog(tr("Local translation started"));
        *request = OfflineTranslator::instance().translate(
            source->toPlainText(), direction->currentData().toString(), window,
            [this, target, state, generation, current](QString result, QString error) {
                if (*generation != current)
                    return;
                target->setPlainText(result);
                state->setText(error.isEmpty()
                                   ? tr("Complete · the model exits after 60 seconds idle")
                                   : error);
                appendLog(error.isEmpty() ? tr("Local translation complete") : error);
            });
    };
    button(tr("Translate"), translateColumn, translate)
        ->setObjectName(QStringLiteral("TranslateText"));
    button(tr("Copy original"), copyColumn, [source] {
        QApplication::clipboard()->setText(source->toPlainText());
    })->setObjectName(QStringLiteral("CopyOriginal"));
    button(tr("Copy translation"), copyColumn, [target] {
        QApplication::clipboard()->setText(target->toPlainText());
    })->setObjectName(QStringLiteral("CopyTranslation"));
    auto* close = button(tr("Exit"), row, [window] { window->close(); });
    close->setObjectName(QStringLiteral("CloseTranslation"));
    close->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    row->setStretchFactor(close, 1);
    auto* debounce = new QTimer(window);
    debounce->setSingleShot(true);
    debounce->setInterval(600);
    connect(debounce, &QTimer::timeout, window, translate);
    connect(source, &QPlainTextEdit::textChanged, debounce, [debounce] { debounce->start(); });
    connect(direction, &QComboBox::currentIndexChanged, window, [translate](int) { translate(); });
    source->setPlainText(text);
    window->show();
    window->raise();
    window->activateWindow();
    appendLog(tr("Text and translation panel opened"));
}
void ToolboxController::showCaptureSettings() {
    auto* dialog = new QDialog(m_home);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Screenshot session shortcuts"));
    dialog->setMinimumWidth(440);
    auto* layout = new QVBoxLayout(dialog);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    QSettings settings(settingsPath(), QSettings::IniFormat);
    const QList<QStringList> actions{
        {QStringLiteral("text_recognition"), tr("Text recognition"), QStringLiteral("Ctrl+O")},
        {QStringLiteral("qr_code_recognition"), tr("QR codes and barcodes"),
         QStringLiteral("Ctrl+Q")},
        {QStringLiteral("text_translation"), tr("Offline translation"), QStringLiteral("Ctrl+T")},
        {QStringLiteral("pin_to_screen"), tr("Pinned images"), QStringLiteral("Ctrl+P")},
        {QStringLiteral("copy_to_clipboard"), tr("Copy screenshot"), QStringLiteral("Ctrl+C")},
        {QStringLiteral("save_as_file"), tr("Save screenshot"), QStringLiteral("Ctrl+S")}};
    QList<QKeySequenceEdit*> edits;
    for (const auto& action : actions) {
        auto* edit = new QKeySequenceEdit(QKeySequence(
            settings.value(QStringLiteral("localShortcut/") + action[0], action[2]).toString()));
        edit->setMaximumSequenceLength(1);
        form->addRow(action[1], edit);
        edits.append(edit);
    }
    label(tr("Before or after selection, press Shift to change the color format, C to copy in "
             "that format, or X to copy HEX. Esc cancels. Line width, colors and text size are "
             "available in the screenshot toolbar"),
          layout);
    auto* status = label({}, layout);
    button(tr("Save and apply"), layout, [this, dialog, actions, edits, status] {
        QSet<QString> used{QStringLiteral("C"), QStringLiteral("X"), QStringLiteral("Esc")};
        QStringList keys;
        for (auto* edit : edits) {
            const auto sequence = edit->keySequence();
            const auto key = sequence.toString(QKeySequence::PortableText);
            if (sequence.count() > 1 || (!key.isEmpty() && used.contains(key))) {
                status->setText(tr("Choose a different single shortcut for each action"));
                return;
            }
            if (!key.isEmpty())
                used.insert(key);
            keys.append(key);
        }
        QSettings values(settingsPath(), QSettings::IniFormat);
        for (int i = 0; i < actions.size(); ++i)
            values.setValue(QStringLiteral("localShortcut/") + actions[i][0], keys[i]);
        values.sync();
        m_actions.cancelCapture();
        configureCapture();
        dialog->accept();
        appendLog(tr("Screenshot shortcuts saved"));
        AppFeedback::show(tr("Settings"), tr("Screenshot shortcuts saved"));
    });
    dialog->show();
}
void ToolboxController::capture() {
    if (!enabled(QStringLiteral("capture"))) {
        m_status->setText(tr("Enable screenshots in Settings first"));
        return;
    }
    m_home->hide();
    m_card->hide();
    m_detail->hide();
    appendLog(tr("Region capture requested"));
    QTimer::singleShot(100, this, [this] { m_actions.capture(); });
}
void ToolboxController::captureActivity(bool active) {
    if (active != m_capture)
        appendLog(QStringLiteral("[screenshot.capture] pid=%1 stage=%2")
                      .arg(QCoreApplication::applicationPid())
                      .arg(active ? QStringLiteral("started") : QStringLiteral("ended")));
    m_capture = active;
    m_hover.stop();
    if (active) {
        m_taskbarTracking.stop();
        m_taskbarEvents->setEnabled(false);
        m_card->hide();
        m_detail->hide();
        m_leave.stop();
    } else {
        positionCard();
    }
}
void ToolboxController::clean(bool deep) {
    if (!enabled(QStringLiteral("cleanup")))
        return;
    OfflineTranslator::instance().releaseIdle();
    m_status->setText(tr("Cleaning memory…"));
    appendLog(deep ? tr("Deep cleanup requested") : tr("Regular cleanup requested"));
    m_memory.clean(deep);
    refreshUi();
}
QString ToolboxController::quotaHtml() const {
    QString html = QStringLiteral("<b style='color:#64748b'>") + tr("Codex quota").toHtmlEscaped() +
                   QStringLiteral("</b>");
    html += QStringLiteral(" · ") + tr("Remaining").toHtmlEscaped();
    if (!enabled(QStringLiteral("quota")))
        return html + QStringLiteral("<p>") + tr("Disabled").toHtmlEscaped();
    const auto windows = m_quota.windows();
    if (windows.isEmpty())
        html += QStringLiteral("<p style='font-size:18pt'>-- &nbsp; / &nbsp; --</p>");
    if (!windows.isEmpty())
        html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='2'><tr>");
    int count = 0;
    for (const auto& window : windows) {
        if (count > 0 && count % 2 == 0)
            html += QStringLiteral("</tr><tr>");
        html += QStringLiteral("<td width='50%'>");
        ++count;
        const auto date = window.resets.isValid()
                              ? window.resets.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                              : QStringLiteral("--");
        html += QStringLiteral("<p style='margin:7px 0'><b>%1</b>&nbsp; <span "
                               "style='font-size:%2pt;color:%3;font-weight:600'>%4%</"
                               "span><br><span style='color:#7b8799'>%5 %6</span></p>")
                    .arg(window.title.toHtmlEscaped())
                    .arg(20)
                    .arg(m_quota.stale() ? QStringLiteral("#94a3b8") : themeColor())
                    .arg(window.remaining, 0, 'f', 0)
                    .arg(tr("Resets").toHtmlEscaped(), date);
        html += QStringLiteral("</td>");
    }
    if (!windows.isEmpty())
        html += QStringLiteral("</tr></table>");
    return html.replace(QStringLiteral("THEME_ACCENT"), themeColor());
}
QString ToolboxController::memoryHtml(bool compact) const {
    if (!enabled(QStringLiteral("memory")))
        return tr("Memory information is disabled");
    const auto memory = m_memory.snapshot();
    if (!memory.total)
        return tr("Reading memory…");
    const auto capacity = [](const QString& title, quint64 total, quint64 available) {
        return QStringLiteral("<b>%1</b><br><span style='font-size:16pt;color:THEME_ACCENT'>%2%</span>"
                              " · %3<br>%4<br>%5")
            .arg(title.toHtmlEscaped())
            .arg(total ? 100.0 * (1 - double(available) / double(total)) : 0, 0, 'f', 0)
            .arg(tr("Used").toHtmlEscaped())
            .arg(tr("%1 / %2 GiB")
                     .arg((total - available) / 1073741824.0, 0, 'f', 2)
                     .arg(total / 1073741824.0, 0, 'f', 2))
            .arg(tr("Available: %1 GiB").arg(available / 1073741824.0, 0, 'f', 2));
    };
    const QStringList cells{
        capacity(tr("Physical memory"), memory.total, memory.available),
        capacity(tr("Virtual memory (commit)"), memory.virtualTotal, memory.virtualAvailable),
        QStringLiteral("<b>%1</b><br><span style='font-size:16pt;color:THEME_ACCENT'>%2 GiB</span>")
            .arg(tr("System cache").toHtmlEscaped())
            .arg(memory.systemCache / 1073741824.0, 0, 'f', 2)};
    QString html = compact
                       ? QStringLiteral("<table width='100%' cellspacing='0' cellpadding='2' "
                                        "style='font-size:9pt'><tr>")
                       : QStringLiteral("<table width='100%' cellspacing='0' cellpadding='4'><tr>");
    for (int i = 0; i < cells.size(); ++i) {
        html += QStringLiteral("<td valign='top'>") + cells[i] + QStringLiteral("</td>");
    }
    html += compact ? QStringLiteral("</tr></table><p style='font-size:8.5pt;color:#64748b'>")
                    : QStringLiteral("</tr></table><p style='color:#64748b'>");
    html += tr("Toolbox and %1 processes: %2 MiB private working set · %3 MiB private commit")
                .arg(memory.ownedProcesses)
                .arg(memory.privateWorking / 1048576.0, 0, 'f', 1)
                .arg(memory.committed / 1048576.0, 0, 'f', 1);
    return (html + QStringLiteral("</p>")).replace(QStringLiteral("THEME_ACCENT"), themeColor());
}
void ToolboxController::refreshUi() {
    if (!m_card)
        return;
    QSettings settings(settingsPath(), QSettings::IniFormat);
    const auto cardStyle = settings.value(QStringLiteral("cardStyle")).toString();
    const bool docked = cardStyle == QStringLiteral("taskbar");
    if (docked != m_taskbarDocked) {
        m_dragging = false;
        m_hover.stop();
        m_detail->hide();
        if (docked) {
            settings.setValue(QStringLiteral("cardPosition"), m_card->pos());
        } else {
            m_card->setTaskbarHeight(0);
            m_card->move(settings.value(QStringLiteral("cardPosition"), m_card->pos()).toPoint());
        }
        m_taskbarDocked = docked;
        if (docked)
            m_taskbarTracking.start();
        else
            m_taskbarTracking.stop();
    }
    m_card->present(m_quota.windows(), m_quota.stale(), enabled(QStringLiteral("quota")),
                    cardStyle == QStringLiteral("stacked"), tr("Disabled"));
    positionCard();
    m_quotaLabel->setText(quotaHtml());
    m_quotaStatus->setText(m_quota.status());
    m_quotaStatus->setToolTip(m_quota.status());
    m_quotaUpdated->setText(
        tr("Updated: %1")
            .arg(m_quota.updatedAt().isValid()
                     ? m_quota.updatedAt().toString(QStringLiteral("MM-dd HH:mm:ss"))
                     : QStringLiteral("--")));
    m_memoryLabel->setText(memoryHtml());
    m_detailLabel->setText(quotaHtml() + QStringLiteral("<p style='font-size:9pt'>") +
                           m_quota.status().toHtmlEscaped() + QStringLiteral(" &nbsp;·&nbsp; ") +
                           m_quotaUpdated->text().toHtmlEscaped() + QStringLiteral("</p><hr>") +
                           memoryHtml(true));
    for (auto* window : {m_home, m_detail})
        for (auto* buttonWidget : window->findChildren<QPushButton*>()) {
            if (buttonWidget->objectName() == QStringLiteral("CleanupButton")) {
                buttonWidget->setVisible(enabled(QStringLiteral("cleanup")));
                buttonWidget->setEnabled(!m_memory.busy());
            }
            if (buttonWidget->objectName() == QStringLiteral("PinButton"))
                buttonWidget->setEnabled(enabled(QStringLiteral("pin")));
            if (buttonWidget->property("requiresCapture").toBool())
                buttonWidget->setEnabled(enabled(QStringLiteral("capture")));
        }
    m_menu.actions().value(1)->setEnabled(enabled(QStringLiteral("capture")));
    m_menu.actions().value(2)->setEnabled(enabled(QStringLiteral("pin")));
}
void ToolboxController::positionCard() {
    if (!m_card)
        return;
    bool visible = !m_capture && enabled(QStringLiteral("card"));
    m_taskbarEvents->setEnabled(visible && m_taskbarDocked);
    if (!visible) {
        m_taskbarTracking.stop();
    } else if (m_taskbarDocked) {
        if (!m_taskbarTracking.isActive())
            m_taskbarTracking.start();
        const auto dock = currentTaskbarDock();
        if (!dock.bar.isEmpty())
            m_card->setTaskbarHeight(std::max(1, dock.bar.height() - 4));
        const auto position = taskbarCardPosition(dock, m_card->size());
        visible = visible && position.has_value();
        if (position)
            m_card->move(*position);
    } else if (!m_dragging) {
        m_card->move(visiblePosition(m_card->pos(), m_card->size()));
    }
    m_card->setVisible(visible);
    if (!visible) {
        m_hover.stop();
        m_leave.stop();
        m_detail->hide();
    }
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows") &&
        m_card->testAttribute(Qt::WA_WState_Created)) {
        const auto handle = reinterpret_cast<HWND>(m_card->winId());
        const auto previous = m_card->property("TaskbarPeekExcluded");
        if (!previous.isValid() || previous.toBool() != m_taskbarDocked) {
            const BOOL excluded = m_taskbarDocked ? TRUE : FALSE;
            if (SUCCEEDED(DwmSetWindowAttribute(handle, DWMWA_EXCLUDED_FROM_PEEK, &excluded,
                                                sizeof(excluded))))
                m_card->setProperty("TaskbarPeekExcluded", m_taskbarDocked);
        }
        // Raising an already correctly ordered window can itself generate another reorder event.
        if (visible && m_taskbarDocked && taskbarCoversCard(handle))
            SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#endif
}
void ToolboxController::showDetail() {
    if (m_capture || !m_card->isVisible() || m_dragging)
        return;
    refreshUi();
    if (!m_card->isVisible())
        return;
    m_detail->adjustSize();
    auto* screen = QGuiApplication::screenAt(m_card->geometry().center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const auto area = screen->availableGeometry();
    QPoint position = m_taskbarDocked ? QPoint(m_card->x() + m_card->width() - m_detail->width(),
                                               m_card->y() - m_detail->height() - 6)
                                      : m_card->pos() - QPoint(m_detail->width() + 6, 0);
    if (position.x() < area.left())
        position.setX(m_card->geometry().right() + 7);
    m_detail->move(visiblePosition(position, m_detail->size()));
    m_detail->show();
    m_leave.start();
}
bool ToolboxController::eventFilter(QObject* object, QEvent* event) {
    if (object == m_home && (event->type() == QEvent::Hide || event->type() == QEvent::Close))
        setShortcutEditing(false);
    if (object == qApp && event->type() == QEvent::Quit && m_libraryWindow && !m_libraryWindow->notes()->flush()) {
        showNotes();
        event->ignore();
        return true;
    }
    if (object == qApp && event->type() == QEvent::Quit)
        m_quitting = true;
    if (object == m_home && event->type() == QEvent::Close && !m_quitting) {
        m_home->hide();
        event->ignore();
        return true;
    }
    if (object != m_card)
        return QObject::eventFilter(object, event);
    if (event->type() == QEvent::WinIdChange)
        m_card->setProperty("TaskbarPeekExcluded", QVariant());
    if (event->type() == QEvent::Enter) {
        m_quota.refreshOnInteraction();
        m_hover.start();
    }
    if (event->type() == QEvent::Leave)
        m_hover.stop();
    if (event->type() == QEvent::ContextMenu) {
        m_hover.stop();
        m_detail->hide();
        m_menu.exec(static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            m_dragging = true;
            m_moved = false;
            m_dragOrigin = mouse->globalPosition().toPoint();
            m_dragPosition = m_card->pos();
            m_hover.stop();
            m_detail->hide();
        }
    }
    if (event->type() == QEvent::MouseMove && m_dragging) {
        const auto delta =
            static_cast<QMouseEvent*>(event)->globalPosition().toPoint() - m_dragOrigin;
        if (delta.manhattanLength() > QApplication::startDragDistance())
            m_moved = true;
        if (m_moved && !m_taskbarDocked)
            m_card->move(m_dragPosition + delta);
    }
    if (event->type() == QEvent::MouseButtonRelease && m_dragging) {
        m_dragging = false;
        positionCard();
        if (!m_taskbarDocked)
            QSettings(settingsPath(), QSettings::IniFormat)
                .setValue(QStringLiteral("cardPosition"), m_card->pos());
        if (!m_moved)
            showHome();
    }
    return QObject::eventFilter(object, event);
}
bool ToolboxController::configureCapture() {
#ifndef SNOW_TOOLBOX_TEST_NO_STORAGE
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    QMap<QString, QJsonValue> values;
    const auto snapshot = configuration.snapshot();
    for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it)
        if (it.key().startsWith(QStringLiteral("global_shortcuts/")) && it.value().isArray())
            values.insert(it.key(), QJsonArray{});
        else if (it.key().startsWith(QStringLiteral("global_mouse/")) && it.value().isObject())
            values.insert(it.key(), QJsonObject{});
    QSettings settings(settingsPath(), QSettings::IniFormat);
    const QString captureKey =
        settings.value(QStringLiteral("captureShortcut"), QStringLiteral("Ctrl+Alt+1")).toString();
    values.insert(QStringLiteral("global_shortcuts/screenshot"),
                  enabled(QStringLiteral("capture")) && !captureKey.isEmpty()
                      ? QJsonArray{captureKey}
                      : QJsonArray{});
    values.insert(QStringLiteral("tray/enabled"), false);
    values.insert(QStringLiteral("interface/language"), QStringLiteral("zh_CN"));
    values.insert(QStringLiteral("updates/mode"), QStringLiteral("manual"));
    values.insert(QStringLiteral("mcp/enabled"), false);
    values.insert(QStringLiteral("system/context_menu_mode"), QStringLiteral("disabled"));
    values.insert(QStringLiteral("floating_toolbar/enabled"), false);
    values.insert(QStringLiteral("system/application_priority"), QStringLiteral("normal"));
    values.insert(QStringLiteral("text_recognition/resident_process"), false);
    values.insert(QStringLiteral("text_recognition/model_hot_start"), false);
    values.insert(QStringLiteral("screenshot_translation/source_language"), QStringLiteral("auto"));
    values.insert(QStringLiteral("screenshot_translation/target_language"),
                  QStringLiteral("zh-Hans"));
    values.insert(QStringLiteral("screenshot_translation/secondary_target_language"),
                  QStringLiteral("en"));
    values.insert(QStringLiteral("screenshot_translation/model"), QStringLiteral("toolbox-local"));
    values.insert(QStringLiteral("system/memory_optimization_policy"), QStringLiteral("disabled"));
    values.insert(QStringLiteral("pin_to_screen/automatic_text_recognition"), false);
    values.insert(QStringLiteral("screenshot_translation/original_image_translation"), true);
    QJsonArray actions;
    if (enabled(QStringLiteral("barcode")))
        actions.append(QJsonArray{QStringLiteral("barcode-recognition")});
    if (enabled(QStringLiteral("ocr")))
        actions.append(QJsonArray{QStringLiteral("text-recognition")});
    if (enabled(QStringLiteral("translation")))
        actions.append(QJsonArray{QStringLiteral("text-translation")});
    if (enabled(QStringLiteral("pin")))
        actions.append(QJsonArray{QStringLiteral("pin-to-screen")});
    actions.append(QJsonArray{QStringLiteral("save-as-file")});
    actions.append(QJsonArray{QStringLiteral("cancel")});
    actions.append(QJsonArray{QStringLiteral("copy")});
    const auto toolbarLayout = [](const QString& key, const QJsonArray& positions) {
        const auto defaults = snow_shot::storage::ConfigurationSchema::defaultValue(key).toObject();
        QSet<QString> visible;
        for (const auto& position : positions)
            for (const auto& id : position.toArray())
                visible.insert(id.toString());
        QSet<QString> hidden;
        for (const auto& position : defaults.value(QStringLiteral("positions")).toArray())
            for (const auto& id : position.toArray())
                if (!visible.contains(id.toString()))
                    hidden.insert(id.toString());
        for (const auto& id : defaults.value(QStringLiteral("hidden")).toArray())
            if (!visible.contains(id.toString()))
                hidden.insert(id.toString());
        auto hiddenIds = hidden.values();
        hiddenIds.sort();
        return QJsonObject{{QStringLiteral("positions"), positions},
                           {QStringLiteral("hidden"), QJsonArray::fromStringList(hiddenIds)}};
    };
    const auto actionKey = QStringLiteral("screenshot_toolbar/action_tools_layout");
    values.insert(actionKey, toolbarLayout(actionKey, actions));
    QJsonArray drawing;
    if (enabled(QStringLiteral("annotation")))
        for (const auto& id : {"select", "shape", "arrow", "line", "free-draw", "text", "filter",
                               "highlighter", "undo", "redo"})
            drawing.append(QJsonArray{QString::fromLatin1(id)});
    const auto drawingKey = QStringLiteral("screenshot_toolbar/layout");
    values.insert(drawingKey, toolbarLayout(drawingKey, drawing));
    for (const auto& item : QList<QPair<QString, QString>>{
             {QStringLiteral("text_recognition"), QStringLiteral("Ctrl+O")},
             {QStringLiteral("qr_code_recognition"), QStringLiteral("Ctrl+Q")},
             {QStringLiteral("text_translation"), QStringLiteral("Ctrl+T")},
             {QStringLiteral("pin_to_screen"), QStringLiteral("Ctrl+P")},
             {QStringLiteral("copy_to_clipboard"), QStringLiteral("Ctrl+C")},
             {QStringLiteral("save_as_file"), QStringLiteral("Ctrl+S")}})
        values.insert(
            QStringLiteral("screenshot_shortcuts/") + item.first,
            settings.value(QStringLiteral("localShortcut/") + item.first, item.second)
                    .toString()
                    .isEmpty()
                ? QJsonArray{}
                : QJsonArray{
                      settings.value(QStringLiteral("localShortcut/") + item.first, item.second)
                          .toString()});
    values.insert(QStringLiteral("screenshot_shortcuts/copy_color"),
                  enabled(QStringLiteral("color")) ? QJsonArray{QStringLiteral("C")}
                                                   : QJsonArray{});
    values.insert(QStringLiteral("screenshot_ui/color_picker_display_mode"),
                  enabled(QStringLiteral("color")) ? QStringLiteral("always_show")
                                                   : QStringLiteral("always_hide"));
    values.insert(QStringLiteral("screenshot_selection/background_opacity_percent"), 25);
    values.insert(QStringLiteral("screenshot/auto_recognize_qr_code"), false);
    values.insert(QStringLiteral("screenshot_ui/cursor_guide_line_color"),
                  QStringLiteral("#00000000"));
    values.insert(QStringLiteral("screenshot_ui/monitor_center_guide_line_color"),
                  QStringLiteral("#00000000"));
    for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it)
        if (it.key().startsWith(QStringLiteral("drawing_shortcuts/")))
            values.insert(it.key(),
                          enabled(QStringLiteral("annotation"))
                              ? snow_shot::storage::ConfigurationSchema::defaultValue(it.key())
                              : QJsonArray{});
    for (const auto& item : QList<QPair<QString, QString>>{
             {QStringLiteral("ocr"), QStringLiteral("text_recognition")},
             {QStringLiteral("barcode"), QStringLiteral("qr_code_recognition")},
             {QStringLiteral("translation"), QStringLiteral("text_translation")},
             {QStringLiteral("pin"), QStringLiteral("pin_to_screen")}})
        if (!enabled(item.first))
            values.insert(QStringLiteral("screenshot_shortcuts/") + item.second, QJsonArray{});
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        if (!snow_shot::storage::ConfigurationSchema::contains(it.key()) ||
            !snow_shot::storage::ConfigurationSchema::normalize(it.key(), it.value()).valid) {
            qWarning().noquote() << "Invalid toolbox configuration:" << it.key();
            return false;
        }
    return configuration.setValues(values);
#else
    return true;
#endif
}
} // namespace toolbox
