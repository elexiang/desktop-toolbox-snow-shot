// SPDX-License-Identifier: GPL-3.0-or-later
#include "locallibrary.h"
#include <QCoreApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>

namespace toolbox {
namespace {
constexpr qint64 MiB = 1024 * 1024;
bool validHash(const QString& hash) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(hash).hasMatch();
}
QString hashBytes(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
bool insideDirectory(const QString& path, const QString& directory) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath())
        .startsWith(QDir::cleanPath(directory) + QLatin1Char('/'), Qt::CaseInsensitive);
}
QString searchPattern(QString text) {
    text.replace(QStringLiteral("!"), QStringLiteral("!!"));
    text.replace(QStringLiteral("%"), QStringLiteral("!%"));
    text.replace(QStringLiteral("_"), QStringLiteral("!_"));
    return QLatin1Char('%') + text + QLatin1Char('%');
}
bool atomicCopy(const QString& source, const QString& destination) {
    QFile input(source);
    QSaveFile output(destination);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        return false;
    while (!input.atEnd()) {
        const auto chunk = input.read(MiB);
        if (chunk.isEmpty() && input.error() != QFileDevice::NoError)
            return false;
        if (output.write(chunk) != chunk.size())
            return false;
    }
    return output.commit();
}
} // namespace

LocalLibrary::LocalLibrary(const QString& directory, QObject* parent)
    : QObject(parent), m_directory(QFileInfo(directory).absoluteFilePath()),
      m_connection(QUuid::createUuid().toString(QUuid::Id128)) {
    open();
}
LocalLibrary::~LocalLibrary() {
    close();
}
bool LocalLibrary::isOpen() const {
    return m_ready && m_db.isOpen();
}
bool LocalLibrary::fail(const QString& message) {
    m_error = message;
    return false;
}
void LocalLibrary::close() {
    m_ready = false;
    if (!m_db.isValid())
        return;
    m_db.close();
    m_db = {};
    QSqlDatabase::removeDatabase(m_connection);
}
bool LocalLibrary::execute(const QString& sql, const QVariantList& arguments) {
    QSqlQuery query(m_db);
    if (!query.prepare(sql))
        return fail(QStringLiteral("本地数据库操作失败：%1").arg(query.lastError().text()));
    for (const auto& arg : arguments)
        query.addBindValue(arg);
    if (!query.exec())
        return fail(QStringLiteral("本地数据库操作失败：%1").arg(query.lastError().text()));
    return true;
}
bool LocalLibrary::open() {
    if (!QDir().mkpath(m_directory + QStringLiteral("/images")))
        return fail(QStringLiteral("无法创建本地资料目录"));
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    m_db.setDatabaseName(m_directory + QStringLiteral("/library.sqlite"));
    m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=3000"));
    if (!m_db.open())
        return fail(QStringLiteral("本地资料库打开失败：%1").arg(m_db.lastError().text()));
    if (!execute(QStringLiteral("PRAGMA foreign_keys=ON")) ||
        !execute(QStringLiteral("PRAGMA trusted_schema=OFF")) ||
        !execute(QStringLiteral("PRAGMA journal_mode=WAL")))
        return false;
    QSqlQuery version(QStringLiteral("PRAGMA user_version"), m_db);
    if (!version.next() || version.value(0).toInt() > 2)
        return fail(QStringLiteral("资料库版本较新，请使用对应版本的工具箱"));
    const auto schemaVersion = version.value(0).toInt();
    const QStringList schema{
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS clips (id INTEGER PRIMARY KEY, kind TEXT NOT NULL "
            "CHECK(kind IN ('text','image')), text TEXT NOT NULL DEFAULT '', image TEXT NOT "
            "NULL DEFAULT '', digest TEXT NOT NULL UNIQUE, bytes INTEGER NOT NULL, favorite "
            "INTEGER NOT NULL DEFAULT 0, updated INTEGER NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS clip_order ON clips(updated DESC,id DESC)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, title TEXT NOT "
            "NULL, text TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '', favorite "
            "INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0, updated INTEGER "
            "NOT NULL)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS note_order ON notes(deleted,favorite DESC,updated DESC)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS attachments (note_id INTEGER NOT NULL REFERENCES "
            "notes(id) ON DELETE CASCADE, hash TEXT NOT NULL, PRIMARY KEY(note_id,hash))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS settings (id INTEGER PRIMARY KEY CHECK(id=1), "
                       "texts INTEGER NOT NULL, images INTEGER NOT NULL, bytes INTEGER NOT NULL)"),
        QStringLiteral("INSERT OR IGNORE INTO settings VALUES(1,1000,100,268435456)")};
    for (const auto& statement : schema)
        if (!execute(statement))
            return false;
    QSqlQuery clock(QStringLiteral("SELECT MAX(updated) FROM (SELECT updated FROM clips UNION ALL "
                                   "SELECT updated FROM notes)"),
                    m_db);
    if (clock.next())
        m_clock = clock.value(0).toLongLong();
    clock.finish();
    version.finish();
    if ((schemaVersion == 1 && !migrateFavorites()) ||
        (schemaVersion < 2 && !execute(QStringLiteral("PRAGMA user_version=2"))))
        return false;
    m_ready = true;
    return true;
}
qint64 LocalLibrary::timestamp() {
    m_clock = std::max(m_clock + 1, QDateTime::currentMSecsSinceEpoch());
    return m_clock;
}
LibraryLimits LocalLibrary::limits() const {
    QSqlQuery query(QStringLiteral("SELECT texts,images,bytes FROM settings WHERE id=1"), m_db);
    if (query.next())
        return {query.value(0).toInt(), query.value(1).toInt(), query.value(2).toLongLong()};
    return {};
}
bool LocalLibrary::setLimits(LibraryLimits limits) {
    if (limits.texts < 1 || limits.texts > 100000 || limits.images < 1 || limits.images > 10000 ||
        limits.bytes < MiB || limits.bytes > 4096 * MiB)
        return fail(QStringLiteral("容量设置超出允许范围"));
    if (!m_db.transaction())
        return fail(QCoreApplication::translate("ToolboxLibrary",
                                                "Could not start the library transaction"));
    if (!execute(QStringLiteral("UPDATE settings SET texts=?,images=?,bytes=? WHERE id=1"),
                 {limits.texts, limits.images, limits.bytes}) ||
        !prune() || !m_db.commit()) {
        m_db.rollback();
        return fail(QStringLiteral("容量设置保存失败"));
    }
    collectImages();
    emit changed();
    return true;
}
QString LocalLibrary::imagePath(const QString& hash) const {
    return validHash(hash)
               ? m_directory + QStringLiteral("/images/") + hash + QStringLiteral(".png")
               : QString();
}
QImage LocalLibrary::loadImage(const QString& hash) const {
    const auto path = imagePath(hash);
    if (path.isEmpty())
        return {};
    QImageReader reader(path);
    const auto size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > 40000000)
        return {};
    return reader.read();
}
QString LocalLibrary::storeImage(const QImage& image) {
    if (image.isNull() || qint64(image.width()) * image.height() > 40000000) {
        fail(QStringLiteral("图片为空或超过 4000 万像素"));
        return {};
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG") || png.size() > 32 * MiB) {
        fail(QStringLiteral("图片编码失败或 PNG 超过 32 MiB"));
        return {};
    }
    const auto hash = hashBytes(png);
    const auto path = imagePath(hash);
    if (!QFile::exists(path)) {
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(png) != png.size() ||
            !output.commit()) {
            fail(QStringLiteral("图片保存失败"));
            return {};
        }
    }
    return hash;
}
bool LocalLibrary::recordText(const QString& text) {
    const auto bytes = text.toUtf8().size();
    if (text.isEmpty() || bytes > MiB)
        return fail(QStringLiteral("已跳过空文本或超过 1 MiB 的文本"));
    return record(QStringLiteral("text"), text, QString(), bytes);
}
bool LocalLibrary::recordImage(const QImage& image) {
    const auto hash = storeImage(image);
    if (hash.isEmpty())
        return false;
    const bool result =
        record(QStringLiteral("image"), QString(), hash, QFileInfo(imagePath(hash)).size());
    if (!result)
        collectImages();
    return result;
}
bool LocalLibrary::record(const QString& kind, const QString& text, const QString& hash,
                          qint64 bytes) {
    const auto digest = hashBytes(kind.toUtf8() + '\0' +
                                  (kind == QStringLiteral("text") ? text.toUtf8() : hash.toUtf8()));
    if (!m_db.transaction())
        return fail(QStringLiteral("无法保存剪贴板记录"));
    if (!execute(QStringLiteral(
                     "INSERT INTO clips(kind,text,image,digest,bytes,updated) VALUES(?,?,?,?,?,?) "
                     "ON CONFLICT(digest) DO UPDATE SET updated=excluded.updated"),
                 {kind, text.isNull() ? QStringLiteral("") : text,
                  hash.isNull() ? QStringLiteral("") : hash, digest, bytes, timestamp()}) ||
        !prune() || !m_db.commit()) {
        m_db.rollback();
        return fail(QStringLiteral("剪贴板记录保存失败：%1").arg(m_error));
    }
    collectImages();
    emit changed();
    QSqlQuery retained(m_db);
    retained.prepare(QStringLiteral("SELECT 1 FROM clips WHERE digest=?"));
    retained.addBindValue(digest);
    if (!retained.exec() || !retained.next())
        return fail(
            QStringLiteral("容量已被收藏记录占满，本条内容未保留；可调整容量或取消部分收藏"));
    return true;
}
bool LocalLibrary::prune() {
    const auto cap = limits();
    for (const auto& kind : {QStringLiteral("text"), QStringLiteral("image")}) {
        QSqlQuery count(m_db);
        count.prepare(QStringLiteral("SELECT COUNT(*) FROM clips WHERE kind=?"));
        count.addBindValue(kind);
        if (!count.exec() || !count.next())
            return fail(QStringLiteral("剪贴板容量检查失败"));
        const int excess =
            count.value(0).toInt() - (kind == QStringLiteral("text") ? cap.texts : cap.images);
        if (excess > 0 &&
            !execute(QStringLiteral("DELETE FROM clips WHERE id IN (SELECT id FROM clips "
                                    "WHERE kind=? AND favorite=0 ORDER BY updated,id LIMIT ?)"),
                     {kind, excess}))
            return false;
    }
    QSqlQuery total(QStringLiteral("SELECT COALESCE(SUM(bytes),0) FROM clips"), m_db);
    if (!total.next())
        return fail(QStringLiteral("剪贴板容量检查失败"));
    qint64 bytes = total.value(0).toLongLong();
    QSqlQuery candidates(
        QStringLiteral("SELECT id,bytes FROM clips WHERE favorite=0 ORDER BY updated,id"), m_db);
    QList<qint64> remove;
    while (bytes > cap.bytes && candidates.next()) {
        remove.append(candidates.value(0).toLongLong());
        bytes -= candidates.value(1).toLongLong();
    }
    candidates.finish();
    total.finish();
    for (const auto id : remove)
        if (!execute(QStringLiteral("DELETE FROM clips WHERE id=?"), {id}))
            return false;
    return true;
}
void LocalLibrary::collectImages() {
    // An unsuccessful query must never be interpreted as an empty reference set.
    QSqlQuery refs(m_db);
    if (!refs.exec(QStringLiteral(
            "SELECT image FROM clips WHERE kind='image' UNION SELECT hash FROM attachments")))
        return;
    QSet<QString> used;
    while (refs.next())
        used.insert(refs.value(0).toString() + QStringLiteral(".png"));
    if (refs.lastError().isValid())
        return;
    QDir images(m_directory + QStringLiteral("/images"));
    for (const auto& name : images.entryList({QStringLiteral("*.png")}, QDir::Files))
        if (validHash(QFileInfo(name).completeBaseName()) && !used.contains(name))
            QFile::remove(images.filePath(name));
}
QList<LibraryItem> LocalLibrary::queryItems(bool notes, const QString& filter,
                                            const QVariantList& args, bool fullText) {
    QSqlQuery query(m_db);
    const auto textColumn =
        fullText ? QStringLiteral("text") : QStringLiteral("substr(text,1,160)");
    query.prepare(
        (notes ? QStringLiteral(
                     "SELECT id,'note',title,%1,tags,'',favorite,deleted,updated FROM notes ")
               : QStringLiteral("SELECT id,kind,'',%1,'',image,favorite,0,updated FROM clips "))
            .arg(textColumn) +
        filter);
    for (const auto& arg : args)
        query.addBindValue(arg);
    QList<LibraryItem> result;
    if (!query.exec()) {
        fail(QStringLiteral("资料读取失败：%1").arg(query.lastError().text()));
        return result;
    }
    while (query.next())
        result.append(
            {query.value(0).toLongLong(), query.value(1).toString(), query.value(2).toString(),
             query.value(3).toString(), query.value(4).toString(), query.value(5).toString(),
             query.value(6).toBool(), query.value(7).toBool(), query.value(8).toLongLong()});
    return result;
}
QList<LibraryItem> LocalLibrary::clipboard(const QString& search, bool favorites, int offset,
                                           int count) {
    return queryItems(
        false,
        QStringLiteral("WHERE text LIKE ? ESCAPE '!' AND (?=0 OR favorite=1) "
                       "ORDER BY favorite DESC,updated DESC,id DESC LIMIT ? OFFSET ?"),
        {searchPattern(search), favorites, std::clamp(count, 1, 100), std::max(0, offset)});
}
LibraryItem LocalLibrary::clipboardItem(qint64 id) {
    const auto found = queryItems(false, QStringLiteral("WHERE id=?"), {id}, true);
    return found.isEmpty() ? LibraryItem{} : found.first();
}
bool LocalLibrary::favoriteClip(qint64 id, bool favorite) {
    if (!execute(QStringLiteral("UPDATE clips SET favorite=? WHERE id=?"), {favorite, id}))
        return false;
    emit changed();
    return true;
}
bool LocalLibrary::deleteClip(qint64 id) {
    return deleteClips({id});
}
bool LocalLibrary::deleteClips(const QList<qint64>& ids) {
    const bool deleted = commitBatch(ids, [this](qint64 id) {
        return execute(QStringLiteral("DELETE FROM clips WHERE id=?"), {id});
    });
    if (deleted)
        collectImages();
    return deleted;
}
bool LocalLibrary::clearClipboard(bool includePinned) {
    if (!execute(QStringLiteral("DELETE FROM clips WHERE ? OR favorite=0"), {includePinned}))
        return false;
    // Wipe deleted text from freed SQLite pages.
    execute(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
    execute(QStringLiteral("VACUUM"));
    collectImages();
    emit changed();
    return true;
}
QList<LibraryItem> LocalLibrary::notes(const QString& search, bool trash, int offset, int count,
                                       const QString& category) {
    const auto pattern = searchPattern(search);
    return queryItems(
        true,
        QStringLiteral("WHERE deleted=? AND (title LIKE ? ESCAPE '!' OR text LIKE ? "
                       "ESCAPE '!' OR tags LIKE ? ESCAPE '!') AND (?='' OR tags=?) ORDER BY "
                       "%1updated DESC,id DESC LIMIT ? OFFSET ?")
            .arg(trash ? QString() : QStringLiteral("favorite DESC,")),
        {trash, pattern, pattern, pattern, category.isNull() ? QStringLiteral("") : category,
         category, std::clamp(count, 1, 100), std::max(0, offset)});
}
QStringList LocalLibrary::categories() {
    QStringList names;
    QSqlQuery query(QStringLiteral("SELECT DISTINCT tags FROM notes WHERE deleted=0 AND tags<>''"),
                    m_db);
    while (query.next())
        names.append(query.value(0).toString());
    std::sort(names.begin(), names.end(), [](const QString& left, const QString& right) {
        const auto order = left.localeAwareCompare(right);
        return order == 0 ? left < right : order < 0;
    });
    return names;
}
LibraryItem LocalLibrary::note(qint64 id) {
    const auto found = queryItems(true, QStringLiteral("WHERE id=?"), {id}, true);
    return found.isEmpty() ? LibraryItem{} : found.first();
}
QString LibraryItem::displayTitle() const {
    if (!title.isEmpty())
        return title;
    const auto firstLine = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    return firstLine.isEmpty() ? QCoreApplication::translate("ToolboxLibrary", "Untitled note")
                               : firstLine.left(256);
}
qint64 LocalLibrary::saveNote(qint64 id, const QString& title, const QString& text,
                              const QString& tags, bool pinned) {
    if (title.size() > 256 || tags.size() > 512 || text.toUtf8().size() > 2 * MiB) {
        fail(QCoreApplication::translate("ToolboxLibrary",
                                         "Note limits are 256 characters for the title, 512 for "
                                         "the category and 2 MiB for the body"));
        return 0;
    }
    const auto actualTitle = title.trimmed().isEmpty() ? QStringLiteral("") : title.trimmed();
    const auto actualText = text.isNull() ? QStringLiteral("") : text;
    const auto actualTags = tags.isNull() ? QStringLiteral("") : tags;
    if (id) {
        if (!note(id).id || note(id).deleted) {
            fail(QStringLiteral("笔记不存在或已进入回收站"));
            return 0;
        }
        if (!execute(QStringLiteral(
                         "UPDATE notes SET title=?,text=?,tags=?,favorite=?,updated=? WHERE id=?"),
                     {actualTitle, actualText, actualTags, pinned, timestamp(), id}))
            return 0;
    } else {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "INSERT INTO notes(title,text,tags,favorite,updated) VALUES(?,?,?,?,?)"));
        query.addBindValue(actualTitle);
        query.addBindValue(actualText);
        query.addBindValue(actualTags);
        query.addBindValue(pinned);
        query.addBindValue(timestamp());
        if (!query.exec()) {
            fail(QStringLiteral("笔记保存失败：%1").arg(query.lastError().text()));
            return 0;
        }
        id = query.lastInsertId().toLongLong();
    }
    emit changed();
    return id;
}
bool LocalLibrary::favoriteNote(qint64 id, bool favorite) {
    if (!execute(QStringLiteral("UPDATE notes SET favorite=? WHERE id=? AND deleted=0"),
                 {favorite, id}))
        return false;
    emit changed();
    return true;
}
bool LocalLibrary::trashNote(qint64 id, bool deleted) {
    return trashNotes({id}, deleted);
}
bool LocalLibrary::trashNotes(const QList<qint64>& ids, bool deleted) {
    return commitBatch(ids, [this, deleted](qint64 id) {
        return execute(
            QStringLiteral("UPDATE notes SET deleted=?,updated=? WHERE id=? AND deleted<>?"),
            {deleted, timestamp(), id, deleted});
    });
}
bool LocalLibrary::categorizeNotes(const QList<qint64>& ids, const QString& category) {
    const auto name = category.isNull() ? QStringLiteral("") : category;
    if (name.size() > 512)
        return fail(QCoreApplication::translate("ToolboxLibrary",
                                                "Categories can contain at most 512 characters"));
    return commitBatch(ids, [this, &name](qint64 id) {
        return execute(QStringLiteral("UPDATE notes SET tags=?,updated=? WHERE id=? AND deleted=0"),
                       {name, timestamp(), id});
    });
}
bool LocalLibrary::removeNote(qint64 id) {
    return removeNotes({id});
}
bool LocalLibrary::removeNotes(const QList<qint64>& ids) {
    const bool removed = commitBatch(ids, [this](qint64 id) {
        return execute(QStringLiteral("DELETE FROM notes WHERE id=? AND deleted=1"), {id});
    });
    if (removed)
        collectImages();
    return removed;
}
QStringList LocalLibrary::attachments(qint64 id) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT hash FROM attachments WHERE note_id=? ORDER BY hash"));
    query.addBindValue(id);
    QStringList hashes;
    if (query.exec())
        while (query.next())
            hashes.append(query.value(0).toString());
    return hashes;
}
QString LocalLibrary::attachImage(qint64 id, const QImage& image) {
    if (!note(id).id || note(id).deleted) {
        fail(QStringLiteral("请先创建并保存笔记"));
        return {};
    }
    const auto hash = storeImage(image);
    if (hash.isEmpty())
        return {};
    if (!execute(QStringLiteral("INSERT OR IGNORE INTO attachments(note_id,hash) VALUES(?,?)"),
                 {id, hash})) {
        collectImages();
        return {};
    }
    emit changed();
    return hash;
}
qint64 LocalLibrary::clipToNote(qint64 clipId, qint64 appendTo) {
    qint64 noteId = 0;
    if (!commitBatch({clipId}, [this, appendTo, &noteId](qint64 id) {
            noteId = writeClipNote(id, appendTo);
            return noteId != 0;
        }))
        return 0;
    return noteId;
}
bool LocalLibrary::clipsToNotes(const QList<qint64>& ids) {
    return commitBatch(ids, [this](qint64 id) { return writeClipNote(id) != 0; });
}
bool LocalLibrary::commitBatch(const QList<qint64>& ids,
                               const std::function<bool(qint64)>& operation) {
    if (ids.isEmpty())
        return true;
    if (!m_db.transaction())
        return fail(QCoreApplication::translate("ToolboxLibrary",
                                                "Could not start the library transaction"));
    QSignalBlocker blocker(this);
    for (const auto id : ids) {
        if (!operation(id)) {
            m_db.rollback();
            return false;
        }
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return fail(QCoreApplication::translate("ToolboxLibrary",
                                                "Could not commit the library transaction"));
    }
    blocker.unblock();
    emit changed();
    return true;
}
bool LocalLibrary::migrateFavorites() {
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT id FROM clips WHERE favorite<>0 ORDER BY updated,id")))
        return fail(
            QCoreApplication::translate("ToolboxLibrary", "Could not read legacy favorites"));
    QList<qint64> ids;
    while (query.next())
        ids.append(query.value(0).toLongLong());
    query.finish();
    const QSignalBlocker blocker(this);
    return commitBatch(ids, [this](qint64 id) {
        // Recheck inside the transaction in case another instance already migrated this row.
        if (!clipboardItem(id).favorite)
            return true;
        return writeClipNote(id) &&
               execute(QStringLiteral("UPDATE clips SET favorite=0 WHERE id=?"), {id});
    });
}
qint64 LocalLibrary::writeClipNote(qint64 clipId, qint64 appendTo) {
    const auto clip = clipboardItem(clipId);
    const auto existing = appendTo ? note(appendTo) : LibraryItem{};
    if (!clip.id || (appendTo && (!existing.id || existing.deleted))) {
        fail(QStringLiteral("记录或目标笔记已不存在"));
        return 0;
    }
    const auto addition = clip.kind == QStringLiteral("image")
                              ? QStringLiteral("![剪贴板图片](images/%1.png)").arg(clip.image)
                              : clip.text;
    const auto title = appendTo ? existing.title
                       : clip.kind == QStringLiteral("image")
                           ? QCoreApplication::translate("ToolboxLibrary", "Clipboard image")
                           : QString();
    const auto id = saveNote(
        appendTo, title, appendTo ? existing.text + QStringLiteral("\n\n") + addition : addition,
        existing.tags, existing.favorite);
    if (!id ||
        (!clip.image.isEmpty() &&
         !execute(QStringLiteral("INSERT OR IGNORE INTO attachments(note_id,hash) VALUES(?,?)"),
                  {id, clip.image}))) {
        return 0;
    }
    return id;
}
bool LocalLibrary::exportNote(qint64 id, const QString& path) {
    if (insideDirectory(path, m_directory))
        return fail(QStringLiteral("请将笔记导出到资料库目录之外"));
    const auto item = note(id);
    if (!item.id)
        return fail(QStringLiteral("笔记不存在"));
    // Export the entire attachment set, preserving the relative Markdown image links.
    const auto parent = QFileInfo(path).absoluteDir();
    const auto hashes = attachments(id);
    if (!hashes.isEmpty() && !QDir().mkpath(parent.filePath(QStringLiteral("images"))))
        return fail(QStringLiteral("无法创建导出附件目录"));
    for (const auto& hash : hashes) {
        const auto destination = parent.filePath(QStringLiteral("images/%1.png").arg(hash));
        if (QFileInfo(destination).absoluteFilePath() !=
                QFileInfo(imagePath(hash)).absoluteFilePath() &&
            !atomicCopy(imagePath(hash), destination))
            return fail(QStringLiteral("笔记附件导出失败"));
    }
    QSaveFile output(path);
    const auto bytes = (item.displayTitle() + QStringLiteral("\n\n") + item.text).toUtf8();
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() ||
        !output.commit())
        return fail(QStringLiteral("笔记导出失败"));
    return true;
}

bool LocalLibrary::backup(const QString& path) {
    if (insideDirectory(path, m_directory))
        return fail(QStringLiteral("请将备份保存在资料库目录之外"));
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return fail(QStringLiteral("无法创建备份临时目录"));
    const auto snapshot = temporary.filePath(QStringLiteral("backup.sqlite"));
    if (!execute(QStringLiteral("VACUUM INTO ?"), {snapshot}))
        return false;
    const auto connection = QUuid::createUuid().toString(QUuid::Id128);
    bool success = false;
    {
        auto backup = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        backup.setDatabaseName(snapshot);
        if (backup.open()) {
            QSqlQuery refs(QStringLiteral("SELECT image FROM clips WHERE kind='image' UNION SELECT "
                                          "hash FROM attachments"),
                           backup);
            QStringList hashes;
            while (refs.next())
                hashes.append(refs.value(0).toString());
            refs.finish();
            QSqlQuery query(backup);
            success = backup.transaction() &&
                      query.exec(QStringLiteral(
                          "CREATE TABLE backup_images(hash TEXT PRIMARY KEY,data BLOB NOT NULL)"));
            for (const auto& hash : hashes) {
                QFile input(imagePath(hash));
                if (!success || !input.open(QIODevice::ReadOnly) || input.size() > 32 * MiB) {
                    success = false;
                    break;
                }
                const auto bytes = input.readAll();
                if (hashBytes(bytes) != hash) {
                    success = false;
                    break;
                }
                query.prepare(QStringLiteral("INSERT INTO backup_images VALUES(?,?)"));
                query.addBindValue(hash);
                query.addBindValue(bytes);
                success = query.exec();
            }
            if (success)
                success = backup.commit();
            else
                backup.rollback();
            query.finish();
            backup.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    if (success && QFileInfo(snapshot).size() > 4LL * 1024 * MiB)
        return fail(QStringLiteral("备份超过 4 GiB 上限，请先分批导出笔记"));
    if (!success || !atomicCopy(snapshot, path))
        return fail(QStringLiteral("备份失败，请检查剩余空间和图片文件是否完整"));
    return true;
}

bool LocalLibrary::restore(const QString& path) {
    // Validate and stage everything before closing or moving the current library.
    if (!QFileInfo(path).isFile() || QFileInfo(path).size() > 4LL * 1024 * MiB)
        return fail(QStringLiteral("备份文件不存在或超过 4 GiB"));
    QTemporaryDir staged(QFileInfo(m_directory).absolutePath() +
                         QStringLiteral("/.library-restore-XXXXXX"));
    if (!staged.isValid() || !atomicCopy(path, staged.filePath(QStringLiteral("library.sqlite"))))
        return fail(QStringLiteral("备份暂存失败"));
    const auto connection = QUuid::createUuid().toString(QUuid::Id128);
    bool valid = false;
    {
        auto backup = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        backup.setDatabaseName(staged.filePath(QStringLiteral("library.sqlite")));
        if (backup.open()) {
            QSqlQuery query(backup);
            valid = query.exec(QStringLiteral("PRAGMA trusted_schema=OFF")) &&
                    query.exec(QStringLiteral("PRAGMA quick_check")) && query.next() &&
                    query.value(0).toString() == QStringLiteral("ok");
            valid = valid && query.exec(QStringLiteral("PRAGMA user_version")) && query.next() &&
                    (query.value(0).toInt() == 1 || query.value(0).toInt() == 2);
            valid = valid &&
                    query.exec(QStringLiteral(
                        "SELECT COUNT(*) FROM sqlite_master WHERE type IN ('trigger','view')")) &&
                    query.next() && query.value(0).toInt() == 0;
            const QStringList checks{
                QStringLiteral(
                    "SELECT id,kind,text,image,digest,bytes,favorite,updated FROM clips LIMIT 0"),
                QStringLiteral(
                    "SELECT id,title,text,tags,favorite,deleted,updated FROM notes LIMIT 0"),
                QStringLiteral("SELECT note_id,hash FROM attachments LIMIT 0"),
                QStringLiteral("SELECT texts,images,bytes FROM settings WHERE id=1"),
                QStringLiteral("SELECT hash,data FROM backup_images LIMIT 0")};
            for (const auto& check : checks)
                valid = valid && query.exec(check);
            valid =
                valid && query.exec(QStringLiteral("PRAGMA foreign_key_check")) && !query.next();
            QSet<QString> needed;
            valid = valid && query.exec(QStringLiteral("SELECT image FROM clips WHERE kind='image' "
                                                       "UNION SELECT hash FROM attachments"));
            while (valid && query.next()) {
                const auto hash = query.value(0).toString();
                valid = validHash(hash);
                needed.insert(hash);
            }
            QDir().mkpath(staged.filePath(QStringLiteral("images")));
            for (const auto& hash : needed) {
                if (!valid)
                    break;
                query.prepare(QStringLiteral(
                    "SELECT data FROM backup_images WHERE hash=? AND length(data)<=?"));
                query.addBindValue(hash);
                query.addBindValue(32 * MiB);
                valid = query.exec() && query.next();
                if (!valid)
                    break;
                const auto bytes = query.value(0).toByteArray();
                QBuffer buffer;
                buffer.setData(bytes);
                buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer, "PNG");
                const auto size = reader.size();
                valid = hashBytes(bytes) == hash && size.isValid() &&
                        qint64(size.width()) * size.height() <= 40000000 && !reader.read().isNull();
                QSaveFile output(staged.filePath(QStringLiteral("images/%1.png").arg(hash)));
                valid = valid && output.open(QIODevice::WriteOnly) &&
                        output.write(bytes) == bytes.size() && output.commit();
            }
            query.finish();
            if (valid)
                valid = query.exec(QStringLiteral("DROP TABLE backup_images")) &&
                        query.exec(QStringLiteral("VACUUM"));
            query.finish();
            backup.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    if (!valid)
        return fail(QStringLiteral("备份校验失败，当前资料未修改"));
    const auto recovery =
        m_directory + QStringLiteral("-before-restore-") +
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    close();
    if (!QDir().rename(m_directory, recovery)) {
        open();
        return fail(QStringLiteral("无法保存恢复前的资料，请关闭其他工具箱实例后重试"));
    }
    if (!QDir().rename(staged.path(), m_directory)) {
        QDir().rename(recovery, m_directory);
        open();
        return fail(QStringLiteral("资料恢复失败，已保留原资料"));
    }
    staged.setAutoRemove(false);
    if (!open()) {
        close();
        // Preserve the failed staged data as well, then put the original back.
        QDir().rename(m_directory, staged.path());
        staged.setAutoRemove(true);
        QDir().rename(recovery, m_directory);
        open();
        return fail(QStringLiteral("恢复后的资料库无法打开，已回退原资料"));
    }
    m_recoveryDirectory = recovery;
    emit restored();
    emit changed();
    return true;
}
} // namespace toolbox
