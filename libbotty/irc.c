#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "builtin.h"
#include "irc.h"
#include "ircmsg.h"
#include "commands.h"
#include "callback.h"
#include "connection.h"
#include "cmddata.h"

#define QUEUE_SEND_MSG 1

HashTable *IrcApiActions = NULL;

const char IrcApiActionText[API_ACTION_COUNT][MAX_CMD_LEN] = {
  "NOP", "DIE", "WHO", "KICK", "NICK", "MODE", "INFO", "KILL",
  "PING", "TIME", "JOIN", "AWAY", "MOTD", "PONG", "OPER",
  "PART", "ISON", "LIST", "USER", "QUIT", "ADMIN", "TRACE",
  "NAMES", "TOPIC", "LINKS", "ERROR", "WHOIS", "STATS",
  "USERS", "SQUIT", "REHASH", "INVITE", "WHOWAS", "LUSERS",
  "SUMMON", "SQUERY", "CONNECT", "SERVICE", "WALLOPS", "RESTART",
  "VERSION", "SERVLIST", "USERHOST"
};

static IRC_API_Actions IrcApiActionValues[API_ACTION_COUNT];
int bot_parse(BotInfo *bot, char *line);

static int timeDiffInUS(struct timeval *first, struct timeval *second) {
	unsigned int us1 = (first->tv_sec * ONE_SEC_IN_US) + first->tv_usec;
	unsigned int us2 = (second->tv_sec * ONE_SEC_IN_US) + second->tv_usec;
	return us1 - us2;
}

static void calculateNextMsgTime(struct timeval *time, char throttled) {
	gettimeofday(time, NULL);
	if (!throttled) {
		if (MSG_PER_SECOND_LIM == 1) time->tv_sec += 1;
		else time->tv_usec += (ONE_SEC_IN_US/MSG_PER_SECOND_LIM);
	}
	else {
		time->tv_sec += THROTTLE_WAIT_SEC;
	}
}


static BotQueuedMessage *newQueueMsg(char *msg, char *responseTarget, size_t len) {
	BotQueuedMessage *newMsg = calloc(1, sizeof(BotQueuedMessage));
	if (!newMsg) {
		fprintf(stderr, "newQueueMsg: Error allocating new message for:\n%s to %s\n", msg, responseTarget);
		return NULL;
	}
	strncpy(newMsg->msg, msg, MAX_MSG_LEN);
	strncpy(newMsg->channel, responseTarget, MAX_CHAN_LEN);
	newMsg->status = QUEUED_STATE_INIT;
	newMsg->len = len;
	return newMsg;
}

static void freeQueueMsg(BotQueuedMessage *msg) {
	free(msg);
}

static BotQueuedMessage *peekQueueMsg(BotSendMessageQueue *queue) {
	if (!queue || !queue->start)
		return NULL;

	return queue->start;
}

static BotQueuedMessage *popQueueMsg(BotSendMessageQueue *queue) {
	if (!queue || !queue->start)
		return NULL;

	BotQueuedMessage *poppedMsg = queue->start;
	queue->start = poppedMsg->next;
	if (queue->end == poppedMsg) queue->end = NULL;
	queue->count--;
	fprintf(stderr, "%d queued messages\n", queue->count);
	return poppedMsg;
}

static void pushQueueMsg(BotSendMessageQueue *queue, BotQueuedMessage *msg) {
	if (!queue || !msg)
		return;

	msg->next = queue->start;
	if (!queue->start)
		queue->end = msg;
	queue->start = msg;
	queue->count++;
	fprintf(stderr, "%d queued messages\n", queue->count);
}

static void enqueueMsg(BotSendMessageQueue *queue, BotQueuedMessage *msg) {
	if (!queue || !msg)
		return;

	queue->count++;
	if (!queue->end && !queue->start) {
		queue->start = msg;
		queue->end = msg;
		return;
	}
	queue->end->next = msg;
	queue->end = msg;
}




static void processMsgQueue(BotInfo *bot) {
	BotSendMessageQueue *queue = &bot->msgQueue;
	struct timeval currentTime = {};
	gettimeofday(&currentTime, NULL);

	if (timeDiffInUS(&currentTime, &queue->nextSendTime) < 0)
		return;

	int ret = 0;
	if (!connection_client_poll(&bot->conInfo, POLLOUT, &ret)) {
		fprintf(stderr, "processMsgQueue: socket not ready for output\n");
		return;
	}


	BotQueuedMessage *msg = peekQueueMsg(queue);
	if (!msg) return;

	switch (msg->status) {
		case QUEUED_STATE_INIT: {
			msg->status = QUEUED_STATE_SENT;
			fprintf(stdout, "SENDING (%d bytes): %s\n", msg->len, msg->msg);
			queue->writeStatus = connection_client_send(&bot->conInfo, msg->msg, msg->len);
			calculateNextMsgTime(&queue->nextSendTime, 0);
		} break;
		case QUEUED_STATE_SENT: {
			if (bot_isThrottled(bot)) {
				fprintf(stderr, "Throttled, will retry sending %s\n", msg->msg);
				msg->status = QUEUED_STATE_INIT;
				calculateNextMsgTime(&queue->nextSendTime, 1);
			} else {
				fprintf(stderr, "Successfully sent: %d bytes\n", msg->len);
				msg = popQueueMsg(queue);
				freeQueueMsg(msg);
				calculateNextMsgTime(&queue->nextSendTime, 0);
			}
		} break;

		case QUEUED_STATE_THROTTLED: {
			msg->status = QUEUED_STATE_INIT;
		} break;

		default: break;
	}
}




/*
 * Send an irc formatted message to the server.
 * Assumes your message is appropriately sized for a single
 * message.
 */
static int _send(BotInfo *bot, char *command, char *target, char *msg, char *ctcp, char queued) {
	SSLConInfo *conInfo = &bot->conInfo;
  char curSendBuf[MAX_MSG_LEN];
  int written = 0;
  char *sep = PARAM_DELIM_STR;

  if (!command || !target) sep = ACTION_EMPTY;
  if (!command) command = ACTION_EMPTY;
  if (!target) target = ACTION_EMPTY;


  if (!ctcp)
    written = snprintf(curSendBuf, MAX_MSG_LEN, "%s %s %s%s%s", command, target, sep, msg, MSG_FOOTER);
  else {
    written = snprintf(curSendBuf, MAX_MSG_LEN, "%s %s %s"CTCP_MARKER"%s %s"CTCP_MARKER"%s",
                       command, target, sep, ctcp, msg, MSG_FOOTER);
  }

  if (queued) {
	  BotQueuedMessage *toSend = newQueueMsg(curSendBuf, target, written);
	  if (toSend) enqueueMsg(&bot->msgQueue, toSend);
	  else fprintf(stderr, "Failed to queue message: %s\n", curSendBuf);
  	return 0;
  }

  fprintf(stdout, "SENDING (%d bytes): %s\n", written, curSendBuf);
  return connection_client_send(conInfo, curSendBuf, written);
}

/*
 * Returns the number of bytes of overhead that is sent with each message.
 * This overhead goes towards the IRC message limit.
 */
static unsigned int _getMsgOverHeadLen(char *command, char *target, char *ctcp, char *usernick) {
  unsigned int len =  MAX_CMD_LEN + ARG_DELIM_LEN + MAX_CHAN_LEN + ARG_DELIM_LEN;
  len += strlen(MSG_FOOTER) + strlen(usernick);
  if (ctcp)
    len += strlen(ctcp) + (strlen(CTCP_MARKER) << 1) + ARG_DELIM_LEN;

  return len;
}

/*
 * Split and send an irc formatted message to the server.
 * If your message is too long, it will be split up and sent in up to
 * MAX_MSG_SPLITS chunks.
 *
 */
int bot_irc_send_s(BotInfo *bot, char *command, char *target, char *msg, char *ctcp, char *nick) {
  unsigned int overHead = _getMsgOverHeadLen(command, target, ctcp, nick);
  unsigned int msgLen =  overHead + strlen(msg);
  if (msgLen >= MAX_MSG_LEN) {
    //split the message into chunks
    int maxSplitSize = MAX_MSG_LEN - overHead;
    int chunks = (strlen(msg) / maxSplitSize) + 1;
    chunks = (chunks < MAX_MSG_SPLITS) ? chunks : MAX_MSG_SPLITS;

    char replaced = 0;
    char *nextMsg = msg, *end = 0, *last = msg + strlen(msg);
    do {
      end = nextMsg + maxSplitSize;
      //split on words, so scan back for last space
      while (*end != ' ' && end > nextMsg) end--;
      //if we couldn't find a word to split on, so just split at the character limit
      if (end <= nextMsg)  end = nextMsg + maxSplitSize;
      if (end < last) {
        replaced = *end;
        *end = '\0';
      }
      _send(bot, command, target, nextMsg, ctcp, QUEUE_SEND_MSG);
      nextMsg = end;
      if (end < last) *end = replaced;
      //remove any leading spaces for the next message
      if (*nextMsg == ' ') nextMsg++;
    } while (--chunks && nextMsg < last);

    return 0;
  }

  return _send(bot, command, target, msg, ctcp, QUEUE_SEND_MSG);
}

int bot_irc_send(BotInfo *bot, char *msg) {
  return _send(bot, NULL, NULL, msg, NULL, 0);
}


/*
 * Automatically formats a PRIVMSG command for the bot to speak.
 */

static int _botSend(BotInfo *bot, char *target, char *action, char *ctcp, char *fmt, va_list a) {
  char *msgBuf;
  if (!target) target = bot->info->channel;

  //only buffer up to 4 message splits worth of text
  size_t msgBufLen = MAX_MSG_LEN * MAX_MSG_SPLITS;
  msgBuf = malloc(msgBufLen);
  if (!msgBuf) {
    perror("Msg alloc failed:");
    return -1;
  }
  vsnprintf(msgBuf, msgBufLen - 1, fmt, a);
  int status = bot_irc_send_s(bot, action, target, msgBuf, ctcp, bot_getNick(bot));
  free(msgBuf);
  return status;
}

int bot_send(BotInfo *bot, char *target, char *action, char *ctcp, char *fmt, ...) {
  int status = 0;
  va_list args;
  va_start(args, fmt);
  status = _botSend(bot, target, action, ctcp, fmt, args);
  va_end(args);
  return status;
}

int bot_ctcp_send(BotInfo *bot, char *target, char *command, char *msg, ...) {
  char outbuf[MAX_MSG_LEN];
  va_list args;
  va_start(args, msg);
  vsnprintf(outbuf, MAX_MSG_LEN, msg, args);
  va_end(args);
  return bot_send(bot, target, ACTION_MSG, NULL, CTCP_MARKER"%s %s"CTCP_MARKER, command, outbuf);
}

/*
 * Default actions for handling various server responses such as nick collisions
 */
static int defaultServActions(BotInfo *bot, IrcMsg *msg, char *line) {
  //if nick is already registered, try a new one
  if (!strncmp(msg->action, REG_ERR_CODE, strlen(REG_ERR_CODE))) {
    if (bot->nickAttempt < NICK_ATTEMPTS) bot->nickAttempt++;
    else {
      fprintf(stderr, "Exhuasted nick attempts, please configure a unique nick\n");
      return -1;
    }
    fprintf(stderr, "Nick is already in use, attempting to use: %s\n", bot->nick[bot->nickAttempt]);
    //then attempt registration again
    bot->state = CONSTATE_CONNECTED;
    //return bot_parse(bot, line);
    return 0;
  }
  //otherwise, nick is not in use
  else if (!strncmp(msg->action, REG_SUC_CODE, strlen(REG_SUC_CODE))) {
    bot->state = CONSTATE_REGISTERED;
  }
  //store all current users in the channel
  else if (!strncmp(msg->action, NAME_REPLY, strlen(NAME_REPLY))) {
    char *start = msg->msgTok[1], *next = start, *end = start + strlen(start);
    while (start < end) {
      while (*next != BOT_ARG_DELIM && next <= end) next++;
      *next = '\0';
      bot_regName(bot, start);
      fprintf(stdout, "Registered nick: %s\n", start);
      if (next < end) {
        *next = BOT_ARG_DELIM;
        next++;
      }
      start = next;
    }
  }
  //attempt to detect any messages indicating throttling
  else if (!strncmp(msg->action, NOTICE_ACTION, strlen(NOTICE_ACTION))) {
    char *result = strstr(msg->msgTok[0], THROTTLE_NEEDLE);
    if (result) {
      bot->conInfo.throttled += (result != NULL);
      fprintf(stderr, "Detected throttling!\n");
      return 1;
    }
  }

  return 0;
}


/*
 * Parse out any server responses that may need to be attended to
 * and pass them into the appropriate callbacks.
 */
static int parseServer(BotInfo *bot, char *line) {
  char buf[MAX_MSG_LEN];
  snprintf(buf, sizeof(buf), ":%s", bot->info->server);
  //not a server response
  if (strncmp(line, buf, strlen(buf))) return 0;
  //is a server response
  strncpy(buf, line, MAX_MSG_LEN);
  IrcMsg *msg = ircMsg_server_new(buf);
  int status = defaultServActions(bot, msg, line);
  if (status) {
    free(msg);
    return status;
  }

  callback_call_r(bot->cb, CALLBACK_SERVERCODE, (void *)bot, msg);
  free(msg);
  return 1;
}

static int userJoined(BotInfo *bot, IrcMsg *msg) {
  bot_regName(bot, msg->nick);
  return callback_call_r(bot->cb, CALLBACK_USRJOIN, (void *)bot, msg);
}

static int userLeft(BotInfo *bot, IrcMsg *msg) {
  bot_rmName(bot, msg->nick);
  return callback_call_r(bot->cb, CALLBACK_USRPART, (void *)bot, msg);
}

static int userNickChange(BotInfo *bot, IrcMsg *msg) {
  bot_rmName(bot, msg->nick);
  bot_regName(bot, msg->msg);
  return callback_call_r(bot->cb, CALLBACK_USRNICKCHANGE, (void *)bot, msg);
}

/*
 * Parses any incomming line from the irc server and
 * invokes callbacks depending on the message type and
 * current state of the connection.
 */
int bot_parse(BotInfo *bot, char *line) {
  if (!line) return 0;

  int servStat = 0;
  char sysBuf[MAX_MSG_LEN];
  char *space = NULL, *space_off = NULL;
  fprintf(stdout, "SERVER: %s\n", line);

  //respond to server pings
  if (!strncmp(line, "PING", strlen("PING"))) {
    char *pong = line + strlen("PING") + 1;
    snprintf(sysBuf, sizeof(sysBuf), "PONG %s", pong);
    bot_irc_send(bot, sysBuf);
    return 0;
  }

  if ((servStat = parseServer(bot, line)) < 0) return servStat;

  switch (bot->state) {
  case CONSTATE_NONE:
    //initialize data here
    space = strtok_r(line, " ", &space_off);
    if (space) {
      //grab new server name if we've been redirected
      memcpy(bot->info->server, space+1, strlen(space) - 1);
      printf("given server: %s\n", bot->info->server);
    }
    callback_call_r(bot->cb, CALLBACK_CONNECT, (void*)bot, NULL);
    bot->state = CONSTATE_CONNECTED;
    break;

  case CONSTATE_CONNECTED:
    //register the bot
    snprintf(sysBuf, sizeof(sysBuf), "NICK %s", bot->nick[bot->nickAttempt]);
    bot_irc_send(bot, sysBuf);
    snprintf(sysBuf, sizeof(sysBuf), "USER %s %s test: %s", bot->ident, bot->host, bot->realname);
    bot_irc_send(bot, sysBuf);
    gettimeofday(&bot->startTime, NULL);
    //go to listening state to wait for registration confirmation
    bot->state = CONSTATE_LISTENING;
    break;

  case CONSTATE_REGISTERED:
    snprintf(sysBuf, sizeof(sysBuf), "JOIN %s", bot->info->channel);
    bot_irc_send(bot, sysBuf);
    bot->state = CONSTATE_JOINED;
    break;
  case CONSTATE_JOINED:
    bot->joined = 1;
    callback_call_r(bot->cb, CALLBACK_JOIN, (void*)bot, NULL);
    bot->state = CONSTATE_LISTENING;
    break;
  default:
  case CONSTATE_LISTENING:
    //filter out server messages
    if (servStat) break;

    snprintf(sysBuf, sizeof(sysBuf), ":%s", bot->nick[bot->nickAttempt]);
    if (!strncmp(line, sysBuf, strlen(sysBuf))) {
      //filter out messages that the bot says itself
      break;
    }
    else {
      BotCmd *cmd = NULL;
      IrcMsg *msg = ircMsg_irc_new(line, bot->commands, &cmd);
      IRC_API_Actions action = IRC_ACTION_NOP;
      HashEntry *a = HashTable_find(IrcApiActions, msg->action);

      if (cmd) {
        CmdData data = { .bot = bot, .msg = msg };
        //make sure who ever is calling the command has permission to do so
        if (cmd->flags & CMDFLAG_MASTER && strcmp(msg->nick, bot->master))
          fprintf(stderr, "%s is not %s\n", msg->nick, bot->master);
        else if ((servStat = command_call_r(cmd, (void *)&data, msg->msgTok)) < 0)
          fprintf(stderr, "Command '%s' gave exit code\n,", cmd->cmd);
      }
      else if (a) {
        if (a->data) action = *(IRC_API_Actions*)a->data;

        switch(action) {
        default: break;
        case IRC_ACTION_JOIN:
          servStat = userJoined(bot, msg);
          break;
        case IRC_ACTION_QUIT:
        case IRC_ACTION_PART:
          servStat = userLeft(bot, msg);
          break;
        case IRC_ACTION_NICK:
          servStat = userNickChange(bot, msg);
          break;
        }
      }
      else
        callback_call_r(bot->cb, CALLBACK_MSG, (void*)bot, msg);

      free(msg);
    }
    break;
  }
  return servStat;
}

/*
 * initialize the hash table used for looking up api calls
 */
int bot_irc_init(void) {
  if (IrcApiActions) return 0;

  IrcApiActions = HashTable_init(ACTION_HASH_SIZE);
  if (!IrcApiActions) {
    fprintf(stderr, "Error initializing IRC API hash\n");
    return -1;
  }

  for (int i = 0; i < API_ACTION_COUNT; i++) {
    IrcApiActionValues[i] = (IRC_API_Actions) i;
    HashTable_add(IrcApiActions,
                  HashEntry_create((char *)IrcApiActionText[i], (void *)&IrcApiActionValues[i]));
  }

  return 0;
}

void bot_irc_cleanup(void) {
  HashTable_destroy(IrcApiActions);
  IrcApiActions = NULL;
}

int bot_init(BotInfo *bot, int argc, char *argv[], int argstart) {
  if (!bot) return -1;

  bot->commands = HashTable_init(COMMAND_HASH_SIZE);
  if (!bot->commands) {
    fprintf(stderr, "Error allocating command hash for bot\n");
    return -1;
  }
  //initialize the built in commands
  botcmd_builtin(bot);
  return 0;
}

int bot_connect(BotInfo *bot) {
  if (!bot) return -1;

  bot->state = CONSTATE_NONE;

  if (bot->useSSL) {
    if (connection_ssl_client_init(bot->info->server, bot->info->port, &bot->conInfo))
      exit(1);

    return 0;
  }

  bot->conInfo.servfds.fd = connection_client_init(bot->info->server, bot->info->port, &bot->conInfo.res);
  if (bot->conInfo.servfds.fd < 0) exit(1);
  bot->conInfo.servfds.events = POLLIN | POLLPRI | POLLOUT | POLLWRBAND;

  int n = strlen(SERVER_PREFIX);
  if (strncmp(SERVER_PREFIX, bot->info->server, n)) {
    int servLen = strlen(bot->info->server);
    if (servLen + n < MAX_SERV_LEN) {
      memmove(bot->info->server + n, bot->info->server, servLen);
      memcpy(bot->info->server, SERVER_PREFIX, n);
      printf("NEW SERVER NAME: %s\n", bot->info->server);
    }
  }
  return 0;
}

char *bot_getNick(BotInfo *bot) {
  return bot->nick[bot->nickAttempt];
}

void bot_cleanup(BotInfo *bot) {
  if (!bot) return;

  bot_purgeNames(bot);
  if (bot->commands) command_cleanup(bot->commands);
  bot->commands = NULL;
  close(bot->conInfo.servfds.fd);
  freeaddrinfo(bot->conInfo.res);
}

void bot_setCallback(BotInfo *bot, BotCallbackID id, Callback fn) {
  callback_set_r(bot->cb, id, fn);
}

void bot_addcommand(BotInfo *bot, char *cmd, int flags, int args, CommandFn fn) {
  command_reg(bot->commands, cmd, flags, args, fn);
}


BotProcessArgs *bot_makeProcessArgs(void *data, char *responseTarget, BotProcessArgsFreeFn fn) {
  BotProcessArgs *args = calloc(1, sizeof(BotProcessArgs));
  if (!args) return NULL;

  args->data = data;
  if (responseTarget) {
		size_t responseTargetLen = strlen(responseTarget);
  	args->target = calloc(1, responseTargetLen + 1);
  	if (!args->target) return NULL;
  	strncpy(args->target, responseTarget, responseTargetLen);
  }
  args->free = fn;
  return args;
}

void bot_freeProcessArgs(BotProcessArgs *args) {
	if (!args) return;

	if (args->free) args->free(args->data);
	if (args->target) {
		free(args->target);
		args->target = NULL;
	}
	free(args);
}


void bot_queueProcess(BotInfo *bot, BotProcessFn fn, BotProcessArgs *args, char *cmd, char *caller) {
	BotProcess *process = calloc(1, sizeof(BotProcess));
	if (!process) {
		fprintf(stderr, "bot_queueProcess: error allocating new process\n");
		return;
	}

	process->fn = fn;
	process->arg = args;
	process->busy = 1;

	if (bot->procQueue.head) {
		BotProcess *curProc = bot->procQueue.head;
		while (curProc->next) {
			curProc = curProc->next;
		}
		curProc->next = process;
	}
	else {
		bot->procQueue.head = process;
	}

	bot->procQueue.count++;
	process->pid = (++bot->procQueue.pidTicker);
	gettimeofday(&process->updated, NULL);
	snprintf(process->details, MAX_MSG_LEN, "PID: %d: %s - %s", process->pid, cmd, caller);
	fprintf(stderr, "bot_queueProcess: Added new process to queue:\n %s\n", process->details);
}

void bot_dequeueProcess(BotInfo *bot, BotProcess *process) {
	if (!process) return;

	if (bot->procQueue.head != process) {
		BotProcess *proc = bot->procQueue.head;
		while (proc->next != process) {
			proc = proc->next;
		}
		proc->next = process->next;
	}
	else {
		bot->procQueue.head = process->next;
	}

	if (bot->procQueue.current == process)
		bot->procQueue.current = process->next;

	bot->procQueue.count--;
	//if process is dequeued while it was running, cleanup the process data
	if (process->busy >= 0) bot_freeProcessArgs(process->arg);

	fprintf(stderr, "bot_queueProcess: Removed process:\n %s\n", process->details);
	free(process);
}

BotProcess *bot_findProcessByPid(BotInfo *bot, unsigned int pid) {
	BotProcess *process = bot->procQueue.head;

	while (process && process->pid != pid) {
		process = process->next;
	}

	if (!process)
		fprintf(stderr, "Failed to located PID: %d\n", pid);
	else
		fprintf(stderr, "Located Process:\n %s\n", process->details);

	return process;
}

void bot_updateProcesses(BotInfo *bot) {
	if (!bot->procQueue.current)
		bot->procQueue.current = bot->procQueue.head;

	BotProcess *proc = bot->procQueue.current;
	if (proc && proc->fn) {
		struct timeval curTime = {};
		gettimeofday(&curTime, NULL);

		if (timeDiffInUS(&curTime, &proc->updated) > ONE_SEC_IN_US/MSG_PER_SECOND_LIM) {
			gettimeofday(&proc->updated, NULL);
	  	if ((proc->busy = proc->fn((void *)bot, proc->arg)) < 0)
	      bot_dequeueProcess(bot, proc);
	    else
	    	bot->procQueue.current = proc->next;
	  }
	  else
	  	bot->procQueue.current = proc->next;
  }
}

/*
 * Run the bot! The bot will connect to the server and start
 * parsing replies.
 */
int bot_run(BotInfo *bot) {
  int n = 0, ret = 0;

  bot->conInfo.isThrottled = (bot->conInfo.throttled != bot->conInfo.lastThrottled);
  bot->conInfo.lastThrottled = bot->conInfo.throttled;

  if (!bot->joined && bot->startTime.tv_sec != 0) {
    struct timeval current = {};
    gettimeofday(&current, NULL);
    if(current.tv_sec - bot->startTime.tv_sec >= REGISTER_TIMEOUT_SEC) {
      bot->state = CONSTATE_REGISTERED;
      gettimeofday(&bot->startTime, NULL);
    }
  }

  //bot_runProcess(bot);
  bot_updateProcesses(bot);
  processMsgQueue(bot);
  //process all input first before receiving more
  if (bot->line) {
    if ((n = bot_parse(bot, bot->line)) < 0) return n;
    bot->line = strtok_r(NULL, "\r\n", &bot->line_off);
    return 0;
  }

  bot->line_off = NULL;
  memset(bot->recvbuf, 0, sizeof(bot->recvbuf));

  if (connection_client_poll(&bot->conInfo, POLLIN, &ret)) {
    n = connection_client_read(&bot->conInfo, bot->recvbuf, sizeof(bot->recvbuf));
    if (!n) {
      printf("Remote closed connection\n");
      return -2;
    }
    else if (!bot->conInfo.enableSSL && n < 0) {
      perror("Response error: ");
      return -3;
    }
  }
  //parse replies one line at a time
  if (n > 0) bot->line = strtok_r(bot->recvbuf, "\r\n", &bot->line_off);
  return 0;
}

/*
 * Keep a list of all nicks in the channel
 */
void bot_regName(BotInfo *bot, char *nick) {
  NickList *curNick;
  NickList *newNick = calloc(1, sizeof(NickList));
  if (!newNick) {
    perror("NickList Alloc Error: ");
    exit(1);
  }

  size_t diff = strcspn(nick, ILLEGAL_NICK_CHARS);
  nick += (diff == 0);
  fprintf(stderr, "Registering Nick: %s\n", nick);
  strncpy(newNick->nick, nick, MAX_NICK_LEN);
  if (!bot->names) {
    //first name
    bot->names = newNick;
    return;
  }

  curNick = bot->names;
  while (curNick->next) curNick = curNick->next;
  curNick->next = newNick;
}

void bot_rmName(BotInfo *bot, char *nick) {
  NickList *curNick, *lastNick;

  curNick = bot->names;
  lastNick = curNick;
  while (curNick && strncmp(curNick->nick, nick, MAX_NICK_LEN)) {
    fprintf(stderr, "Matching: %s with %s\n", curNick->nick, nick);
    lastNick = curNick;
    curNick = curNick->next;
  }

  //make sure the node we stopped on is the right one
  if (curNick && !strncmp(curNick->nick, nick, MAX_NICK_LEN)) {
    if (bot->names == curNick) bot->names = curNick->next;
    else lastNick->next = curNick->next;
    free(curNick);
  } else
    fprintf(stderr, "Failed to remove \'%s\' from nick list, does not exist\n", nick);

}

void bot_purgeNames(BotInfo *bot) {
  NickList *curNick = bot->names, *next;
  while (curNick) {
    next = curNick->next;
    free(curNick);
    curNick = next;
  }
  bot->names = NULL;
}


void bot_foreachName(BotInfo *bot, void *d, void (*fn) (NickList *nick, void *data)) {
  NickList *curNick = bot->names;
  while (curNick) {
    if (fn) fn(curNick, d);
    curNick = curNick->next;
  }
}

int bot_isThrottled(BotInfo *bot) {
  return bot->conInfo.isThrottled;
}
