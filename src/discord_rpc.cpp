#include "discord_rpc.h"

#include "backoff.h"
#include "discord_register.h"
#include "msg_queue.h"
#include "rpc_connection.h"
#include "serialization.h"

#include <atomic>
#include <chrono>
#include <mutex>

#ifndef DISCORD_DISABLE_IO_THREAD
#include <condition_variable>
#include <thread>
#endif

constexpr size_t MaxMessageSize{16 * 1024};
constexpr size_t MessageQueueSize{8};
constexpr size_t JoinQueueSize{8};
constexpr int MaxIpcConnections{10};

struct QueuedMessage {
    size_t length;
    char buffer[MaxMessageSize];

    void Copy(const QueuedMessage& other)
    {
        length = other.length;
        if (length) {
            memcpy(buffer, other.buffer, length);
        }
    }
};

struct User {
    // snowflake (64bit int), turned into a ascii decimal string, at most 20 chars +1 null
    // terminator = 21
    char userId[32];
    // 32 unicode glyphs is max name size => 4 bytes per glyph in the worst case, +1 for null
    // terminator = 129
    char username[344];
    // 4 decimal digits + 1 null terminator = 5
    char discriminator[8];
    // optional 'a_' + md5 hex digest (32 bytes) + null terminator = 35
    char avatar[128];
    // Rounded way up because I'm paranoid about games breaking from future changes in these sizes
};

// Per-connection state: one slot per IPC pipe (discord-ipc-0 through discord-ipc-9).
struct PerConnectionState {
    RpcConnection* rpc{nullptr};
    User connectedUser{};
    std::atomic_bool wasJustConnected{false};
    std::atomic_bool wasJustDisconnected{false};
    std::atomic_bool gotErrorMessage{false};
    int lastErrorCode{0};
    char lastErrorMessage[256]{};
    int lastDisconnectErrorCode{0};
    char lastDisconnectErrorMessage[256]{};
    std::atomic_bool updatePresence{false};
    QueuedMessage queuedPresence{};
    std::mutex presenceMutex;
    Backoff reconnectTimeMs{500, 10000};
    std::chrono::system_clock::time_point nextConnect{};
};

static PerConnectionState Connections[MaxIpcConnections];
static int NumConnections{0};

static DiscordEventHandlers QueuedHandlers{};
static DiscordEventHandlers Handlers{};
static std::atomic_bool WasJoinGame{false};
static std::atomic_bool WasSpectateGame{false};
static char JoinGameSecret[256];
static char SpectateGameSecret[256];
static std::atomic_bool GotAnyErrorMessage{false};
static int LastErrorCode{0};
static char LastErrorMessage[256];
static std::mutex HandlerMutex;
static MsgQueue<QueuedMessage, MessageQueueSize> SendQueue;
static MsgQueue<User, JoinQueueSize> JoinAskQueue;

static int Pid{0};
static int Nonce{1};

#ifndef DISCORD_DISABLE_IO_THREAD
static void Discord_UpdateConnection(void);
class IoThreadHolder {
private:
    std::atomic_bool keepRunning{true};
    std::mutex waitForIOMutex;
    std::condition_variable waitForIOActivity;
    std::thread ioThread;

public:
    void Start()
    {
        keepRunning.store(true);
        ioThread = std::thread([&]() {
            const std::chrono::duration<int64_t, std::milli> maxWait{500LL};
            Discord_UpdateConnection();
            while (keepRunning.load()) {
                std::unique_lock<std::mutex> lock(waitForIOMutex);
                waitForIOActivity.wait_for(lock, maxWait);
                Discord_UpdateConnection();
            }
        });
    }

    void Notify() { waitForIOActivity.notify_all(); }

    void Stop()
    {
        keepRunning.exchange(false);
        Notify();
        if (ioThread.joinable()) {
            ioThread.join();
        }
    }

    ~IoThreadHolder() { Stop(); }
};
#else
class IoThreadHolder {
public:
    void Start() {}
    void Stop() {}
    void Notify() {}
};
#endif // DISCORD_DISABLE_IO_THREAD
static IoThreadHolder* IoThread{nullptr};

static void SignalIOActivity()
{
    if (IoThread != nullptr) {
        IoThread->Notify();
    }
}

static bool RegisterForEvent(const char* evtName)
{
    auto qmessage = SendQueue.GetNextAddMessage();
    if (qmessage) {
        qmessage->length =
          JsonWriteSubscribeCommand(qmessage->buffer, sizeof(qmessage->buffer), Nonce++, evtName);
        SendQueue.CommitAdd();
        SignalIOActivity();
        return true;
    }
    return false;
}

static bool DeregisterForEvent(const char* evtName)
{
    auto qmessage = SendQueue.GetNextAddMessage();
    if (qmessage) {
        qmessage->length =
          JsonWriteUnsubscribeCommand(qmessage->buffer, sizeof(qmessage->buffer), Nonce++, evtName);
        SendQueue.CommitAdd();
        SignalIOActivity();
        return true;
    }
    return false;
}

#ifdef DISCORD_DISABLE_IO_THREAD
extern "C" DISCORD_EXPORT void Discord_UpdateConnection(void)
#else
static void Discord_UpdateConnection(void)
#endif
{
    if (NumConnections == 0) {
        return;
    }

    // Per-connection: reconnect, read events, write presence.
    for (int i = 0; i < NumConnections; ++i) {
        auto& cs = Connections[i];
        if (!cs.rpc) {
            continue;
        }

        if (!cs.rpc->IsOpen()) {
            if (std::chrono::system_clock::now() >= cs.nextConnect) {
                cs.nextConnect = std::chrono::system_clock::now() +
                  std::chrono::duration<int64_t, std::milli>{cs.reconnectTimeMs.nextDelay()};
                cs.rpc->Open();
            }
        }
        else {
            // reads
            for (;;) {
                JsonDocument message;

                if (!cs.rpc->Read(message)) {
                    break;
                }

                const char* evtName = GetStrMember(&message, "evt");
                const char* nonce = GetStrMember(&message, "nonce");

                if (nonce) {
                    // in responses only -- should use to match up response when needed.

                    if (evtName && strcmp(evtName, "ERROR") == 0) {
                        auto data = GetObjMember(&message, "data");
                        LastErrorCode = GetIntMember(data, "code");
                        StringCopy(LastErrorMessage, GetStrMember(data, "message", ""));
                        GotAnyErrorMessage.store(true);
                    }
                }
                else {
                    // should have evt == name of event, optional data
                    if (evtName == nullptr) {
                        continue;
                    }

                    auto data = GetObjMember(&message, "data");

                    if (strcmp(evtName, "ACTIVITY_JOIN") == 0) {
                        auto secret = GetStrMember(data, "secret");
                        if (secret) {
                            StringCopy(JoinGameSecret, secret);
                            WasJoinGame.store(true);
                        }
                    }
                    else if (strcmp(evtName, "ACTIVITY_SPECTATE") == 0) {
                        auto secret = GetStrMember(data, "secret");
                        if (secret) {
                            StringCopy(SpectateGameSecret, secret);
                            WasSpectateGame.store(true);
                        }
                    }
                    else if (strcmp(evtName, "ACTIVITY_JOIN_REQUEST") == 0) {
                        auto user = GetObjMember(data, "user");
                        auto userId = GetStrMember(user, "id");
                        auto username = GetStrMember(user, "username");
                        auto avatar = GetStrMember(user, "avatar");
                        auto joinReq = JoinAskQueue.GetNextAddMessage();
                        if (userId && username && joinReq) {
                            StringCopy(joinReq->userId, userId);
                            StringCopy(joinReq->username, username);
                            auto discriminator = GetStrMember(user, "discriminator");
                            if (discriminator) {
                                StringCopy(joinReq->discriminator, discriminator);
                            }
                            if (avatar) {
                                StringCopy(joinReq->avatar, avatar);
                            }
                            else {
                                joinReq->avatar[0] = 0;
                            }
                            JoinAskQueue.CommitAdd();
                        }
                    }
                }
            }

            // write presence to this connection if needed
            if (cs.updatePresence.exchange(false) && cs.queuedPresence.length) {
                QueuedMessage local;
                {
                    std::lock_guard<std::mutex> guard(cs.presenceMutex);
                    local.Copy(cs.queuedPresence);
                }
                if (!cs.rpc->Write(local.buffer, local.length)) {
                    // requeue for retry on next cycle
                    cs.updatePresence.store(true);
                }
            }
        }
    }

    // Drain the send queue and broadcast each message to all open connections.
    while (SendQueue.HavePendingSends()) {
        auto qmessage = SendQueue.GetNextSendMessage();
        QueuedMessage local;
        local.Copy(*qmessage);
        SendQueue.CommitSend();
        for (int i = 0; i < NumConnections; ++i) {
            if (Connections[i].rpc && Connections[i].rpc->IsOpen()) {
                Connections[i].rpc->Write(local.buffer, local.length);
            }
        }
    }
}

extern "C" DISCORD_EXPORT void Discord_Initialize(const char* applicationId,
                                                  DiscordEventHandlers* handlers,
                                                  int autoRegister,
                                                  const char* optionalSteamId)
{
    IoThread = new (std::nothrow) IoThreadHolder();
    if (IoThread == nullptr) {
        return;
    }

    if (autoRegister) {
        if (optionalSteamId && optionalSteamId[0]) {
            Discord_RegisterSteamGame(applicationId, optionalSteamId);
        }
        else {
            Discord_Register(applicationId, nullptr);
        }
    }

    Pid = GetProcessId();

    {
        std::lock_guard<std::mutex> guard(HandlerMutex);

        if (handlers) {
            QueuedHandlers = *handlers;
        }
        else {
            QueuedHandlers = {};
        }

        Handlers = {};
    }

    if (NumConnections > 0) {
        return;
    }

    NumConnections = MaxIpcConnections;
    for (int i = 0; i < MaxIpcConnections; ++i) {
        Connections[i].rpc = RpcConnection::CreateForPipe(applicationId, i);

        Connections[i].rpc->onConnect = [i](JsonDocument& readyMessage) {
            Discord_UpdateHandlers(&QueuedHandlers);
            if (Connections[i].queuedPresence.length > 0) {
                Connections[i].updatePresence.store(true);
                SignalIOActivity();
            }
            auto data = GetObjMember(&readyMessage, "data");
            auto user = GetObjMember(data, "user");
            auto userId = GetStrMember(user, "id");
            auto username = GetStrMember(user, "username");
            auto avatar = GetStrMember(user, "avatar");
            if (userId && username) {
                StringCopy(Connections[i].connectedUser.userId, userId);
                StringCopy(Connections[i].connectedUser.username, username);
                auto discriminator = GetStrMember(user, "discriminator");
                if (discriminator) {
                    StringCopy(Connections[i].connectedUser.discriminator, discriminator);
                }
                if (avatar) {
                    StringCopy(Connections[i].connectedUser.avatar, avatar);
                }
                else {
                    Connections[i].connectedUser.avatar[0] = 0;
                }
            }
            Connections[i].wasJustConnected.store(true);
            Connections[i].reconnectTimeMs.reset();
        };

        Connections[i].rpc->onDisconnect = [i](int err, const char* message) {
            Connections[i].lastDisconnectErrorCode = err;
            StringCopy(Connections[i].lastDisconnectErrorMessage, message);
            Connections[i].wasJustDisconnected.store(true);
        };
    }

    IoThread->Start();
}

extern "C" DISCORD_EXPORT bool Discord_Connected(void)
{
    for (int i = 0; i < NumConnections; ++i) {
        if (Connections[i].rpc && Connections[i].rpc->IsOpen()) {
            return true;
        }
    }
    return false;
}

extern "C" DISCORD_EXPORT void Discord_Shutdown(void)
{
    if (NumConnections == 0) {
        return;
    }
    for (int i = 0; i < NumConnections; ++i) {
        if (Connections[i].rpc) {
            Connections[i].rpc->onConnect = nullptr;
            Connections[i].rpc->onDisconnect = nullptr;
        }
    }
    Handlers = {};
    for (int i = 0; i < NumConnections; ++i) {
        Connections[i].queuedPresence.length = 0;
        Connections[i].updatePresence.store(false);
    }
    if (IoThread != nullptr) {
        IoThread->Stop();
        delete IoThread;
        IoThread = nullptr;
    }
    for (int i = 0; i < NumConnections; ++i) {
        if (Connections[i].rpc) {
            RpcConnection::Destroy(Connections[i].rpc);
        }
    }
    NumConnections = 0;
}

extern "C" DISCORD_EXPORT void Discord_UpdatePresence(const DiscordRichPresence* presence)
{
    for (int i = 0; i < NumConnections; ++i) {
        std::lock_guard<std::mutex> guard(Connections[i].presenceMutex);
        Connections[i].queuedPresence.length = JsonWriteRichPresenceObj(
          Connections[i].queuedPresence.buffer,
          sizeof(Connections[i].queuedPresence.buffer),
          Nonce++, Pid, presence);
        Connections[i].updatePresence.store(true);
    }
    SignalIOActivity();
}

extern "C" DISCORD_EXPORT void Discord_ClearPresence(void)
{
    Discord_UpdatePresence(nullptr);
}

extern "C" DISCORD_EXPORT void Discord_UpdatePresenceForUser(const char* userId,
                                                             const DiscordRichPresence* presence)
{
    if (!userId) {
        return;
    }
    for (int i = 0; i < NumConnections; ++i) {
        if (Connections[i].rpc && strcmp(Connections[i].connectedUser.userId, userId) == 0) {
            std::lock_guard<std::mutex> guard(Connections[i].presenceMutex);
            Connections[i].queuedPresence.length = JsonWriteRichPresenceObj(
              Connections[i].queuedPresence.buffer,
              sizeof(Connections[i].queuedPresence.buffer),
              Nonce++, Pid, presence);
            Connections[i].updatePresence.store(true);
            SignalIOActivity();
            return;
        }
    }
}

extern "C" DISCORD_EXPORT void Discord_ClearPresenceForUser(const char* userId)
{
    Discord_UpdatePresenceForUser(userId, nullptr);
}

extern "C" DISCORD_EXPORT void Discord_Respond(const char* userId, /* DISCORD_REPLY_ */ int reply)
{
    // if no connections are open, let's not batch up stale messages for later
    if (!Discord_Connected()) {
        return;
    }
    auto qmessage = SendQueue.GetNextAddMessage();
    if (qmessage) {
        qmessage->length =
          JsonWriteJoinReply(qmessage->buffer, sizeof(qmessage->buffer), userId, reply, Nonce++);
        SendQueue.CommitAdd();
        SignalIOActivity();
    }
}

extern "C" DISCORD_EXPORT void Discord_RunCallbacks(void)
{
    // Note on some weirdness: internally we might connect, get other signals, disconnect any number
    // of times inbetween calls here. Externally, we want the sequence to seem sane, so any other
    // signals are book-ended by calls to ready and disconnect.

    if (NumConnections == 0) {
        return;
    }

    bool wasDisconnected[MaxIpcConnections]{};
    bool isConnected[MaxIpcConnections]{};
    for (int i = 0; i < NumConnections; ++i) {
        wasDisconnected[i] = Connections[i].wasJustDisconnected.exchange(false);
        isConnected[i] = Connections[i].rpc && Connections[i].rpc->IsOpen();
    }

    // If a connection is currently open, fire its disconnect cb first (before other signals).
    for (int i = 0; i < NumConnections; ++i) {
        if (isConnected[i] && wasDisconnected[i]) {
            std::lock_guard<std::mutex> guard(HandlerMutex);
            if (Handlers.disconnected) {
                Handlers.disconnected(Connections[i].lastDisconnectErrorCode,
                                      Connections[i].lastDisconnectErrorMessage);
            }
        }
    }

    // Fire ready for each newly connected user.
    for (int i = 0; i < NumConnections; ++i) {
        if (Connections[i].wasJustConnected.exchange(false)) {
            std::lock_guard<std::mutex> guard(HandlerMutex);
            if (Handlers.ready) {
                DiscordUser du{Connections[i].connectedUser.userId,
                               Connections[i].connectedUser.username,
                               Connections[i].connectedUser.discriminator,
                               Connections[i].connectedUser.avatar};
                Handlers.ready(&du);
            }
        }
    }

    if (GotAnyErrorMessage.exchange(false)) {
        std::lock_guard<std::mutex> guard(HandlerMutex);
        if (Handlers.errored) {
            Handlers.errored(LastErrorCode, LastErrorMessage);
        }
    }

    if (WasJoinGame.exchange(false)) {
        std::lock_guard<std::mutex> guard(HandlerMutex);
        if (Handlers.joinGame) {
            Handlers.joinGame(JoinGameSecret);
        }
    }

    if (WasSpectateGame.exchange(false)) {
        std::lock_guard<std::mutex> guard(HandlerMutex);
        if (Handlers.spectateGame) {
            Handlers.spectateGame(SpectateGameSecret);
        }
    }

    // Right now this batches up any requests and sends them all in a burst; I could imagine a world
    // where the implementer would rather sequentially accept/reject each one before the next invite
    // is sent. I left it this way because I could also imagine wanting to process these all and
    // maybe show them in one common dialog and/or start fetching the avatars in parallel, and if
    // not it should be trivial for the implementer to make a queue themselves.
    while (JoinAskQueue.HavePendingSends()) {
        auto req = JoinAskQueue.GetNextSendMessage();
        {
            std::lock_guard<std::mutex> guard(HandlerMutex);
            if (Handlers.joinRequest) {
                DiscordUser du{req->userId, req->username, req->discriminator, req->avatar};
                Handlers.joinRequest(&du);
            }
        }
        JoinAskQueue.CommitSend();
    }

    // If a connection is not open, fire its disconnect cb last.
    for (int i = 0; i < NumConnections; ++i) {
        if (!isConnected[i] && wasDisconnected[i]) {
            std::lock_guard<std::mutex> guard(HandlerMutex);
            if (Handlers.disconnected) {
                Handlers.disconnected(Connections[i].lastDisconnectErrorCode,
                                      Connections[i].lastDisconnectErrorMessage);
            }
        }
    }
}

extern "C" DISCORD_EXPORT void Discord_UpdateHandlers(DiscordEventHandlers* newHandlers)
{
    if (newHandlers) {
#define HANDLE_EVENT_REGISTRATION(handler_name, event)              \
    if (!Handlers.handler_name && newHandlers->handler_name) {      \
        RegisterForEvent(event);                                    \
    }                                                               \
    else if (Handlers.handler_name && !newHandlers->handler_name) { \
        DeregisterForEvent(event);                                  \
    }

        std::lock_guard<std::mutex> guard(HandlerMutex);
        HANDLE_EVENT_REGISTRATION(joinGame, "ACTIVITY_JOIN")
        HANDLE_EVENT_REGISTRATION(spectateGame, "ACTIVITY_SPECTATE")
        HANDLE_EVENT_REGISTRATION(joinRequest, "ACTIVITY_JOIN_REQUEST")

#undef HANDLE_EVENT_REGISTRATION

        Handlers = *newHandlers;
    }
    else {
        std::lock_guard<std::mutex> guard(HandlerMutex);
        Handlers = {};
    }
    return;
}
