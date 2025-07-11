#pragma once
#include <stdint.h>

// clang-format off

#if defined(DISCORD_DYNAMIC_LIB)
#  if defined(_WIN32)
#    if defined(DISCORD_BUILDING_SDK)
#      define DISCORD_EXPORT __declspec(dllexport)
#    else
#      define DISCORD_EXPORT __declspec(dllimport)
#    endif
#  else
#    define DISCORD_EXPORT __attribute__((visibility("default")))
#  endif
#else
#  define DISCORD_EXPORT
#endif

// clang-format on

#ifdef __cplusplus
extern "C" {
#endif

#define DISCORD_PRESENCE_MAX_KEY_LENGTH 256
#define DISCORD_PRESENCE_MIN_TEXT_LENGTH 2
#define DISCORD_PRESENCE_MAX_TEXT_LENGTH 128
#define DISCORD_PRESENCE_MIN_BUTTON_LABEL_LENGTH 1
#define DISCORD_PRESENCE_MAX_BUTTON_LABEL_LENGTH 32
#define DISCORD_PRESENCE_MAX_BUTTON_COUNT 2
#define DISCORD_PRESENCE_MAX_URL_LENGTH 256

typedef struct DiscordButton {
    const char* label; /* FIXME limit? */
    const char* url;   /* max 256 bytes */
} DiscordButton;

typedef enum DiscordActivityType {
    DiscordActivityType_Playing = 0, // the default
    // DiscordActivityType_Streaming = 1, // not allowed
    DiscordActivityType_Listening = 2,
    DiscordActivityType_Watching = 3,
    // DiscordActivityType_Custom = 4, // not allowed
    DiscordActivityType_Competing = 5
} DiscordActivityType;

typedef enum DiscordStatusDisplayType {
    DiscordStatusDisplayType_Name = 0, // the default
    DiscordStatusDisplayType_State = 1,
    DiscordStatusDisplayType_Details = 2
} DiscordStatusDisplayType;

typedef struct DiscordRichPresence {
    DiscordActivityType type;
    DiscordStatusDisplayType status_display_type;
    const char* state;      // text
    const char* stateUrl;   // url
    const char* details;    // text
    const char* detailsUrl; // url
    int64_t startTimestamp;
    int64_t endTimestamp;
    const char* largeImageKey;  // key
    const char* largeImageText; // text
    const char* largeImageUrl;  // url
    const char* smallImageKey;  // key
    const char* smallImageText; // text
    const char* smallImageUrl;  // url
    const char* partyId;        // max 128 bytes
    int partySize;
    int partyMax;
    int partyPrivacy;
    const char* matchSecret;    // max 128 bytes
    const char* joinSecret;     // max 128 bytes
    const char* spectateSecret; // max 128 bytes
    int8_t instance;
    DiscordButton buttons[DISCORD_PRESENCE_MAX_BUTTON_COUNT];
} DiscordRichPresence;

typedef struct DiscordUser {
    const char* userId;
    const char* username;
    const char* discriminator;
    const char* avatar;
} DiscordUser;

typedef struct DiscordEventHandlers {
    void (*ready)(const DiscordUser* request);
    void (*disconnected)(int errorCode, const char* message);
    void (*errored)(int errorCode, const char* message);
    void (*joinGame)(const char* joinSecret);
    void (*spectateGame)(const char* spectateSecret);
    void (*joinRequest)(const DiscordUser* request);
} DiscordEventHandlers;

#define DISCORD_REPLY_NO 0
#define DISCORD_REPLY_YES 1
#define DISCORD_REPLY_IGNORE 2
#define DISCORD_PARTY_PRIVATE 0
#define DISCORD_PARTY_PUBLIC 1

DISCORD_EXPORT void Discord_Initialize(const char* applicationId,
                                       DiscordEventHandlers* handlers,
                                       int autoRegister,
                                       const char* optionalSteamId);
DISCORD_EXPORT bool Discord_Connected(void);
DISCORD_EXPORT void Discord_Shutdown(void);

/* checks for incoming messages, dispatches callbacks */
DISCORD_EXPORT void Discord_RunCallbacks(void);

/* If you disable the lib starting its own io thread, you'll need to call this from your own */
#ifdef DISCORD_DISABLE_IO_THREAD
DISCORD_EXPORT void Discord_UpdateConnection(void);
#endif

DISCORD_EXPORT void Discord_UpdatePresence(const DiscordRichPresence* presence);
DISCORD_EXPORT void Discord_ClearPresence(void);

DISCORD_EXPORT void Discord_Respond(const char* userid, /* DISCORD_REPLY_ */ int reply);

DISCORD_EXPORT void Discord_UpdateHandlers(DiscordEventHandlers* handlers);

#ifdef __cplusplus
} /* extern "C" */
#endif
