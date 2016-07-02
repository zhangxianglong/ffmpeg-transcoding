#include "packet.h"

int PacketListInit(PacketList* pl){
	pl->header=NULL;
	pl->tail = NULL;
	pl->length=0;
	pthread_mutex_init(&(pl->packetLocker),NULL);
	return 0;
};

int PacketListPushBack(PacketList* pl,AVPacket* packet){
	if(pl==NULL){
		av_log(NULL,AV_LOG_INFO,"pl is null\n");
		return -1;
	}
	pthread_mutex_lock(&pl->packetLocker);
	PacketNode* pn = (PacketNode*)malloc(sizeof(PacketNode));
	pn->packet = packet;
	pn->next = NULL;
	if(pl->length==0){
		pl->header = pn;
		pl->tail = pl->header;
	}else{
		pl->tail->next = pn;
		pl->tail = pn;
		pl->tail->next = NULL;
	}
	pl->length++;
	pthread_mutex_unlock(&pl->packetLocker);
	return 0;
}

AVPacket* PacketListGetFront(PacketList* pl){
	if(pl==NULL){
		av_log(NULL,AV_LOG_INFO,"can not get packet\n");
		return NULL;
	}
	pthread_mutex_lock(&pl->packetLocker);
	if(pl->length==0||pl->header==NULL){
		return NULL;
	}
	PacketNode* pn = pl->header;
	//av_log(NULL,AV_LOG_INFO,"can get packet,packet address is %x,length is %d,%d\n",pl->header->packet,pl->length,pthread_self());
	pl->header = pl->header->next;
	pl->length--;
	pthread_mutex_unlock(&pl->packetLocker);
	return pn->packet;
}

int ReleasePacketList(PacketList* pl){
	if(pl==NULL){
		return 0;
	}
	while(pl->header!=NULL){
		av_packet_unref(pl->header->packet);
		pl->header = pl->header->next;
	}
	return 0;
}