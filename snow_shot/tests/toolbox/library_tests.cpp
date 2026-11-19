// SPDX-License-Identifier: GPL-3.0-or-later
#include "locallibrary.h"
#include "librarywindows.h"
#include "toolboxpaths.h"
#include <QtWidgets>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QElapsedTimer>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
void waitEvents(int milliseconds = 120) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}
bool waitUntil(const std::function<bool()>& condition, int timeout = 2500) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeout)
        waitEvents(10);
    return condition();
}
void mutateBackup(const QString& path, const QString& sql) {
    const QString name = QStringLiteral("backup-test");
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        require(db.open(), "Backup fixture must open");
        QSqlQuery query(db);
        require(query.exec(sql), "Backup mutation must succeed");
        query.finish();
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
}
void readOnly(toolbox::LocalLibrary& library, bool enabled) {
    for (const auto& name : QSqlDatabase::connectionNames()) {
        const auto db = QSqlDatabase::database(name);
        if (db.databaseName() != library.directory() + QStringLiteral("/library.sqlite"))
            continue;
        QSqlQuery query(db);
        require(query.exec(enabled ? QStringLiteral("PRAGMA query_only=ON")
                                   : QStringLiteral("PRAGMA query_only=OFF")),
                "Write failure fixture must configure");
        return;
    }
    require(false, "Library connection must exist");
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    QTemporaryDir temporary;
    require(temporary.isValid(), "Temporary directory must exist");
    qputenv("SNOW_TOOLBOX_DATA", temporary.path().toUtf8());
    const auto directory = temporary.filePath(QStringLiteral("library"));
    qint64 persistentNote = 0;
    QString imageHash;
    QImage image(64, 48, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#176fde")));
    {
        toolbox::LocalLibrary library(directory);
        std::cerr << "Checking storage and clipboard retention\n";
        require(library.isOpen(), qPrintable(library.error()));
        require(library.recordText(QStringLiteral("中文\n  原样保留 😀  ")),
                "Unicode text must persist");
        require(!library.clipboard().isEmpty(), "The first record must be queryable");
        const auto first = library.clipboard().first();
        require(library.recordText(QStringLiteral("another")), "Second text must save");
        require(library.recordText(QStringLiteral("中文\n  原样保留 😀  ")),
                "Duplicate text must update recency");
        require(library.clipboard().size() == 2 && library.clipboard().first().id == first.id,
                "Duplicate content must reuse the row and move to the top");
        require(library.clipboardItem(first.id).text == QStringLiteral("中文\n  原样保留 😀  "),
                "Text must preserve whitespace, newlines and Unicode");
        require(library.recordText(QStringLiteral("SQL_100%\\path")),
                "Literal search fixture must save");
        require(library.clipboard(QStringLiteral("100%")).size() == 1 &&
                    library.clipboard(QStringLiteral("_")).size() == 1,
                "Search wildcard characters must be treated literally");
        require(library.favoriteClip(first.id, true), "Favorite must save");
        std::cerr << "Checking count limits\n";
        require(library.setLimits({2, 2, 256LL * 1024 * 1024}), "Limits must save");
        require(library.recordText(QStringLiteral("newest")),
                "Text insertion must prune old records");
        require(library.clipboard().size() == 2 && library.clipboardItem(first.id).favorite,
                "Count eviction must preserve favorites");
        require(library.recordImage(image), "Image must persist");
        const auto clipImage = library.clipboard().at(1);
        imageHash = clipImage.image;
        require(library.recordImage(image) && library.clipboard().at(1).id == clipImage.id,
                "Identical images must be deduplicated");
        require(library.loadImage(imageHash) == image, "Stored PNG must preserve pixels");
        persistentNote = library.clipToNote(clipImage.id);
        std::cerr << "Checking notes\n";
        require(persistentNote && library.attachments(persistentNote).contains(imageHash),
                "Image conversion must save an independent attachment reference");
        require(library.clearClipboard(), "Clear clipboard must succeed");
        require(library.clipboard().size() == 1 && library.clipboard().first().favorite,
                "Clear must retain favorites");
        require(QFile::exists(library.imagePath(imageHash)) && library.note(persistentNote).id,
                "Clearing history must not delete notes or their image files");
        const auto textClip = library.clipboard().first().id;
        require(library.clipToNote(textClip, persistentNote) == persistentNote &&
                    library.note(persistentNote).text.contains(QStringLiteral("原样保留")),
                "Append must add exact clipboard text to an existing note");
        require(library.saveNote(persistentNote, QStringLiteral("排查笔记"),
                                 library.note(persistentNote).text, QStringLiteral("SQL、项目"),
                                 true) == persistentNote,
                "Title, tags and pin state must save");
        require(library.notes(QStringLiteral("项目")).size() == 1,
                "Notes must be searchable by tags");
        require(library.trashNote(persistentNote, true) && library.notes().isEmpty() &&
                    library.notes({}, true).size() == 1,
                "Trash must separate notes without removing attachments");
        require(library.saveNote(persistentNote, QStringLiteral("bad"), {}, {}, false) == 0,
                "Trashed notes must reject accidental updates");
        require(library.trashNote(persistentNote, false), "Trashed notes must restore");
        const auto exported = temporary.filePath(QStringLiteral("export/排查.md"));
        QDir().mkpath(QFileInfo(exported).absolutePath());
        require(library.exportNote(persistentNote, exported), "Markdown export must succeed");
        QFile exportedText(exported);
        require(exportedText.open(QIODevice::ReadOnly) &&
                    QString::fromUtf8(exportedText.readAll()).contains(QStringLiteral("排查笔记")),
                "Exported Markdown must preserve title and body");
        require(QImage(temporary.filePath(QStringLiteral("export/images/%1.png").arg(imageHash))) ==
                    image,
                "Export must include referenced images");
        const auto orphan = library.saveNote(0, QStringLiteral("附件回收测试"), {}, {}, false);
        QImage other(8, 8, QImage::Format_RGB32);
        other.fill(Qt::red);
        const auto otherHash = library.attachImage(orphan, other);
        require(!otherHash.isEmpty(), "Note image must save");
        require(library.trashNote(orphan, true) && QFile::exists(library.imagePath(otherHash)),
                "Trash must retain its images");
        require(library.removeNote(orphan) && !QFile::exists(library.imagePath(otherHash)),
                "Permanent delete must collect unreferenced images");
        const auto backup = temporary.filePath(QStringLiteral("all.sntbackup"));
        std::cerr << "Checking backup and restore\n";
        require(library.backup(backup), qPrintable(library.error()));
        const auto late = library.saveNote(0, QStringLiteral("恢复前新增"), {}, {}, false);
        require(late != 0, "Pre-restore note must save");
        require(library.restore(backup), qPrintable(library.error()));
        require(library.notes(QStringLiteral("恢复前新增")).isEmpty() &&
                    library.note(persistentNote).title == QStringLiteral("排查笔记") &&
                    library.loadImage(imageHash) == image,
                "Backup restore must reproduce notes and image attachments");
        require(QFile::exists(library.recoveryDirectory() + QStringLiteral("/library.sqlite")),
                "Restore must retain the previous library for recovery");
        const auto corrupt = temporary.filePath(QStringLiteral("bad.sntbackup"));
        require(QFile::copy(backup, corrupt), "Corrupt backup fixture must copy");
        mutateBackup(corrupt, QStringLiteral("UPDATE backup_images SET data=x'000102'"));
        require(!library.restore(corrupt) && library.note(persistentNote).id &&
                    library.loadImage(imageHash) == image,
                "Corrupt attachment backup must leave the current library unchanged");
        mutateBackup(corrupt, QStringLiteral("PRAGMA user_version=99"));
        require(!library.restore(corrupt) && library.note(persistentNote).id,
                "Unsupported backup versions must not alter current data");
        require(library.recordText(QString(2000, QLatin1Char('x'))),
                "Long clipboard text must save");
        const auto longClip = library.clipboard(QString(100, QLatin1Char('x'))).first();
        require(longClip.text.size() == 160 && library.clipboardItem(longClip.id).text.size() == 2000,
                "Lists must load summaries while detail access preserves full text");
    }
    toolbox::LocalLibrary library(directory);
    std::cerr << "Checking reopening and pagination\n";
    require(library.isOpen() && library.note(persistentNote).title == QStringLiteral("排查笔记") &&
                library.loadImage(imageHash) == image && library.limits().texts == 2,
            "Reopening must preserve notes, images and capacity settings");
    require(library.setLimits({200, 100, 256LL * 1024 * 1024}), "Pagination limits must save");
    for (int i = 0; i < 115; ++i)
        require(library.recordText(QStringLiteral("分页%1").arg(i)),
                "Pagination fixture must save");
    const auto page1 = library.clipboard(QStringLiteral("分页"), false, 0, 50);
    const auto page2 = library.clipboard(QStringLiteral("分页"), false, 50, 50);
    const auto page3 = library.clipboard(QStringLiteral("分页"), false, 100, 50);
    require(page1.size() == 50 && page2.size() == 50 && page3.size() == 15 &&
                page1.last().id != page2.first().id,
            "Pagination must not lose or duplicate rows");
    require(!library.recordText(QString(1024 * 1024 + 1, QLatin1Char('x'))),
            "Oversized clipboard text must be skipped");
    {
        toolbox::LocalLibrary capped(temporary.filePath(QStringLiteral("byte-cap")));
        require(capped.setLimits({1000, 100, 1024 * 1024}), "Byte capacity fixture must configure");
        require(capped.recordText(QString(700000, QLatin1Char('a'))), "Large text must fit");
        const auto favorite = capped.clipboard().first().id;
        require(capped.recordText(QString(700000, QLatin1Char('b'))) &&
                    !capped.clipboardItem(favorite).id,
                "Byte capacity must evict the oldest record");
        const auto protectedId = capped.clipboard().first().id;
        require(capped.favoriteClip(protectedId, true), "Large record favorite must save");
        require(!capped.recordText(QString(700000, QLatin1Char('c'))) &&
                    capped.clipboardItem(protectedId).favorite,
                "A full favorite budget must report a skipped new record and preserve favorites");
        require(!capped.backup(capped.directory() + QStringLiteral("/library.sqlite")),
                "Backup output must not overwrite the active library");
    }
    {
        const auto path = temporary.filePath(QStringLiteral("legacy-favorites"));
        {
            toolbox::LocalLibrary legacy(path);
            require(legacy.recordText(QStringLiteral("旧收藏原文")) &&
                        legacy.favoriteClip(legacy.clipboard().first().id, true) &&
                        legacy.recordImage(image) &&
                        legacy.favoriteClip(legacy.clipboard().at(1).id, true),
                    "Legacy fixture must create protected text and image records");
        }
        mutateBackup(path + QStringLiteral("/library.sqlite"), QStringLiteral("PRAGMA user_version=1"));
        for (int attempt = 0; attempt < 2; ++attempt) {
            toolbox::LocalLibrary migrated(path);
            const auto text = migrated.notes(QStringLiteral("旧收藏原文"));
            const auto pictures = migrated.notes(QCoreApplication::translate("ToolboxLibrary", "Clipboard image"));
            require(migrated.isOpen() && migrated.clipboard({}, true).isEmpty() &&
                        text.size() == 1 && pictures.size() == 1 &&
                        migrated.attachments(pictures.first().id) == QStringList{imageHash},
                    "Upgrade must migrate every favorite once and release its retention flag");
            require(migrated.clearClipboard() && migrated.loadImage(imageHash) == image,
                    "Migrated images must remain available after history is cleared");
        }
    }
    {
        const auto path = temporary.filePath(QStringLiteral("pinned-clips"));
        {
            toolbox::LocalLibrary pinned(path);
            require(pinned.recordText(QStringLiteral("pinned")) &&
                        pinned.favoriteClip(pinned.clipboard().first().id, true) &&
                        pinned.recordText(QStringLiteral("newer")), "Pin fixture must save");
        }
        toolbox::LocalLibrary pinned(path);
        require(pinned.clipboard().first().text == QStringLiteral("pinned") &&
                    pinned.clipboard().first().favorite && pinned.notes().isEmpty(),
                "New pins must survive reopening and sort above more recent clips");
        require(pinned.clearClipboard(true) && pinned.clipboard().isEmpty(),
                "Explicit full clear must include pinned clipboard entries");
    }
    toolbox::ClipboardTracker tracker(&library);
    QStringList captureStages;
    QObject::connect(&tracker, &toolbox::ClipboardTracker::captureDiagnostic, &app,
                     [&](const QString& stage) { captureStages.append(stage); });
    std::cerr << "Checking clipboard tracker\n";
    tracker.setRecording(false);
    QApplication::clipboard()->setText(QStringLiteral("暂停时不记录"));
    waitEvents();
    require(library.clipboard(QStringLiteral("暂停时不记录")).isEmpty(), "Pause must stop capture");
    tracker.setRecording(true);
    QApplication::clipboard()->setText(QStringLiteral("监视器记录中文"));
    require(
        waitUntil([&] { return library.clipboard(QStringLiteral("监视器记录中文")).size() == 1; }),
        "Clipboard dataChanged must persist text");
    QApplication::clipboard()->setImage(image);
    require(waitUntil([&] {
                const auto entries = library.clipboard();
                return std::any_of(entries.cbegin(), entries.cend(), [](const auto& entry) {
                    return entry.kind == QStringLiteral("image");
                });
            }),
            "Clipboard dataChanged must persist images");
    require(captureStages == QStringList{QStringLiteral("image.read"), QStringLiteral("image.stored")},
            "Clipboard tracker must report image read and storage without capturing payload data");
    auto* excluded = new QMimeData;
    excluded->setText(QStringLiteral("禁止历史记录"));
    excluded->setData(
        QStringLiteral(
            "application/x-qt-windows-mime;value=\"ExcludeClipboardContentFromMonitorProcessing\""),
        "1");
    QApplication::clipboard()->setMimeData(excluded);
    waitEvents();
    require(library.clipboard(QStringLiteral("禁止历史记录")).isEmpty(),
            "Private clipboard formats must not be recorded");
    tracker.setRecording(false);
    toolbox::ClipboardTracker secondTracker(&library);
    require(!secondTracker.recording(), "Recording preference must persist");
    toolbox::NotesWindow notes(&library);
    std::cerr << "Checking note editor\n";
    notes.setAttribute(Qt::WA_DeleteOnClose, false);
    notes.show();
    require(notes.selectNote(persistentNote), "Note window must open a stored note");
    auto* title = notes.findChild<QLineEdit*>(QStringLiteral("NoteTitle"));
    auto* editor = notes.findChild<QTextEdit*>(QStringLiteral("NoteBody"));
    auto* category = notes.findChild<QLineEdit*>(QStringLiteral("NoteCategory"));
    auto* categories = notes.findChild<QTabBar*>(QStringLiteral("NoteCategories"));
    require(editor->toPlainText().contains(QChar::ObjectReplacementCharacter) &&
                !editor->toPlainText().contains(QStringLiteral("images/")),
            "Existing image references must render as inline images");
    waitEvents(20);
    QPoint imagePoint;
    for (int y = 2; y < editor->viewport()->height() && imagePoint.isNull(); y += 4)
        for (int x = 2; x < editor->viewport()->width() && imagePoint.isNull(); x += 4)
            if (editor->anchorAt(QPoint(x, y)) == imageHash)
                imagePoint = QPoint(x, y);
    require(!imagePoint.isNull(), "The inline image must be clickable");
    bool previewOpened = false;
    QTimer::singleShot(0, &notes, [&] {
        auto* preview = notes.findChild<QDialog*>(QStringLiteral("NoteImagePreview"));
        require(preview && preview->isVisible(), "Clicking an inline image must open its preview");
        previewOpened = true;
        preview->accept();
    });
    const auto imageGlobal = editor->viewport()->mapToGlobal(imagePoint);
    QMouseEvent imagePress(QEvent::MouseButtonPress, imagePoint, imageGlobal,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent imageRelease(QEvent::MouseButtonRelease, imagePoint, imageGlobal,
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(editor->viewport(), &imagePress);
    QApplication::sendEvent(editor->viewport(), &imageRelease);
    require(previewOpened, "The image preview must open on a single click");
    const auto savedImageBody = library.note(persistentNote).text;
    title->setText(QStringLiteral("保留旧图片"));
    require(notes.flush() && library.note(persistentNote).text == savedImageBody,
            "Editing metadata must preserve the original image references");
    title->clear();
    editor->setPlainText(QStringLiteral("自动保存标题\n正文 😀"));
    category->setText(QStringLiteral("测试"));
    require(waitUntil([&] {
                return library.note(persistentNote).displayTitle() == QStringLiteral("自动保存标题") &&
                       library.note(persistentNote).text == editor->toPlainText() &&
                       library.note(persistentNote).tags == QStringLiteral("测试");
            }),
            "Idle autosave must persist the inferred title, body and category");
    const auto another = library.saveNote(0, QStringLiteral("其他"), {}, QStringLiteral("其他"), false);
    const auto testIndex = [&] {
        for (int i = 1; i < categories->count(); ++i)
            if (categories->tabData(i).toString() == QStringLiteral("测试"))
                return i;
        return 0;
    }();
    require(another && testIndex > 0, "New note categories must appear in the second-level tabs");
    categories->setCurrentIndex(testIndex);
    require(notes.findChild<QListWidget*>(QStringLiteral("NoteList"))->count() == 1,
            "Category switching must filter the existing note list");
    categories->setCurrentIndex(0);
    require(notes.selectNote(persistentNote), "The editor must still open the original note");
    title->setText(QStringLiteral("关闭前保存"));
    const auto lastSavedBody = library.note(persistentNote).text;
    QApplication::clipboard()->setText(QString(2 * 1024 * 1024 + 1, QLatin1Char('x')));
    editor->paste();
    require(editor->toPlainText() == lastSavedBody,
            "An oversized paste must be rejected before changing the note document");
    require(library.saveNote(persistentNote, QStringLiteral("过长正文"),
                             QString(2 * 1024 * 1024 + 1, QLatin1Char('x')), {}, false) == 0,
            "Oversized note bodies must be rejected without altering stored content");
    readOnly(library, true);
    editor->setPlainText(QStringLiteral("写入失败时保留的内容"));
    require(!notes.flush() && library.note(persistentNote).text == lastSavedBody,
            "Failed autosave must preserve the previous stored note");
    notes.close();
    require(notes.isVisible() && editor->toPlainText() == QStringLiteral("写入失败时保留的内容"),
            "A failed save must keep the note window open and retain unsaved content");
    readOnly(library, false);
    editor->setPlainText(lastSavedBody);
    notes.close();
    require(library.note(persistentNote).title == QStringLiteral("关闭前保存"),
            "Closing a note must flush pending edits");
    notes.show();
    title->setText(QStringLiteral("最终编辑"));
    toolbox::ClipboardWindow clips(&library, &tracker, nullptr, [&] { return notes.flush(); });
    std::cerr << "Checking clipboard window\n";
    clips.setAttribute(Qt::WA_DeleteOnClose, false);
    clips.show();
    waitEvents(20);
    auto* clipList = clips.findChild<QListWidget*>(QStringLiteral("ClipboardList"));
    auto* clipSearch = clips.findChild<QLineEdit*>(QStringLiteral("ClipboardSearch"));
    clipSearch->setText(QStringLiteral("__single_click_test__"));
    require(library.recordText(QStringLiteral("__single_click_test__\n保留格式")),
            "The click fixture must save clipboard text");
    int clipPastes = 0;
    QString chosenClip;
    QObject::connect(&clips, &toolbox::ClipboardWindow::pasteRequested, &app,
                     [&](const QString& text, const QImage&) { chosenClip = text; ++clipPastes; });
    require(clipList->count() == 1, "Clipboard search must filter rows");
    auto* firstRow = clipList->itemWidget(clipList->item(0));
    require(firstRow->findChildren<QToolButton*>().size() == 3,
            "Clipboard rows must expose exactly three icon actions");
    firstRow->findChild<QToolButton*>(QStringLiteral("ClipboardRowPin"))->click();
    require(library.clipboardItem(clipList->item(0)->data(Qt::UserRole).toLongLong()).favorite &&
                clipPastes == 0, "Pinning must not paste the row");
    waitEvents(20);
    const auto point = clipList->visualItemRect(clipList->item(0)).center();
    const auto sendClick = [&](Qt::MouseButton button) {
        auto* hit = clipList->viewport()->childAt(point);
        if (!hit)
            hit = clipList->viewport();
        const QPoint global = clipList->viewport()->mapToGlobal(point);
        const QPoint local = hit->mapFromGlobal(global);
        QMouseEvent press(QEvent::MouseButtonPress, local, global, button, button, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, button, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(hit, &press);
        QApplication::sendEvent(hit, &release);
    };
    sendClick(Qt::RightButton);
    require(clipPastes == 0, "Right click must never paste clipboard content");
    sendClick(Qt::LeftButton);
    require(clipPastes == 1 && chosenClip == QStringLiteral("__single_click_test__\n保留格式"),
            "A single click on the clipboard content must paste its unmodified text");
    require(library.note(persistentNote).title == QStringLiteral("关闭前保存"),
            "Editing an unrelated clipboard must leave unsaved note changes in the editor");
    QImage pasted(12, 8, QImage::Format_RGB32);
    pasted.fill(Qt::yellow);
    QApplication::clipboard()->setImage(pasted);
    editor->paste();
    require(library.note(persistentNote).title == QStringLiteral("最终编辑") &&
                library.attachments(persistentNote).size() == 2,
            "Pasting an image into the editor must preserve the title and store the attachment");
    int reuseRequests = 0;
    QString pastedText, pastedHtml;
    QImage pastedImage;
    QObject::connect(&notes, &toolbox::NotesWindow::pasteRequested, &app,
                     [&](const QString& text, const QImage& selectedImage, const QString& html) {
                         pastedText = text;
                         pastedImage = selectedImage;
                         pastedHtml = html;
                         ++reuseRequests;
                     });
    auto* pasteShortcut = notes.findChild<QShortcut*>();
    QMetaObject::invokeMethod(pasteShortcut, "activated");
    require(reuseRequests == 1 && pastedText.contains(QStringLiteral("正文 😀")) &&
                !pastedText.contains(QStringLiteral("images/")) &&
                pastedImage.pixelColor(0, 0) == Qt::yellow &&
                pastedHtml.contains(QStringLiteral("data:image/png;base64,")),
            "The note shortcut must paste plain text and embedded images without paths");
    require(library.recordImage(image), "The clipboard image fixture must save");
    qint64 pictureClip = 0;
    for (const auto& entry : library.clipboard())
        if (entry.image == imageHash)
            pictureClip = entry.id;
    const auto pictureNote = library.clipToNote(pictureClip);
    require(pictureNote != 0, "The clipboard image must be saved as a note");
    auto* noteList = notes.findChild<QListWidget*>(QStringLiteral("NoteList"));
    QPushButton* rowPaste = nullptr;
    for (int index = 0; index < noteList->count(); ++index) {
        auto* row = noteList->item(index);
        if (row->data(Qt::UserRole).toLongLong() == pictureNote) {
            rowPaste = noteList->itemWidget(row)->findChild<QPushButton*>(QStringLiteral("NoteRowPaste"));
            break;
        }
    }
    require(rowPaste && rowPaste->isVisible(), "Every note row must offer a visible paste button");
    rowPaste->click();
    require(reuseRequests == 2 && pastedText.isEmpty() && pastedHtml.isEmpty() &&
                pastedImage.pixelColor(0, 0) == image.pixelColor(0, 0),
            "The row button must target its own image note and paste only real image data");

    const auto firstNoteId = [&] { return noteList->item(0)->data(Qt::UserRole).toLongLong(); };
    const auto runPinAction = [&](const char* label) {
        QListWidgetItem* pictureRow = nullptr;
        for (int index = 0; index < noteList->count(); ++index) {
            auto* row = noteList->item(index);
            if (row->data(Qt::UserRole).toLongLong() == pictureNote)
                pictureRow = row;
        }
        require(pictureRow, "The picture note must remain in the list");
        noteList->scrollToItem(pictureRow);
        const auto point = noteList->visualItemRect(pictureRow).center();
        bool triggered = false;
        QTimer::singleShot(0, &notes, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            require(menu, "A note row must open its context menu");
            for (auto* action : menu->actions()) {
                if (action->text() == QCoreApplication::translate("ToolboxLibrary", label)) {
                    action->trigger();
                    triggered = true;
                    break;
                }
            }
            menu->close();
        });
        require(QMetaObject::invokeMethod(noteList, "customContextMenuRequested", point) && triggered,
                "The note context menu must expose the requested pin action");
    };
    const auto beforePin = library.note(pictureNote);
    require(firstNoteId() == persistentNote && !beforePin.favorite,
            "Pinned notes must appear before newer unpinned notes");
    runPinAction("Pin to top");
    require(firstNoteId() == pictureNote && library.note(pictureNote).favorite &&
                library.note(pictureNote).text == beforePin.text &&
                library.note(pictureNote).updated == beforePin.updated,
            "Pinning a note must save its pin and reorder the list while preserving the note");
    runPinAction("Unpin");
    require(firstNoteId() == persistentNote && !library.note(pictureNote).favorite &&
                reuseRequests == 2,
            "Unpinning must restore normal sorting without triggering paste");

    clips.findChild<QPushButton*>(QStringLiteral("ClipboardBatch"))->click();
    clipList->itemWidget(clipList->item(0))->findChild<QCheckBox*>()->click();
    clips.findChild<QPushButton*>(QStringLiteral("ClipboardBatchSave"))->click();
    require(library.notes(QStringLiteral("__single_click_test__")).size() == 1 &&
                clipPastes == 1, "Clipboard batch save must not paste the checked item");
    clipList->itemWidget(clipList->item(0))->findChild<QCheckBox*>()->click();
    clips.findChild<QPushButton*>(QStringLiteral("ClipboardBatchDelete"))->click();
    require(clipList->count() == 0, "Clipboard batch delete must remove the checked row");
    clips.findChild<QPushButton*>(QStringLiteral("ClipboardBatch"))->click();
    clipSearch->clear();

    const auto keep = library.saveNote(0, QStringLiteral("回收站保留"), {}, {}, false);
    const auto purge = library.saveNote(0, QStringLiteral("回收站删除"), {}, {}, false);
    notes.findChild<QPushButton*>(QStringLiteral("NoteBatch"))->click();
    for (int index = 0; index < noteList->count(); ++index) {
        auto* row = noteList->item(index);
        const auto id = row->data(Qt::UserRole).toLongLong();
        if (id == keep || id == purge)
            row->setCheckState(Qt::Checked);
    }
    notes.findChild<QComboBox*>(QStringLiteral("NoteBatchCategory"))->setEditText(QStringLiteral("待处理"));
    notes.findChild<QPushButton*>(QStringLiteral("NoteBatchSave"))->click();
    require(library.note(keep).tags == QStringLiteral("待处理") &&
                library.note(purge).tags == QStringLiteral("待处理"),
            "Note batch category must apply to checked notes");
    for (int index = 0; index < noteList->count(); ++index) {
        auto* row = noteList->item(index);
        const auto id = row->data(Qt::UserRole).toLongLong();
        if (id == keep || id == purge)
            row->setCheckState(Qt::Checked);
    }
    notes.findChild<QPushButton*>(QStringLiteral("NoteBatchDelete"))->click();
    require(library.note(keep).deleted && library.note(purge).deleted && !notes.selectNote(keep),
            "Deleted notes must disappear from the normal editor and remain recoverable");
    auto* recycleEntry = notes.findChild<QPushButton*>(QStringLiteral("NoteRecycleBin"));
    QTimer::singleShot(0, &notes, [&] {
        auto* dialog = notes.findChild<QDialog*>(QStringLiteral("NoteRecycleBinDialog"));
        require(dialog && dialog->isVisible(), "The recycle bin must open separately");
        auto* deletedList = dialog->findChild<QListWidget*>(QStringLiteral("NoteRecycleBinList"));
        for (int index = 0; index < deletedList->count(); ++index) {
            auto* row = deletedList->item(index);
            if (row->data(Qt::UserRole).toLongLong() == keep)
                row->setCheckState(Qt::Checked);
        }
        dialog->findChild<QPushButton*>(QStringLiteral("NoteRecycleBinRestore"))->click();
        require(!library.note(keep).deleted && library.note(purge).deleted,
                "Restoring checked notes must not affect other deleted notes");
        dialog->findChild<QCheckBox*>(QStringLiteral("NoteRecycleBinSelectAll"))->click();
        const auto count = library.notes({}, true, 0, 100).size();
        QTimer::singleShot(0, dialog, [&] {
            auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            require(confirmation && confirmation->defaultButton() == confirmation->button(QMessageBox::No),
                    "Permanent deletion must default to cancellation");
            confirmation->button(QMessageBox::Yes)->click();
        });
        dialog->findChild<QPushButton*>(QStringLiteral("NoteRecycleBinDelete"))->click();
        require(count > 0 && deletedList->count() == 0 && !library.note(purge).id &&
                    library.note(keep).id, "Select all and purge must delete only recycled notes");
        dialog->accept();
    });
    recycleEntry->click();

    auto* options = clips.findChild<QToolButton*>(QStringLiteral("ClipboardOptions"))->menu();
    const auto actions = options->actions();
    require(actions.size() == 2, "Clipboard options must contain capacity and clear actions");
    for (const auto answer : {QMessageBox::No, QMessageBox::Yes}) {
        QTimer::singleShot(0, &clips, [answer] {
            auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            require(confirmation && confirmation->defaultButton() == confirmation->button(QMessageBox::No),
                    "Clearing the history must ask for confirmation and default to No");
            confirmation->button(answer)->click();
        });
        actions.last()->trigger();
        require(library.clipboard().isEmpty() == (answer == QMessageBox::Yes) && library.note(keep).id,
                "Clearing must respect the answer and preserve existing notes");
    }
    if (app.arguments().contains(QStringLiteral("--render"))) {
        const auto output = app.arguments().last();
        QDir().mkpath(output);
        require(clips.grab().save(output + QStringLiteral("/clipboard.png")),
                "Clipboard preview must render");
        require(notes.grab().save(output + QStringLiteral("/notes.png")),
                "Notes preview must render");
    }
    std::cout << "Library persistence, deduplication, eviction, attachments, backups, restore, "
                 "tracker and UI checks passed\n";
    return 0;
}
