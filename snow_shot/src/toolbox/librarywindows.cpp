// SPDX-License-Identifier: GPL-3.0-or-later
#include "librarywindows.h"
#include "librarycategories.h"
#include "libraryclipboard.h"
#include "toolboxpaths.h"
#include <QtWidgets>
#include <QBuffer>
#include <QSaveFile>
#include <QRegularExpression>
#include <QTextBlock>

namespace toolbox {
namespace {
const QRegularExpression& imageReference() {
    static const QRegularExpression pattern(
        QStringLiteral(R"(!\[(?:剪贴板图片|图片)\]\(images/([a-f0-9]{64})\.png\))"));
    return pattern;
}
constexpr int imageReferenceProperty = QTextFormat::UserProperty;
} // namespace

class NoteEditor final : public QTextEdit {
  public:
    NoteEditor() {
        setAcceptRichText(false);
    }

    std::function<void()> tooLarge;
    std::function<void(const QImage&)> pasteImage;
    std::function<void(const QString&)> previewImage;

    void setBody(const QString& body, const QSet<QString>& attachments, LocalLibrary* library) {
        setPlainText({});
        setUndoRedoEnabled(false);
        auto cursor = textCursor();
        qsizetype offset = 0;
        auto matches = imageReference().globalMatch(body);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const auto hash = match.captured(1);
            if (!attachments.contains(hash))
                continue;
            cursor.insertText(body.mid(offset, match.capturedStart() - offset), QTextCharFormat());
            insertImage(cursor, match.captured(), hash, library->loadImage(hash));
            offset = match.capturedEnd();
        }
        cursor.insertText(body.mid(offset), QTextCharFormat());
        moveCursor(QTextCursor::Start);
        setCurrentCharFormat(QTextCharFormat());
        setUndoRedoEnabled(true);
    }

    QString body() const {
        return serializedRange(0, document()->characterCount() - 1);
    }

    bool fitsReplacement(const QString& replacement) const {
        const auto cursor = textCursor();
        const auto remaining =
            serializedRange(0, cursor.selectionStart()) +
            serializedRange(cursor.selectionEnd(), document()->characterCount() - 1);
        return (remaining + replacement).toUtf8().size() <= 2 * 1024 * 1024;
    }

    void insertAttachment(const QString& hash, const QImage& image) {
        auto cursor = textCursor();
        cursor.beginEditBlock();
        cursor.insertText(QStringLiteral("\n"), QTextCharFormat());
        insertImage(cursor, QStringLiteral("![图片](images/%1.png)").arg(hash), hash, image);
        cursor.insertText(QStringLiteral("\n"), QTextCharFormat());
        cursor.endEditBlock();
        setTextCursor(cursor);
    }

  protected:
    bool canInsertFromMimeData(const QMimeData* source) const override {
        return source && (source->hasImage() || source->hasText());
    }
    void insertFromMimeData(const QMimeData* source) override {
        if (source && source->hasImage()) {
            pasteImage(qvariant_cast<QImage>(source->imageData()));
            return;
        }
        if (!source || !source->hasText())
            return;
        if (!fitsReplacement(source->text())) {
            if (tooLarge)
                tooLarge();
            return;
        }
        auto cursor = textCursor();
        cursor.insertText(source->text(), QTextCharFormat());
        setTextCursor(cursor);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        const auto hash = anchorAt(event->position().toPoint());
        QTextEdit::mouseReleaseEvent(event);
        if (!textCursor().hasSelection()) {
            if (currentCharFormat().isImageFormat())
                setCurrentCharFormat(QTextCharFormat());
            if (event->button() == Qt::LeftButton && !hash.isEmpty())
                previewImage(hash);
        }
    }

  private:
    QString serializedRange(int start, int end) const {
        QString result;
        for (auto block = document()->findBlock(start); block.isValid() && block.position() < end;
             block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                const int first = std::max(start, fragment.position());
                const int last = std::min(end, fragment.position() + fragment.length());
                if (first >= last)
                    continue;
                auto text = fragment.text().mid(first - fragment.position(), last - first);
                const auto format = fragment.charFormat();
                if (format.isImageFormat())
                    text.replace(QChar::ObjectReplacementCharacter,
                                 format.stringProperty(imageReferenceProperty));
                result += text;
            }
            if (block.position() + block.length() <= end)
                result += QLatin1Char('\n');
        }
        return result;
    }

    void insertImage(QTextCursor& cursor, const QString& reference, const QString& hash,
                     const QImage& image) {
        const auto thumbnail =
            image.isNull()
                ? style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(32).toImage()
                : image.scaled(image.size().boundedTo(QSize(300, 180)), Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
        document()->addResource(QTextDocument::ImageResource, QUrl(hash), thumbnail);
        QTextImageFormat format;
        format.setName(hash);
        format.setAnchor(true);
        format.setAnchorHref(hash);
        format.setProperty(imageReferenceProperty, reference);
        format.setToolTip(QCoreApplication::translate("ToolboxLibrary", "Preview image"));
        cursor.insertImage(format);
    }
};

namespace {
QPushButton* action(const QString& text, const QString& name, QBoxLayout* row,
                    const std::function<void()>& handler) {
    auto* result = new QPushButton(text);
    result->setObjectName(name);
    row->addWidget(result);
    QObject::connect(result, &QPushButton::clicked, result, handler);
    return result;
}
void iconAction(QWidget* owner, QBoxLayout* row, const QString& name, const QIcon& icon,
                const QString& label, const std::function<void()>& handler) {
    auto* button = new QToolButton;
    button->setObjectName(name);
    button->setIcon(icon);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
    button->setToolTip(label);
    button->setAccessibleName(label);
    row->addWidget(button);
    QObject::connect(button, &QToolButton::clicked, owner, handler);
}
QString pinLabel(bool pinned) {
    return pinned ? QCoreApplication::translate("ToolboxLibrary", "Unpin")
                  : QCoreApplication::translate("ToolboxLibrary", "Pin to top");
}
QIcon pinIcon(bool pinned) {
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto color = pinned ? QColor(208, 140, 35) : QApplication::palette().color(QPalette::Text);
    painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (pinned)
        painter.setBrush(color);
    painter.drawPolygon(QPolygonF{{7, 3}, {13, 3}, {12, 8}, {15, 11}, {15, 12},
                                  {5, 12}, {5, 11}, {8, 8}});
    painter.drawLine(QPointF(10, 12), QPointF(10, 17));
    return QIcon(pixmap);
}
QMenu* moreMenu(QBoxLayout* row, const QString& name) {
    auto* button = new QToolButton;
    button->setText(QCoreApplication::translate("ToolboxLibrary", "More"));
    button->setObjectName(name);
    button->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(button);
    button->setMenu(menu);
    row->addWidget(button);
    return menu;
}
QString chooseSave(QWidget* parent, const QString& title, const QString& filter,
                   const QString& suffix, const QString& name) {
    QFileDialog dialog(parent, title);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(filter);
    dialog.setDefaultSuffix(suffix);
    dialog.selectFile(name);
    return dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()
               ? dialog.selectedFiles().first()
               : QString();
}
QString summary(const LibraryItem& item) {
    const auto text = item.kind == QStringLiteral("image")
                          ? QCoreApplication::translate("ToolboxLibrary", "[Image]")
                      : item.kind == QStringLiteral("note") ? item.displayTitle()
                                                            : item.text.left(120);
    return QString(text).replace(QLatin1Char('\n'), QStringLiteral(" / ")).left(120) +
           QStringLiteral("\n") +
           QDateTime::fromMSecsSinceEpoch(item.updated).toString(QStringLiteral("MM-dd HH:mm")) +
           (item.tags.isEmpty() ? QString() : QStringLiteral(" · ") + item.tags);
}
qint64 itemId(const QListWidget* list) {
    return list->currentItem() ? list->currentItem()->data(Qt::UserRole).toLongLong() : 0;
}
void appendItems(QListWidget* list, const QList<LibraryItem>& entries) {
    for (const auto& entry : entries) {
        auto* item = new QListWidgetItem(summary(entry), list);
        item->setData(Qt::UserRole, entry.id);
        item->setSizeHint(QSize(200, 56));
    }
}
QList<qint64> checkedIds(const QListWidget* list) {
    QList<qint64> ids;
    for (int index = 0; index < list->count(); ++index) {
        const auto* item = list->item(index);
        if (item->checkState() == Qt::Checked)
            ids.append(item->data(Qt::UserRole).toLongLong());
    }
    return ids;
}
} // namespace

ClipboardTracker::ClipboardTracker(LocalLibrary* library, QObject* parent)
    : QObject(parent), m_library(library),
      m_recording(QSettings(settingsPath(), QSettings::IniFormat)
                      .value(QStringLiteral("clipboardRecording"), true)
                      .toBool()) {
    m_capture.setSingleShot(true);
    m_capture.setInterval(80);
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
        if (m_historyEnabled && m_recording && m_library->isOpen())
            m_capture.start();
    });
    connect(&m_capture, &QTimer::timeout, this, &ClipboardTracker::capture);
}
void ClipboardTracker::setRecording(bool enabled) {
    m_recording = enabled;
    m_capture.stop();
    QSettings(settingsPath(), QSettings::IniFormat)
        .setValue(QStringLiteral("clipboardRecording"), enabled);
    emit stateChanged();
}
void ClipboardTracker::setHistoryEnabled(bool enabled) {
    m_historyEnabled = enabled;
    if (!enabled)
        m_capture.stop();
}
void ClipboardTracker::capture() {
    if (!m_historyEnabled || !m_recording || !m_library->isOpen())
        return;
    const auto* mime = QApplication::clipboard()->mimeData();
    if (!mime || mime->hasUrls() ||
        mime->hasFormat(QStringLiteral(
            "application/"
            "x-qt-windows-mime;value=\"ExcludeClipboardContentFromMonitorProcessing\"")) ||
        mime->data(QStringLiteral("x-kde-passwordManagerHint")) == "secret")
        return;
    bool saved = false;
    if (mime->hasImage()) {
        emit captureDiagnostic(QStringLiteral("image.read"));
        const auto image = QApplication::clipboard()->image();
        if (image.isNull()) {
            emit captureDiagnostic(QStringLiteral("image.read_failed"));
            return;
        }
        saved = m_library->recordImage(image);
        emit captureDiagnostic(saved ? QStringLiteral("image.stored")
                                     : QStringLiteral("image.store_failed"));
    } else if (mime->hasText())
        saved = m_library->recordText(mime->text());
    else
        return;
    emit status(saved ? QCoreApplication::translate("ToolboxLibrary", "Clipboard history updated")
                      : m_library->error());
}

ClipboardWindow::ClipboardWindow(LocalLibrary* library, ClipboardTracker* tracker, QWidget* parent,
                                 std::function<bool()> prepareNotes)
    : QWidget(parent), m_library(library), m_tracker(tracker),
      m_prepareNotes(std::move(prepareNotes)) {
    setObjectName(QStringLiteral("ClipboardWindow"));
    setWindowTitle(QCoreApplication::translate("ToolboxLibrary", "Clipboard history"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto* filter = new QHBoxLayout;
    root->addLayout(filter);
    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("ClipboardSearch"));
    m_search->setPlaceholderText(
        QCoreApplication::translate("ToolboxLibrary", "Search clipboard text"));
    filter->addWidget(m_search, 1);
    auto* menu = moreMenu(filter, QStringLiteral("ClipboardOptions"));
    m_record = new QCheckBox(QCoreApplication::translate("ToolboxLibrary", "Record clipboard"));
    m_record->setObjectName(QStringLiteral("ClipboardRecording"));
    m_record->setChecked(tracker->recording());
    filter->insertWidget(1, m_record);
    menu->addAction(QCoreApplication::translate("ToolboxLibrary", "History limits…"), this,
                    &ClipboardWindow::capacity);
    menu->addAction(QCoreApplication::translate("ToolboxLibrary", "Clear history…"), this, [this] {
        if (QMessageBox::question(
                this, QCoreApplication::translate("ToolboxLibrary", "Clear clipboard history"),
                QCoreApplication::translate("ToolboxLibrary",
                                            "Clear clipboard history? Saved notes will be kept."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {
            const bool cleared = m_library->clearClipboard(true);
            m_status->setText(cleared
                                  ? QCoreApplication::translate("ToolboxLibrary", "History cleared")
                                  : m_library->error());
        }
    });
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("ClipboardList"));
    m_list->setUniformItemSizes(true);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(m_list, 1);
    auto* row = new QHBoxLayout;
    root->addLayout(row);
    m_batch = action(QCoreApplication::translate("ToolboxLibrary", "Batch actions"),
                     QStringLiteral("ClipboardBatch"), row, [this] {
                         m_batchMode = !m_batchMode;
                         m_checked.clear();
                         refresh();
                     });
    m_batchSave = action(QCoreApplication::translate("ToolboxLibrary", "Save as notes"),
                         QStringLiteral("ClipboardBatchSave"), row, [this] { applyBatch(false); });
    m_batchDelete = action(QCoreApplication::translate("ToolboxLibrary", "Delete selected"),
                           QStringLiteral("ClipboardBatchDelete"), row, [this] { applyBatch(true); });
    row->addStretch();
    m_more = action(QCoreApplication::translate("ToolboxLibrary", "Load more"),
                    QStringLiteral("ClipboardMore"), row, [this] { refresh(true); });
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        const auto* rowItem = m_list->itemAt(point);
        if (!rowItem)
            return;
        const auto id = rowItem->data(Qt::UserRole).toLongLong();
        const bool pinned = m_library->clipboardItem(id).favorite;
        QMenu contextMenu(this);
        if (m_notesEnabled)
            contextMenu.addAction(QCoreApplication::translate("ToolboxLibrary", "Save as notes"),
                                  this, [this, id] { saveAsNote(id); });
        contextMenu.addAction(pinLabel(pinned), this, [this, id, pinned] {
            if (!m_library->favoriteClip(id, !pinned))
                m_status->setText(m_library->error());
        });
        contextMenu.addAction(QCoreApplication::translate("ToolboxLibrary", "Delete"), this,
                              [this, id] { deleteItem(id); });
        contextMenu.exec(m_list->viewport()->mapToGlobal(point));
    });
    m_status = new QLabel(QCoreApplication::translate(
        "ToolboxLibrary", "Clipboard history is recorded while the toolbox is running"));
    m_status->setWordWrap(true);
    root->addWidget(m_status);
    connect(m_search, &QLineEdit::textChanged, this, [this] {
        m_loaded = 0;
        m_checked.clear();
        m_list->verticalScrollBar()->setValue(0);
        refresh();
    });
    connect(m_record, &QCheckBox::toggled, tracker, &ClipboardTracker::setRecording);
    connect(tracker, &ClipboardTracker::stateChanged, this, [this] {
        const QSignalBlocker blocker(m_record);
        m_record->setChecked(m_tracker->recording());
    });
    connect(tracker, &ClipboardTracker::status, m_status, &QLabel::setText);
    connect(m_library, &LocalLibrary::changed, this, [this] { refresh(); });
    connect(m_library, &LocalLibrary::restored, this, [this] { m_checked.clear(); });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const auto id = item->data(Qt::UserRole).toLongLong();
        if (m_batchMode) {
            auto* check = m_list->itemWidget(item)->findChild<QCheckBox*>();
            check->toggle();
            return;
        }
        pasteItem(id);
    });
    refresh();
    if (!library->isOpen())
        m_status->setText(library->error());
}
void ClipboardWindow::setNotesEnabled(bool enabled) {
    if (m_notesEnabled == enabled)
        return;
    m_notesEnabled = enabled;
    refresh();
}
void ClipboardWindow::refresh(bool more) {
    const auto scroll = m_list->verticalScrollBar()->value();
    const QSignalBlocker blocker(m_list);
    const int wanted = more ? 50 : std::max(50, ((m_loaded + 49) / 50) * 50);
    if (!more) {
        m_list->clear();
        m_loaded = 0;
    }
    QList<LibraryItem> entries;
    for (int remaining = wanted; remaining > 0; remaining -= 50) {
        const int count = std::min(remaining, 50);
        entries = m_library->clipboard(m_search->text(), false, m_loaded, count);
        for (const auto& entry : entries)
            appendRow(entry);
        m_loaded += static_cast<int>(entries.size());
        if (entries.size() < count)
            break;
    }
    QSet<qint64> visibleIds;
    for (int index = 0; index < m_list->count(); ++index)
        visibleIds.insert(m_list->item(index)->data(Qt::UserRole).toLongLong());
    m_checked.intersect(visibleIds);
    m_more->setVisible(entries.size() == 50);
    m_list->verticalScrollBar()->setValue(scroll);
    updateBatch();
}
void ClipboardWindow::appendRow(const LibraryItem& entry) {
    const auto id = entry.id;
    auto* item = new QListWidgetItem(m_list);
    const auto text = summary(entry);
    item->setData(Qt::UserRole, id);
    item->setData(Qt::AccessibleTextRole, text);
    item->setSizeHint(QSize(200, 48));
    auto* widget = new QWidget;
    auto* row = new QHBoxLayout(widget);
    row->setContentsMargins(8, 2, 6, 2);
    row->setSpacing(4);
    if (m_batchMode) {
        auto* check = new QCheckBox;
        check->setAccessibleName(text);
        check->setChecked(m_checked.contains(id));
        row->addWidget(check);
        connect(check, &QCheckBox::toggled, this, [this, id](bool checked) {
            if (checked)
                m_checked.insert(id);
            else
                m_checked.remove(id);
            updateBatch();
        });
    }
    auto* label = new QLabel((entry.favorite ? QStringLiteral("★  ") : QString()) + text);
    label->setTextFormat(Qt::PlainText);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(label, 1);
    if (!m_batchMode) {
        if (m_notesEnabled)
            iconAction(this, row, QStringLiteral("ClipboardRowSave"),
                       style()->standardIcon(QStyle::SP_DialogSaveButton),
                       QCoreApplication::translate("ToolboxLibrary", "Save as notes"),
                       [this, id] { saveAsNote(id); });
        iconAction(this, row, QStringLiteral("ClipboardRowPin"),
                   pinIcon(entry.favorite), pinLabel(entry.favorite),
                   [this, id, pinned = entry.favorite] {
                       if (!m_library->favoriteClip(id, !pinned))
                           m_status->setText(m_library->error());
                   });
        iconAction(this, row, QStringLiteral("ClipboardRowDelete"),
                   style()->standardIcon(QStyle::SP_TrashIcon),
                   QCoreApplication::translate("ToolboxLibrary", "Delete"),
                   [this, id] { deleteItem(id); });
    }
    m_list->setItemWidget(item, widget);
}
void ClipboardWindow::updateBatch() {
    m_batch->setText(m_batchMode ? QCoreApplication::translate("ToolboxLibrary", "Done")
                                 : QCoreApplication::translate("ToolboxLibrary", "Batch actions"));
    for (auto* button : {m_batchSave, m_batchDelete}) {
        button->setVisible(m_batchMode);
        button->setEnabled(!m_checked.isEmpty());
    }
    if (!m_notesEnabled)
        m_batchSave->hide();
}
void ClipboardWindow::applyBatch(bool remove) {
    QList<qint64> ids;
    for (int index = 0; index < m_list->count(); ++index) {
        const auto id = m_list->item(index)->data(Qt::UserRole).toLongLong();
        if (m_checked.contains(id))
            ids.append(id);
    }
    if (ids.isEmpty() || (!remove && (!m_notesEnabled || !m_prepareNotes())))
        return;
    const bool ok = remove ? m_library->deleteClips(ids) : m_library->clipsToNotes(ids);
    if (ok) {
        m_checked.clear();
        refresh();
    }
    m_status->setText(!ok ? m_library->error()
                         : remove ? QCoreApplication::translate("ToolboxLibrary", "Deleted %1").arg(ids.size())
                                  : QCoreApplication::translate("ToolboxLibrary", "Saved %1 notes").arg(ids.size()));
}
void ClipboardWindow::pasteItem(qint64 id) {
    const auto item = m_library->clipboardItem(id);
    if (!item.id)
        return;
    const auto image = item.image.isEmpty() ? QImage() : m_library->loadImage(item.image);
    if (!item.image.isEmpty() && image.isNull()) {
        m_status->setText(
            QCoreApplication::translate("ToolboxLibrary", "The image file is missing or damaged"));
        return;
    }
    emit pasteRequested(item.text, image);
}
void ClipboardWindow::saveAsNote(qint64 id) {
    if (!m_notesEnabled || !m_prepareNotes())
        return;
    const auto saved = m_library->clipToNote(id);
    m_status->setText(saved ? QCoreApplication::translate("ToolboxLibrary", "Saved as a note")
                            : m_library->error());
}
void ClipboardWindow::deleteItem(qint64 id) {
    const bool deleted = m_library->deleteClip(id);
    m_status->setText(deleted ? QCoreApplication::translate("ToolboxLibrary", "Deleted")
                              : m_library->error());
}
void ClipboardWindow::capacity() {
    QDialog dialog(this);
    dialog.setWindowTitle(
        QCoreApplication::translate("ToolboxLibrary", "Clipboard history limits"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    const auto limits = m_library->limits();
    auto* texts = new QSpinBox;
    texts->setRange(1, 100000);
    texts->setValue(limits.texts);
    auto* images = new QSpinBox;
    images->setRange(1, 10000);
    images->setValue(limits.images);
    auto* bytes = new QSpinBox;
    bytes->setRange(1, 4096);
    bytes->setValue(static_cast<int>(limits.bytes / (1024 * 1024)));
    form->addRow(QCoreApplication::translate("ToolboxLibrary", "Text items"), texts);
    form->addRow(QCoreApplication::translate("ToolboxLibrary", "Image items"), images);
    form->addRow(QCoreApplication::translate("ToolboxLibrary", "Total size (MiB)"), bytes);
    auto* hint = new QLabel(QCoreApplication::translate(
        "ToolboxLibrary", "Old clipboard items are removed at the limit. Saved notes are kept."));
    hint->setWordWrap(true);
    hint->setMaximumWidth(430);
    layout->addWidget(hint);
    auto* buttons = new QDialogButtonBox;
    buttons->addButton(QCoreApplication::translate("ToolboxLibrary", "Save and apply"),
                       QDialogButtonBox::AcceptRole);
    buttons->addButton(QCoreApplication::translate("ToolboxLibrary", "Cancel"),
                       QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        const bool saved = m_library->setLimits(
            {texts->value(), images->value(), qint64(bytes->value()) * 1024 * 1024});
        m_status->setText(
            saved ? QCoreApplication::translate("ToolboxLibrary", "History limits saved")
                  : m_library->error());
    }
}

NotesWindow::NotesWindow(LocalLibrary* library, QWidget* parent)
    : QWidget(parent), m_library(library) {
    setObjectName(QStringLiteral("NotesWindow"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    m_categories = new QTabBar;
    m_categories->setObjectName(QStringLiteral("NoteCategories"));
    m_categories->setExpanding(false);
    root->addWidget(m_categories);
    auto* top = new QHBoxLayout;
    root->addLayout(top);
    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("NoteSearch"));
    m_search->setPlaceholderText(QCoreApplication::translate("ToolboxLibrary", "Search notes"));
    top->addWidget(m_search, 1);
    auto* add = action(QStringLiteral("＋"), QStringLiteral("NoteNew"), top, [this] {
        if (!flush())
            return;
        m_search->clear();
        const auto id = m_library->saveNote(0, {}, {}, currentCategory(m_categories), false);
        if (id && selectNote(id))
            m_editor->setFocus();
        else
            m_status->setText(m_library->error());
    });
    add->setAccessibleName(QCoreApplication::translate("ToolboxLibrary", "New note"));
    add->setToolTip(add->accessibleName());
    auto* menu = moreMenu(top, QStringLiteral("NoteOptions"));
    m_export = menu->addAction(QCoreApplication::translate("ToolboxLibrary", "Export note…"), this,
                               &NotesWindow::exportCurrent);
    m_export->setObjectName(QStringLiteral("NoteExport"));
    menu->addSeparator();
    menu->addAction(QCoreApplication::translate("ToolboxLibrary", "Back up library…"), this,
                    &NotesWindow::backup);
    menu->addAction(QCoreApplication::translate("ToolboxLibrary", "Restore backup…"), this,
                    &NotesWindow::restore);
    auto* split = new QSplitter;
    root->addWidget(split, 1);
    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 6, 0);
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("NoteList"));
    m_list->setMinimumWidth(260);
    m_list->setUniformItemSizes(true);
    leftLayout->addWidget(m_list, 1);
    m_more = new QPushButton(QCoreApplication::translate("ToolboxLibrary", "Load more"));
    leftLayout->addWidget(m_more);
    split->addWidget(left);
    m_fields = new QWidget;
    auto* fields = new QVBoxLayout(m_fields);
    fields->setContentsMargins(6, 0, 0, 0);
    auto* metadata = new QHBoxLayout;
    fields->addLayout(metadata);
    m_title = new QLineEdit;
    m_title->setObjectName(QStringLiteral("NoteTitle"));
    m_title->setPlaceholderText(
        QCoreApplication::translate("ToolboxLibrary", "Title (defaults to the first line)"));
    m_title->setMaxLength(256);
    metadata->addWidget(m_title, 2);
    m_category = new QLineEdit;
    m_category->setObjectName(QStringLiteral("NoteCategory"));
    m_category->setPlaceholderText(
        QCoreApplication::translate("ToolboxLibrary", "Category (optional)"));
    m_category->setMaxLength(512);
    m_categoryModel = new QStringListModel(this);
    auto* completer = new QCompleter(m_categoryModel, m_category);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_category->setCompleter(completer);
    metadata->addWidget(m_category, 1);
    auto* noteEditor = new NoteEditor;
    noteEditor->tooLarge = [this] {
        m_status->setText(QCoreApplication::translate(
            "ToolboxLibrary", "Pasting would exceed 2 MiB. The note was left unchanged."));
    };
    noteEditor->pasteImage = [this](const QImage& image) { addImage(image); };
    noteEditor->previewImage = [this](const QString& hash) { previewImage(hash); };
    m_editor = noteEditor;
    m_editor->setObjectName(QStringLiteral("NoteBody"));
    m_editor->setPlaceholderText(QCoreApplication::translate(
        "ToolboxLibrary", "Type text or paste images. Changes are saved automatically."));
    fields->addWidget(m_editor, 1);
    auto* pasteShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), this);
    pasteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(pasteShortcut, &QShortcut::activated, this, &NotesWindow::pasteCurrent);
    m_attachments = new QListWidget;
    m_attachments->setObjectName(QStringLiteral("NoteAttachments"));
    m_attachments->setMaximumHeight(78);
    fields->addWidget(m_attachments);
    split->addWidget(m_fields);
    split->setSizes({300, 700});
    auto* bottom = new QHBoxLayout;
    root->addLayout(bottom);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("NoteStatus"));
    m_status->setWordWrap(true);
    bottom->addWidget(m_status, 1);
    m_paste = action(QCoreApplication::translate("ToolboxLibrary", "Paste body"),
                     QStringLiteral("NotePaste"), bottom, [this] { pasteCurrent(); });
    m_batch = action(QCoreApplication::translate("ToolboxLibrary", "Batch actions"),
                     QStringLiteral("NoteBatch"), bottom, [this] {
                         if (!flush())
                             return;
                         m_batchMode = !m_batchMode;
                         m_batchCategory->clearEditText();
                         refresh();
                     });
    m_batchCategory = new QComboBox;
    m_batchCategory->setObjectName(QStringLiteral("NoteBatchCategory"));
    m_batchCategory->setEditable(true);
    m_batchCategory->lineEdit()->setPlaceholderText(
        QCoreApplication::translate("ToolboxLibrary", "Category (optional)"));
    m_batchCategory->lineEdit()->setMaxLength(512);
    bottom->addWidget(m_batchCategory);
    m_batchSave = action(QCoreApplication::translate("ToolboxLibrary", "Change category"),
                         QStringLiteral("NoteBatchSave"), bottom, [this] { applyBatch(false); });
    m_batchDelete = action(QCoreApplication::translate("ToolboxLibrary", "Delete selected"),
                           QStringLiteral("NoteBatchDelete"), bottom, [this] { applyBatch(true); });
    action(QCoreApplication::translate("ToolboxLibrary", "Recycle bin"),
           QStringLiteral("NoteRecycleBin"), bottom, [this] { openRecycleBin(); });
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        const auto* item = m_list->itemAt(point);
        if (m_batchMode || !item)
            return;
        const auto id = item->data(Qt::UserRole).toLongLong();
        if (!selectNote(id))
            return;
        const bool pinned = m_library->note(id).favorite;
        QMenu contextMenu(this);
        contextMenu.addAction(QCoreApplication::translate("ToolboxLibrary", "Paste into the input field"),
                              this, &NotesWindow::pasteCurrent);
        contextMenu.addAction(QCoreApplication::translate("ToolboxLibrary", "Copy to clipboard"), this,
                              [this] { pasteNote(m_id, true); });
        contextMenu.addAction(pinIcon(pinned), pinLabel(pinned), this, [this, id, pinned] {
            if (!m_library->favoriteNote(id, !pinned))
                m_status->setText(m_library->error());
        });
        contextMenu.addAction(QCoreApplication::translate("ToolboxLibrary", "Delete"), this,
                              &NotesWindow::deleteCurrent);
        contextMenu.exec(m_list->viewport()->mapToGlobal(point));
    });
    connect(library, &LocalLibrary::restored, this, [this] {
        m_list->clear();
        load(0);
    });
    m_save.setSingleShot(true);
    m_save.setInterval(600);
    connect(&m_save, &QTimer::timeout, this, [this] { flush(); });
    connect(m_title, &QLineEdit::textChanged, this, &NotesWindow::markDirty);
    connect(m_category, &QLineEdit::textChanged, this, &NotesWindow::markDirty);
    connect(m_editor, &QTextEdit::textChanged, this, &NotesWindow::markDirty);
    connect(m_list, &QListWidget::currentRowChanged, this, [this] {
        const auto id = itemId(m_list);
        if (!m_batchMode && id && id != m_id)
            selectNote(id);
    });
    connect(m_list, &QListWidget::itemChanged, this, [this] { updateBatch(); });
    connect(m_search, &QLineEdit::textChanged, this, [this] {
        m_loaded = 0;
        refresh();
    });
    connect(m_categories, &QTabBar::currentChanged, this, [this] {
        const auto category = currentCategory(m_categories);
        if (!flush()) {
            const QSignalBlocker blocker(m_categories);
            m_categories->setCurrentIndex(m_library->categories().indexOf(m_categoryFilter) + 1);
            return;
        }
        m_categoryFilter = category;
        m_loaded = 0;
        load(0);
        refresh();
        if (m_list->count())
            m_list->setCurrentRow(0);
    });
    connect(m_more, &QPushButton::clicked, this, [this] { refresh(true); });
    connect(m_library, &LocalLibrary::changed, this, [this] {
        if (!m_saving)
            refresh();
    });
    connect(m_attachments, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        previewImage(item->data(Qt::UserRole).toString());
    });
    // Flush before QObject child teardown as well as on explicit window closing.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { flush(); });
    load(0);
    refresh();
    if (!library->isOpen())
        m_status->setText(library->error());
}
void NotesWindow::markDirty() {
    if (m_loading || !m_id)
        return;
    m_dirty = true;
    m_status->setText(QCoreApplication::translate("ToolboxLibrary", "Editing…"));
    m_save.start();
}
bool NotesWindow::flush() {
    m_save.stop();
    if (!m_dirty)
        return true;
    m_saving = true;
    const auto previous = m_library->note(m_id);
    const auto category =
        m_category->text() == previous.tags ? previous.tags : m_category->text().trimmed();
    const auto body = m_editor->body();
    const auto id = m_library->saveNote(m_id, m_title->text(), body, category, previous.favorite);
    m_saving = false;
    if (!id) {
        m_status->setText(m_library->error());
        return false;
    }
    m_savedBody = body;
    m_dirty = false;
    refresh();
    m_status->setText(QCoreApplication::translate("ToolboxLibrary", "Saved · %1")
                          .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))));
    return true;
}
void NotesWindow::load(qint64 id) {
    m_loading = true;
    m_save.stop();
    m_dirty = false;
    auto item = id ? m_library->note(id) : LibraryItem{};
    if (item.deleted)
        item = {};
    m_id = item.id;
    m_title->setText(item.title);
    m_category->setText(item.tags);
    const auto attachments = item.id ? m_library->attachments(item.id) : QStringList{};
    m_savedBody = item.text;
    m_editor->setBody(item.text, QSet<QString>(attachments.cbegin(), attachments.cend()),
                      m_library);
    m_fields->setEnabled(item.id && !m_batchMode);
    m_export->setEnabled(item.id && !m_batchMode);
    m_paste->setEnabled(item.id);
    loadAttachments(attachments);
    m_status->setText(item.id ? QCoreApplication::translate("ToolboxLibrary", "Saved")
                              : QCoreApplication::translate(
                                    "ToolboxLibrary", "Create a note or select one from the list"));
    m_loading = false;
}
void NotesWindow::loadAttachments(const QStringList& attachments) {
    m_attachments->clear();
    for (const auto& hash : attachments) {
        auto* attachment = new QListWidgetItem(
            QCoreApplication::translate("ToolboxLibrary",
                                        "Image attachment %1 (double-click to preview)")
                .arg(m_attachments->count() + 1),
            m_attachments);
        attachment->setData(Qt::UserRole, hash);
    }
    m_attachments->setVisible(m_attachments->count() > 0);
}
bool NotesWindow::selectNote(qint64 id) {
    if (!flush()) {
        refresh();
        return false;
    }
    const auto item = m_library->note(id);
    if (!item.id || item.deleted)
        return false;
    load(id);
    refresh();
    return true;
}
void NotesWindow::refresh(bool more) {
    const auto current = m_id;
    const auto categories = m_library->categories();
    if (m_batchCategory->property("categories").toStringList() != categories) {
        const auto typed = m_batchCategory->currentText();
        m_batchCategory->clear();
        m_batchCategory->addItems(categories);
        m_batchCategory->setEditText(typed);
        m_batchCategory->setProperty("categories", categories);
    }
    updateCategoryTabs(m_categories, categories);
    m_categoryFilter = currentCategory(m_categories);
    if (m_categoryModel->stringList() != categories)
        m_categoryModel->setStringList(categories);
    QSignalBlocker blocker(m_list);
    const auto checked = m_batchMode ? checkedIds(m_list) : QList<qint64>{};
    const int wanted = more ? 50 : std::max(50, ((m_loaded + 49) / 50) * 50);
    if (!more) {
        m_list->clear();
        m_loaded = 0;
    }
    QList<LibraryItem> entries;
    for (int remaining = wanted; remaining > 0; remaining -= 50) {
        const int count = std::min(remaining, 50);
        entries = m_library->notes(m_search->text(), false, m_loaded, count, m_categoryFilter);
        for (const auto& entry : entries)
            appendRow(entry);
        m_loaded += static_cast<int>(entries.size());
        if (entries.size() < count)
            break;
    }
    m_more->setVisible(entries.size() == 50);
    const QSet<qint64> checkedSet(checked.cbegin(), checked.cend());
    for (int i = 0; i < m_list->count(); ++i) {
        auto* row = m_list->item(i);
        const auto id = row->data(Qt::UserRole).toLongLong();
        if (m_batchMode)
            row->setCheckState(checkedSet.contains(id) ? Qt::Checked : Qt::Unchecked);
        if (id == current)
            m_list->setCurrentRow(i);
    }
    // External changes (e.g. appending from clipboard) refresh a clean editor only.
    if (current && !m_dirty && !m_saving) {
        const auto item = m_library->note(current);
        if (!item.id || item.deleted) {
            load(0);
        } else if (item.title != m_title->text() || item.text != m_savedBody ||
                   item.tags != m_category->text()) {
            load(current);
        } else {
            const auto attachments = m_library->attachments(current);
            if (attachments.size() != m_attachments->count())
                loadAttachments(attachments);
        }
    }
    updateBatch();
}
void NotesWindow::appendRow(const LibraryItem& entry) {
    if (m_batchMode) {
        appendItems(m_list, {entry});
        return;
    }
    auto* item = new QListWidgetItem(m_list);
    const auto text = summary(entry);
    item->setData(Qt::UserRole, entry.id);
    item->setData(Qt::AccessibleTextRole, text);
    item->setSizeHint(QSize(200, 56));
    auto* widget = new QWidget;
    auto* row = new QHBoxLayout(widget);
    row->setContentsMargins(8, 2, 6, 2);
    auto* label = new QLabel((entry.favorite ? QStringLiteral("★  ") : QString()) + text);
    label->setTextFormat(Qt::PlainText);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(label, 1);
    auto* paste =
        action(QCoreApplication::translate("ToolboxLibrary", "Paste"),
               QStringLiteral("NoteRowPaste"), row, [this, id = entry.id] { pasteNote(id); });
    paste->setToolTip(QCoreApplication::translate("ToolboxLibrary", "Paste into the input field"));
    paste->setAccessibleName(paste->toolTip());
    paste->setCursor(Qt::PointingHandCursor);
    paste->setStyleSheet(QStringLiteral(
        "QPushButton { background: #2759b5; color: white; font-weight: 600; "
        "border: 1px solid #2759b5; border-radius: 6px; padding: 4px 10px; }"
        "QPushButton:hover, QPushButton:focus { background: #1e468f; border-color: #102d64; }"
        "QPushButton:pressed { background: #17366f; }"));
    m_list->setItemWidget(item, widget);
}
void NotesWindow::addImage(const QImage& image) {
    if (!m_id || !flush())
        return;
    const auto link =
        QStringLiteral("\n![图片](images/%1.png)\n").arg(QString(64, QLatin1Char('0')));
    if (!m_editor->fitsReplacement(link)) {
        m_status->setText(QCoreApplication::translate(
            "ToolboxLibrary", "Pasting would exceed 2 MiB. The note was left unchanged."));
        return;
    }
    m_saving = true;
    const auto hash = m_library->attachImage(m_id, image);
    m_saving = false;
    if (hash.isEmpty()) {
        m_status->setText(m_library->error());
        return;
    }
    m_editor->insertAttachment(hash, image);
    flush();
}
void NotesWindow::previewImage(const QString& hash) {
    const auto image = m_library->loadImage(hash);
    if (image.isNull()) {
        m_status->setText(
            QCoreApplication::translate("ToolboxLibrary", "The image file is missing or damaged"));
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("NoteImagePreview"));
    dialog.setWindowTitle(QCoreApplication::translate("ToolboxLibrary", "Note image"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* preview = new QLabel;
    const auto limit = dialog.screen()->availableGeometry().size() * 0.8;
    preview->setPixmap(QPixmap::fromImage(image.scaled(
        image.size().boundedTo(limit), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    layout->addWidget(preview);
    dialog.exec();
}
void NotesWindow::deleteCurrent() {
    if (!m_id || !flush())
        return;
    const auto id = m_id;
    if (!m_library->trashNote(id, true)) {
        m_status->setText(m_library->error());
        return;
    }
    load(0);
    refresh();
    m_status->setText(QCoreApplication::translate("ToolboxLibrary", "Note deleted"));
}
void NotesWindow::updateBatch() {
    m_batch->setText(m_batchMode ? QCoreApplication::translate("ToolboxLibrary", "Done")
                                 : QCoreApplication::translate("ToolboxLibrary", "Batch actions"));
    m_fields->setEnabled(m_id && !m_batchMode);
    m_export->setEnabled(m_id && !m_batchMode);
    m_paste->setVisible(!m_batchMode);
    const bool checked = m_batchMode && !checkedIds(m_list).isEmpty();
    m_batchCategory->setVisible(m_batchMode);
    for (auto* button : {m_batchSave, m_batchDelete}) {
        button->setVisible(m_batchMode);
        button->setEnabled(checked);
    }
}
void NotesWindow::applyBatch(bool remove) {
    if (!flush())
        return;
    const auto ids = checkedIds(m_list);
    if (ids.isEmpty())
        return;
    const bool ok = remove ? m_library->trashNotes(ids, true)
                           : m_library->categorizeNotes(ids, m_batchCategory->currentText().trimmed());
    if (ok) {
        const QSignalBlocker blocker(m_list);
        for (int index = 0; index < m_list->count(); ++index)
            m_list->item(index)->setCheckState(Qt::Unchecked);
        updateBatch();
    }
    m_status->setText(!ok ? m_library->error()
                         : remove ? QCoreApplication::translate("ToolboxLibrary", "Deleted %1").arg(ids.size())
                                  : QCoreApplication::translate("ToolboxLibrary", "Changed the category of %1 notes").arg(ids.size()));
}
void NotesWindow::openRecycleBin() {
    if (!flush())
        return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("NoteRecycleBinDialog"));
    dialog.setWindowTitle(QCoreApplication::translate("ToolboxLibrary", "Recycle bin"));
    dialog.resize(620, 440);
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget;
    list->setObjectName(QStringLiteral("NoteRecycleBinList"));
    layout->addWidget(list, 1);
    auto* row = new QHBoxLayout;
    layout->addLayout(row);
    auto* selectAll = new QCheckBox(QCoreApplication::translate("ToolboxLibrary", "Select all"));
    selectAll->setObjectName(QStringLiteral("NoteRecycleBinSelectAll"));
    row->addWidget(selectAll);
    row->addStretch();

    const auto apply = [&](const QList<qint64>& ids, bool purge) {
        if (ids.isEmpty())
            return;
        if (purge && QMessageBox::question(
                         &dialog, QCoreApplication::translate("ToolboxLibrary", "Permanently delete note"),
                         QCoreApplication::translate("ToolboxLibrary",
                                                     "Permanently delete %1 notes? This cannot be undone.")
                             .arg(ids.size()),
                         QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        const bool ok = purge ? m_library->removeNotes(ids) : m_library->trashNotes(ids, false);
        if (!ok)
            QMessageBox::warning(&dialog, dialog.windowTitle(), m_library->error());
    };
    auto* restore = action(QCoreApplication::translate("ToolboxLibrary", "Restore selected"),
                           QStringLiteral("NoteRecycleBinRestore"), row,
                           [&] { apply(checkedIds(list), false); });
    auto* remove = action(QCoreApplication::translate("ToolboxLibrary", "Permanently delete selected…"),
                          QStringLiteral("NoteRecycleBinDelete"), row,
                          [&] { apply(checkedIds(list), true); });
    const auto update = [&] {
        const auto count = checkedIds(list).size();
        const QSignalBlocker blocker(selectAll);
        selectAll->setEnabled(list->count() > 0);
        selectAll->setChecked(count > 0 && count == list->count());
        restore->setEnabled(count > 0);
        remove->setEnabled(count > 0);
    };
    const auto refresh = [&] {
        const auto checked = checkedIds(list);
        const QSet<qint64> checkedSet(checked.cbegin(), checked.cend());
        const QSignalBlocker blocker(list);
        list->clear();
        for (int offset = 0;; offset += 100) {
            const auto entries = m_library->notes({}, true, offset, 100);
            appendItems(list, entries);
            if (entries.size() < 100)
                break;
        }
        for (int index = 0; index < list->count(); ++index) {
            auto* item = list->item(index);
            item->setCheckState(checkedSet.contains(item->data(Qt::UserRole).toLongLong())
                                    ? Qt::Checked : Qt::Unchecked);
        }
        update();
    };
    connect(selectAll, &QCheckBox::toggled, &dialog, [&](bool checked) {
        const QSignalBlocker blocker(list);
        for (int index = 0; index < list->count(); ++index)
            list->item(index)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        update();
    });
    connect(list, &QListWidget::itemChanged, &dialog, update);
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list, &QListWidget::customContextMenuRequested, &dialog, [&](const QPoint& point) {
        const auto* item = list->itemAt(point);
        if (!item)
            return;
        const auto id = item->data(Qt::UserRole).toLongLong();
        QMenu menu(&dialog);
        menu.addAction(QCoreApplication::translate("ToolboxLibrary", "Restore this note"), &dialog,
                       [&, id] { apply({id}, false); });
        menu.addAction(QCoreApplication::translate("ToolboxLibrary", "Permanently delete this note…"),
                       &dialog, [&, id] { apply({id}, true); });
        menu.exec(list->viewport()->mapToGlobal(point));
    });
    connect(m_library, &LocalLibrary::changed, &dialog, refresh);
    refresh();
    dialog.exec();
}
void NotesWindow::pasteCurrent() {
    pasteNote(m_id);
}
void NotesWindow::pasteNote(qint64 id, bool copyOnly) {
    QGuiApplication::inputMethod()->commit();
    if (!id || m_batchMode || !flush())
        return;
    const auto item = m_library->note(id);
    if (!item.id || item.deleted)
        return;
    const auto attachments = m_library->attachments(id);
    QString text;
    QList<QRegularExpressionMatch> images;
    qsizetype offset = 0;
    auto matches = imageReference().globalMatch(item.text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (!attachments.contains(match.captured(1)))
            continue;
        text += item.text.mid(offset, match.capturedStart() - offset);
        offset = match.capturedEnd();
        images.append(match);
    }
    text += item.text.mid(offset);
    if (!images.isEmpty() && text.trimmed().isEmpty())
        text.clear();
    if (images.isEmpty() && text.isEmpty()) {
        m_status->setText(
            QCoreApplication::translate("ToolboxLibrary", "The note has no body text"));
        return;
    }

    QImage firstImage;
    QString html;
    offset = 0;
    const bool richContent = images.size() > 1 || (!images.isEmpty() && !text.isEmpty());
    for (const auto& match : images) {
        auto image = m_library->loadImage(match.captured(1));
        if (image.isNull()) {
            m_status->setText(QCoreApplication::translate("ToolboxLibrary",
                                                          "The image file is missing or damaged"));
            return;
        }
        if (firstImage.isNull())
            firstImage = image;
        if (richContent) {
            QByteArray png;
            QBuffer buffer(&png);
            if (!image.save(&buffer, "PNG")) {
                m_status->setText(QCoreApplication::translate(
                    "ToolboxLibrary", "The image file is missing or damaged"));
                return;
            }
            html += item.text.mid(offset, match.capturedStart() - offset).toHtmlEscaped() +
                    QStringLiteral("<img src=\"data:image/png;base64,%1\">")
                        .arg(QString::fromLatin1(png.toBase64()));
            offset = match.capturedEnd();
        }
    }
    if (richContent) {
        html += item.text.mid(offset).toHtmlEscaped();
        html = QStringLiteral("<div style=\"white-space:pre-wrap\">%1</div>").arg(html);
    }
    if (copyOnly)
        setLibraryClipboard(text, firstImage, html);
    else
        emit pasteRequested(text, firstImage, html);
}
void NotesWindow::exportCurrent() {
    if (!flush() || !m_id)
        return;
    auto name = m_library->note(m_id).displayTitle();
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")), QStringLiteral("_"));
    const auto path =
        chooseSave(this, QCoreApplication::translate("ToolboxLibrary", "Export note"),
                   QCoreApplication::translate("ToolboxLibrary", "Markdown or text (*.md *.txt)"),
                   QStringLiteral("md"), name.left(80) + QStringLiteral(".md"));
    if (!path.isEmpty())
        m_status->setText(
            m_library->exportNote(m_id, path)
                ? QCoreApplication::translate("ToolboxLibrary", "Note and attachments exported")
                : m_library->error());
}
void NotesWindow::backup() {
    if (!flush())
        return;
    const auto path = chooseSave(
        this, QCoreApplication::translate("ToolboxLibrary", "Back up clipboard and notes"),
        QCoreApplication::translate("ToolboxLibrary", "Toolbox library backup (*.sntbackup)"),
        QStringLiteral("sntbackup"),
        QCoreApplication::translate("ToolboxLibrary", "Toolbox-library-%1.sntbackup")
            .arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd"))));
    if (!path.isEmpty())
        m_status->setText(
            m_library->backup(path)
                ? QCoreApplication::translate("ToolboxLibrary",
                                              "Clipboard history, notes and images backed up")
                : m_library->error());
}
void NotesWindow::restore() {
    if (!flush())
        return;
    const auto path = QFileDialog::getOpenFileName(
        this, QCoreApplication::translate("ToolboxLibrary", "Restore toolbox library"), {},
        QCoreApplication::translate("ToolboxLibrary", "Toolbox library backup (*.sntbackup)"));
    if (path.isEmpty())
        return;
    if (QMessageBox::question(
            this, QCoreApplication::translate("ToolboxLibrary", "Restore library"),
            QCoreApplication::translate("ToolboxLibrary",
                                        "Replace current clipboard history and notes with this "
                                        "backup? A copy of the current library will be kept."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    if (!m_library->restore(path)) {
        m_status->setText(m_library->error());
        return;
    }
    load(0);
    refresh();
    m_status->setText(QCoreApplication::translate("ToolboxLibrary",
                                                  "Library restored. Previous library saved in %1")
                          .arg(m_library->recoveryDirectory()));
}
void NotesWindow::closeEvent(QCloseEvent* event) {
    if (!flush()) {
        event->ignore();
        return;
    }
    QWidget::closeEvent(event);
}
NotesWindow::~NotesWindow() {
    flush();
}
LibraryWindow::LibraryWindow(LocalLibrary* library, ClipboardTracker* tracker, QWidget* parent)
    : QWidget(parent, Qt::Window) {
    setObjectName(QStringLiteral("LibraryWindow"));
    setWindowTitle(QCoreApplication::translate("ToolboxLibrary", "Clipboard and notes"));
    resize(780, 560);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 18);
    m_tabs = new QTabBar;
    m_tabs->setObjectName(QStringLiteral("LibraryViewMenu"));
    m_tabs->setExpanding(false);
    m_tabs->addTab(QCoreApplication::translate("ToolboxLibrary", "Clipboard"));
    m_tabs->addTab(QCoreApplication::translate("ToolboxLibrary", "Notes"));
    root->addWidget(m_tabs);
    m_pages = new QStackedWidget;
    root->addWidget(m_pages, 1);
    m_notes = new NotesWindow(library, m_pages);
    m_clipboard =
        new ClipboardWindow(library, tracker, m_pages, [this] { return m_notes->flush(); });
    m_pages->addWidget(m_clipboard);
    m_pages->addWidget(m_notes);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) { selectPage(index == 1); });
    connect(m_clipboard, &ClipboardWindow::pasteRequested, this,
            [this](const QString& text, const QImage& image) { emit pasteRequested(text, image); });
    connect(m_notes, &NotesWindow::pasteRequested, this, &LibraryWindow::pasteRequested);
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, [this] { close(); });
}
void LibraryWindow::setFeatures(bool clipboard, bool notes) {
    const QSignalBlocker blocker(m_tabs);
    m_tabs->setTabVisible(0, clipboard);
    m_tabs->setTabVisible(1, notes);
    m_clipboard->setNotesEnabled(notes);
    if (!clipboard && !notes)
        hide();
    else if (!m_tabs->isTabVisible(m_pages->currentIndex()))
        selectPage(notes);
    else
        m_tabs->setCurrentIndex(m_pages->currentIndex());
}
bool LibraryWindow::notesMode() const {
    return m_pages->currentIndex() == 1;
}
bool LibraryWindow::selectPage(bool notes) {
    if (!m_tabs->isTabVisible(notes ? 1 : 0))
        return false;
    const bool saved = notes || m_pages->currentIndex() != 1 || m_notes->flush();
    const int index = saved ? (notes ? 1 : 0) : m_pages->currentIndex();
    const QSignalBlocker blocker(m_tabs);
    m_tabs->setCurrentIndex(index);
    m_pages->setCurrentIndex(index);
    return saved;
}
void LibraryWindow::closeEvent(QCloseEvent* event) {
    if (!m_notes->flush()) {
        selectPage(true);
        event->ignore();
        return;
    }
    QWidget::closeEvent(event);
    emit canceled();
}
} // namespace toolbox
