// SPDX-License-Identifier: GPL-3.0-or-later
#include "toolboxcontroller.h"
#include "toolboxpaths.h"
#include "offlinetranslator.h"
#ifndef SNOW_TOOLBOX_TEST_NO_STORAGE
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/configurationschema.h"
#endif
#include <QtWidgets>
#include <QTranslator>
#include <QJsonArray>
#include <algorithm>

namespace toolbox {
namespace {
QString style() {
    return QStringLiteral(
        "QWidget{font-family:'Microsoft YaHei UI';font-size:10pt;color:#263342;}"
        "QWidget#ToolboxHome{background:#f4f6fa;} "
        "QFrame#Box,QWidget#QuotaCard,QWidget#QuotaDetail{background:#fff;border:1px solid "
        "#dce2eb;border-radius:12px;}"
        "QLabel{background:transparent;border:0;} "
        "QLabel#Title{font-size:23pt;font-weight:600;color:#142234;}"
        "QPushButton{border:1px solid #d8dfea;border-radius:7px;padding:10px "
        "15px;background:#fff;color:#334155;}"
        "QPushButton:hover{background:#edf3ff;border-color:#93b5f5;} "
        "QPushButton#Primary{background:#3976e9;color:white;border:0;font-weight:600;}"
        "QPushButton#Primary:hover{background:#2563cb;} "
        "QPushButton:disabled{color:#96a0b0;background:#edf0f5;}"
        "QTabWidget::pane{border:0;} QTabBar::tab{padding:10px 24px;border:0;color:#778396;}"
        "QTabBar::tab:selected{color:#2563cb;border-bottom:2px solid #3976e9;}"
        "QCheckBox{spacing:8px;padding:7px 0;} "
        "QLineEdit,QKeySequenceEdit{background:white;border:1px solid "
        "#d8dfea;border-radius:6px;padding:7px;}"
        "QGroupBox{border:1px solid #dce2eb;border-radius:9px;margin-top:14px;padding:16px 12px "
        "10px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:12px;padding:0 5px;} "
        "QMenu{background:white;padding:6px;} QMenu::item{padding:8px 20px;} "
        "QMenu::item:selected{background:#edf3ff;}");
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
} // namespace
ToolboxController::ToolboxController(ToolboxActions actions, QObject* parent)
    : QObject(parent), m_actions(std::move(actions)), m_quota(this), m_memory(this) {
    qApp->setQuitOnLastWindowClosed(false);
    qApp->installEventFilter(this);
    buildHome();
    m_menu.setStyleSheet(style());
    m_menu.addAction(tr("Open toolbox"), this, &ToolboxController::showHome);
    m_menu.addAction(tr("Capture region"), this, &ToolboxController::capture);
    m_menu.addAction(tr("Restore pinned images"), this, [this] {
        if (enabled(QStringLiteral("pin")))
            m_actions.restorePins();
    });
    m_menu.addAction(tr("Refresh quota"), &m_quota, &QuotaService::refresh);
    m_menu.addSeparator();
    m_menu.addAction(tr("Quit toolbox"), qApp, &QApplication::quit);
    m_tray.setIcon(QApplication::windowIcon());
    m_tray.setToolTip(tr("Snow Toolbox · local and lightweight"));
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
    button(tr("Open toolbox"), detailLayout, [this] { showHome(); });
    m_detailStatus = label({}, detailLayout);
    const auto reposition = [this] {
        m_card->move(visiblePosition(m_card->pos(), m_card->size()));
        m_detail->hide();
    };
    for (auto* screen : QGuiApplication::screens())
        connect(screen, &QScreen::availableGeometryChanged, this, reposition);
    connect(qApp, &QGuiApplication::screenAdded, this, reposition);
    connect(qApp, &QGuiApplication::screenRemoved, this, reposition);
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
    connect(&m_quota, &QuotaService::changed, this, &ToolboxController::refreshUi);
    connect(&m_memory, &MemoryService::changed, this, &ToolboxController::refreshUi);
    connect(&m_memory, &MemoryService::cleaned, this, [this](const QString& result) {
        m_status->setText(result);
        refreshUi();
    });
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        QSettings(settingsPath(), QSettings::IniFormat)
            .setValue(QStringLiteral("cardPosition"), m_card->pos());
        m_quota.shutdown();
        OfflineTranslator::instance().stop();
    });
    applySettings();
    if (!settings.value(QStringLiteral("hasStarted"), false).toBool()) {
        settings.setValue(QStringLiteral("hasStarted"), true);
        QTimer::singleShot(0, this, &ToolboxController::showHome);
    }
}
ToolboxController::~ToolboxController() {
    if (m_contextProcess.state() != QProcess::NotRunning) {
        if (!m_contextProcess.waitForFinished(1000)) {
            m_contextProcess.kill();
            m_contextProcess.waitForFinished(1000);
        }
    }
    delete m_detail;
    delete m_card;
    delete m_home;
}
void ToolboxController::buildHome() {
    m_home = new QWidget;
    m_home->setObjectName(QStringLiteral("ToolboxHome"));
    m_home->setWindowTitle(tr("Snow Toolbox · 0.3.0"));
    m_home->resize(850, 770);
    m_home->setMinimumSize(740, 740);
    m_home->setStyleSheet(style());
    m_home->installEventFilter(this);
    auto* layout = new QVBoxLayout(m_home);
    layout->setContentsMargins(28, 22, 28, 22);
    layout->setSpacing(13);
    label(tr("Snow Toolbox"), layout)->setObjectName(QStringLiteral("Title"));
    label(tr("Capture, read and keep your quota in view"), layout);
    auto* tabs = new QTabWidget;
    layout->addWidget(tabs, 1);
    auto* overview = new QWidget;
    auto* body = new QVBoxLayout(overview);
    body->setContentsMargins(0, 16, 0, 0);
    body->setSpacing(14);
    auto* overviewScroll = new QScrollArea;
    overviewScroll->setFrameShape(QFrame::NoFrame);
    overviewScroll->setWidgetResizable(true);
    overviewScroll->setWidget(overview);
    body->setSizeConstraint(QLayout::SetMinimumSize);
    tabs->addTab(overviewScroll, tr("Overview"));
    auto* quotaBox = box(body);
    auto* quotaLayout = new QVBoxLayout(quotaBox);
    quotaLayout->setContentsMargins(20, 17, 20, 17);
    m_quotaLabel = label({}, quotaLayout);
    m_quotaLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_quotaLabel->setObjectName(QStringLiteral("QuotaValues"));
    m_quotaLabel->setFixedHeight(128);
    m_quotaLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    button(tr("Refresh quota"), quotaLayout, [this] { m_quota.refresh(); });
    auto* captureRow = new QHBoxLayout;
    body->addLayout(captureRow);
    auto* captureButton = button(tr("Capture region"), captureRow, [this] { capture(); });
    captureButton->setObjectName(QStringLiteral("Primary"));
    captureButton->setProperty("requiresCapture", true);
    captureButton->setMinimumHeight(52);
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
    label(tr("Select once, then annotate, recognize text, translate, copy, save or pin. C: RGB · "
             "X: HEX · Esc: cancel"),
          body);
    button(tr("Text and translation panel"), body, [this] {
        if (m_textWindow) {
            m_textWindow->show();
            m_textWindow->raise();
            m_textWindow->activateWindow();
        } else
            presentText({});
    });
    auto* memoryBox = box(body);
    auto* memoryLayout = new QVBoxLayout(memoryBox);
    memoryLayout->setContentsMargins(20, 17, 20, 17);
    m_memoryLabel = label({}, memoryLayout);
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
    m_status = label(tr("Ready · OCR and translation run locally"), body);
    body->addStretch();
    auto* settingsPage = new QWidget;
    auto* settingsLayout = new QVBoxLayout(settingsPage);
    settingsLayout->setContentsMargins(0, 12, 0, 0);
    auto* settingsScroll = new QScrollArea;
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setWidget(settingsPage);
    settingsLayout->setSizeConstraint(QLayout::SetMinimumSize);
    tabs->addTab(settingsScroll, tr("Settings"));
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
        {QStringLiteral("cleanup"), tr("Memory cleanup")}};
    QSettings settings(settingsPath(), QSettings::IniFormat);
    int index = 0;
    QList<QCheckBox*> checks;
    for (const auto& item : names) {
        auto* check = new QCheckBox(item.second);
        check->setProperty("module", item.first);
        check->setChecked(enabled(item.first));
        moduleGrid->addWidget(check, index / 2, index % 2);
        ++index;
        checks.append(check);
    }
    auto* form = new QFormLayout;
    settingsLayout->addLayout(form);
    auto* key = new QKeySequenceEdit(
        QKeySequence(settings.value(QStringLiteral("captureShortcut"), QStringLiteral("Ctrl+Alt+1"))
                         .toString()));
    key->setMaximumSequenceLength(1);
    form->addRow(tr("Screenshot shortcut"), key);
    auto* cardStyle = new QComboBox;
    cardStyle->setObjectName(QStringLiteral("QuotaCardStyle"));
    cardStyle->addItem(tr("Compact dual capsules"), QStringLiteral("compact"));
    cardStyle->addItem(tr("Four-row pebble"), QStringLiteral("stacked"));
    cardStyle->setCurrentIndex(
        settings.value(QStringLiteral("cardStyle")).toString() == QStringLiteral("stacked") ? 1
                                                                                            : 0);
    form->addRow(tr("Desktop card style"), cardStyle);
    auto* autoStart = new QCheckBox(tr("Start with Windows"));
    autoStart->setObjectName(QStringLiteral("ToolboxAutoStart"));
    autoStart->setChecked(m_actions.autoStartEnabled());
    settingsLayout->addWidget(autoStart);
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
           [this, checks, key, executable, cardStyle, autoStart] {
               const auto error = m_actions.setAutoStart(autoStart->isChecked());
               if (!error.isEmpty()) {
                   autoStart->setChecked(m_actions.autoStartEnabled());
                   m_status->setText(error);
                   QMessageBox::warning(m_home, tr("Start with Windows"), error);
                   return;
               }
               QSettings values(settingsPath(), QSettings::IniFormat);
               values.setValue(QStringLiteral("cardStyle"), cardStyle->currentData());
               for (auto* check : checks)
                   values.setValue(check->property("module").toString(), check->isChecked());
               values.setValue(QStringLiteral("captureShortcut"),
                               key->keySequence().toString(QKeySequence::PortableText));
               values.setValue(QStringLiteral("codexExecutable"), executable->text().trimmed());
               values.sync();
               m_actions.cancelCapture();
               applySettings();
               m_status->setText(
                   tr("Settings saved. Check shortcut conflicts if capture does not respond"));
           })
        ->setObjectName(QStringLiteral("SaveToolboxSettings"));
    button(tr("Screenshot session shortcuts…"), settingsLayout,
           [this] { m_actions.openCaptureSettings(); });
    auto* contextBox = new QGroupBox(tr("Codex context"));
    settingsLayout->addWidget(contextBox);
    auto* contextLayout = new QVBoxLayout(contextBox);
    m_contextToggle = new QCheckBox(tr("Enable 1M context"));
    m_contextToggle->setObjectName(QStringLiteral("CodexContextToggle"));
    m_contextToggle->setEnabled(false);
    contextLayout->addWidget(m_contextToggle);
    label(tr("On: 1,000,000 tokens, compact at 900,000. Off: remove both overrides and use "
             "the model default. Restart the Codex session to apply. Model support is required"),
          contextLayout);
    m_contextStatus =
        label(tr("Read the current Codex configuration before changing it"), contextLayout);
    m_contextStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* contextRow = new QHBoxLayout;
    contextLayout->addLayout(contextRow);
    m_contextRefresh = button(tr("Refresh context setting"), contextRow,
                              [this] { runContext(QStringLiteral("read")); });
    m_contextDefault = button(tr("Restore model defaults"), contextRow,
                              [this] { runContext(QStringLiteral("off")); });
    m_contextDefault->setObjectName(QStringLiteral("CodexContextDefault"));
    m_contextDefault->setEnabled(false);
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
        if (index == 1)
            runContext(QStringLiteral("read"));
    });
    settingsLayout->addStretch();
    auto* footer = new QHBoxLayout;
    layout->addLayout(footer);
    label(tr("Snow Shot 1.5.7 base · offline models sleep after use"), footer);
    button(tr("Quit toolbox"), footer, [] { QApplication::quit(); });
}
void ToolboxController::runContext(const QString& action) {
    if (m_contextProcess.state() != QProcess::NotRunning)
        return;
    m_contextToggle->setEnabled(false);
    m_contextRefresh->setEnabled(false);
    m_contextDefault->setEnabled(false);
    m_contextWriting = action != QStringLiteral("read");
    m_contextStatus->setText(tr("Reading or updating the Codex context setting…"));
    const auto runtime =
        qEnvironmentVariable("SNOW_TOOLBOX_RUNTIME",
                             QCoreApplication::applicationDirPath() + QStringLiteral("/runtime"));
    QStringList arguments{runtime + QStringLiteral("/codex_context.py"), action};
    if (m_contextWriting)
        arguments.append(m_contextRevision);
    m_contextProcess.start(runtime + QStringLiteral("/python/python.exe"), arguments);
    m_contextTimeout.start();
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
        m_contextStatus->setText(
            error == QStringLiteral("changed")
                ? tr("The configuration changed elsewhere. Refresh and try again")
                : tr("Unable to read or update the Codex configuration. Check access and TOML "
                     "syntax; unsupported layouts are left unchanged"));
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
    m_contextStatus->setText(text);
}
void ToolboxController::applySettings() {
    configureCapture();
    m_quota.setEnabled(enabled(QStringLiteral("quota")));
    m_memory.setEnabled(enabled(QStringLiteral("memory")));
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
}
void ToolboxController::showHome() {
    m_home->showNormal();
    m_home->raise();
    m_home->activateWindow();
    refreshUi();
}
void ToolboxController::reportShortcutFailure() {
    const auto message =
        tr("The screenshot shortcut could not be registered. Choose another shortcut in Settings");
    m_status->setText(message);
    m_tray.showMessage(tr("Screenshot shortcut"), message, QSystemTrayIcon::Warning);
}
void ToolboxController::presentText(const QString& text) {
    if (m_textWindow)
        m_textWindow->close();
    auto* window = new QWidget(nullptr, Qt::Window | Qt::WindowStaysOnTopHint);
    m_textWindow = window;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(tr("Local text recognition and translation"));
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
    layout->addLayout(row);
    auto* direction = new QComboBox;
    direction->addItem(tr("Auto Chinese ↔ English"), QString());
    direction->addItem(tr("English → Chinese"), QStringLiteral("zh"));
    direction->addItem(tr("Chinese → English"), QStringLiteral("en"));
    row->addWidget(direction);
    auto request = std::make_shared<quint64>(0);
    auto generation = std::make_shared<quint64>(0);
    const auto translate = [window, source, target, state, direction, request, generation] {
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
        *request = OfflineTranslator::instance().translate(
            source->toPlainText(), direction->currentData().toString(), window,
            [target, state, generation, current](QString result, QString error) {
                if (*generation != current)
                    return;
                target->setPlainText(result);
                state->setText(error.isEmpty()
                                   ? tr("Complete · the model exits after 60 seconds idle")
                                   : error);
            });
    };
    button(tr("Translate"), row, translate);
    button(tr("Copy original"), row,
           [source] { QApplication::clipboard()->setText(source->toPlainText()); });
    button(tr("Copy translation"), row,
           [target] { QApplication::clipboard()->setText(target->toPlainText()); });
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
    label(tr("C copies RGB and X copies HEX before or after selection. Esc cancels. Line width, "
             "colors and text size are available in the screenshot toolbar"),
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
    QTimer::singleShot(100, this, [this] { m_actions.capture(); });
}
void ToolboxController::captureActivity(bool active) {
    m_capture = active;
    m_hover.stop();
    if (active) {
        m_card->hide();
        m_detail->hide();
        m_leave.stop();
    } else if (enabled(QStringLiteral("card")))
        m_card->show();
}
void ToolboxController::clean(bool deep) {
    if (!enabled(QStringLiteral("cleanup")))
        return;
    OfflineTranslator::instance().releaseIdle();
    m_status->setText(tr("Cleaning memory…"));
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
                    .arg(m_quota.stale() ? QStringLiteral("#94a3b8") : QStringLiteral("#3976e9"))
                    .arg(window.remaining, 0, 'f', 0)
                    .arg(tr("Resets").toHtmlEscaped(), date);
        html += QStringLiteral("</td>");
    }
    if (!windows.isEmpty())
        html += QStringLiteral("</tr></table>");
    html += QStringLiteral("<p>%1<br>%2 %3</p>")
                .arg(m_quota.status().toHtmlEscaped(), tr("Updated:").toHtmlEscaped(),
                     m_quota.updatedAt().isValid()
                         ? m_quota.updatedAt().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                         : QStringLiteral("--"));
    return html;
}
QString ToolboxController::memoryHtml() const {
    if (!enabled(QStringLiteral("memory")))
        return tr("Memory information is disabled");
    const auto memory = m_memory.snapshot();
    if (!memory.total)
        return tr("Reading memory…");
    return tr("<b>Memory</b> · %1% used<br>Available %2 / %3 GiB<br>Toolbox and %4 processes: %5 "
              "MiB private working set · %6 MiB private commit")
        .arg(100.0 * (1 - double(memory.available) / double(memory.total)), 0, 'f', 0)
        .arg(memory.available / 1073741824.0, 0, 'f', 2)
        .arg(memory.total / 1073741824.0, 0, 'f', 2)
        .arg(memory.ownedProcesses)
        .arg(memory.privateWorking / 1048576.0, 0, 'f', 1)
        .arg(memory.committed / 1048576.0, 0, 'f', 1);
}
void ToolboxController::refreshUi() {
    if (!m_card)
        return;
    m_card->present(m_quota.windows(), m_quota.stale(), enabled(QStringLiteral("quota")),
                    QSettings(settingsPath(), QSettings::IniFormat)
                            .value(QStringLiteral("cardStyle"))
                            .toString() == QStringLiteral("stacked"),
                    tr("Disabled"));
    m_card->move(visiblePosition(m_card->pos(), m_card->size()));
    m_quotaLabel->setText(quotaHtml());
    m_memoryLabel->setText(memoryHtml());
    m_detailLabel->setText(quotaHtml() + QStringLiteral("<hr>") + memoryHtml());
    m_detailStatus->setText(m_status->text());
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
void ToolboxController::showDetail() {
    if (m_capture || !m_card->isVisible() || m_dragging)
        return;
    refreshUi();
    m_detail->adjustSize();
    const auto area = QGuiApplication::screenAt(m_card->geometry().center())->availableGeometry();
    QPoint position = m_card->pos() - QPoint(m_detail->width() + 6, 0);
    if (position.x() < area.left())
        position.setX(m_card->geometry().right() + 7);
    m_detail->move(visiblePosition(position, m_detail->size()));
    m_detail->show();
    m_leave.start();
}
bool ToolboxController::eventFilter(QObject* object, QEvent* event) {
    if (object == qApp && event->type() == QEvent::Quit)
        m_quitting = true;
    if (object == m_home && event->type() == QEvent::Close && !m_quitting) {
        m_home->hide();
        event->ignore();
        return true;
    }
    if (object != m_card)
        return QObject::eventFilter(object, event);
    if (event->type() == QEvent::Enter)
        m_hover.start();
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
        if (m_moved)
            m_card->move(m_dragPosition + delta);
    }
    if (event->type() == QEvent::MouseButtonRelease && m_dragging) {
        m_dragging = false;
        m_card->move(visiblePosition(m_card->pos(), m_card->size()));
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
