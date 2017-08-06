#include "botmsgqueues.h"



static long long calculateNextMsgTime(char throttled) {

	long long curTime = botty_currentTimestamp();
	if (throttled) curTime += THROTTLE_WAIT_SEC * ONE_SEC_IN_MS;
	else curTime += (ONE_SEC_IN_MS/MSG_PER_SECOND_LIM);
	return curTime;
}

static void initMsgQueue(BotSendMessageQueue *queue) {
	queue->nextSendTimeMS = botty_currentTimestamp();
}


BotQueuedMessage *BotQueuedMsg_newMsg(char *msg, char *responseTarget, size_t len) {
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

void BotMsgQueue_enqueueTargetMsg(HashTable *msgQueues, char *target, BotQueuedMessage *msg) {
	HashEntry *targetQueue = HashTable_find(msgQueues, target);
	if (!targetQueue) {
		fprintf(stderr, "Creating new message queue hash for: %s\n", target);
		BotSendMessageQueue *newQueue = calloc(1, sizeof(BotSendMessageQueue));
		if (!newQueue) {
			fprintf(stderr, "Error creating message queue for target: %s\n", target);
			return;
		}
		initMsgQueue(newQueue);
		targetQueue = HashEntry_create(target, (void*)newQueue);
		HashTable_add(msgQueues, targetQueue);
	}

	enqueueMsg((BotSendMessageQueue *)targetQueue->data, msg);
}

void BotMsgQueue_setThrottle(HashTable *msgQueues, char *target) {
	HashEntry *targetQueueEntry = HashTable_find(msgQueues, target);
	if (!targetQueueEntry) return;

	BotSendMessageQueue *msgQueue = (BotSendMessageQueue *)targetQueueEntry->data;
	msgQueue->throttled++;
}

void BotMsgQueue_processQueue(SSLConInfo *conInfo, BotSendMessageQueue *queue) {
	//BotSendMessageQueue *queue = &bot->msgQueue;
	TimeStamp_t currentTime = botty_currentTimestamp();
	TimeStamp_t timeDiff = currentTime - queue->nextSendTimeMS;

	if (timeDiff < 0) return;

	if (queue->isThrottled) {
		queue->lastThrottled = queue->throttled;
		fprintf(stderr, "resetting throttle limit");
	}
	queue->isThrottled = (queue->throttled != queue->lastThrottled);

	int ret = 0;
	if (!connection_client_poll(conInfo, POLLOUT, &ret)) {
		fprintf(stderr, "processMsgQueue: socket not ready for output\n");
		return;
	}

	BotQueuedMessage *msg = peekQueueMsg(queue);
	if (!msg) return;

	switch (msg->status) {
		case QUEUED_STATE_INIT: {
			msg->status = QUEUED_STATE_SENT;
			fprintf(stdout, "SENDING (%d bytes): %s\n", (int)msg->len, msg->msg);
			queue->writeStatus = connection_client_send(conInfo, msg->msg, msg->len);
			queue->nextSendTimeMS = calculateNextMsgTime(0);
		} break;
		case QUEUED_STATE_SENT: {
			if (queue->isThrottled) {
				fprintf(stderr, "Throttled, will retry sending %s\n", msg->msg);
				queue->nextSendTimeMS = calculateNextMsgTime(1);
				msg->status = QUEUED_STATE_INIT;
			} else {
				fprintf(stderr, "Successfully sent: %d bytes\n", (int)msg->len);
				msg = popQueueMsg(queue);
				freeQueueMsg(msg);
				queue->nextSendTimeMS = calculateNextMsgTime(0);
			}
		} break;
	}
}

static int cleanQueue(HashEntry *entry, void *data) {
  if (entry->data) {
  	BotSendMessageQueue *queue = (BotSendMessageQueue *)data;
  	while (queue->count > 0) {
  		BotQueuedMessage *msg = popQueueMsg(queue);
  		freeQueueMsg(msg);
  	}
  	free(entry->data);
  }
  return 0;
}

void BotMsgQueues_cleanQueues(HashTable *msgQueues) {
  HashTable_forEach(msgQueues, NULL, &cleanQueue);
  HashTable_destroy(msgQueues);
}

