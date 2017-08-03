/*
    This is a simple example in C of using the rich presence API asyncronously.
*/

#define _CRT_SECURE_NO_WARNINGS /* thanks Microsoft */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "discord-rpc.h"

static const char* APPLICATION_ID = "338030514596216832";
static int FrustrationLevel = 0;
static int64_t StartTime;

static void updateDiscordPresence()
{
    char buffer[256];
    DiscordRichPresence discordPresence;
    memset(&discordPresence, 0, sizeof(discordPresence));
    discordPresence.state = "West of House";
    sprintf(buffer, "Frustration level: %d", FrustrationLevel);
    discordPresence.details = buffer;
    discordPresence.startTimestamp = StartTime;
    discordPresence.endTimestamp = time(0) + 5 * 60;
    discordPresence.partyId = "party1234";
    discordPresence.partySize = 1;
    discordPresence.partyMax = 6;
    discordPresence.matchSecret = "xyzzy";
    discordPresence.joinSecret = "join";
    discordPresence.spectateSecret = "look";
    discordPresence.instance = 0;
    Discord_UpdatePresence(&discordPresence);
}

static void handleDiscordReady()
{
    printf("\nDiscord: ready\n");
}

static void handleDiscordDisconnected(int errcode, const char* message)
{
    printf("\nDiscord: disconnected (%d: %s)\n", errcode, message);
}

static void handleDiscordError(int errcode, const char* message)
{
    printf("\nDiscord: error (%d: %s)\n", errcode, message);
}

static void handleDiscordJoin(const char* secret)
{
    printf("\nDiscord: join (%s)\n", secret);
}

static void handleDiscordSpectate(const char* secret)
{
    printf("\nDiscord: spectate (%s)\n", secret);
}

static int prompt(char* line, size_t size)
{
    int res;
    char* nl;
    printf("\n> ");
    fflush(stdout);
    res = fgets(line, (int)size, stdin) ? 1 : 0;
    line[size - 1] = 0;
    nl = strchr(line, '\n');
    if (nl) {
        *nl = 0;
    }
    return res;
}

static void gameLoop()
{
    char line[512];
    char* space;

    StartTime = time(0);

    printf("You are standing in an open field west of a white house.\n");
    while (prompt(line, sizeof(line))) {
        if (line[0]) {
            if (line[0] == 'q') {
                break;
            }
            if (time(NULL) & 1) {
                printf("I don't understand that.\n");
            }
            else {
                space = strchr(line, ' ');
                if (space) {
                    *space = 0;
                }
                printf("I don't know the word \"%s\".\n", line);
            }

            ++FrustrationLevel;

            updateDiscordPresence();
        }

#ifdef DISCORD_DISABLE_IO_THREAD
        Discord_UpdateConnection();
#endif
        Discord_RunCallbacks();
    }
}

int main(int argc, char* argv[])
{
    DiscordEventHandlers handlers;
    memset(&handlers, 0, sizeof(handlers));
    handlers.ready = handleDiscordReady;
    handlers.disconnected = handleDiscordDisconnected;
    handlers.errored = handleDiscordError;
    handlers.joinGame = handleDiscordJoin;
    handlers.spectateGame = handleDiscordSpectate;

    Discord_Initialize(APPLICATION_ID, &handlers, 1);

    gameLoop();

    Discord_Shutdown();
    return 0;
}
