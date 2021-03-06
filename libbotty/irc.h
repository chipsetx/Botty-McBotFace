#ifndef __IRC_H__
#define __IRC_H__

#include <stdarg.h>
#include "globals.h"
#include "callback.h"
#include "connection.h"
#include "botprocqueue.h"
#include "botinputqueue.h"
#include "nicklist.h"

typedef enum {
  CONSTATE_NONE,
  CONSTATE_CONNECTED,
  CONSTATE_REGISTERED,
  CONSTATE_LISTENING,
} ConState;


typedef struct IrcInfo {
  char port[MAX_PORT_LEN];
  char server[MAX_SERV_LEN];
  char channel[MAX_CONNECTED_CHANS][MAX_CHAN_LEN];
} IrcInfo;

typedef struct BotInfo {
  //user config values
  int id;
  IrcInfo *info;
  char host[MAX_HOST_LEN];
  char nick[NICK_ATTEMPTS][MAX_NICK_LEN];
  char ident[MAX_IDENT_LEN];
  char realname[MAX_REALNAME_LEN];
  char master[MAX_NICK_LEN];
  char useSSL;
  char joined;

  //connection state info
  char recvbuf[MAX_MSG_LEN];

  ConState state;
  int nickAttempt;

  Callback cb[CALLBACK_COUNT];
  HashTable *commands;

  BotInputQueue inputQueue;
  BotProcessQueue procQueue;

  SSLConInfo conInfo;
  TimeStamp_t startTime;

  HashTable *msgQueues;
  HashTable *cmdAliases;
  HashTable *botPermissions;

  ChannelNickLists allChannelNicks;
  //some pointer the user can use
  void *data;
} BotInfo;


int bot_irc_init(void);

void bot_irc_cleanup(void);

int bot_irc_send(BotInfo *bot, char *msg);

int bot_init(BotInfo *bot, int argc, char *argv[], int argstart);

int bot_connect(BotInfo *info);

char *bot_getNick(BotInfo *bot);

void bot_cleanup(BotInfo *info);

void bot_setCallback(BotInfo *bot, BotCallbackID id, Callback fn);

int bot_run(BotInfo *info);

int bot_send(BotInfo *info, char *target, char *action, char *ctcp, char *msg, ...);

int bot_ctcp_send(BotInfo *info, char *target, char *command, char *msg, ...);

int bot_regName(BotInfo *bot, char *channel, char *nick);

void bot_rmName(BotInfo *bot, char *channel, char *nick);

void bot_rmDisconnectedName(BotInfo *bot, char *nick);

void bot_purgeNames(BotInfo *bot);

void bot_foreachName(BotInfo *bot, char *channel, void *d, NickListIterator iterator);

int bot_isThrottled(BotInfo *bot);

void bot_runProcess(BotInfo *bot, BotProcessFn fn, BotProcessArgs *args, char *cmd, char *caller);

int bot_registerAlias(BotInfo *bot, char *alias, char *cmd);

void bot_join(BotInfo *bot, char *channel);

#endif //__IRC_H__
