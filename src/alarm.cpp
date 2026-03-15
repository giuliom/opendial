#include <alarm.h>
#include <utils.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>

namespace opendial::alarm {

// ── Helpers ──────────────────────────────────────────────────────────────

static constexpr char kFieldSep = '\x1F'; // ASCII Unit Separator

static std::string timeToString(std::chrono::system_clock::time_point tp) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  tp.time_since_epoch())
                  .count();
    return std::to_string(us);
}

static std::chrono::system_clock::time_point timeFromString(std::string_view sv) {
    std::int64_t us = std::stoll(std::string(sv));
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

    Alarm a;
    a.uuid            = std::string(fields[0]);
    a.time            = timeFromString(fields[1]);
    a.label           = std::string(fields[2]);
    a.enabled         = (fields[3] == "1");
    a.version         = std::stoull(std::string(fields[4]));
    a.device_id       = std::string(fields[5]);
    a.recurrence_days = static_cast<std::uint8_t>(
                            std::stoul(std::string(fields[6])));
    a.last_modified   = timeFromString(fields[7]);
    return a;
}

// ── AlarmManager ─────────────────────────────────────────────────────────

AlarmManager::AlarmManager(std::string device_id)
    : device_id_(std::move(device_id)) {}

std::string AlarmManager::addAlarm(Alarm alarm) {
    std::lock_guard lock(mutex_);
    alarm.device_id = device_id_;
    alarm.version = 1;
    alarm.last_modified = std::chrono::system_clock::now();
    std::string id = alarm.uuid;
    alarms_.emplace(id, std::move(alarm));
    notifyAdded(alarms_.at(id));
    return id;
}

bool AlarmManager::updateAlarm(const std::string& uuid,
                               std::function<void(Alarm&)> mutator) {
    std::lock_guard lock(mutex_);
    auto it = alarms_.find(uuid);
    if (it == alarms_.end()) return false;
    mutator(it->second);
    it->second.version++;
    it->second.device_id = device_id_;
    it->second.last_modified = std::chrono::system_clock::now();
    notifyUpdated(it->second);
    return true;
}

bool AlarmManager::removeAlarm(const std::string& uuid) {
    std::lock_guard lock(mutex_);
    auto it = alarms_.find(uuid);
    if (it == alarms_.end()) return false;
    tombstones_[uuid] = it->second.version + 1;
    alarms_.erase(it);
    notifyRemoved(uuid);
    return true;
}

std::optional<Alarm> AlarmManager::getAlarm(const std::string& uuid) const {
    std::lock_guard lock(mutex_);
    auto it = alarms_.find(uuid);
    if (it == alarms_.end()) return std::nullopt;
    return it->second;
}

std::vector<Alarm> AlarmManager::getAllAlarms() const {
    std::lock_guard lock(mutex_);
    std::vector<Alarm> result;
    result.reserve(alarms_.size());
    for (const auto& [_, a] : alarms_) result.push_back(a);
    return result;
}

bool AlarmManager::mergeAlarm(const Alarm& remote) {
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
        alarms_.emplace(remote.uuid, remote);
        notifyAdded(alarms_.at(remote.uuid));
        return true;
    }

    // Last-writer-wins based on version; tie-break on last_modified.
    if (remote.version > it->second.version ||
        (remote.version == it->second.version &&
         remote.last_modified > it->second.last_modified)) {
        it->second = remote;
        notifyUpdated(it->second);
        return true;
    }
    return false;
}

bool AlarmManager::mergeDelete(const std::string& uuid,
                               std::uint64_t remote_version) {
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
    if (remote_version > it->second.version) {
        tombstones_[uuid] = remote_version;
        alarms_.erase(it);
        notifyRemoved(uuid);
        return true;
    }
    return false;
}

std::vector<Alarm> AlarmManager::getAlarmsModifiedSince(
    std::chrono::system_clock::time_point since) const {
    std::lock_guard lock(mutex_);
    std::vector<Alarm> result;
    for (const auto& [_, a] : alarms_) {
        if (a.last_modified > since) result.push_back(a);
    }
    return result;
}

void AlarmManager::addObserver(std::shared_ptr<AlarmObserver> observer) {
    std::lock_guard lock(mutex_);
    observers_.push_back(std::move(observer));
}

void AlarmManager::removeObserver(std::shared_ptr<AlarmObserver> observer) {
    std::lock_guard lock(mutex_);
    std::erase(observers_, observer);
}

void AlarmManager::notifyAdded(const Alarm& alarm) {
    for (auto& obs : observers_) obs->onAlarmAdded(alarm);
}

void AlarmManager::notifyUpdated(const Alarm& alarm) {
    for (auto& obs : observers_) obs->onAlarmUpdated(alarm);
}

void AlarmManager::notifyRemoved(const std::string& uuid) {
    for (auto& obs : observers_) obs->onAlarmRemoved(uuid);
}

} // namespace opendial::alarm