#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/types.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "libavutil/mathematics.h"
#include "libavfilter/avfilter.h"
#include "libavutil/audio_fifo.h"
#include "packet.h"

typedef struct FilteringContext{
	AVFilterContext* buffersrc_ctx;
	AVFilterContext* buffersink_ctx;
	AVFilterGraph* filter_graph;
}FilteringContext;

typedef struct qiniu_ADTSContext{
	AVClass* class;
	int write_adts;
	int objecttype;
	int sample_rate_index;
	int channel_conf;
	int pce_size;
	int apetag;
	int id3v2tag;
	uint8_t pce_data[320];
}qiniu_ADTSContext;

typedef int(*DECFUNC)(AVCodecContext*,AVFrame*,int*,const AVPacket*) ;

typedef struct Transfer_Thread_Task{
	char* outputfilename;
	AVCodecContext** decoder_context;
	AVFormatContext* output_format_context;
	FilteringContext* filter_context;
	AVBitStreamFilterContext* extrabsfc;
	AVBitStreamFilterContext* h264_mp4toannexbbsfc;
	AVAudioFifo* avaf;
	PacketList pl;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int pushover;
	AVRational* input_stream_time_base;
	AVRational* input_codec_time_base;
	uint64_t pts;
	uint64_t dts;
	int frame_index;
}Transfer_Thread_Task;

extern void init_ffmpeg();

extern int CreateTransTask(char* inputfilename, char* outputpath);