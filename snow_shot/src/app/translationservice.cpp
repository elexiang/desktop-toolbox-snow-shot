#include "snow_shot/translation/translationservice.h"
#include "snow_shot/translation/fasttranslation.h"
#include "snow_shot/translation/freetranslation.h"

#include "snow_shot/translation/translationlanguages.h"
#include "snow_shot/storage/configurationstore.h"

#include <QTimer>
#include <algorithm>
#include <utility>
#include "../toolbox/offlinetranslator.h"

namespace snow_shot::translation {
namespace {
const QString sourceKey = QStringLiteral("screenshot_translation/source_language");
const QString targetKey = QStringLiteral("screenshot_translation/target_language");
const QString secondaryTargetKey =
    QStringLiteral("screenshot_translation/secondary_target_language");
const QString confidenceThresholdKey =
    QStringLiteral("screenshot_translation/language_detection_confidence_threshold");
const QString modelKey = QStringLiteral("screenshot_translation/model");
const QString customKey = QStringLiteral("api_configuration/custom_models");
const QString textKey = QStringLiteral("api_configuration/text_translation");
const QString proxyKey = QStringLiteral("network/proxy");
const QString httpProxyKey = QStringLiteral("network/http_proxy");
const QString serverKey = QStringLiteral("api_configuration/server_url");

qsizetype fastTranslationCut(const QString& text, qsizetype start) {
    auto end = std::min(text.size(), start + fastTranslationMaximumLength);
    if (end == text.size())
        return end;

    // Prefer nearby breaks while keeping most of the request available for translation context.
    const auto minimum = start + fastTranslationMaximumLength / 2;
    qsizetype paragraph = 0;
    qsizetype sentence = 0;
    qsizetype space = 0;
    for (auto index = end; index > minimum; --index) {
        const auto previous = text.at(index - 1);
        if (previous == u'\n') {
            paragraph = index;
            break;
        }
        if (sentence == 0 &&
            (previous == u'\u3002' || previous == u'\uff01' || previous == u'\uff1f' ||
             ((previous == u'.' || previous == u'!' || previous == u'?') &&
              text.at(index).isSpace())))
            sentence = index;
        if (space == 0 && text.at(index).isSpace())
            space = index;
    }
    if (paragraph || sentence || space) {
        end = paragraph ? paragraph : sentence ? sentence : space;
        while (end > minimum && text.at(end - 1).isSpace())
            --end;
    }
    if (text.at(end - 1).isHighSurrogate() && text.at(end).isLowSurrogate())
        --end;
    return end;
}
} // namespace

TranslationService& TranslationService::forClient(SnowShotApiClient& client,
                                                  storage::ConfigurationStore& settings,
                                                  const QLocale& locale,
                                                  TranslationLanguageDetector languageDetector) {
    for (auto* service :
         client.findChildren<TranslationService*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (service->m_settings == &settings) {
            return *service;
        }
    }
    return *new TranslationService(client, settings, locale, std::move(languageDetector));
}

TranslationService::TranslationService(SnowShotApiClient& client,
                                       storage::ConfigurationStore& settings, QLocale locale,
                                       TranslationLanguageDetector languageDetector)
    : QObject(&client), m_client(&client), m_settings(&settings), m_locale(std::move(locale)),
      m_defaultTarget(defaultTranslationTargetLanguage(m_locale)),
      m_languageDetector(languageDetector
                             ? std::move(languageDetector)
                             : TranslationLanguageDetector(detectTranslationLanguage)) {
    connect(&settings, &storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue&) {
                if (!m_saving && (key == sourceKey || key == targetKey ||
                                  key == secondaryTargetKey || key == modelKey))
                    syncPreferences(ChangeReason::UserPreferences);
            });
    syncPreferences(ChangeReason::Catalog);
    publishModels(false);
}

TranslationService::~TranslationService() {
    if (m_client != nullptr) {
        m_client->cancel(m_modelsToken);
        m_client->cancel(m_fastCatalogToken);
    }
}

void TranslationService::syncPreferences(ChangeReason reason) {
    if (m_settings == nullptr)
        return;
    TranslationPreferences next{
        m_settings->value(sourceKey).toString(), m_settings->value(targetKey).toString(),
        m_settings->value(modelKey).toString(), m_settings->value(secondaryTargetKey).toString(),
        m_settings->value(confidenceThresholdKey).toInt(60)};
    if (next.sourceLanguage.isEmpty())
        next.sourceLanguage = QStringLiteral("auto");
    if (next.targetLanguage.isEmpty())
        next.targetLanguage = m_defaultTarget;
    if (next.secondaryTargetLanguage.isEmpty())
        next.secondaryTargetLanguage = QStringLiteral("en");
    if (next != m_preferences) {
        m_preferences = next;
        emit preferencesChanged(reason);
    }
}

std::optional<bool> TranslationService::writePreferences(const QMap<QString, QJsonValue>& values,
                                                         ChangeReason reason) {
    if (m_settings == nullptr)
        return std::nullopt;
    const QPointer<TranslationService> lifetime(this);
    const quint64 generation = ++m_preferencesGeneration;
    const bool previousSaving = std::exchange(m_saving, true);
    const bool saved = m_settings->setValues(values);
    if (!lifetime)
        return std::nullopt;
    m_saving = previousSaving;
    if (generation != m_preferencesGeneration)
        return std::nullopt;
    m_storageError = !saved;
    syncPreferences(reason);
    if (!lifetime || generation != m_preferencesGeneration)
        return std::nullopt;
    return saved;
}

bool TranslationService::savePreferences(const TranslationPreferences& preferences) {
    const auto saved = writePreferences(
        {{sourceKey, preferences.sourceLanguage},
         {targetKey, preferences.targetLanguage},
         {secondaryTargetKey, preferences.secondaryTargetLanguage},
         {confidenceThresholdKey, preferences.languageDetectionConfidenceThreshold},
         {modelKey, preferences.modelId}},
        ChangeReason::UserPreferences);
    if (!saved.has_value())
        return false;
    const QPointer<TranslationService> lifetime(this);
    const quint64 generation = m_preferencesGeneration;
    emit catalogChanged();
    return lifetime && generation == m_preferencesGeneration && *saved;
}

void TranslationService::publishModels(bool resolveSelection) {
    static_cast<void>(resolveSelection);
    m_models = {{QStringLiteral("toolbox-local"), tr("Offline Chinese / English"), false,
                 QStringLiteral("default"), false}};
    m_preferences.modelId = QStringLiteral("toolbox-local");
    emit catalogChanged();
}

void TranslationService::refreshModels(bool force) {
    static_cast<void>(force);
    publishModels(false);
}

bool TranslationService::refreshFastCatalog(bool force) {
    static_cast<void>(force);
    return false;
}

void TranslationService::setLocale(const QLocale& locale) {
    if (locale == m_locale) {
        // LanguageChange can arrive before the language manager publishes its new locale.
        publishModels(false);
        return;
    }
    m_locale = locale;
    const QPointer<TranslationService> guard(this);
    publishModels(false);
    if (!guard)
        return;
    if (m_client != nullptr)
        m_client->cancel(std::exchange(m_modelsToken, 0));
    // The effective target belongs to the translation, not to the language of its UI.
    refreshModels();
}

QString TranslationService::errorText() const {
    if (m_storageError)
        return tr("Unable to save translation preferences. Your previous selections were restored");
    if (m_preferences.modelId == fastTranslationId && m_fastCatalogAttempted &&
        m_fastCatalogToken == 0 && !m_fastAvailable)
        return tr("Translation service is unavailable");
    if (!m_models.isEmpty() || loadingModels() || !m_catalogAttempted)
        return {};
    return m_catalogError.isEmpty()
               ? tr("No translation services are available")
               : tr("Unable to load translation services: %1").arg(m_catalogError);
}

QString TranslationService::modelConfigurationChangedText() {
    return tr("Model configuration changed. Retry to use the updated settings");
}

TranslationJob* TranslationService::createJob(const QStringList& texts, QObject* owner) {
    return new TranslationJob(*this, texts, owner);
}

TranslationJob* TranslationService::createJob(const QStringList& texts,
                                              const TranslationPreferences& preferences,
                                              QObject* owner) {
    auto* job = new TranslationJob(*this, texts, owner);
    job->m_preferences = preferences;
    job->m_fixedPreferences = true;
    return job;
}

TranslationJob::TranslationJob(TranslationService& service, const QStringList& texts,
                               QObject* owner)
    : QObject(owner), m_service(&service), m_client(service.m_client),
      m_preferences(service.preferences()) {
    QHash<QString, int> groupsBySource;
    for (const auto& text : texts) {
        const int index = static_cast<int>(m_units.size());
        m_units.append({text, {}, State::Idle});
        const auto existing = groupsBySource.constFind(text);
        if (existing == groupsBySource.cend()) {
            groupsBySource.insert(text, static_cast<int>(m_unitGroups.size()));
            m_unitGroups.append(QVector<int>{index});
        } else {
            m_unitGroups[*existing].append(index);
        }
    }
    connect(&service, &TranslationService::catalogChanged, this, [this] {
        if (m_state == State::WaitingForModels)
            prepare();
    });
    connect(&service, &TranslationService::modelInvalidated, this,
            &TranslationJob::invalidateModel);
    connect(&service, &TranslationService::unavailable, this, [this] {
        if (busy())
            fail(tr("Translation service is unavailable"));
    });
    connect(&service, &QObject::destroyed, this, [this] {
        if (busy())
            fail(tr("Translation service is unavailable"));
    });
}

TranslationJob::~TranslationJob() {
    cancel();
}

void TranslationJob::cancel() {
    toolbox::OfflineTranslator::instance().cancel(std::exchange(m_offlineRequest, 0));
    ++m_generation;
    const auto requests = std::exchange(m_requests, {});
    if (m_client != nullptr)
        for (auto token : requests)
            m_client->cancel(token);
    if (busy()) {
        m_state = State::Cancelled;
        for (auto& unit : m_units) {
            if (unit.state == State::Streaming || unit.state == State::Idle)
                unit.state = State::Cancelled;
        }
    }
}

void TranslationJob::start() {
    retry();
}

void TranslationJob::retry() {
    cancel();
    m_result = {};
    m_dispatchBlocked = false;
    if (m_service == nullptr || m_client == nullptr) {
        fail(tr("Translation service is unavailable"));
        return;
    }
    const bool changed = (!m_fixedPreferences && m_preferences != m_service->preferences()) ||
                         m_state == State::Invalidated;
    if (!m_fixedPreferences)
        m_preferences = m_service->preferences();
    if (changed) {
        m_fastChunks.clear();
        m_fastGroupOffsets.clear();
        m_fastNextChunk = 0;
    }
    bool hasPendingUnits = false;
    for (auto& unit : m_units) {
        if (changed) {
            unit.text.clear();
            unit.state = State::Idle;
        }
        if (unit.sourceText.trimmed().isEmpty())
            unit.state = State::Completed;
        else if (unit.state != State::Completed)
            unit.state = State::Idle;
        hasPendingUnits |= unit.state == State::Idle;
    }
    QStringList localSources;
    for (const auto& unit : m_units)
        localSources.append(unit.sourceText);
    m_effectiveTargetLanguage = toolbox::OfflineTranslator::direction(localSources.join(u'\n'),
                                                                      {}) == QStringLiteral("zh_en")
                                    ? QStringLiteral("en")
                                    : QStringLiteral("zh");
    m_state = State::WaitingForModels;
    const auto generation = m_generation;
    const QPointer<TranslationJob> guard(this);
    if (hasPendingUnits && m_preferences.modelId == fastTranslationId &&
        m_service->refreshFastCatalog(false)) {
        emit m_service->catalogChanged();
        if (!guard || generation != m_generation || m_state != State::WaitingForModels)
            return;
    }
    emit stateChanged();
    if (!guard || generation != m_generation || m_state != State::WaitingForModels)
        return;
    prepare();
}

void TranslationJob::prepare() {
    if (m_state != State::WaitingForModels || m_service == nullptr)
        return;
    m_state = State::Streaming;
    pump();
}

void TranslationJob::pump() {
    if (m_state != State::Streaming || m_offlineRequest != 0)
        return;
    for (int group = 0; group < m_unitGroups.size(); ++group) {
        const auto indices = m_unitGroups[group];
        if (m_units[indices.first()].state != State::Idle)
            continue;
        for (int index : indices)
            m_units[index].state = State::Streaming;
        const auto generation = m_generation;
        m_offlineRequest = toolbox::OfflineTranslator::instance().translate(
            m_units[indices.first()].sourceText, m_effectiveTargetLanguage, this,
            [this, generation, indices](QString text, QString error) {
                if (generation != m_generation || m_state != State::Streaming)
                    return;
                m_offlineRequest = 0;
                const QPointer<TranslationJob> guard(this);
                for (int index : indices) {
                    m_units[index].text = text;
                    m_units[index].state = error.isEmpty() ? State::Completed : State::Failed;
                    emit unitChanged(index);
                    if (!guard)
                        return;
                }
                if (!error.isEmpty()) {
                    fail(error);
                    return;
                }
                pump();
            });
        return;
    }
    if (m_state == State::Streaming) {
        m_state = State::Completed;
        publishState();
    }
    return;
}

void TranslationJob::prepareFastChunks() {
    m_fastGroupOffsets.reserve(m_unitGroups.size() + 1);
    for (qsizetype group = 0; group < m_unitGroups.size(); ++group) {
        m_fastGroupOffsets.append(m_fastChunks.size());
        const auto& unit = m_units.at(m_unitGroups.at(group).first());
        if (unit.state == State::Completed)
            continue;
        const auto& text = unit.sourceText;
        for (qsizetype start = 0; start < text.size();) {
            const auto end = fastTranslationCut(text, start);
            auto next = end;
            while (next < text.size() && text.at(next).isSpace())
                ++next;
            m_fastChunks.append(
                {text.mid(start, end - start), text.mid(end, next - end), {}, group});
            start = next;
        }
    }
    m_fastGroupOffsets.append(m_fastChunks.size());
}

void TranslationJob::pumpFast() {
    if (!m_requests.isEmpty())
        return;
    if (m_fastNextChunk == m_fastChunks.size()) {
        m_state = State::Completed;
        publishState();
        return;
    }

    const auto start = m_fastNextChunk;
    QStringList texts;
    qsizetype length = 0;
    for (auto index = start;
         index < m_fastChunks.size() && texts.size() < fastTranslationMaximumItems; ++index) {
        const auto& chunk = m_fastChunks.at(index);
        length += chunk.source.size();
        if (length > fastTranslationMaximumLength)
            break;
        texts.append(chunk.source);
        for (const int unit : m_unitGroups.at(chunk.group))
            m_units[unit].state = State::Streaming;
    }

    const auto generation = m_generation;
    const auto token = m_client->translateFastBatch(
        texts, m_preferences.sourceLanguage, m_effectiveTargetLanguage, this,
        [this, generation, start](SnowShotTranslationResult result,
                                  const QStringList& translations) {
            if (generation != m_generation || !m_requests.remove(0))
                return;
            if (!result.succeeded()) {
                m_result = std::move(result);
                fail(m_result.error);
                return;
            }

            for (qsizetype index = 0; index < translations.size(); ++index)
                m_fastChunks[start + index].translated = translations.at(index);
            m_fastNextChunk += translations.size();
            const auto firstGroup = m_fastChunks.at(start).group;
            const auto lastGroup = m_fastChunks.at(m_fastNextChunk - 1).group;
            QList<int> completed;
            for (auto group = firstGroup; group <= lastGroup; ++group) {
                const auto first = m_fastGroupOffsets.at(group);
                const auto end = m_fastGroupOffsets.at(group + 1);
                if (first == end || end > m_fastNextChunk)
                    continue;
                QString text;
                for (auto index = first; index < end; ++index) {
                    const auto& chunk = m_fastChunks.at(index);
                    text += chunk.translated + chunk.separator;
                }
                for (const int unit : m_unitGroups.at(group)) {
                    m_units[unit].text = text;
                    m_units[unit].state = State::Completed;
                    completed.append(unit);
                }
            }

            const QPointer<TranslationJob> guard(this);
            for (const int unit : completed) {
                emit unitChanged(unit);
                if (!guard || generation != m_generation || m_state != State::Streaming)
                    return;
            }
            pumpFast();
        });
    if (token == 0) {
        fail(tr("Translation request could not be prepared"));
        return;
    }
    m_requests.insert(0, token);
    publishState();
}

void TranslationJob::publishState() {
    const auto generation = m_generation;
    const auto state = m_state;
    const QPointer<TranslationJob> guard(this);
    emit stateChanged();
    // A listener may retire this transition even when its retry reaches the same state.
    if (!guard || generation != m_generation || state != m_state)
        return;
    if (state == State::Completed || state == State::Failed)
        emit finished();
    else if (state == State::Invalidated)
        emit invalidated();
}

void TranslationJob::fail(const QString& error) {
    cancel();
    m_result.error = error.isEmpty() ? tr("Translation service is unavailable") : error;
    m_state = State::Failed;
    for (auto& unit : m_units)
        if (unit.state != State::Completed)
            unit.state = State::Failed;
    publishState();
}

void TranslationJob::invalidateModel(const QString& id) {
    if (m_preferences.modelId != id)
        return;
    cancel();
    for (auto& unit : m_units) {
        unit.text.clear();
        unit.state = State::Idle;
    }
    m_fastChunks.clear();
    m_fastGroupOffsets.clear();
    m_fastNextChunk = 0;
    m_state = State::Invalidated;
    m_result.error = TranslationService::modelConfigurationChangedText();
    publishState();
}

QString TranslationJob::errorText() const {
    if (m_state == State::Invalidated) {
        return TranslationService::modelConfigurationChangedText();
    }
    if (m_preferences.modelId == freeTranslationId && !m_result.error.isEmpty())
        return freeTranslationErrorText();
    return m_result.error;
}
} // namespace snow_shot::translation
