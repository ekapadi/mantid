# Sub-spec 01 — UDS Transport

**Part of:** [`00-index.md`](00-index.md) (master)  
**Agent read-order:** Read after §0 in `00-index.md`.

---

## 3. Transport: Unix-domain socket

### 3.1 Rationale

- TCP loopback (`127.0.0.1`) is blocked or filtered on several Mantid CI
  runners — historically the reason the legacy suite was disabled.
- The SNS production deployment is Linux-only, so a Linux-only integration
  test is appropriate. SNS-listener testing on Windows has no operational
  value.
- The listener already supports UDS through its `Poco::Net::SocketAddress`
  argument (`SNSLiveEventDataListener.cpp:141-156`); no production change is
  required.

### 3.2 Platform guard

Wrap the entire body of `SNSLiveEventDataListenerTest.h` (after the `#pragma
once`) and the body of `MockSMSServer.{h,cpp}` in:

```cpp
#ifndef _WIN32
  // ... entire suite ...
#else
  // Suite is intentionally empty on Windows — see file header.
  class SNSLiveEventDataListenerTest : public CxxTest::TestSuite {};
#endif
```

Precedent: search the framework for `#ifndef _WIN32` in `Framework/*/test/`
for existing skipped-on-Windows suites.

### 3.3 Socket path

Use **`Poco::TemporaryFile`** to obtain a unique, tempdir-respecting path.
This is the framework-wide convention (`SaveGSSTest`, `SaveGDATest`,
`SaveOpenGenieAsciiTest`, `DownloadFileTest`, `InternetHelperTest`, etc.).
It internally uses `Poco::Path::temp()`, which honours `$TMPDIR` on Unix,
and provides RAII deletion.

```cpp
Poco::TemporaryFile m_sockFileHandle; // owns the path lifetime
const std::string m_sockPath = m_sockFileHandle.path();
```

`sun_path` length guard (defence in depth — `Poco::Path::temp()` is normally
short enough, but `$TMPDIR` can be set arbitrarily):

```cpp
if (m_sockPath.size() >= 100) {
    TS_SKIP("UDS path too long for sun_path on this platform: " + m_sockPath);
}
```

The `TemporaryFile` is created but the file is removed (`std::filesystem::remove`)
**before** the server binds, because `bind()` requires the path to not exist.

### 3.4 Correct Poco UDS `SocketAddress` construction

> **IMPORTANT — the `"unix:path"` string syntax is not valid Poco.**
>
> Poco's `SocketAddress` string constructor parses `"host:port"` for TCP.
> It does **not** recognise a `"unix:/path"` or `"unix:path"` prefix.
> Attempting `Poco::Net::SocketAddress("unix:/tmp/sock")` will throw or
> misinterpret the string as a hostname.

**The correct constructor** for a Unix-domain socket address is the
two-argument form that accepts an `AddressFamily` enum:

```cpp
Poco::Net::SocketAddress(Poco::Net::AddressFamily::UNIX_LOCAL, m_sockPath)
```

#### 3.4.1 Server side (`MockSMSServer`)

```cpp
// In MockSMSServer::start():
m_listenSocket = Poco::Net::ServerSocket(
    Poco::Net::SocketAddress(Poco::Net::AddressFamily::UNIX_LOCAL, m_path),
    /*backlog=*/1);
```

#### 3.4.2 Client side (the listener)

The `SNSLiveEventDataListener::connect(Poco::Net::SocketAddress)` method at
`SNSLiveEventDataListener.cpp:133-175` accepts a pre-built `SocketAddress`.
When the caller passes a **non-empty** address, the method uses that address
directly (line 158: `m_socket.connect(address)`).  When the caller passes
`Poco::Net::SocketAddress()` (the default-constructed, empty address), it
falls through to the `testAddress` config string (line 149), which only
supports TCP `"host:port"` format.

Therefore, in every test, pass the UDS address explicitly:

```cpp
// In each test method, after m_server->start() and before m_listener->start():
Poco::Net::SocketAddress udsAddr(
    Poco::Net::AddressFamily::UNIX_LOCAL, m_sockPath);
TS_ASSERT(m_listener->connect(udsAddr));
```

Do **not** use the `SNSLiveEventDataListener.testAddress` config key for
UDS paths.  That mechanism only works for TCP addresses.

#### 3.4.3 Why not patch `connect()` to accept a string path?

That would be a production code change, which is out of scope (§0 item 8).
The existing `connect(const Poco::Net::SocketAddress&)` signature already
handles UDS correctly via the enum constructor above.

### 3.5 Connection sequence in tests

Every test that exercises the full listener lifecycle must follow this order:

1. Build and queue the server script (see [`02-mock-sms-server.md`](02-mock-sms-server.md)).
2. Call `m_server->start()` — the server binds and begins `accept()` on its
   background thread.
3. Construct `m_listener = std::make_unique<SNSLiveEventDataListener>()`.
4. Call `m_listener->connect(Poco::Net::SocketAddress(Poco::Net::AddressFamily::UNIX_LOCAL, m_sockPath))`.
   Assert the return value is `true`.
5. Call `m_listener->start(startTime)` — this launches the listener's
   background thread.
6. Drive the test scenario (see [`04-test-scenarios.md`](04-test-scenarios.md)).

The `m_server->start()` call must precede `connect()` because `connect()` is
a **blocking** call (`SNSLiveEventDataListener.cpp:152`) — it blocks until
the server accepts or a connection error occurs.
