# opendial
An open source, cross-platform C++ alarm synchronization library.

[![Tests](https://github.com/giuliom/opendial/actions/workflows/cmake-multi-platform.yml/badge.svg)](https://github.com/giuliom/opendial/actions/workflows/cmake-multi-platform.yml)

OpenDial provides a thread-safe alarm store and a small TCP synchronization
layer for sharing alarms between devices. It is implemented in C++23, uses
standalone Asio for networking, and has no runtime dependency on Boost.

## Features

- Thread-safe alarm CRUD operations with lifecycle observers.
- One-shot and recurring alarms using a Monday-to-Sunday bitmask.
- Versioned updates with last-writer-wins conflict resolution.
- Tombstones that prevent deleted alarms from being resurrected by stale data.
- Full synchronization and incremental alarm updates over TCP.
- Length-prefixed, bounded protocol frames with strict input validation.

## Requirements

- A C++23 compiler.
- CMake 3.23 or newer.
- A network connection during the first configure, so CMake can fetch GoogleTest
  and standalone Asio with `FetchContent`.

## Build And Test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The build produces the `OpenDialLib` library and the `OpenDialTest` test
executable. To build in parallel, pass a job count to the build command:

```sh
cmake --build build --parallel 2
```

## Local Alarm Store

The public API is in [`include/alarm.h`](include/alarm.h). An
`AlarmManager` owns alarms for one device and stamps locally added or updated
alarms with its device identifier and version.

```cpp
#include <alarm.h>

#include <chrono>

using namespace std::chrono_literals;
using opendial::alarm::Alarm;
using opendial::alarm::AlarmManager;
using opendial::alarm::Weekdays;

int main() {
	AlarmManager manager{"device-a"};

	const auto id = manager.addAlarm(
		Alarm{std::chrono::system_clock::now() + 8h,
			  "Wake up",
			  "device-a",
			  Weekdays});

	manager.updateAlarm(id, [](Alarm& alarm) {
		alarm.enabled = false;
		alarm.label = "Morning alarm";
	});

	const auto alarm = manager.getAlarm(id);
	return alarm && !alarm->enabled ? 0 : 1;
}
```

`updateAlarm` applies the mutator to a copy and commits it atomically. The
alarm UUID, device identifier, version, and modification timestamp remain
under manager control. Observers are notified after the store has been
updated, so they can safely query the manager from their callbacks.

## Synchronization

The TCP API is in [`include/sync.h`](include/sync.h). Start a server with an
alarm manager, then connect a client and request its initial full sync:

```cpp
#include <alarm.h>
#include <sync.h>

int main() {
	opendial::alarm::AlarmManager server_manager{"server"};
	opendial::sync::SyncServer server{server_manager, 0};
	server.start();

	opendial::alarm::AlarmManager client_manager{"client"};
	opendial::sync::SyncClient client{
		client_manager, "127.0.0.1", server.port()};
	client.connect();
	client.requestFullSync();

	// Push local changes with client.pushAlarm(alarm) or client.pushDelete(...).

	client.disconnect();
	server.stop();
}
```

Passing port `0` asks the operating system for an available port. Call
`server.port()` after `start()` to retrieve the bound port.

Alarm conflicts are resolved by the alarm version, with `last_modified` as a
tie-breaker. Deleted alarms are tracked by version so older updates cannot
resurrect them. `SyncClient` and `SyncServer` manage their Asio worker threads
and can be explicitly stopped before destruction.

## Wire Format

Each TCP message is framed as follows:

```text
[4-byte big-endian frame length] [1-byte message type] [payload]
```

Alarm records use ASCII Unit Separator (`0x1f`) between fields. Full-sync
responses use ASCII Record Separator (`0x1e`) between records. These reserved
separators are rejected in UUIDs, labels, and device identifiers; attempting
to serialize or add such an alarm reports `std::invalid_argument`.

Incoming frames are bounded to 16 MiB and malformed numeric values, message
types, and delete payloads are ignored or rejected without escaping into the
network handlers.

## License

OpenDial is licensed under the GNU General Public License, version 3. See
[`LICENSE`](LICENSE) for the complete terms.
