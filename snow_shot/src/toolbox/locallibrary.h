// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QImage>
#include <QSqlDatabase>
#include <QVariant>
#include <functional>

namespace toolbox {
struct LibraryItem {
    qint64 id = 0;
    QString kind, title, text, tags, image;
    bool favorite = false, deleted = false;
    qint64 updated = 0;
    QString displayTitle() const;
};
struct LibraryLimits {
    int texts = 1000, images = 100;
    qint64 bytes = 256LL * 1024 * 1024;
};
class LocalLibrary final : public QObject {
    Q_OBJECT
  public:
    explicit LocalLibrary(const QString& directory, QObject* parent = nullptr);
    ~LocalLibrary() override;
    bool isOpen() const;
    QString error() const {
        return m_error;
    }
    QString directory() const {
        return m_directory;
    }
    LibraryLimits limits() const;
    bool setLimits(LibraryLimits limits);
    bool recordText(const QString& text);
    bool recordImage(const QImage& image);
    QList<LibraryItem> clipboard(const QString& search = {}, bool favorites = false, int offset = 0,
                                 int count = 100);
    LibraryItem clipboardItem(qint64 id);
    bool favoriteClip(qint64 id, bool favorite);
    bool deleteClip(qint64 id);
    bool deleteClips(const QList<qint64>& ids);
    bool clearClipboard(bool includePinned = false);
    QList<LibraryItem> notes(const QString& search = {}, bool trash = false, int offset = 0,
                             int count = 100, const QString& category = {});
    QStringList categories();
    LibraryItem note(qint64 id);
    qint64 saveNote(qint64 id, const QString& title, const QString& text, const QString& tags,
                    bool pinned);
    bool favoriteNote(qint64 id, bool favorite);
    bool trashNote(qint64 id, bool deleted);
    bool trashNotes(const QList<qint64>& ids, bool deleted);
    bool categorizeNotes(const QList<qint64>& ids, const QString& category);
    bool removeNote(qint64 id); // Only accepts already trashed notes.
    bool removeNotes(const QList<qint64>& ids);
    qint64 clipToNote(qint64 clip, qint64 appendTo = 0);
    bool clipsToNotes(const QList<qint64>& ids);
    QStringList attachments(qint64 note);
    QString attachImage(qint64 note, const QImage& image);
    QString imagePath(const QString& hash) const;
    QImage loadImage(const QString& hash) const;
    bool exportNote(qint64 id, const QString& path);
    bool backup(const QString& path);
    bool restore(const QString& path);
    QString recoveryDirectory() const {
        return m_recoveryDirectory;
    }

  signals:
    void changed();
    void restored();

  private:
    QString m_directory, m_connection, m_error, m_recoveryDirectory;
    QSqlDatabase m_db;
    qint64 m_clock = 0;
    bool m_ready = false;
    bool open();
    bool migrateFavorites();
    qint64 writeClipNote(qint64 clip, qint64 appendTo = 0);
    bool commitBatch(const QList<qint64>& ids, const std::function<bool(qint64)>& operation);
    void close();
    bool execute(const QString& sql, const QVariantList& arguments = {});
    bool fail(const QString& message);
    qint64 timestamp();
    QString storeImage(const QImage& image);
    bool record(const QString& kind, const QString& text, const QString& hash, qint64 bytes);
    bool prune();
    void collectImages();
    QList<LibraryItem> queryItems(bool note, const QString& filter, const QVariantList& args,
                                  bool fullText = false);
};
} // namespace toolbox
