#include "trans.h"

void init_ffmpeg(){
	avfilter_register_all();
	av_register_all();
	return;
}

static int fileindex=0;
static pthread_mutex_t locker;
static AVFormatContext* input_format_context=NULL;

int getEffectiveUID(const char* filepath){
	struct stat buf;
	int ret;
	char current_work_dir[255];
	long current_path_length;
	DIR* dirp;
	if((dirp=opendir(filepath))==NULL){
		if(ENOTDIR==errno){
			if((ret=stat(filepath,&buf))<0){
				av_log(NULL,AV_LOG_INFO,"1get filepath stat error!error code:%d\n",errno);
				return -1;
			}
			if((ret=seteuid(buf.st_uid))<0){
				av_log(NULL,AV_LOG_ERROR,"set effective uid %d error!error code:%d\n",buf.st_uid,errno);
				return -1;
			}
			return 0;
		}
		return -1;
	}else{
		if(getcwd(current_work_dir,current_path_length)==NULL){
			return -1;
		}
		ret=chdir(filepath);
		if(ret<0){
			av_log(NULL,AV_LOG_ERROR,"change dir error!");
			return 0;
		}
		if((ret=stat(filepath,&buf))<0){
			av_log(NULL,AV_LOG_INFO,"2get filepath stat error!error code:%d,%s\n",errno,filepath);
			return -1;
		}
		if((ret=seteuid(buf.st_uid))<0){
			av_log(NULL,AV_LOG_ERROR,"set effective uid %d error!error code:%d\n",buf.st_uid,errno);
			return -1;
		}
		ret = chdir(current_work_dir);
		if(ret<0){
			av_log(NULL,AV_LOG_ERROR,"change dir to current work dir %s error\n",current_work_dir);
			return -1;
		}
		return 0;
	}
}

int init_transfer_task(Transfer_Thread_Task* task,int size){
	int i=0;	
	for(i=0;i<size;i++){
		PacketListInit(&task[i].pl);
		task[i].output_format_context = NULL;
		pthread_mutex_init(&(task[i].mutex),NULL);
		pthread_cond_init(&(task[i].cond),NULL);
		task[i].pushover = 0;
		task[i].outputfilename = (char*)av_malloc(255);
		memset(task[i].outputfilename,0,255);
		task[i].avaf = NULL;
		task[i].pts=0;
		task[i].dts=0;
		task[i].frame_index=0;
	}
	return 0;
}

int open_input_file(const char* inputfilename,AVFormatContext** input_avformat_context,Transfer_Thread_Task* task,int size){
	int ret;
	int i;
	int j;
	
	if((ret=avformat_open_input(input_avformat_context,inputfilename,NULL,NULL))<0){
		av_log(NULL,AV_LOG_ERROR,"avformat_open_input error!,%s\n",av_err2str(ret));
		return ret;
	}
	if ((ret=avformat_find_stream_info(*input_avformat_context,NULL))<0){
		av_log(NULL,AV_LOG_ERROR,"avformat_find_stream error!");
		avformat_close_input(input_avformat_context);
		return ret;
	}
	
	for(j=0;j<size;j++){
		task[j].decoder_context = (AVCodecContext**)av_malloc_array((*input_avformat_context)->nb_streams,sizeof(AVCodecContext*));
		task[j].input_stream_time_base = (AVRational*)av_malloc_array((*input_avformat_context)->nb_streams,sizeof(AVRational));
		task[j].input_codec_time_base = (AVRational*)av_malloc_array((*input_avformat_context)->nb_streams,sizeof(AVRational));
		av_log(NULL,AV_LOG_INFO,"length %d\n",(*input_avformat_context)->nb_streams);
		for(i=0;i<(*input_avformat_context)->nb_streams;i++){
			AVStream* stream;
			AVCodecContext* avc_context;
			
			stream = (*input_avformat_context)->streams[i];
			avc_context = stream->codec;
			
			(task[j].decoder_context)[i] = avcodec_alloc_context3(avc_context->codec);
			
			if(avc_context->codec_type==AVMEDIA_TYPE_VIDEO||avc_context->codec_type==AVMEDIA_TYPE_AUDIO){
				ret = avcodec_open2(avc_context,avcodec_find_decoder(avc_context->codec_id),NULL);
				if(ret<0){
					av_log(NULL,AV_LOG_ERROR,"open decoder error");
					avformat_close_input(input_avformat_context);
					return ret;
				}
				task[j].input_stream_time_base[i] = stream->time_base;
				task[j].input_codec_time_base[i] = avc_context->time_base;
			}
		}
	}

	av_dump_format(*input_avformat_context,0,inputfilename,0);
	
	return 0;
}

int changeOutputFile(char *name,AVFormatContext* output_context){
	av_log(NULL,AV_LOG_INFO,"change outputfile\n");
	int ret=0;
	avio_close(output_context->pb);
	if(name){
		av_strlcpy(output_context->filename,name,sizeof(output_context->filename));
	}
	return 0;
}

int OpenDecoder(Transfer_Thread_Task* task,AVFormatContext* avfc_input){
	int i;
	int ret;
	av_log(NULL,AV_LOG_INFO,"reopen decoder\n");
	for(i=0;i<avfc_input->nb_streams;i++){
		AVStream* stream;
		AVCodecContext* avc_context;
		
		stream = avfc_input->streams[i];
		avc_context = stream->codec;
		
		if(avc_context->codec_type==AVMEDIA_TYPE_VIDEO||avc_context->codec_type==AVMEDIA_TYPE_AUDIO){
			av_log(NULL,AV_LOG_INFO,"reopen decoder,stream %d\n",i);
			avcodec_copy_context(task->decoder_context[i],avc_context);
			ret = avcodec_open2(task->decoder_context[i],avcodec_find_decoder(avc_context->codec_id),NULL);
			
			if(ret<0){
				av_log(NULL,AV_LOG_ERROR,"open decoder error");
				avformat_close_input(avfc_input);
				return ret;
			}
		}
	}
	av_log(NULL,AV_LOG_INFO,"open decoder extra data size is %d\n",task->decoder_context[0]->extradata_size);
	return 0;
}

int open_output_file(const AVFormatContext* avfc_input,const char* filename,Transfer_Thread_Task* task){
	int i;
	int ret;
	AVStream* out_stream;
	AVStream* in_stream;
	AVCodecContext *dec_ctx,*enc_ctx;
	AVCodec* encoder;
	
	OpenDecoder(task,avfc_input);
	
	task->extrabsfc =  av_bitstream_filter_init("dump_extra");
	task->h264_mp4toannexbbsfc =  av_bitstream_filter_init("h264_mp4toannexb");
	
	if(task->output_format_context==NULL||task->output_format_context->filename==NULL){
		av_log(NULL,AV_LOG_INFO,"first create output format context,%s\n",filename);
		avformat_alloc_output_context2(&task->output_format_context,NULL,NULL,filename);
		if(!task->output_format_context){
			av_log(NULL,AV_LOG_INFO,"open encoder error\n");
			return AVERROR_UNKNOWN;
		}
	}
	for(i=0;i<avfc_input->nb_streams;i++){
		out_stream = avformat_new_stream(task->output_format_context,NULL);
		if(!out_stream){
			av_log(NULL,AV_LOG_ERROR,"failed allocating output stream\n");
		}
		in_stream = avfc_input->streams[i];
		dec_ctx = in_stream->codec;
		enc_ctx = out_stream->codec;
		if(dec_ctx->codec_type==AVMEDIA_TYPE_VIDEO
		||dec_ctx->codec_type==AVMEDIA_TYPE_AUDIO){
			if(dec_ctx->codec_type==AVMEDIA_TYPE_VIDEO){
				encoder = avcodec_find_encoder(AV_CODEC_ID_VP8);
				if(encoder==NULL){
					av_log(NULL,AV_LOG_ERROR,"not support video encoder type\n");
					return -1;
				}
				enc_ctx->width = dec_ctx->width;
				enc_ctx->height = dec_ctx->height;
				enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
				enc_ctx->pix_fmt=encoder->pix_fmts[0];
				enc_ctx->time_base = dec_ctx->time_base;
				enc_ctx->codec_id = encoder->id;
				enc_ctx->codec_type = encoder->type;
				enc_ctx->me_range = 16;
				enc_ctx->qcompress=0.6;
				enc_ctx->qmin=30;//决定文件大小，qmin越大，编码压缩率越高
				enc_ctx->qmax=40;
				enc_ctx->me_subpel_quality=1;//决定编码速度，越小，编码速度越快
				enc_ctx->has_b_frames=0;
				enc_ctx->max_b_frames=0; 
				out_stream->time_base = in_stream->time_base;
			}else{
				encoder = avcodec_find_encoder(AV_CODEC_ID_MP3);
				if(encoder==NULL){
					av_log(NULL,AV_LOG_INFO,"not support audio encoder type\n");
					return -1;
				}
				enc_ctx->sample_rate = dec_ctx->sample_rate;
				enc_ctx->channel_layout = dec_ctx->channel_layout;
				enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
				if(encoder->sample_fmts){
					enc_ctx->sample_fmt = encoder->sample_fmts[0];
				}
				enc_ctx->time_base=dec_ctx->time_base;
				out_stream->time_base = in_stream->time_base;
			}
			
			ret = avcodec_open2(enc_ctx,encoder,NULL);
			if(ret<0){
				av_log(NULL,AV_LOG_ERROR,"can not open encoder for stream #%d\n",i);
				return ret;
			}
			if(dec_ctx->codec_type==AVMEDIA_TYPE_AUDIO && task->avaf==NULL){
				task->avaf = av_audio_fifo_alloc(enc_ctx->sample_fmt,enc_ctx->channels,1);
				if(task->avaf==NULL){
					av_log(NULL,AV_LOG_ERROR,"alloc audio fifo error!,%s\n",av_err2str(errno));
				}
			}
		}else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
	           av_log(NULL, AV_LOG_FATAL, "Elementary stream #%d is of unknown type, cannot proceed\n", i);
	           return AVERROR_INVALIDDATA;
	       } else {
	           /* if this stream must be remuxed */
	           ret = avcodec_copy_context(task->output_format_context->streams[i]->codec,
	                   avfc_input->streams[i]->codec);
	           if (ret < 0) {
	               av_log(NULL, AV_LOG_ERROR, "Copying stream context failed,%d,%s\n",i,av_err2str(ret));
	               return ret;
	           }
	       }
		if(task->output_format_context->oformat->flags&AVFMT_GLOBALHEADER){
			enc_ctx->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
		}
	}
	//av_dump_format(task->output_format_context,0,filename,1);
	
	if(!(task->output_format_context->oformat->flags&AVFMT_NOFILE)){
		ret = avio_open(&(task->output_format_context->pb),filename,AVIO_FLAG_WRITE);
		if(ret<0){
			av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file,%s\n",av_err2str(ret));
	       	return ret;
		}
	}
	
	ret = avformat_write_header(task->output_format_context,NULL);
	if(ret<0){
		av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
		return ret;
	}
	if((ret = init_filters(&(task->filter_context),input_format_context,task->output_format_context))<0){
			av_log(NULL,AV_LOG_ERROR,"init filters error!\n");
		}
	
	return 0;
}

int init_filter(FilteringContext* fctx, AVCodecContext *dec_ctx,
        AVCodecContext *enc_ctx, const char *filter_spec)
{
    char args[512];
    int ret = 0;
    AVFilter *buffersrc = NULL;
    AVFilter *buffersink = NULL;
    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterGraph *filter_graph = avfilter_graph_alloc();

    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        buffersrc = avfilter_get_by_name("buffer");
        buffersink = avfilter_get_by_name("buffersink");
        if (!buffersrc || !buffersink) {
            av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        snprintf(args, sizeof(args),
                "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                dec_ctx->time_base.num, dec_ctx->time_base.den,
                dec_ctx->sample_aspect_ratio.num,
                dec_ctx->sample_aspect_ratio.den);

        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
            goto end;
        }

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                NULL, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "pix_fmts",
                (uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
            goto end;
        }
    } else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        buffersrc = avfilter_get_by_name("abuffer");
        buffersink = avfilter_get_by_name("abuffersink");
        if (!buffersrc || !buffersink) {
            av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        if (!dec_ctx->channel_layout)
            dec_ctx->channel_layout =
                av_get_default_channel_layout(dec_ctx->channels);
        snprintf(args, sizeof(args),
                "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%llx",
                dec_ctx->time_base.num, dec_ctx->time_base.den, dec_ctx->sample_rate,
                av_get_sample_fmt_name(dec_ctx->sample_fmt),
                dec_ctx->channel_layout);
        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
            goto end;
        }

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                NULL, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "sample_fmts",
                (uint8_t*)&enc_ctx->sample_fmt, sizeof(enc_ctx->sample_fmt),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "channel_layouts",
                (uint8_t*)&enc_ctx->channel_layout,
                sizeof(enc_ctx->channel_layout), AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "sample_rates",
                (uint8_t*)&enc_ctx->sample_rate, sizeof(enc_ctx->sample_rate),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate\n");
            goto end;
        }
    } else {
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_spec,
                    &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

    /* Fill FilteringContext */
    fctx->buffersrc_ctx = buffersrc_ctx;
    fctx->buffersink_ctx = buffersink_ctx;
    fctx->filter_graph = filter_graph;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

int init_filters(FilteringContext** fc,const AVFormatContext* avfc_input,const AVFormatContext* avfc_output){
	int i;
	int ret;
	const char* filter_spec;
	
	*fc = (FilteringContext*)av_malloc_array(avfc_input->nb_streams,sizeof(**fc));
	if(!*fc){
		av_log(NULL,AV_LOG_ERROR,"create filtering context error!");
		return AVERROR(ENOMEM);
	}
	for(i=0;i<avfc_input->nb_streams;i++){
		(*fc)[i].buffersrc_ctx = NULL;
		(*fc)[i].buffersink_ctx = NULL;
		(*fc)[i].filter_graph = NULL;
		if(avfc_input->streams[i]->codec->codec_type!=AVMEDIA_TYPE_VIDEO&&avfc_input->streams[i]->codec->codec_type!=AVMEDIA_TYPE_AUDIO){
			continue;
		}
		if(avfc_input->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO){
			filter_spec = "null";
		}
		else{
			filter_spec = "anull";
		}
		ret = init_filter(&(*fc)[i],avfc_input->streams[i]->codec,avfc_output->streams[i]->codec,filter_spec);
		if(ret<0){
			return ret;
		}
	}
	return 0;
}

int filter_encode_write_frame(Transfer_Thread_Task* task,AVFrame *frame, unsigned int stream_index)
{
    int ret;
    AVFrame *filt_frame;
    av_log(NULL, AV_LOG_INFO, "Pushing decoded frame to filters,%d\n",pthread_self());
    /* push the decoded frame into the filtergraph */
	ret = av_buffersrc_add_frame_flags(task->filter_context[stream_index].buffersrc_ctx,
            frame, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph,%s,frame exist is %d\n",av_err2str(ret),frame!=NULL);
        return ret;
    }
	av_log(NULL,AV_LOG_INFO,"我日1\n");

    /* pull filtered frames from the filtergraph */
    while (1) {
        filt_frame = av_frame_alloc();
        if (!filt_frame) {
            ret = AVERROR(ENOMEM);
            break;
        }
        av_log(NULL, AV_LOG_INFO, "Pulling filtered frame from filters\n");
        ret = av_buffersink_get_frame(task->filter_context[stream_index].buffersink_ctx,
                filt_frame);
        if (ret < 0) {
            /* if no more frames for output - returns AVERROR(EAGAIN)
             * if flushed and no more frames for output - returns AVERROR_EOF
             * rewrite retcode to 0 to show it as normal procedure completion
             */
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                ret = 0;
            av_frame_free(&filt_frame);
            break;
        }

        filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
        ret = encode_write_frame(task,filt_frame, stream_index, NULL);
        if (ret < 0)
            break;
    }

    return ret;
}

int encode_video(Transfer_Thread_Task* task,AVFrame *filt_frame, unsigned int stream_index, int *got_frame){
	int ret;
	int got_frame_local;
	AVPacket enc_pkt;
	int (*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *);
	enc_func = avcodec_encode_video2;
	if(!got_frame){
		got_frame=&got_frame_local;
	}
	
	enc_pkt.data = NULL;
    enc_pkt.size = 0;
	enc_pkt.duration=0;
    av_init_packet(&enc_pkt);	
	av_log(NULL,AV_LOG_INFO,"start encoder video\n");
	ret = enc_func(task->output_format_context->streams[stream_index]->codec, &enc_pkt,
           			filt_frame, got_frame);
					av_log(NULL,AV_LOG_INFO,"end encoder video\n");
    av_frame_free(&filt_frame);
	av_log(NULL,AV_LOG_INFO,"free filt_frame\n");
    if (ret < 0){
		av_log(NULL, AV_LOG_INFO,"encoder error!%s,%d\n",av_err2str(ret),pthread_self());
		return ret;
	}
        
    if (!(*got_frame)){
		return 0;
	}
	
	/* prepare packet for muxing */
    enc_pkt.stream_index = stream_index;
	enc_pkt.pts = av_rescale_q_rnd(enc_pkt.pts,task->output_format_context->streams[stream_index]->codec->time_base,
									task->output_format_context->streams[stream_index]->time_base,
									AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
	enc_pkt.dts = av_rescale_q_rnd(enc_pkt.dts,task->output_format_context->streams[stream_index]->codec->time_base,
									task->output_format_context->streams[stream_index]->time_base,
									AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
	enc_pkt.duration = av_rescale_q(enc_pkt.duration,task->output_format_context->streams[stream_index]->codec->time_base,
									task->output_format_context->streams[stream_index]->time_base);
	if(enc_pkt.flags&AV_PKT_FLAG_KEY){
		av_bitstream_filter_filter(task->extrabsfc,task->decoder_context[stream_index],NULL,&(enc_pkt.data),&(enc_pkt.size),enc_pkt.data,enc_pkt.size,enc_pkt.flags&AV_PKT_FLAG_KEY);
	}
    ret = av_write_frame(task->output_format_context, &enc_pkt);
    return ret;
}

int encode_audio(Transfer_Thread_Task* task,AVFrame *filt_frame, unsigned int stream_index, int *got_frame){
	int ret;
	int got_frame_local;
	AVPacket enc_pkt;
	int encoder_frame_size = task->output_format_context->streams[stream_index]->codec->frame_size;
	
	int (*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *);
	enc_func = avcodec_encode_audio2;
	if(!got_frame){
		got_frame=&got_frame_local;
	}
	
	enc_pkt.data = NULL;
    enc_pkt.size = 0;
	enc_pkt.duration=0;
    av_init_packet(&enc_pkt);
	if(filt_frame==NULL){
		goto FLUSH;
	}
	
	int nb_samples = filt_frame->nb_samples;
	int fifo_size=0;
	av_log(NULL,AV_LOG_INFO,"pts is %d,dts is %d\n",filt_frame->pkt_pts,filt_frame->pkt_dts);
	if(nb_samples==encoder_frame_size){
FLUSH:
		av_log(NULL,AV_LOG_INFO,"first branch,%d,%d\n",nb_samples,encoder_frame_size);
		ret = enc_func(task->output_format_context->streams[stream_index]->codec, &enc_pkt,filt_frame, got_frame);
		av_frame_free(&filt_frame);
		if (ret < 0){
			av_log(NULL, AV_LOG_INFO,"encoder error!%s,%d\n",av_err2str(ret),pthread_self());
			return ret;
		}
	        
	    if (!(*got_frame)){
			av_log(NULL, AV_LOG_INFO,"got no frame,%d\n",pthread_self());
			return 0;
		}
		enc_pkt.stream_index = stream_index;
		if(task->pts!=0){
			av_log(NULL,AV_LOG_INFO,"hahahaha,pts is %d \n",task->pts);
			enc_pkt.pts = task->frame_index*encoder_frame_size+task->pts;
			enc_pkt.dts = task->frame_index*encoder_frame_size+task->dts;
			enc_pkt.duration = encoder_frame_size;
			task->frame_index++;
		}
		av_log(NULL,AV_LOG_INFO,"before rescale pts is %d,dts is %d,length is %d\n",enc_pkt.pts,enc_pkt.dts,enc_pkt.duration);
		enc_pkt.pts = av_rescale_q_rnd(enc_pkt.pts,task->output_format_context->streams[stream_index]->codec->time_base,
										task->output_format_context->streams[stream_index]->time_base,
										AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
		enc_pkt.dts = av_rescale_q_rnd(enc_pkt.dts,task->output_format_context->streams[stream_index]->codec->time_base,
										task->output_format_context->streams[stream_index]->time_base,
										AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
		enc_pkt.duration = av_rescale_q(enc_pkt.duration,task->output_format_context->streams[stream_index]->codec->time_base,
										task->output_format_context->streams[stream_index]->time_base)/2;
		 av_log(NULL, AV_LOG_INFO,"after rescale pts is %d,dts is %d,duration is %d\n",enc_pkt.pts,enc_pkt.dts,enc_pkt.duration);
				
		ret = av_write_frame(task->output_format_context, &enc_pkt);
    		return ret;
	}
	else{
		if(task->avaf==NULL){
			av_frame_free(&filt_frame);
			return 0;
		}
		if(task->pts==0){
			av_log(NULL,AV_LOG_INFO,"new thread first audio frame,pts is %d,dts is %d\n",filt_frame->pkt_pts,filt_frame->pkt_dts);
			task->pts = filt_frame->pkt_pts;
			task->dts = filt_frame->pkt_dts;
		}
		av_audio_fifo_realloc(task->avaf,av_audio_fifo_size(task->avaf)+nb_samples);
		av_audio_fifo_write(task->avaf,&filt_frame->data,nb_samples);
		fifo_size = av_audio_fifo_size(task->avaf);

		AVFrame* newframe=NULL;
		while(av_audio_fifo_size(task->avaf)>=encoder_frame_size){
			av_log(NULL,AV_LOG_INFO,"fifo while start,fifosize is %d,encoder_frame_size is %d,%x\n",av_audio_fifo_size(task->avaf),encoder_frame_size,&enc_pkt);			
			av_log(NULL,AV_LOG_INFO,"什么情况\n");
			if(newframe!=NULL){
				av_frame_free(&newframe);
				newframe = NULL;
			}
			newframe = av_frame_alloc();
			newframe->nb_samples = encoder_frame_size;
			newframe->channel_layout = task->output_format_context->streams[stream_index]->codec->channel_layout;
			newframe->format = task->output_format_context->streams[stream_index]->codec->sample_fmt;
			newframe->sample_rate = task->output_format_context->streams[stream_index]->codec->sample_rate;
			newframe->channels = task->output_format_context->streams[stream_index]->codec->channels;
			newframe->pkt_pts = task->frame_index*encoder_frame_size+task->pts;
			newframe->pts = task->frame_index*encoder_frame_size+task->pts;
			newframe->pkt_dts = task->frame_index*encoder_frame_size+task->dts;
			av_frame_get_buffer(newframe,0);
			//filt_frame->nb_samples = encoder_frame_size;
			ret=av_audio_fifo_read(task->avaf,(void**)newframe->data,encoder_frame_size);
			if(ret<0){
				av_log(NULL,AV_LOG_INFO,"av audio fifo read error,%s",av_err2str(ret));
			}
			av_log(NULL,AV_LOG_INFO,"这次读了%d个字节，还剩下%d个字节，咋了\n",ret,av_audio_fifo_size(task->avaf));
			
			enc_pkt.data=NULL;
			enc_pkt.size = 0;
			enc_pkt.duration=0;
			av_init_packet(&enc_pkt);
			
			ret = enc_func(task->output_format_context->streams[stream_index]->codec, &enc_pkt,newframe,got_frame);
			if (ret < 0){
				break;
			}
		        
		    if (!(*got_frame)){
				ret = 0;
				break;
			}
		    /* prepare packet for muxing */
		    enc_pkt.stream_index = stream_index;
			enc_pkt.pts = task->frame_index*encoder_frame_size+task->pts;
			enc_pkt.dts = task->frame_index*encoder_frame_size+task->dts;
			enc_pkt.duration = encoder_frame_size;
			task->frame_index++;
			av_log(NULL,AV_LOG_INFO,"before rescale pts is %d,dts is %d,length is %d\n",enc_pkt.pts,enc_pkt.dts,enc_pkt.duration);
			enc_pkt.pts = av_rescale_q_rnd(enc_pkt.pts,task->output_format_context->streams[stream_index]->codec->time_base,
											task->output_format_context->streams[stream_index]->time_base,
											AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
			enc_pkt.dts = av_rescale_q_rnd(enc_pkt.dts,task->output_format_context->streams[stream_index]->codec->time_base,
											task->output_format_context->streams[stream_index]->time_base,
											AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
			enc_pkt.duration = av_rescale_q(enc_pkt.duration,task->output_format_context->streams[stream_index]->codec->time_base,
											task->output_format_context->streams[stream_index]->time_base);
		    av_log(NULL, AV_LOG_INFO,"after rescale pts is %d,dts is %d,duration is %d\n",enc_pkt.pts,enc_pkt.dts,enc_pkt.duration);
			//ret = av_interleaved_write_frame(task->output_format_context, &enc_pkt);
			ret = av_write_frame(task->output_format_context, &enc_pkt);
		}
		av_frame_free(&filt_frame);
		filt_frame=NULL;
		av_log(NULL,AV_LOG_INFO,"fifo while end\n");

		return ret;
	}
}
int encode_write_frame(Transfer_Thread_Task* task,AVFrame *filt_frame, unsigned int stream_index, int *got_frame) {
    int ret;
	if(task->output_format_context->streams[stream_index]->codec->codec_type ==AVMEDIA_TYPE_VIDEO){
		ret = encode_video(task,filt_frame,stream_index,got_frame);
	}else{
		ret = encode_audio(task,filt_frame,stream_index,got_frame);
	}
	return ret;
}

int flush_encoder(Transfer_Thread_Task* task,unsigned int stream_index)
{
    int ret;
    int got_frame;

    if (!(task->output_format_context->streams[stream_index]->codec->codec->capabilities &
                AV_CODEC_CAP_DELAY))
        return 0;

    while (1) {
        av_log(NULL, AV_LOG_INFO, "Flushing stream #%u encoder,%d\n", stream_index,pthread_self());
        ret = encode_write_frame(task,NULL, stream_index, &got_frame);
        if (ret < 0)
            break;
        if (!got_frame)
            return 0;
    }
    return ret;
}

void* thread_func(void* arg){
	int ret=0;
	int got_frame;
	int streamindex;
	int nbstreams;
	int i;
	int doonce = 1;
	unsigned char *dummy=NULL;   
	int dummy_len;
	enum AVMediaType type;
	AVFrame* frame;
	AVPacket* packet;
	DECFUNC def_func;
	Transfer_Thread_Task* task = (Transfer_Thread_Task*)arg;
	
	pthread_mutex_lock(&locker);	
	if((ret = open_output_file(input_format_context,task->outputfilename,task))<0){
		av_log(NULL,AV_LOG_ERROR,"open output avformat context error!");
		goto end;
	}
	nbstreams = input_format_context->nb_streams;
	pthread_mutex_unlock(&locker);	
	av_log(NULL,AV_LOG_INFO,"output oformat file name is %s,%d\n",task->output_format_context->filename,pthread_self());
	
	pthread_mutex_lock(&task->mutex);
	while(!task->pushover){
		pthread_cond_wait(&task->cond,&task->mutex);
	}
	pthread_mutex_unlock(&task->mutex);
	av_log(NULL,AV_LOG_INFO,"start,%s,length is %d,tid %d\n",task->outputfilename,task->pl.length,pthread_self());
	
	while(task->pl.length>0){
		packet = PacketListGetFront(&task->pl);
		streamindex = packet->stream_index;
		type = (task->decoder_context)[streamindex]->codec_type;
		frame = av_frame_alloc();
		if(!frame){
			av_log(NULL,AV_LOG_ERROR,"alloc frame error!");
			ret= AVERROR(ENOMEM);
			av_packet_unref(packet);
			goto end;
		}
		av_log(NULL,AV_LOG_INFO,"dec:%d,%d\n",task->input_stream_time_base[streamindex].den,task->input_codec_time_base[streamindex].den);
		packet->pts = av_rescale_q_rnd(packet->pts,task->input_stream_time_base[streamindex],
										task->input_codec_time_base[streamindex],
										AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
		packet->dts = av_rescale_q_rnd(packet->dts,task->input_stream_time_base[streamindex],
										task->input_codec_time_base[streamindex],
										AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
		if(type==AVMEDIA_TYPE_VIDEO){
			def_func = avcodec_decode_video2;
			if(doonce==1){
				ret = av_bitstream_filter_filter(task->h264_mp4toannexbbsfc, task->decoder_context[streamindex], NULL, &dummy, &dummy_len, packet->data, packet->size, 1);
				doonce=0;
				av_log(NULL,AV_LOG_INFO,"extradatasize is %d,return is %s\n",(task->decoder_context)[streamindex]->extradata_size,av_err2str(ret));
			}
		}else{
			def_func = avcodec_decode_audio4;
		}
		
		ret = def_func((task->decoder_context)[streamindex],frame,&got_frame,packet);
		av_log(NULL,AV_LOG_INFO,"decoder over\n");
		if (ret < 0) {
			av_frame_free(&frame);
			av_log(NULL, AV_LOG_ERROR, "Decoding failed,%d,%s\n",pthread_self(),av_err2str(ret));
			av_packet_unref(packet);
			goto end;
		}
		if (got_frame) {
			frame->pts = av_frame_get_best_effort_timestamp(frame);
			ret = filter_encode_write_frame(task,frame, streamindex);
			av_frame_free(&frame);
			if (ret < 0){
				av_packet_unref(packet);
				goto end;
			}  
		} else {
			av_log(NULL, AV_LOG_INFO,"got no frame\n");
			av_frame_free(&frame);
		}
		av_packet_unref(packet);
	}
	/* flush filters and encoders */
    for (i = 0; i < nbstreams; i++) {
		av_log(NULL, AV_LOG_INFO, "goto flushing\n");
        /* flush filter */
        if (!task->filter_context[i].filter_graph)
            continue;
        ret = filter_encode_write_frame(task,NULL, i);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing filter failed\n");
            goto end;
        }

		av_log(NULL,AV_LOG_INFO,"start flush encoder\n");
        /* flush encoder */
        ret = flush_encoder(task,i);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
            goto end;
        }
    }
	av_log(NULL,AV_LOG_INFO,"over,%s,length is %d,tid %d\n",task->outputfilename,task->pl.length,pthread_self());
	av_write_trailer(task->output_format_context);

end:	
av_log(NULL,AV_LOG_INFO,"start release resource \n");
	if(ret<0){
		ReleasePacketList(task->pl);
	}
	av_log(NULL,AV_LOG_INFO,"start release resource \n");
	for (i = 0; i < nbstreams; i++) {
		if(task->decoder_context){
			avcodec_close(task->decoder_context[i]);
		}
        if (task->output_format_context && task->output_format_context->nb_streams > i && task->output_format_context->streams[i] && task->output_format_context->streams[i]->codec)
            avcodec_close(task->output_format_context->streams[i]->codec);
        if (task->filter_context && task->filter_context[i].filter_graph)
            avfilter_graph_free(&task->filter_context[i].filter_graph);
    }
	if (task->output_format_context && !(task->output_format_context->oformat->flags & AVFMT_NOFILE))
        avio_closep(&task->output_format_context->pb);
	if(task->output_format_context){
		avformat_free_context(task->output_format_context);
		task->output_format_context = NULL;
	}
	
	if(task->avaf){
		av_audio_fifo_free(task->avaf);
		task->avaf=NULL;
	}
	if(task->h264_mp4toannexbbsfc){
		av_bitstream_filter_close(task->h264_mp4toannexbbsfc);
	}
	if(task->extrabsfc){
		av_bitstream_filter_close(task->extrabsfc);
	}
	
	
	task->pts=0;
	task->dts=0;
	task->frame_index=0;
	return (void*)ret;
}

int needChangeOutputFile(int* tsbase,AVPacket* packet,AVRational time_base){
	if(packet->flags!=1){
		return -1;
	}
	int ts;
	int tsnum;
	int tsden;
	int time;
	ts = packet->pts-*tsbase;
	tsnum = time_base.num;
	tsden = time_base.den;
	
	time = (ts*tsnum)/tsden;
	av_log(NULL,AV_LOG_INFO,"%lld-%d-%d-%d\n",packet->pts,ts,tsden,time);
	if(time>5){
		*tsbase = packet->pts;
		return 0;
	}
	return -1;
}

int getNextOutputFileName(char* inputfilename,char* outputpath,char* outputfilename){
	int i=0;
	char buf1[100];
	char buf2[100];
	memset(buf1,0,100);
	memset(buf2,0,100);
	
	strncpy(buf1,inputfilename,sizeof(buf1));
	for(i=0;i<strlen(buf1);i++){
		if(buf1[i]=='.'){
			buf1[i]='\0';
			break;
		}
	}
	av_log(NULL,AV_LOG_INFO,"current fileindex is %d\n",fileindex);
	snprintf(buf2,sizeof(buf2),"%s%d","-",fileindex);
	snprintf(outputfilename,255,"%s%s%s%s%s",outputpath,"/",buf1,buf2,".ts");
	return 0;
}

int CreateTransTask(char* inputfilename, char* outputpath){
	if(inputfilename==NULL || outputpath==NULL){
		return -1;
	}
	av_log_set_level(AV_LOG_INFO);
	
	int ret;
	int got_frame=0;
	int i;
	int j;
	int ts=0;
	int thread_count=5;
	int start=1;
	int streamindex;
	AVPacket* startPacket=NULL;
	int index=0;
	int thread_ret;
	enum AVMediaType type;
	Transfer_Thread_Task task[thread_count];
	pthread_t id[thread_count];
	AVFrame* frame;
	char outputfilename[255];
	
	ret=getEffectiveUID(outputpath);
	if(ret<0){
		return -1;
	}
	
	init_transfer_task(task,thread_count);
	
	if ((ret=open_input_file(inputfilename,&input_format_context,task,thread_count))<0){
		goto end;
	}

loop:
	while(1){
		AVPacket* packet;
		packet = (AVPacket*)malloc(sizeof(AVPacket));
		if((ret = av_read_frame(input_format_context,packet))<0){
			if(ret==AVERROR_EOF){
				av_log(NULL,AV_LOG_INFO,"read inputfile frame over!\n");
				task[fileindex%thread_count].pushover = 1;
				if(task[fileindex%thread_count].pushover){
					pthread_cond_signal(&task[fileindex%thread_count].cond);
					pthread_mutex_unlock(&task[fileindex%thread_count].mutex);
				}
				fileindex++;
				break;
			}
			else{
				av_log(NULL,AV_LOG_ERROR,"read inputfile frame error!");
				break;
			}
		}
		
		streamindex = packet->stream_index;
		type = input_format_context->streams[packet->stream_index]->codec->codec_type;
		if(start==1){
			index = fileindex;
			getNextOutputFileName(inputfilename,outputpath,outputfilename);
			strncpy(task[index%thread_count].outputfilename,outputfilename,sizeof(outputfilename));
			pthread_attr_t attr;
			pthread_attr_init(&attr);
			pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_JOINABLE);
			pthread_create(&id[index%thread_count],&attr,thread_func,(void*)&task[index%thread_count]);
			start = 0;
			if(startPacket!=NULL){
				PacketListPushBack(&task[index%thread_count].pl,startPacket);
			}
		}else{
			if(type==AVMEDIA_TYPE_VIDEO){
				if(needChangeOutputFile(&ts,packet,input_format_context->streams[streamindex]->time_base)==0){
					av_log(NULL,AV_LOG_INFO,"need change output file\n");
					task[fileindex%thread_count].pushover = 1;
					if(task[fileindex%thread_count].pushover){
						pthread_cond_signal(&task[fileindex%thread_count].cond);
						pthread_mutex_unlock(&task[fileindex%thread_count].mutex);
					}
					fileindex++;
					if(fileindex%thread_count==0){
						startPacket = packet;
						break;
					}
					index = fileindex;
					getNextOutputFileName(inputfilename,outputpath,outputfilename);
					strncpy(task[index%thread_count].outputfilename,outputfilename,sizeof(outputfilename));
					pthread_attr_t attr;
					pthread_attr_init(&attr);
					pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_JOINABLE);
					pthread_create(&id[index%thread_count],&attr,thread_func,(void*)&task[index%thread_count]);
				}
			}
		}
		PacketListPushBack(&task[index%thread_count].pl,packet);
	}
	//av_log(NULL,AV_LOG_INFO,"current index is %d\n",fileindex);
	for(i=0;i<=(fileindex-1)%thread_count;i++){
		pthread_join(id[i],&thread_ret);
		av_log(NULL,AV_LOG_INFO,"join over,%d,%d\n",i,fileindex);
		task[i].pushover = 0;
		if(thread_ret<0){
			av_log(NULL,AV_LOG_INFO,"thread abnormal quit\n");
			goto end;
		}
	}
	start=1;
	if(ret<0){
		goto end;
	}
	av_log(NULL,AV_LOG_INFO,"next loop\n");
	goto loop;
end:
	av_log(NULL,AV_LOG_INFO,"goto end\n");
	avformat_close_input(&input_format_context);
	return ret;
}
