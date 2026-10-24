# driveclub-server

Driveclub custom server (Qt 6, HTTP). It answers the EGS bootstrap calls (below); everything else is
answered `404` and traced in full, so you can see what the game sends.

## Build and run

Needs Qt 6 (Core, Network). protobuf **v21.12** comes as a git submodule in `externals/` and is
built with the server. That's the last release without an abseil dependency, so the build stays
small. Set it up once:

```sh
git submodule add -b 21.x https://github.com/protocolbuffers/protobuf.git externals/protobuf
git -C externals/protobuf checkout v21.12      # f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c
git add externals/protobuf .gitmodules
```

Later clones: `git clone --recursive <repo>`, or `git submodule update --init --recursive`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/driveclubsrv
```

The first run writes `config.json` in the working folder:

```json
{
    "listen": { "address": "0.0.0.0", "port": 8080 },
    "base_path": "",
    "log": { "file": "server.log", "max_size_mb": 50, "trace": true, "trace_body_bytes": 65536 },
    "egs": { "server_identifier": "dc-community", "version": "1" }
}
```

| Key | Meaning |
| --- | --- |
| `listen.address`, `listen.port` | Where the server listens |
| `base_path` | Prefix stripped before endpoints are matched. Keep it empty: EGS (`/egs`, `/messaging`), the OTG hub (`/titles/...`), OTG login and scores all use different roots. |
| `log.file`, `log.max_size_mb` | Log file, rewritten from the start when it reaches this size |
| `log.trace` | `true`: every request and answer is logged in full. `false`: one line per request and per answer. |
| `log.trace_body_bytes` | How much of each body to dump; `0` = all of it |
| `egs.server_identifier` | Sent in `systemService.getVersion`. Every `OTG_Service_*` value in the TSS must contain it. |
| `egs.version` | Sent in `systemService.getVersion` (stored by the game, not checked) |
| `otg.base_url` | OTG login base (`auth` in the domain list) and service base (`config.gateway`). Empty = `http://` + the request's Host header, i.e. this server. |
| `otg.domain_id` | `id` in the domain list. Must contain the TSS `OTG_Domain` value. |
| `otg.zone` | `zone` in the OTG login answer |

## OTG

1. `GET /titles/driveclub/domains` (the game's hub URL is fixed). The answer is
   `[{"id":"driveclub","auth":"<base>"}]`. The game takes the first entry whose `id` contains the
   TSS `OTG_Domain`.
2. `GET <auth>/login/driveclub?provider=psn&code=<NpAuth code>`, or `/xlogin/driveclub?user=<name>`
   with `OTG_Login_Type fake`. The answer is
   `{"token":"…","config":{"gateway":"<base>"},"zone":"eu"}`. The token is expected back as
   `Authorization: Bearer`, and the gateway is probably the base for `/scores/...`, `/ghosts/...` and
   so on. The code is not checked against shadNet yet.

`GET /egs/messaging` is polled right after the EGS login, without a signature or session. Its answer
format is not known yet, so it gets an empty `200`.

## EGS protocol

The game calls `POST /egs/binary/<service>/<method>` with protobuf bodies. It embeds protobuf 2.4.1
but no EGS schema: the server provides it.

1. `indexService.getMessageDescriptors`, with an empty body. The answer is a serialized
   `google.protobuf.FileDescriptorProto` describing every message (`egsDescriptor()` in `src/egs.cpp`).
2. `indexService.getSvcEndpointList`. The answer is a `SvcEndpointDescriptorList`: for each method,
   `service`, `serviceMethod`, `requestMessage`, `responseMessage`, `httpMethod`, `secure`,
   `initialTokens`, `maxTokens`, `tokensPerMinute` and an optional `available`.
3. The listed methods, starting with `systemService.getVersion` (`version`, `serverIdentifier`).
4. `sessionService.oauthLogin` (and `oauthRefresh`). Request fields: `authCode`, `clientVersion`,
   `name` and `fbAuthToken` (strings), `clientSessionData` (bytes), `clientNatIdentifier` (int),
   `psPlus` and `trial` (bools). The answer is `sessionkey { id, accountId, username }`, plus an
   `X-SessionKey: <id>` header. `retryHint` is read when a login fails. The server gives each
   player a session key and a stable `accountId` derived from the name.

The game reads and writes messages by field name. Field numbers are the server's choice, but names
must match what the game asks for, and so must their kind: strings for strings, bools for bools,
numbers for ints.

All EGS messages are defined in **`proto/egs.proto`**, which protoc compiles at build time. The
schema served by `getMessageDescriptors` is generated from that same file, so the game and the
server can't disagree. The trace prints EGS bodies of known methods with field names, e.g.

```
#3   ~ OAuthLoginRequest {
#3   ~   clientVersion: "CV_bM_tP_p128_x3"
#3   ~   name: "shadow"
#3   ~ }
```

Bodies of methods that are not in `egs.proto` yet are shown by field number. Keep `egs.proto`
`proto2` and without a `package`, because the game looks messages up by bare name.

The game embeds protobuf **2.4.1**, while the server builds with a current protobuf. That's fine:
the wire format hasn't changed, and the schema sent by `getMessageDescriptors` is rebuilt with only
the descriptor fields 2.4.1 knows (names, numbers, labels, types, type names, defaults, nested
types, enums). Newer additions such as `syntax`, `edition`, `json_name`, options and features are
dropped (`egsDescriptor()` in `src/egs.cpp`).

Signing:

- Requests: `X-Request-Signature = base64(HMAC-SHA1("ca91f51f-a7f5-4b95-814a-5796cfff586c",
  method + sessionKey + body))`, e.g. `getMessageDescriptors` with an empty body gives
  `/XKVKkZ7V8g6Q7QSVmVvI1RKXfI=`. The trace checks it on every request.
- Answers: `X-Response-Signature` is computed the same way over the answer body. The game checks it
  once it has a session. This server signs every EGS answer.
- The session key is whatever the server last sent in an `X-SessionKey` answer header. The game
  never sends it back, so the server finds a request's session by trying each issued key against
  the signature (the trace shows `session of <name>`). Sessions are kept in memory, so after a
  server restart the game's signatures stop matching until it logs in again.

Other methods the game calls (from the eboot): `sessionService.plainLogin` and `photoLogin`; `telemetryService.record`, `updateContext` and `updateSession`;
`lobbyService.createLobby` and `joinLobby`; `storageService.getRemoteUrl` and `createBob`.
To add one: define its request and response messages in `proto/egs.proto`, then add an entry to
`endpoints()` in `src/egs.cpp`:

```cpp
endpoint<RecordRequest, Empty>("telemetryService", "record", true, true,
    [](Context& ctx, const RecordRequest& req, Empty& resp) { /* ... */ }),
```

The entry is listed in `getSvcEndpointList` automatically, with the message names taken from the
types.

## Trace log

Each request gets an id (`#12`) that is shared by its `>>>` request lines and `<<<` answer lines, and
a label naming the Driveclub service it belongs to (from the URLs in the eboot):

| Label | Paths |
| --- | --- |
| `EGS index`, `EGS direct <service>.<method>`, `EGS binary`, `EGS messaging` | `/egs`, `/egs/direct/...`, `/egs/binary`, `/messaging` |
| `OTG hub domains` | `/titles/driveclub/domains` |
| `OTG login (psn)`, `OTG login (fake)` | `/login/driveclub?provider=psn&code=`, `/xlogin/driveclub?user=` |
| `OTG scores/common`, `.../club`, `.../bikes`, `scores/fame`, `scores/challenges`, `scores/faceoffs`, `scores/tour` | `/scores/...` |
| `OTG ghosts`, `OTG club`, `OTG player` | `/ghosts/...`, `/club/...`, `/player/...` |
| `WWSGA telemetry`, `telemetry` | `/games/<id>/telemetry`, `/telemetry` |
| `PSN ... API` | `/v1/...` (normally answered by shadNet) |
| `UNKNOWN endpoint` | anything else: worth a look |

A request trace contains:

- all headers, sorted. Headers the game uses are marked `*`: `Authorization`, `X-SessionKey`,
  `X-Request-Signature`, `X-Api-Version`, and the `X-Response-Signature` and `X-Service-Error*`
  answer headers.
- the decoded query parameters.
- the EGS `serviceMethod`, when the body is EGS JSON.
- the body: JSON pretty-printed, text as-is, anything else (protobuf, ghosts) as a hex dump.

An answer trace has the status, the time the server took, its headers and body. Answers with a status
of 400 or higher are logged as warnings.

Connections are logged under `conn:` (opened, closed with lifetime and request count, idle close
after 60 s, malformed requests). A **TLS handshake** on the port is reported as such. The game uses
https for EGS (`gmServer`) and the OTG hub, so put a TLS-terminating proxy in front of this server.

Categories can also be filtered with Qt's `QT_LOGGING_RULES`, e.g. `QT_LOGGING_RULES="conn.info=false"`.

## Adding an endpoint

Handle it in `src/handler.cpp` (`endpoint` is the path without `base_path` and the leading `/`).
Every answer passes through the responder in `main.cpp`, so new endpoints are traced automatically.
Extra answer headers go in `HttpResponse::headers`.
