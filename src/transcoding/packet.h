#include "libavcodec/avcodec.h"
#include <pthread.h>

typedef struct PacketNode{
	AVPacket* packet;
	struct PacketNode* next;
}PacketNode;

typedef struct PacketList{
	PacketNode* header;
	PacketNode* tail;
	int length;
	pthread_mutex_t packetLocker;
}PacketList;

extern int PacketListInit(PacketList* pl);

extern int PacketListPushBack(PacketList* pl,AVPacket* packet);

extern AVPacket* PacketListGetFront(PacketList* pl);

