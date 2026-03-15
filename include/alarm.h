#pragma once

#include <string>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace opendial::alarm {

// Bitmask for days of week: bit 0 = Monday, ..., bit 6 = Sunday
enum DayOfWeek : std::uint8_t {
    Monday    = 1 << 0,
    Tuesday   = 1 << 1,
    Wednesday = 1 << 2,
    Thursday  = 1 << 3,
    Friday    = 1 << 4,
    Saturday  = 1 << 5,
    Sunday    = 1 << 6,
    Weekdays  = Monday | Tuesday | Wednesday | Thursday | Friday,
    Weekends  = Saturday | Sunday,
    EveryDay  = Weekdays | Weekends,
};

struct Alarm {
    std::string uuid;
    std::chrono::system_clock::time_point time;
    std::string label;
    bool enabled{true};
    std::uint64_t version{0};
    std::string device_id;
    std::uint8_t recurrence_days{0}; // 0 = one-shot, otherwise DayOfWeek bitmask
    std::chrono::system_clock::time_point last_modified;

    Alarm();
    Alarm(std::chrono::system_clock::time_point time, std::string label,
          std::string device_id, std::uint8_t recurrence_days = 0);

    // Serialization for the sync protocol (unit-separator delimited)
    std::string serialize() const;
    static std::optional<Alarm> deserialize(std::string_view data);

    bool operator==(const Alarm& other) const { return uuid == other.uuid; }
};

// Observer interface for alarm lifecycle events
class AlarmObserver {
public:
    virtual ~AlarmObserver() = default;
    virtual void onAlarmAdded(const Alarm& alarm) {}
    virtual void onAlarmUpdated(const Alarm& alarm) {}
    virtual void onAlarmRemoved(const std::string& uuid) {}
    virtual void onAlarmTriggered(const Alarm& alarm) {}
};

// Thread-safe local alarm store with observer support and sync merge
class AlarmManager {
public:
    explicit AlarmManager(std::string device_id);

    // CRUD -----------------------------------------------------------------
    std::string addAlarm(Alarm alarm);
    bool updateAlarm(const std::string& uuid,
                     std::function<void(Alarm&)> mutator);
    bool removeAlarm(const std::string& uuid);
    std::optional<Alarm> getAlarm(const std::string& uuid) const;
    std::vector<Alarm> getAllAlarms() const;

    // Sync helpers ---------------------------------------------------------
    // Merge a remote alarm using last-writer-wins on version.
    // Returns true if the local store was changed.
    bool mergeAlarm(const Alarm& remote);
    bool mergeDelete(const std::string& uuid, std::uint64_t remote_version);
    std::vector<Alarm> getAlarmsModifiedSince(
        std::chrono::system_clock::time_point since) const;

    // Observers ------------------------------------------------------------
    void addObserver(std::shared_ptr<AlarmObserver> observer);
    void removeObserver(std::shared_ptr<AlarmObserver> observer);

    const std::string& deviceId() const { return device_id_; }

private:
    void notifyAdded(const Alarm& alarm);
    void notifyUpdated(const Alarm& alarm);
    void notifyRemoved(const std::string& uuid);

    std::string device_id_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Alarm> alarms_;
    // Track deleted UUIDs with their version to avoid resurrection via sync
    std::unordered_map<std::string, std::uint64_t> tombstones_;
    std::vector<std::shared_ptr<AlarmObserver>> observers_;
};

} // namespace opendial::alarm