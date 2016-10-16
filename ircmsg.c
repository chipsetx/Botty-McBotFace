#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "globals.h"
#include "ircmsg.h"


IrcMsg *newMsg(char *input, BotCmd **cmd) {
  IrcMsg *msg = NULL;
  char *end = input + strlen(input);
  char *tok = NULL, *tok_off = NULL;
  int i = 0;
  
  msg = calloc(1, sizeof(IrcMsg));
  if (!msg) {
    fprintf(stderr, "msg alloc error\n");
    exit(1);
  }

  //first get the nick that created the message
  tok = strtok_r(input, "!", &tok_off);
  if (!tok) return msg;
  strncpy(msg->nick, tok+1, MAX_NICK_LEN);
  //skip host name
  tok = strtok_r(NULL, " ", &tok_off);
  if (!tok) return msg;

  //get action issued
  tok = strtok_r(NULL, " ", &tok_off);
  if (!tok) return msg;
  strncpy(msg->action, tok, MAX_CMD_LEN);

  //get the channel or user the message originated from
  tok = strtok_r(NULL, " ", &tok_off);
  if (!tok) return msg;
  strncpy(msg->channel, tok, MAX_CHAN_LEN);

  if (!tok_off || tok_off + 1 >= end) return msg;
  
  //finally save the rest of the message
  strncpy(msg->msg, tok_off+1, MAX_MSG_LEN);

  //parse a given command
  if (msg->msg[0] == CMD_CHAR && cmd) {
    int argCount = MAX_BOT_ARGS;
    tok = msg->msg + 1;
    while(i < argCount) {
      tok_off = strchr(tok, BOT_ARG_DELIM);
      if (tok_off && i < argCount - 1) *tok_off = '\0';
      msg->msgTok[i] = tok;
    
      if (i == 0) {
        *cmd = command_get(msg->msgTok[0]);
        if (*cmd)  argCount = (*cmd)->args;
      }
      
      if (!tok_off) break;
      tok_off++;
      tok = tok_off;
      i++;
    }
  }

  return msg;
}
