#include "serialization.h"
#include "connection.h"
#include "discord_rpc.h"

template <typename T>
void NumberToString(char* dest, T number)
{
    if (!number) {
        *dest++ = '0';
        *dest++ = 0;
        return;
    }
    if (number < 0) {
        *dest++ = '-';
        number = -number;
    }
    char temp[32];
    int place = 0;
    while (number) {
        auto digit = number % 10;
        number = number / 10;
        temp[place++] = '0' + (char)digit;
    }
    for (--place; place >= 0; --place) {
        *dest++ = temp[place];
    }
    *dest = 0;
}

// it's ever so slightly faster to not have to strlen the key
template <typename T>
void WriteKey(JsonWriter& w, T& k)
{
    w.Key(k, sizeof(T) - 1);
}

struct WriteObject {
    JsonWriter& writer;
    WriteObject(JsonWriter& w)
      : writer(w)
    {
        writer.StartObject();
    }
    template <typename T>
    WriteObject(JsonWriter& w, T& name)
      : writer(w)
    {
        WriteKey(writer, name);
        writer.StartObject();
    }
    ~WriteObject() { writer.EndObject(); }
};

struct WriteArray {
    JsonWriter& writer;
    template <typename T>
    WriteArray(JsonWriter& w, T& name)
      : writer(w)
    {
        WriteKey(writer, name);
        writer.StartArray();
    }
    ~WriteArray() { writer.EndArray(); }
};

template <typename T>
void WriteOptionalString(JsonWriter& w, T& k, const char* value)
{
    if (value && value[0]) {
        w.Key(k, sizeof(T) - 1);
        w.String(value);
    }
}

static void JsonWriteNonce(JsonWriter& writer, int nonce)
{
    WriteKey(writer, "nonce");
    char nonceBuffer[32];
    NumberToString(nonceBuffer, nonce);
    writer.String(nonceBuffer);
}

size_t JsonWriteRichPresenceObj(char* dest,
                                size_t maxLen,
                                int nonce,
                                int pid,
                                const DiscordRichPresence* presence)
{
    JsonWriter writer(dest, maxLen);

    {
        WriteObject top(writer);

        JsonWriteNonce(writer, nonce);

        WriteKey(writer, "cmd");
        writer.String("SET_ACTIVITY");

        {
            WriteObject args(writer, "args");

            WriteKey(writer, "pid");
            writer.Int(pid);

            if (presence != nullptr) {
                WriteObject activity(writer, "activity");

                WriteKey(writer, "type");
                writer.Int(presence->type);

                WriteKey(writer, "status_display_type");
                writer.Int(presence->status_display_type);

                WriteOptionalString(writer, "state", presence->state);
                WriteOptionalString(writer, "state_url", presence->stateUrl);

                WriteOptionalString(writer, "details", presence->details);
                WriteOptionalString(writer, "details_url", presence->detailsUrl);

                if (presence->startTimestamp || presence->endTimestamp) {
                    WriteObject timestamps(writer, "timestamps");

                    if (presence->startTimestamp) {
                        WriteKey(writer, "start");
                        writer.Int64(presence->startTimestamp);
                    }

                    if (presence->endTimestamp) {
                        WriteKey(writer, "end");
                        writer.Int64(presence->endTimestamp);
                    }
                }

                if ((presence->largeImageKey && presence->largeImageKey[0]) ||
                    (presence->largeImageText && presence->largeImageText[0]) ||
                    (presence->smallImageKey && presence->smallImageKey[0]) ||
                    (presence->smallImageText && presence->smallImageText[0])) {
                    WriteObject assets(writer, "assets");
                    WriteOptionalString(writer, "large_image", presence->largeImageKey);
                    WriteOptionalString(writer, "large_text", presence->largeImageText);
                    WriteOptionalString(writer, "large_url", presence->largeImageUrl);
                    WriteOptionalString(writer, "small_image", presence->smallImageKey);
                    WriteOptionalString(writer, "small_text", presence->smallImageText);
                    WriteOptionalString(writer, "small_url", presence->smallImageUrl);
                }

                if ((presence->partyId && presence->partyId[0]) || presence->partySize ||
                    presence->partyMax || presence->partyPrivacy) {
                    WriteObject party(writer, "party");
                    WriteOptionalString(writer, "id", presence->partyId);
                    if (presence->partySize && presence->partyMax) {
                        WriteArray size(writer, "size");
                        writer.Int(presence->partySize);
                        writer.Int(presence->partyMax);
                    }

                    if (presence->partyPrivacy) {
                        WriteKey(writer, "privacy");
                        writer.Int(presence->partyPrivacy);
                    }
                }

                if (presence->buttons && presence->buttons[0].label) {
                    WriteArray buttons(writer, "buttons");
                    for (int i = 0; i < DISCORD_PRESENCE_MAX_BUTTON_COUNT; i++) {
                        const auto button = presence->buttons[i];
                        if (!button.label || !button.label[0]) {
                            continue;
                        }
                        WriteObject object(writer);
                        WriteKey(writer, "label");
                        writer.String(button.label);
                        WriteKey(writer, "url");
                        writer.String(button.url);
                    }
                }
                else if ((presence->matchSecret && presence->matchSecret[0]) ||
                         (presence->joinSecret && presence->joinSecret[0]) ||
                         (presence->spectateSecret && presence->spectateSecret[0])) {
                    WriteObject secrets(writer, "secrets");
                    WriteOptionalString(writer, "match", presence->matchSecret);
                    WriteOptionalString(writer, "join", presence->joinSecret);
                    WriteOptionalString(writer, "spectate", presence->spectateSecret);
                }

                writer.Key("instance");
                writer.Bool(presence->instance != 0);
            }
        }
    }

    return writer.Size();
}

size_t JsonWriteHandshakeObj(char* dest, size_t maxLen, int version, const char* applicationId)
{
    JsonWriter writer(dest, maxLen);

    {
        WriteObject obj(writer);
        WriteKey(writer, "v");
        writer.Int(version);
        WriteKey(writer, "client_id");
        writer.String(applicationId);
    }

    return writer.Size();
}

size_t JsonWriteSubscribeCommand(char* dest, size_t maxLen, int nonce, const char* evtName)
{
    JsonWriter writer(dest, maxLen);

    {
        WriteObject obj(writer);

        JsonWriteNonce(writer, nonce);

        WriteKey(writer, "cmd");
        writer.String("SUBSCRIBE");

        WriteKey(writer, "evt");
        writer.String(evtName);
    }

    return writer.Size();
}

size_t JsonWriteUnsubscribeCommand(char* dest, size_t maxLen, int nonce, const char* evtName)
{
    JsonWriter writer(dest, maxLen);

    {
        WriteObject obj(writer);

        JsonWriteNonce(writer, nonce);

        WriteKey(writer, "cmd");
        writer.String("UNSUBSCRIBE");

        WriteKey(writer, "evt");
        writer.String(evtName);
    }

    return writer.Size();
}

size_t JsonWriteJoinReply(char* dest, size_t maxLen, const char* userId, int reply, int nonce)
{
    JsonWriter writer(dest, maxLen);

    {
        WriteObject obj(writer);

        WriteKey(writer, "cmd");
        if (reply == DISCORD_REPLY_YES) {
            writer.String("SEND_ACTIVITY_JOIN_INVITE");
        }
        else {
            writer.String("CLOSE_ACTIVITY_JOIN_REQUEST");
        }

        WriteKey(writer, "args");
        {
            WriteObject args(writer);

            WriteKey(writer, "user_id");
            writer.String(userId);
        }

        JsonWriteNonce(writer, nonce);
    }

    return writer.Size();
}
