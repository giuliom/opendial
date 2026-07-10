#include <gtest/gtest.h>
#include <alarm.h>
#include <sync.h>

#include <chrono>
#include <thread>

using namespace opendial::alarm;
using namespace opendial::sync;
using namespace std::chrono_literals;

// ── Alarm struct tests ───────────────────────────────────────────────────

class AlarmTest : public ::testing::Test {};

TEST_F(AlarmTest, DefaultConstructor) {
    Alarm alarm;
    EXPECT_FALSE(alarm.uuid.empty());
    EXPECT_EQ(alarm.enabled, true);
    EXPECT_EQ(alarm.version, 0u);
    EXPECT_EQ(alarm.recurrence_days, 0);
}

TEST_F(AlarmTest, ParameterizedConstructor) {
    auto t = std::chrono::system_clock::now() + 1h;
    Alarm alarm(t, "Wake up", "device-1", DayOfWeek::Weekdays);
    EXPECT_FALSE(alarm.uuid.empty());
    EXPECT_EQ(alarm.label, "Wake up");
    EXPECT_EQ(alarm.device_id, "device-1");
    EXPECT_EQ(alarm.recurrence_days, DayOfWeek::Weekdays);
    EXPECT_EQ(alarm.time, t);
}

TEST_F(AlarmTest, UniqueUUIDs) {
    Alarm a, b;
    EXPECT_NE(a.uuid, b.uuid);
}

TEST_F(AlarmTest, SerializeDeserializeRoundtrip) {
    auto t = std::chrono::system_clock::now() + 30min;
    Alarm original(t, "Meeting", "laptop-1", DayOfWeek::Monday | DayOfWeek::Wednesday);
    original.enabled = false;
    original.version = 42;

    std::string wire = original.serialize();
    auto restored = Alarm::deserialize(wire);

    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->uuid, original.uuid);
    EXPECT_EQ(restored->time, original.time);
    EXPECT_EQ(restored->label, original.label);
    EXPECT_EQ(restored->enabled, original.enabled);
    EXPECT_EQ(restored->version, original.version);
    EXPECT_EQ(restored->device_id, original.device_id);
    EXPECT_EQ(restored->recurrence_days, original.recurrence_days);
    EXPECT_EQ(restored->last_modified, original.last_modified);
}

TEST_F(AlarmTest, DeserializeInvalidData) {
    EXPECT_FALSE(Alarm::deserialize("").has_value());
    EXPECT_FALSE(Alarm::deserialize("too|few|fields").has_value());
}

TEST_F(AlarmTest, DeserializeRejectsMalformedValues) {
    const auto makeWire = [](std::string enabled, std::string version,
                             std::string recurrence) {
        std::string wire = "id";
        wire += '\x1F';
        wire += "0";
        wire += '\x1F';
        wire += "label";
        wire += '\x1F';
        wire += enabled;
        wire += '\x1F';
        wire += version;
        wire += '\x1F';
        wire += "device";
        wire += '\x1F';
        wire += recurrence;
        wire += '\x1F';
        wire += "0";
        return wire;
    };

    EXPECT_FALSE(Alarm::deserialize(makeWire("yes", "1", "0")));
    EXPECT_FALSE(Alarm::deserialize(makeWire("1", "1tail", "0")));
    EXPECT_FALSE(Alarm::deserialize(makeWire("1", "1", "128")));
}

TEST_F(AlarmTest, SerializeRejectsReservedSeparators) {
    Alarm alarm;
    alarm.label = "bad\x1Fvalue";
    EXPECT_THROW(alarm.serialize(), std::invalid_argument);
}

// ── AlarmManager tests ───────────────────────────────────────────────────

class AlarmManagerTest : public ::testing::Test {
protected:
    AlarmManager manager{"test-device"};
};

TEST_F(AlarmManagerTest, AddAndRetrieve) {
    Alarm a(std::chrono::system_clock::now() + 1h, "Test", "x");
    std::string id = manager.addAlarm(std::move(a));

    auto retrieved = manager.getAlarm(id);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->label, "Test");
    EXPECT_EQ(retrieved->device_id, "test-device"); // stamped by manager
    EXPECT_EQ(retrieved->version, 1u);
}

TEST_F(AlarmManagerTest, GetAllAlarms) {
    manager.addAlarm(Alarm(std::chrono::system_clock::now() + 1h, "A", "x"));
    manager.addAlarm(Alarm(std::chrono::system_clock::now() + 2h, "B", "x"));
    EXPECT_EQ(manager.getAllAlarms().size(), 2u);
}

TEST_F(AlarmManagerTest, UpdateAlarm) {
    Alarm a(std::chrono::system_clock::now() + 1h, "Old", "x");
    std::string id = manager.addAlarm(std::move(a));

    bool ok = manager.updateAlarm(id, [](Alarm& a) {
        a.label = "New";
        a.enabled = false;
    });
    EXPECT_TRUE(ok);

    auto updated = manager.getAlarm(id);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->label, "New");
    EXPECT_FALSE(updated->enabled);
    EXPECT_EQ(updated->version, 2u); // incremented from 1
}

TEST_F(AlarmManagerTest, UpdateNonexistent) {
    EXPECT_FALSE(manager.updateAlarm("no-such-id", [](Alarm&) {}));
}

TEST_F(AlarmManagerTest, UpdatePreservesIdentity) {
    const auto id = manager.addAlarm(
        Alarm(std::chrono::system_clock::now() + 1h, "Test", "x"));

    ASSERT_TRUE(manager.updateAlarm(id, [](Alarm& alarm) {
        alarm.uuid = "a-different-identity";
    }));
    ASSERT_TRUE(manager.getAlarm(id).has_value());
    EXPECT_EQ(manager.getAlarm(id)->uuid, id);
    EXPECT_FALSE(manager.getAlarm("a-different-identity").has_value());
}

TEST_F(AlarmManagerTest, AddRegeneratesDuplicateIdentity) {
    Alarm first(std::chrono::system_clock::now() + 1h, "First", "x");
    Alarm second = first;
    const auto first_id = manager.addAlarm(std::move(first));
    const auto second_id = manager.addAlarm(std::move(second));

    EXPECT_NE(first_id, second_id);
    EXPECT_EQ(manager.getAllAlarms().size(), 2u);
}

TEST_F(AlarmManagerTest, AddRejectsReservedFields) {
    Alarm invalid(std::chrono::system_clock::now() + 1h, "bad\x1Flabel", "x");
    EXPECT_THROW(manager.addAlarm(std::move(invalid)), std::invalid_argument);
    EXPECT_TRUE(manager.getAllAlarms().empty());
}

TEST_F(AlarmManagerTest, RemoveAlarm) {
    std::string id =
        manager.addAlarm(Alarm(std::chrono::system_clock::now() + 1h, "X", "x"));
    EXPECT_TRUE(manager.removeAlarm(id));
    EXPECT_FALSE(manager.getAlarm(id).has_value());
    EXPECT_FALSE(manager.removeAlarm(id)); // double-remove
}

// ── Observer tests ───────────────────────────────────────────────────────

class TestObserver : public AlarmObserver {
public:
    int added = 0, updated = 0, removed = 0;
    void onAlarmAdded(const Alarm&) override { ++added; }
    void onAlarmUpdated(const Alarm&) override { ++updated; }
    void onAlarmRemoved(const std::string&) override { ++removed; }
};

class QueryingObserver : public AlarmObserver {
public:
    explicit QueryingObserver(AlarmManager& manager) : manager_(manager) {}

    void onAlarmAdded(const Alarm& alarm) override {
        observed_alarm = manager_.getAlarm(alarm.uuid);
    }

    std::optional<Alarm> observed_alarm;

private:
    AlarmManager& manager_;
};

TEST_F(AlarmManagerTest, ObserverNotifications) {
    auto obs = std::make_shared<TestObserver>();
    manager.addObserver(obs);

    std::string id =
        manager.addAlarm(Alarm(std::chrono::system_clock::now() + 1h, "Z", "x"));
    EXPECT_EQ(obs->added, 1);

    manager.updateAlarm(id, [](Alarm& a) { a.label = "ZZ"; });
    EXPECT_EQ(obs->updated, 1);

    manager.removeAlarm(id);
    EXPECT_EQ(obs->removed, 1);
}

TEST_F(AlarmManagerTest, ObserverCanQueryManager) {
    auto observer = std::make_shared<QueryingObserver>(manager);
    manager.addObserver(observer);

    std::string id = manager.addAlarm(
        Alarm(std::chrono::system_clock::now() + 1h, "Z", "x"));

    ASSERT_TRUE(observer->observed_alarm.has_value());
    EXPECT_EQ(observer->observed_alarm->uuid, id);
}

// ── Merge / sync conflict tests ──────────────────────────────────────────

class MergeTest : public ::testing::Test {
protected:
    AlarmManager local{"device-A"};
    AlarmManager remote{"device-B"};
};

TEST_F(MergeTest, MergeNewAlarm) {
    Alarm a(std::chrono::system_clock::now() + 1h, "Remote", "device-B");
    a.version = 1;
    EXPECT_TRUE(local.mergeAlarm(a));
    EXPECT_TRUE(local.getAlarm(a.uuid).has_value());
}

TEST_F(MergeTest, MergeHigherVersionWins) {
    Alarm a(std::chrono::system_clock::now() + 1h, "V1", "device-A");
    std::string id = local.addAlarm(a);

    // Simulate remote with higher version.
    auto current = local.getAlarm(id).value();
    Alarm remoteVersion = current;
    remoteVersion.label = "V3";
    remoteVersion.version = 3;
    remoteVersion.last_modified = std::chrono::system_clock::now();

    EXPECT_TRUE(local.mergeAlarm(remoteVersion));
    EXPECT_EQ(local.getAlarm(id)->label, "V3");
}

TEST_F(MergeTest, MergeLowerVersionIgnored) {
    Alarm a(std::chrono::system_clock::now() + 1h, "V5", "device-A");
    std::string id = local.addAlarm(a);

    // Bump local version high.
    for (int i = 0; i < 4; ++i)
        local.updateAlarm(id, [](Alarm& a) {});

    auto current = local.getAlarm(id).value();
    EXPECT_EQ(current.version, 5u);

    Alarm oldRemote = current;
    oldRemote.label = "Old";
    oldRemote.version = 2;

    EXPECT_FALSE(local.mergeAlarm(oldRemote));
    EXPECT_EQ(local.getAlarm(id)->label, "V5");
}

TEST_F(MergeTest, MergeDeletePreventsResurrection) {
    Alarm a(std::chrono::system_clock::now() + 1h, "Del", "device-A");
    a.version = 1;
    local.mergeAlarm(a);
    std::string id = a.uuid;

    // Delete locally with a higher version.
    local.mergeDelete(id, 5);
    EXPECT_FALSE(local.getAlarm(id).has_value());

    // Try merging an older version – should be rejected.
    a.version = 3;
    EXPECT_FALSE(local.mergeAlarm(a));
    EXPECT_FALSE(local.getAlarm(id).has_value());
}

TEST_F(MergeTest, MergeDeleteResurrectionAllowed) {
    Alarm a(std::chrono::system_clock::now() + 1h, "Resurrected", "device-A");
    a.version = 1;
    local.mergeAlarm(a);
    std::string id = a.uuid;

    local.mergeDelete(id, 2);
    EXPECT_FALSE(local.getAlarm(id).has_value());

    // Remote has a version higher than the tombstone.
    a.version = 10;
    a.last_modified = std::chrono::system_clock::now();
    EXPECT_TRUE(local.mergeAlarm(a));
    EXPECT_TRUE(local.getAlarm(id).has_value());
}

// ── SyncMessage encoding tests ───────────────────────────────────────────

TEST(SyncMessageTest, EncodeDecodeRoundtrip) {
    SyncMessage original{MessageType::AlarmUpdate, "hello-payload"};
    auto wire = original.encode();

    // Wire = 4 bytes length + 1 byte type + payload
    ASSERT_EQ(wire.size(), 4 + 1 + original.payload.size());

    auto decoded = SyncMessage::decode(wire.data() + 4, wire.size() - 4);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type, MessageType::AlarmUpdate);
    EXPECT_EQ(decoded->payload, "hello-payload");
}

TEST(SyncMessageTest, EmptyPayload) {
    SyncMessage msg{MessageType::FullSyncRequest, ""};
    auto wire = msg.encode();
    auto decoded = SyncMessage::decode(wire.data() + 4, wire.size() - 4);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type, MessageType::FullSyncRequest);
    EXPECT_TRUE(decoded->payload.empty());
}

TEST(SyncMessageTest, RejectsUnknownTypesAndInvalidPayloads) {
    const char unknown_type[] = {static_cast<char>(99)};
    EXPECT_FALSE(SyncMessage::decode(unknown_type, sizeof(unknown_type)));

    const char request_with_payload[] = {
        static_cast<char>(MessageType::FullSyncRequest), 'x'};
    EXPECT_FALSE(SyncMessage::decode(request_with_payload,
                                     sizeof(request_with_payload)));
    EXPECT_THROW((SyncMessage{MessageType::Ack, "x"}).encode(),
                 std::invalid_argument);
}

// ── Client-Server integration test ───────────────────────────────────────

TEST(SyncIntegrationTest, ClientServerFullSync) {
    AlarmManager serverManager("server-device");
    AlarmManager clientManager("client-device");

    // Add alarms on the server side.
    serverManager.addAlarm(
        Alarm(std::chrono::system_clock::now() + 1h, "ServerAlarm1", "s"));
    serverManager.addAlarm(
        Alarm(std::chrono::system_clock::now() + 2h, "ServerAlarm2", "s"));

    // Start server on an ephemeral port.
    SyncServer server(serverManager, 0);
    // Use a fixed high port to avoid bind issues with port 0 on some systems.
    SyncServer server2(serverManager, 18924);
    server2.start();

    // Give server time to start listening.
    std::this_thread::sleep_for(50ms);

    // Client connects and requests full sync.
    SyncClient client(clientManager, "127.0.0.1", 18924);
    client.connect();
    client.requestFullSync();

    // Wait for async sync to complete.
    std::this_thread::sleep_for(200ms);

    auto alarms = clientManager.getAllAlarms();
    EXPECT_EQ(alarms.size(), 2u);

    client.disconnect();
    server2.stop();
}

TEST(SyncIntegrationTest, ClientPushesUpdateToServer) {
    AlarmManager serverManager("server-device");
    AlarmManager clientManager("client-device");

    SyncServer server(serverManager, 18925);
    server.start();
    std::this_thread::sleep_for(50ms);

    SyncClient client(clientManager, "127.0.0.1", 18925);
    client.connect();

    // Client creates and pushes an alarm.
    Alarm a(std::chrono::system_clock::now() + 1h, "FromClient", "client-device");
    a.version = 1;
    client.pushAlarm(a);

    std::this_thread::sleep_for(200ms);

    auto serverAlarms = serverManager.getAllAlarms();
    EXPECT_EQ(serverAlarms.size(), 1u);
    if (!serverAlarms.empty()) {
        EXPECT_EQ(serverAlarms[0].label, "FromClient");
    }

    client.disconnect();
    server.stop();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}