// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "locallibrary.h"
#include <QWidget>
#include <QTimer>
#include <QSet>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QStringListModel;
class QTabBar;

namespace toolbox {
class NoteEditor;
class ClipboardTracker final : public QObject {
    Q_OBJECT
  public:
    explicit ClipboardTracker(LocalLibrary* library, QObject* parent = nullptr);
    bool recording() const {
        return m_recording;
    }
    void setRecording(bool enabled);
    void setHistoryEnabled(bool enabled);
  signals:
    void stateChanged();
    void status(const QString& message);
    void captureDiagnostic(const QString& stage);

  private:
    LocalLibrary* m_library;
    QTimer m_capture;
    bool m_recording;
    bool m_historyEnabled = true;
    void capture();
};

class ClipboardWindow final : public QWidget {
    Q_OBJECT
  public:
    ClipboardWindow(
        LocalLibrary* library, ClipboardTracker* tracker, QWidget* parent = nullptr,
        std::function<bool()> prepareNotes = [] { return true; });
    void setNotesEnabled(bool enabled);
  signals:
    void pasteRequested(const QString& text, const QImage& image);

  private:
    LocalLibrary* m_library;
    ClipboardTracker* m_tracker;
    QLineEdit* m_search;
    QCheckBox* m_record;
    QListWidget* m_list;
    QLabel* m_status;
    QPushButton *m_more, *m_batch, *m_batchSave, *m_batchDelete;
    QSet<qint64> m_checked;
    bool m_batchMode = false;
    bool m_notesEnabled = true;
    int m_loaded = 0;
    std::function<bool()> m_prepareNotes;
    void refresh(bool more = false);
    void appendRow(const LibraryItem& item);
    void updateBatch();
    void applyBatch(bool remove);
    void capacity();
    void pasteItem(qint64 id);
    void saveAsNote(qint64 id);
    void deleteItem(qint64 id);
};

class NotesWindow final : public QWidget {
    Q_OBJECT
  public:
    explicit NotesWindow(LocalLibrary* library, QWidget* parent = nullptr);
    ~NotesWindow() override;
    bool selectNote(qint64 id);
    bool flush();
  signals:
    void pasteRequested(const QString& text, const QImage& image, const QString& html);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    LocalLibrary* m_library;
    QLineEdit *m_search, *m_title, *m_category;
    QTabBar* m_categories;
    QStringListModel* m_categoryModel;
    QListWidget *m_list, *m_attachments;
    NoteEditor* m_editor;
    QLabel* m_status;
    QPushButton *m_more, *m_batch, *m_batchSave, *m_batchDelete, *m_paste;
    QComboBox* m_batchCategory;
    QAction* m_export;
    QWidget* m_fields;
    QTimer m_save;
    QString m_categoryFilter;
    QString m_savedBody;
    qint64 m_id = 0;
    int m_loaded = 0;
    bool m_dirty = false, m_loading = false, m_saving = false, m_batchMode = false;
    void refresh(bool more = false);
    void appendRow(const LibraryItem& item);
    void load(qint64 id);
    void loadAttachments(const QStringList& attachments);
    void markDirty();
    void addImage(const QImage& image);
    void previewImage(const QString& hash);
    void pasteNote(qint64 id, bool copyOnly = false);
    void exportCurrent();
    void backup();
    void restore();
    void pasteCurrent();
    void deleteCurrent();
    void openRecycleBin();
    void updateBatch();
    void applyBatch(bool remove);
};

class LibraryWindow final : public QWidget {
    Q_OBJECT
  public:
    LibraryWindow(LocalLibrary* library, ClipboardTracker* tracker, QWidget* parent);
    void setFeatures(bool clipboard, bool notes);
    bool selectPage(bool notes);
    bool notesMode() const;
    NotesWindow* notes() const {
        return m_notes;
    }

  signals:
    void pasteRequested(const QString& text, const QImage& image, const QString& html = {});
    void canceled();

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    QTabBar* m_tabs;
    QStackedWidget* m_pages;
    ClipboardWindow* m_clipboard;
    NotesWindow* m_notes;
};
} // namespace toolbox
