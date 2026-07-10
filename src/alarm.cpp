#include <alarm.h>
#include <utils.h>

#include <algorithm>
#include <charconv>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opendial::alarm {

// ── Helpers ──────────────────────────────────────────────────────────────

static constexpr char kFieldSep = '\x1F'; // ASCII Unit Separator
static constexpr char kRecordSep = '\x1E'; // ASCII Record Separator

template <typename Integer>
static bool parseInteger(std::string_view value, Integer& result) {
    if (value.empty()) return false;

    const auto* first = value.data();
    const auto* last = first + value.size();
    const auto [end, error] = std::from_chars(first, last, result);
    return error == std::errc{} && end == last;
}

static bool containsSeparator(std::string_view value) {
    return value.find(kFieldSep) != std::string_view::npos ||
           value.find(kRecordSep) != std::string_view::npos;
}

static bool hasValidWireFields(const Alarm& alarm) {
    return !alarm.uuid.empty() && !containsSeparator(alarm.uuid) &&
           !containsSeparator(alarm.label) &&
           !containsSeparator(alarm.device_id) &&
           alarm.recurrence_days <= EveryDay;
}

static std::string timeToString(std::chrono::system_clock::time_point tp) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  tp.time_since_epoch())
                  .count();
    return std::to_string(us);
}

static std::chrono::system_clock::time_point timeFromString(std::string_view sv) {
    std::int64_t us = 0;
    if (!parseInteger(sv, us)) {
        throw std::invalid_argument("invalid alarm timestamp");
    }
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::microseconds{us})};
}

// ── Alarm ────────────────────────────────────────────────────────────────

Alarm::Alarm()
    : uuid(utils::generateuuid()),
      last_modified(std::chrono::system_clock::now()) {}

Alarm::Alarm(std::chrono::system_clock::time_point time, std::string label,
             std::string device_id, std::uint8_t recurrence_days)
    : uuid(utils::generateuuid()),
      time(time),
      label(std::move(label)),
      device_id(std::move(device_id)),
      recurrence_days(recurrence_days),
      last_modified(std::chrono::system_clock::now()) {}

std::string Alarm::serialize() const {
    if (!hasValidWireFields(*this)) {
        throw std::invalid_argument("alarm contains a reserved separator");
    }

    std::string out;
    out.reserve(256);
    out += uuid;                                out += kFieldSep;
    out += timeToString(time);                  out += kFieldSep;
    out += label;                               out += kFieldSep;
    out += (enabled ? '1' : '0');               out += kFieldSep;
    out += std::to_string(version);             out += kFieldSep;
    out += device_id;                           out += kFieldSep;
    out += std::to_string(recurrence_days);     out += kFieldSep;
    out += timeToString(last_modified);
    return out;
}

std::optional<Alarm> Alarm::deserialize(std::string_view data) {
    // Split on kFieldSep – expect exactly 8 fields
    std::vector<std::string_view> fields;
    fields.reserve(8);
    std::size_t start = 0;
    for (std::size_t i = 0; i <= data.size(); ++i) {
        if (i == data.size() || data[i] == kFieldSep) {
            fields.push_back(data.substr(start, i - start));
            start = i + 1;
        }
    }
    if (fields.size() != 8) return std::nullopt;

    if (fields[0].empty() || containsSeparator(fields[0]) ||
        containsSeparator(fields[2]) || containsSeparator(fields[5]) ||
        (fields[3] != "0" && fields[3] != "1")) {
        return std::nullopt;
    }

    std::uint64_t version = 0;
    unsigned int recurrence_days = 0;
    std::int64_t timestamp = 0;
    std::int64_t last_modified = 0;
    if (!parseInteger(fields[4], version) ||
        !parseInteger(fields[6], recurrence_days) ||
        recurrence_days > EveryDay || !parseInteger(fields[1], timestamp) ||
        !parseInteger(fields[7], last_modified)) {
        return std::nullopt;
    }

    try {
        Alarm a;
        a.uuid            = std::string(fields[0]);
        a.time            = timeFromString(fields[1]);
        a.label           = std::string(fields[2]);
        a.enabled         = fields[3] == "1";
        a.version         = version;
        a.device_id       = std::string(fields[5]);
        a.recurrence_days = static_cast<std::uint8_t>(recurrence_days);
        a.last_modified   = timeFromString(fields[7]);
        return a;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// ── AlarmManager ─────────────────────────────────────────────────────────

AlarmManager::AlarmManager(std::string device_id)
    : device_id_(std::move(device_id)) {
    if (containsSeparator(device_id_)) {
        throw std::invalid_argument("device identifier contains a reserved separator");
    }
}

std::string AlarmManager::addAlarm(Alarm alarm) {
    std::string id;
    std::shared_ptr<const Alarm> added;
    {
        std::lock_guard lock(mutex_);
        if (alarm.uuid.empty()) alarm.uuid = utils::generateuuid();
        if (!hasValidWireFields(alarm)) {
            throw std::invalid_argument("alarm contains invalid wire fields");
        }
        while (alarms_.contains(alarm.uuid) || tombstones_.contains(alarm.uuid)) {
            alarm.uuid = utils::generateuuid();
        }
        alarm.device_id = device_id_;
        alarm.version = 1;
        alarm.last_modified = std::chrono::system_clock::now();
        id = alarm.uuid;
        auto [it, _] = alarms_.emplace(
            id, std::make_shared<Alarm>(std::move(alarm)));
        added = it->second;
    }
    notifyAdded(*added);
    return id;
}

bool AlarmManager::updateAlarm(const std::string& uuid,
                               std::function<void(Alarm&)> mutator) {
    std::shared_ptr<const Alarm> updated;
    {
        std::lock_guard lock(mutex_);
        auto it = alarms_.find(uuid);
        if (it == alarms_.end()) return false;
        if (it->second->version == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        const auto current_version = it->second->version;
        auto replacement = std::make_shared<Alarm>(*it->second);
        mutator(*replacement);
        replacement->uuid = uuid;
        replacement->version = current_version + 1;
        replacement->device_id = device_id_;
        replacement->last_modified = std::chrono::system_clock::now();
        if (!hasValidWireFields(*replacement)) return false;
        updated = std::move(replacement);
        it->second = updated;
    }
    notifyUpdated(*updated);
    return true;
}

bool AlarmManager::removeAlarm(const std::string& uuid) {
    {
        std::lock_guard lock(mutex_);
        auto it = alarms_.find(uuid);
        if (it == alarms_.end()) return false;
        const auto version = it->second->version;
        tombstones_[uuid] =
            version == std::numeric_limits<std::uint64_t>::max()
                ? version
                : version + 1;
        alarms_.erase(it);
    }
    notifyRemoved(uuid);
    return true;
}

std::optional<Alarm> AlarmManager::getAlarm(const std::string& uuid) const {
    std::lock_guard lock(mutex_);
    auto it = alarms_.find(uuid);
    if (it == alarms_.end()) return std::nullopt;
    return *it->second;
}

std::vector<Alarm> AlarmManager::getAllAlarms() const {
    std::lock_guard lock(mutex_);
    std::vector<Alarm> result;
    result.reserve(alarms_.size());
    for (const auto& [_, alarm] : alarms_) result.push_back(*alarm);
    return result;
}

bool AlarmManager::mergeAlarm(const Alarm& remote) {
    if (!hasValidWireFields(remote) || remote.version == 0) {
        return false;
    }

    bool added = false;
    bool updated = false;
    std::shared_ptr<const Alarm> changed;
    {
        std::lock_guard lock(mutex_);

        // Check tombstones – if we deleted this alarm with a higher version, skip.
        if (auto tit = tombstones_.find(remote.uuid); tit != tombstones_.end()) {
            if (tit->second >= remote.version) return false;
            // Remote version is higher – resurrect.
            tombstones_.erase(tit);
        }

        auto it = alarms_.find(remote.uuid);
        if (it == alarms_.end()) {
            // New alarm from a remote device.
            changed = std::make_shared<Alarm>(remote);
            alarms_.emplace(remote.uuid, changed);
            added = true;
        } else if (remote.version > it->second->version ||
                   (remote.version == it->second->version &&
                    remote.last_modified > it->second->last_modified)) {
            // Last-writer-wins based on version; tie-break on last_modified.
            changed = std::make_shared<Alarm>(remote);
            it->second = changed;
            updated = true;
        } else {
            return false;
        }
    }
    if (added) notifyAdded(*changed);
    else if (updated) notifyUpdated(*changed);
    return true;
}

bool AlarmManager::mergeDelete(const std::string& uuid,
                               std::uint64_t remote_version) {
    if (uuid.empty() || containsSeparator(uuid) || remote_version == 0) {
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        auto it = alarms_.find(uuid);
        if (it == alarms_.end()) {
            // Already gone – just update tombstone if newer.
            if (auto tit = tombstones_.find(uuid); tit != tombstones_.end()) {
                if (remote_version > tit->second) tit->second = remote_version;
            } else {
                tombstones_[uuid] = remote_version;
            }
            return false;
        }
        if (remote_version <= it->second->version) return false;
        tombstones_[uuid] = remote_version;
        alarms_.erase(it);
    }
    notifyRemoved(uuid);
    return true;
}

std::vector<Alarm> AlarmManager::getAlarmsModifiedSince(
    std::chrono::system_clock::time_point since) const {
    std::lock_guard lock(mutex_);
    std::vector<Alarm> result;
    for (const auto& [_, alarm] : alarms_) {
        if (alarm->last_modified > since) result.push_back(*alarm);
    }
    return result;
}

void AlarmManager::addObserver(std::shared_ptr<AlarmObserver> observer) {
    if (!observer) return;
    std::lock_guard lock(mutex_);
    if (std::ranges::find(observers_, observer) != observers_.end()) return;
    observers_.push_back(std::move(observer));
}

void AlarmManager::removeObserver(std::shared_ptr<AlarmObserver> observer) {
    std::lock_guard lock(mutex_);
    std::erase(observers_, observer);
}

void AlarmManager::notifyAdded(const Alarm& alarm) {
    std::vector<std::shared_ptr<AlarmObserver>> observers;
    {
        std::lock_guard lock(mutex_);
        observers = observers_;
    }
    for (auto& obs : observers) obs->onAlarmAdded(alarm);
}

void AlarmManager::notifyUpdated(const Alarm& alarm) {
    std::vector<std::shared_ptr<AlarmObserver>> observers;
    {
        std::lock_guard lock(mutex_);
        observers = observers_;
    }
    for (auto& obs : observers) obs->onAlarmUpdated(alarm);
}

void AlarmManager::notifyRemoved(const std::string& uuid) {
    std::vector<std::shared_ptr<AlarmObserver>> observers;
    {
        std::lock_guard lock(mutex_);
        observers = observers_;
    }
    for (auto& obs : observers) obs->onAlarmRemoved(uuid);
}

} // namespace opendial::alarm