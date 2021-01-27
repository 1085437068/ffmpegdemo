// tutorial04.c
// A pedagogical video player that will stream through every video frame as fast as it can,
// and play audio (out of sync).
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard, 
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101, SDL 1.2.15
// on GCC 4.7.2 in Debian February 2015
// Use
//
// gcc -o tutorial04 tutorial04.c -lavformat -lavcodec -lswscale -lz -lm `sdl-config --cflags --libs`
// to build (assuming libavformat and libavcodec are correctly installed, 
// and assuming you have sdl-config. Please refer to SDL docs for your installation.)
//
// Run using
// tutorial04 myvideofile.mpg
//
// to play the video stream on your screen.

#include <stdio.h>
#include <assert.h>
#include <math.h>

#include <SDL.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

//音频队列包
typedef struct PacketQueue {
  AVPacketList *first_pkt, *last_pkt;
  int nb_packets;  //packet个数
  int size;  //队列包大小
  SDL_mutex *mutex;  //互斥量
  SDL_cond *cond;  //信号
} PacketQueue; 

//视频图像
typedef struct VideoPicture {
  AVPicture *pict;  //数据
  int width, height; /* source height & width，frame的长和宽*/
  int allocated;  //标记位，表明是否成功分配
} VideoPicture;

//將所有需要的变量放入结构体进行处理
typedef struct VideoState {

  //for multi-media file
  char            filename[1024]; 
  AVFormatContext *pFormatCtx;  //多媒体上下文

  int             videoStream, audioStream;  //视频、音频的索引号

  //for audio 音频变量
  AVStream        *audio_st;  //音频流
  AVCodecContext  *audio_ctx;  //音频解码器上下文
  PacketQueue     audioq;  //音频队列
  uint8_t         audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];  //分配音频缓冲区空间
  unsigned int    audio_buf_size;  //音频缓冲区的大小
  unsigned int    audio_buf_index;  //读取缓冲区的实时变化位置
  AVFrame         audio_frame;  //音频frame
  AVPacket        audio_pkt;  //音频packet
  uint8_t         *audio_pkt_data;  //packet中的数据指针
  int             audio_pkt_size;  //packet的大小
  struct SwrContext *audio_swr_ctx;  //重采样的上下文

  //for video 视频变量
  AVStream        *video_st;  //视频流
  AVCodecContext  *video_ctx;  //视频解码器上下文
  PacketQueue     videoq;  //视频队列
  struct SwsContext *sws_ctx;  //视频显示scale调整的上下文

  VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];  //？？？VIDEO_PICTURE_QUEUE_SIZE为1,只能存放两个图片
  int             pictq_size, pictq_rindex, pictq_windex;  //？？？和picture有关，暂时不知道什么作用

  //for thread  线程相关变量
  SDL_mutex       *pictq_mutex;  //互斥量
  SDL_cond        *pictq_cond;  //信号量
  
  SDL_Thread      *parse_tid;  //线程id:解复用
  SDL_Thread      *video_tid;  //线程id：视频解码

  int             quit; //退出标志位

} VideoState;

SDL_mutex       *texture_mutex;  //互斥量
SDL_Window      *win;   //窗口
SDL_Renderer    *renderer;  //渲染器
SDL_Texture     *texture;  //纹理

FILE            *audiofd = NULL;  //输出文件
FILE            *audiofd1 = NULL;  //输出文件

/* Since we only have one decoding thread, the Big Struct
   can be global in case we need it. */
VideoState *global_video_state;  //声明一个全局变量

/*@briefly:初始化队列包，分配内存
 *@return:void 
 */
void packet_queue_init(PacketQueue *q) {
  memset(q, 0, sizeof(PacketQueue));  //为队列分配空间
  q->mutex = SDL_CreateMutex(); //创建锁
  q->cond = SDL_CreateCond();  //创建信号量
}

/*@briefly:將packet放入队列包中
 *@return:0--成功， 1-- 失败
 */
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

  AVPacketList *pkt1;  //新建list变量，保存当前的packet
  if(av_dup_packet(pkt) < 0) {  //packet引用计数+1
    return -1;
  }
  pkt1 = av_malloc(sizeof(AVPacketList));
  if (!pkt1)
    return -1;
  pkt1->pkt = *pkt; //packet数据放入list中
  pkt1->next = NULL;
  
  SDL_LockMutex(q->mutex);  //对对象q进行加锁保护，更新变量

  if (!q->last_pkt)
    q->first_pkt = pkt1;
  else
    q->last_pkt->next = pkt1;
  q->last_pkt = pkt1;
  q->nb_packets++;
  q->size += pkt1->pkt.size;
  //fprintf(stderr, "enqueue, packets:%d, send cond signal\n", q->nb_packets);
  SDL_CondSignal(q->cond);  //发送信号，此时现将q解锁，wait得到消息后，这里开始不断寻求將q加锁
  
  SDL_UnlockMutex(q->mutex);  //对对象q解锁，解除保护
  return 0;
}

/*@briefly:將队列包中的packet取出
 *@return:0--成功， -1 -- 失败
 */
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
  AVPacketList *pkt1;
  int ret;

  SDL_LockMutex(q->mutex);  //加锁
  
  for(;;) {
    
    if(global_video_state->quit) {  //退出标志位为1时，退出
      fprintf(stderr, "quit from queue_get\n");
      ret = -1;
      break;
    }

    pkt1 = q->first_pkt;  //从队列头开始取出packet
    if (pkt1) {
      q->first_pkt = pkt1->next;
      if (!q->first_pkt)
	q->last_pkt = NULL;
      q->nb_packets--;  //packet个数-1
      q->size -= pkt1->pkt.size;  //队列的大小减小
      *pkt = pkt1->pkt; 
      av_free(pkt1);  //packet_queue_get中有av_malloc(),所以使用过后，这里要將其内存释放
      ret = 1;
      break;
    } else if (!block) {
      ret = 0;
      break;
    } else {   //队列中没有数据了，此时Conwait先將对象q解锁，之后一直等待cond发来的信号，再将其加上锁，开始获取packet
      fprintf(stderr, "queue is empty, so wait a moment and wait a cond signal\n");
      SDL_CondWait(q->cond, q->mutex);  
    }
  }
  SDL_UnlockMutex(q->mutex);  //解锁
  return ret;
}

/*@briefly:音频解码
 *@return:0--成功， -1-- 失败
 */
int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size) {

  int len1, data_size = 0;  //？？？
  AVPacket *pkt = &is->audio_pkt;  //

  for(;;) {
    while(is->audio_pkt_size > 0) {

      int got_frame = 0;
      len1 = avcodec_decode_audio4(is->audio_ctx, &is->audio_frame, &got_frame, pkt);
      if(len1 < 0) {
	/* if error, skip frame */
	fprintf(stderr, "Failed to decode audio ......\n");
	is->audio_pkt_size = 0;
	break;
      }

      data_size = 0;
      if(got_frame) {
	/*
	fprintf(stderr, "auido: channels:%d, nb_samples:%d, sample_fmt:%d\n",
			is->audio_ctx->channels,
			is->audio_frame.nb_samples,
			is->audio_ctx->sample_fmt);

	data_size = av_samples_get_buffer_size(NULL, 
					       is->audio_ctx->channels,
					       is->audio_frame.nb_samples,
					       is->audio_ctx->sample_fmt,
					       1);
	*/
	data_size = 2 * is->audio_frame.nb_samples * 2;
	assert(data_size <= buf_size);
	//memcpy(audio_buf, is->audio_frame.data[0], data_size);
	//重采样
	swr_convert(is->audio_swr_ctx,
                        &audio_buf,
                        MAX_AUDIO_FRAME_SIZE*3/2,
                        (const uint8_t **)is->audio_frame.data,
                        is->audio_frame.nb_samples);
	
	//将audio_buf数据写入audiofd
	fwrite(audio_buf, 1, data_size, audiofd);
        fflush(audiofd);
      }
      //???没看懂这两个数据为什么一增一减,估计是单纯记录audiofd1中数据变化的两个变量
      is->audio_pkt_data += len1;
      is->audio_pkt_size -= len1;
      if(data_size <= 0) {
	/* No data yet, get more frames */
	continue;
      }
      /* We have data, return it and come back for more later */
      return data_size;
    }

    if(pkt->data)
      av_free_packet(pkt);

    if(is->quit) {
      fprintf(stderr, "will quit program......\n");
      return -1;
    }

    /* next packet 从队列包中获取packet数据*/
    if(packet_queue_get(&is->audioq, pkt, 1) < 0) {
      return -1;
    }
    //更新数据--audiofd的记录数据
    is->audio_pkt_data = pkt->data;
    is->audio_pkt_size = pkt->size;
  }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {

  VideoState *is = (VideoState *)userdata;
  int len1, audio_size;

   SDL_memset(stream, 0, len);

  while(len > 0) {
    if(is->audio_buf_index >= is->audio_buf_size) {  //???此时索引大于缓冲区大小，说明缓冲区中的数据已经没了，需要继续进行解码补充数据
      /* We have already sent all our data; get more */
      audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf));  //將音频解码进行封装下
      if(audio_size < 0) {
	/* If error, output silence，解码失败设置为静默音 */
	is->audio_buf_size = 1024*2*2;
	memset(is->audio_buf, 0, is->audio_buf_size);
      } else { //成功解码
	is->audio_buf_size = audio_size;
      }
      is->audio_buf_index = 0; //索引从头开始，记录缓冲区此时已经使用数据的位置
    }
    len1 = is->audio_buf_size - is->audio_buf_index;  //音频缓冲区中剩余的数据量
    fprintf(stderr, "stream addr:%p, audio_buf_index:%d, len1:%d, len:%d\n",
		    stream,
	  	    is->audio_buf_index, 
		    len1, 
		    len);  //%p表示输出十六进制数据，这里表示將地址输出
    if(len1 > len)
      len1 = len;
    //memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
    fwrite(is->audio_buf, 1, len1, audiofd1);  //將audio_buf的数据写入audiofd1中                 
    fflush(audiofd1);   //保证全部写入，將内存缓冲区剩余的数据全部写入audiofd1中
    SDL_MixAudio(stream,(uint8_t *)is->audio_buf, len1, SDL_MIX_MAXVOLUME);  //设置音量大小
    len -= len1; //冲这两行代码可以看出，当声卡读入数据后，其地址一直在正向增加，len也一直在减少，说明这个缓冲区是类似滑动窗口的效果，不是固定大小
    stream += len1;
    is->audio_buf_index += len1;
  }
}

//更新视频刷新事件
static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
  SDL_Event event;
  event.type = FF_REFRESH_EVENT;
  event.user.data1 = opaque;
  SDL_PushEvent(&event);
  return 0; /* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms */
static void schedule_refresh(VideoState *is, int delay) {
  SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

//图片渲染
void video_display(VideoState *is) {

  SDL_Rect rect;  //渲染区域
  VideoPicture *vp;  
  float aspect_ratio;  //长宽比
  int w, h, x, y;
  int i;

  vp = &is->pictq[is->pictq_rindex];  
  if(vp->pict) { //队列中有图像数据

    if(is->video_ctx->sample_aspect_ratio.num == 0) {  //长宽比
      aspect_ratio = 0;
    } else {
      aspect_ratio = av_q2d(is->video_ctx->sample_aspect_ratio) *
	is->video_ctx->width / is->video_ctx->height;
    }

    if(aspect_ratio <= 0.0) {
      aspect_ratio = (float)is->video_ctx->width /
	(float)is->video_ctx->height;
    }

    /*
    h = screen->h;
    w = ((int)rint(h * aspect_ratio)) & -3;
    if(w > screen->w) {
      w = screen->w;
      h = ((int)rint(w / aspect_ratio)) & -3;
    }
    x = (screen->w - w) / 2;
    y = (screen->h - h) / 2;
    */

    SDL_UpdateYUVTexture( texture, NULL,
                          vp->pict->data[0], vp->pict->linesize[0],
                          vp->pict->data[1], vp->pict->linesize[1],
                          vp->pict->data[2], vp->pict->linesize[2]);
    
    rect.x = 0;
    rect.y = 0;
    rect.w = is->video_ctx->width;
    rect.h = is->video_ctx->height;

    SDL_LockMutex(texture_mutex);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, &rect);
    SDL_RenderPresent(renderer);
    SDL_UnlockMutex(texture_mutex);

  }
}

//视频刷新：demuxing和decode其实都是单独的线程，而视频的刷新其实是在主线程当中进行的
void video_refresh_timer(void *userdata) {

  VideoState *is = (VideoState *)userdata;  //传入的is对象数据
  VideoPicture *vp;  
  
  if(is->video_st) {  //如果存在视频流
    if(is->pictq_size == 0) {  //如果picture中没有数据，快速刷新等待picture queue中的数据 
      schedule_refresh(is, 1); //if the queue is empty, so we shoud be as fast as checking queue of picture
    } else { //正常获取到picture数据
      vp = &is->pictq[is->pictq_rindex];  
      /* Now, normally here goes a ton of code
	 about timing, etc. we're just going to
	 guess at a delay for now. You can
	 increase and decrease this value and hard code
	 the timing - but I don't suggest that ;)
	 We'll learn how to do it for real later.
      */
      schedule_refresh(is, 40); //RR_REFREESH_EVENT事件跟新
      
      /* show the picture! */
      video_display(is);
      
      /* update queue for next picture! */
      if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {  //pictq_rindex为VIDEO_PICTURE_QUEUE_SIZE，说明picture list中的图片播放结束
	is->pictq_rindex = 0;  //索引为0时有数据，而为1时，其实已经超出了索引范围，需要重新置为0
      }
      SDL_LockMutex(is->pictq_mutex);
      is->pictq_size--;
      SDL_CondSignal(is->pictq_cond);  //???通知wait，vp中的数据用了，queue_picture你看下，没有数据的话就快点转到alloc_picture补充数据和分配屏幕显示空间， 有数据的话就快点將数据转换成YUV格式
      SDL_UnlockMutex(is->pictq_mutex);
    }
  } else {  //不存在视频流时，就多延时一段时间，等着
    schedule_refresh(is, 100);
  }
}
//???快点往pictq中塞数据吧，似乎没有塞数据，只是分配空间？
void alloc_picture(void *userdata) {

  VideoState *is = (VideoState *)userdata;
  VideoPicture *vp;

  vp = &is->pictq[is->pictq_windex];
  if(vp->pict) {//free space if vp->pict is not NULL 队列中海还有数据，就先释放掉
    avpicture_free(vp->pict);
    free(vp->pict);
  }
  //需要分配空间给展示在屏幕上的YUV数据
  // Allocate a place to put our YUV image on that screen
  SDL_LockMutex(texture_mutex);
  vp->pict = (AVPicture*)malloc(sizeof(AVPicture));
  if(vp->pict){
    avpicture_alloc(vp->pict,  //分配空间以及设置数据格式
		    AV_PIX_FMT_YUV420P, 
		    is->video_ctx->width,
		    is->video_ctx->height);
  }
  SDL_UnlockMutex(texture_mutex);

  vp->width = is->video_ctx->width;
  vp->height = is->video_ctx->height;
  vp->allocated = 1;  //分配完空间了

}

int queue_picture(VideoState *is, AVFrame *pFrame) {

  VideoPicture *vp;  //???
  int dst_pix_fmt;  //???
  AVPicture pict;  //???

  /* wait until we have space for a new pic */
  SDL_LockMutex(is->pictq_mutex);
  while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE &&
	!is->quit) {
    SDL_CondWait(is->pictq_cond, is->pictq_mutex);  //等待video_refresh_timer发送信号，进行YUV转换
  }
  SDL_UnlockMutex(is->pictq_mutex);

  if(is->quit){
    fprintf(stderr, "quit from queue_picture....\n");
    return -1;
  }

  // ???windex is set to 0 initially 窗口win播放的图片索引吗0代表window没有可以展示的图片了
  vp = &is->pictq[is->pictq_windex];

  /*
  fprintf(stderr, "vp.width=%d, vp.height=%d, video_ctx.width=%d, video_ctx.height=%d\n", 
		  vp->width, 
		  vp->height, 
		  is->video_ctx->width,
		  is->video_ctx->height);
  */

  /* allocate or resize the buffer! 如果当前窗口没有展示图片（没有数据了，需要去alloc_picture中补充数据），或者不符合要求*/
  if(!vp->pict ||
     vp->width != is->video_ctx->width ||
     vp->height != is->video_ctx->height) {

    vp->allocated = 0;  //表明图像队列中未分配图像
    alloc_picture(is);  //获取图片
    if(is->quit) {
      fprintf(stderr, "quit from queue_picture2....\n");
      return -1;
    }
  }

  /* We have a place to put our picture on the queue */

  if(vp->pict) {//有数据就快点转换格式，display中要使用
    //将数据转换为YUV格式，以便在video_display能够从vp中直接得到YUV格式数据，最后得以展示
    // Convert the image into YUV format that SDL uses
    sws_scale(is->sws_ctx, (uint8_t const * const *)pFrame->data,
	      pFrame->linesize, 0, is->video_ctx->height,
	      vp->pict->data, vp->pict->linesize);
    //???通知线程display，我们已经获取了pic 数据，可以展示了
    /* now we inform our display thread that we have a pic ready */
    if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
      is->pictq_windex = 0;
    }
    SDL_LockMutex(is->pictq_mutex);
    is->pictq_size++;
    SDL_UnlockMutex(is->pictq_mutex);
  }
  return 0;
}

int video_thread(void *arg) {
  VideoState *is = (VideoState *)arg;
  AVPacket pkt1, *packet = &pkt1;
  int frameFinished;
  AVFrame *pFrame;

  pFrame = av_frame_alloc();  //先要分配空间给frame，才能播放出来

  for(;;) {
    if(packet_queue_get(&is->videoq, packet, 1) < 0) { //获取packet
      // means we quit getting packets
      break;
    }

    // Decode video frame //获取frame
    avcodec_decode_video2(is->video_ctx, pFrame, &frameFinished, packet);

    // Did we get a video frame?
    if(frameFinished) { //成功获取frame
      if(queue_picture(is, pFrame) < 0) {  //展示frame
	break;
      }      
    }

    av_free_packet(packet);  //packet用完后要释放不然会堆积在内存中，影响效率
  }
  av_frame_free(&pFrame);  //同理frame也要释放
  return 0;
}

//將这一部分单独抽取出来是因为音频和视频的解码很大一部分是重合的，对这些共同部分可以一起处理
int stream_component_open(VideoState *is, int stream_index) {

  int64_t in_channel_layout, out_channel_layout;  //音频重采样时需要的参数

  AVFormatContext *pFormatCtx = is->pFormatCtx;  //format上下文
  AVCodecContext *codecCtx = NULL;  //codec上下文
  AVCodec *codec = NULL;  //codec结构体
  SDL_AudioSpec wanted_spec, spec;  //声卡参数
  //验证是否找错：index超出stream的总数，或者小于0即为错误
  if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
    return -1;  
  }
  //根据信息寻找解码器
  codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codec->codec_id);
  if(!codec) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1;
  }
  //复制codec上下文信息，在复制的结构上进行操作
  codecCtx = avcodec_alloc_context3(codec);
  if(avcodec_copy_context(codecCtx, pFormatCtx->streams[stream_index]->codec) != 0) {
    fprintf(stderr, "Couldn't copy codec context");
    return -1; // Error copying codec context
  }

  //解码：音频
  if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
    // Set audio settings from codec info 音频参数设置，主要是自己设置参数，使得各个文件统一成一样的格式
    wanted_spec.freq = codecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = codecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;  //采样数据量
    wanted_spec.callback = audio_callback;  //回调函数中，音频已经开始解码了，可以看做声卡已经新开了一个线程进行处理
    wanted_spec.userdata = is;
    //打开声卡
    if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
      fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
      return -1;
    }
  }

  //打开编解码器
  if(avcodec_open2(codecCtx, codec, NULL) < 0) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1;
  }

  switch(codecCtx->codec_type) {  
  case AVMEDIA_TYPE_AUDIO:
    is->audioStream = stream_index;  //重采样需要的变量
    is->audio_st = pFormatCtx->streams[stream_index];
    is->audio_ctx = codecCtx;
    is->audio_buf_size = 0;
    is->audio_buf_index = 0;
    memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
    packet_queue_init(&is->audioq);  //初始化音频队列包
    SDL_PauseAudio(0);  //声卡播放

    in_channel_layout=av_get_default_channel_layout(is->audio_ctx->channels);
    out_channel_layout = in_channel_layout;

    is->audio_swr_ctx = swr_alloc();
    swr_alloc_set_opts(is->audio_swr_ctx,
                       out_channel_layout,
                       AV_SAMPLE_FMT_S16,
                       is->audio_ctx->sample_rate,
                       in_channel_layout,
                       is->audio_ctx->sample_fmt,
                       is->audio_ctx->sample_rate,
                       0,
                       NULL);

    fprintf(stderr, "swr opts: out_channel_layout:%lld, out_sample_fmt:%d, out_sample_rate:%d, in_channel_layout:%lld, in_sample_fmt:%d, in_sample_rate:%d",
                    out_channel_layout, 
		    AV_SAMPLE_FMT_S16, 
		    is->audio_ctx->sample_rate, 
		    in_channel_layout, 
		    is->audio_ctx->sample_fmt, 
		    is->audio_ctx->sample_rate);

    swr_init(is->audio_swr_ctx);

    break;

  case AVMEDIA_TYPE_VIDEO:
    is->videoStream = stream_index;
    is->video_st = pFormatCtx->streams[stream_index];
    is->video_ctx = codecCtx;
    packet_queue_init(&is->videoq);
    is->video_tid = SDL_CreateThread(video_thread, "video_thread", is);
    is->sws_ctx = sws_getContext(is->video_ctx->width, 
				 is->video_ctx->height,
				 is->video_ctx->pix_fmt, 
				 is->video_ctx->width,
				 is->video_ctx->height, 
				 AV_PIX_FMT_YUV420P,
				 SWS_BILINEAR, 
				 NULL, NULL, NULL);
    break;
  default:
    break;
  }

  return 0;
}

//解码线程
int decode_thread(void *arg) {

  Uint32 pixformat;

  VideoState *is = (VideoState *)arg;
  AVFormatContext *pFormatCtx;
  AVPacket pkt1, *packet = &pkt1;

  int i;
  int video_index = -1;
  int audio_index = -1;

  is->videoStream = -1;
  is->audioStream = -1;

  global_video_state = is;  //全局对象

  // Open video file
  if(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL)!=0)
    return -1; // Couldn't open file

  is->pFormatCtx = pFormatCtx;
  
  // Retrieve stream information
  if(avformat_find_stream_info(pFormatCtx, NULL)<0)
    return -1; // Couldn't find stream information
  
  // Dump information about file onto standard error
  av_dump_format(pFormatCtx, 0, is->filename, 0);
  
  // Find the first video stream
  for(i=0; i<pFormatCtx->nb_streams; i++) {
    if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO &&
       video_index < 0) {
      video_index=i;
    }
    if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO &&
       audio_index < 0) {
      audio_index=i;
    }
  }

  if(audio_index >= 0) {  //成功找到音频流
    stream_component_open(is, audio_index);
  }
  if(video_index >= 0) {  //成功找到视频流
    stream_component_open(is, video_index);
  }   

  if(is->videoStream < 0 || is->audioStream < 0) {  //没有找到对应的流
    fprintf(stderr, "%s: could not open codecs\n", is->filename);
    goto fail;
  }

  fprintf(stderr, "video context: width=%d, height=%d\n", is->video_ctx->width, is->video_ctx->height);
  win = SDL_CreateWindow("Media Player",
     		   SDL_WINDOWPOS_UNDEFINED,
		   SDL_WINDOWPOS_UNDEFINED,
		   is->video_ctx->width, is->video_ctx->height,
		   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  
  renderer = SDL_CreateRenderer(win, -1, 0);

  pixformat = SDL_PIXELFORMAT_IYUV;
  texture = SDL_CreateTexture(renderer,
			      pixformat, 
			      SDL_TEXTUREACCESS_STREAMING,
			      is->video_ctx->width,
			      is->video_ctx->height);

  // main decode loop
  for(;;) {

    if(is->quit) {
      SDL_CondSignal(is->videoq.cond);
      SDL_CondSignal(is->audioq.cond);
      break;
    }

    // seek stuff goes here
    if(is->audioq.size > MAX_AUDIOQ_SIZE ||
       is->videoq.size > MAX_VIDEOQ_SIZE) {
      SDL_Delay(10);
      continue;
    }

    if(av_read_frame(is->pFormatCtx, packet) < 0) {
      if(is->pFormatCtx->pb->error == 0) {
	SDL_Delay(100); /* no error; wait for user input */
	continue;
      } else {
	break;
      }
    }

    // Is this a packet from the video stream?
    if(packet->stream_index == is->videoStream) {
      packet_queue_put(&is->videoq, packet);
      fprintf(stderr, "put video queue, size :%d\n", is->videoq.nb_packets);
    } else if(packet->stream_index == is->audioStream) {
      packet_queue_put(&is->audioq, packet);
      fprintf(stderr, "put audio queue, size :%d\n", is->audioq.nb_packets);
    } else {
      av_free_packet(packet);
    }

  }

  /* all done - wait for it */
  while(!is->quit) {
    SDL_Delay(100);
  }

 fail:
  if(1){
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = is;
    SDL_PushEvent(&event);
  }

  return 0;
}

//主线程进行一些初始操作，以及根据事件进行相关处理，解析和解码部分在线程中同步开启了
int main(int argc, char *argv[]) {

  int 		  ret = -1;

  SDL_Event       event;  //事件

  VideoState      *is;  //对象

  if(argc < 2) {
    fprintf(stderr, "Usage: test <file>\n");  //
    exit(1);
  }

  audiofd = fopen("testout.pcm", "wb+");
  audiofd1 = fopen("testout1.pcm", "wb+");

  //big struct, it's core 需要对结构体中的对象设置值就必须先分配空间
  is = av_mallocz(sizeof(VideoState));

  // Register all formats and codecs 注册
  av_register_all();
  
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
    exit(1);
  }

  texture_mutex = SDL_CreateMutex();  //创建纹理

  av_strlcpy(is->filename, argv[1], sizeof(is->filename));  //將输入文件复制给is->filename

  is->pictq_mutex = SDL_CreateMutex();  //创建互斥量--用于视频解码的互斥量
  is->pictq_cond = SDL_CreateCond();  //创建信号量--用于视频解码的信号量

  //set timer 
  schedule_refresh(is, 40);  //40ms回调一次，渲染视频帧，在函数中有回调函数，回到函数中设置FF_REFRESH_EVENT这个事件，此事件会一直调用函数，最后调到视频数据渲染部分

  is->parse_tid = SDL_CreateThread(decode_thread, "decode_thread", is);  //创建解码线程
  if(!is->parse_tid) {
    av_free(is); 
    goto __FAIL;
  }

  for(;;) { //主线程

    SDL_WaitEvent(&event);  //等待事件  
    switch(event.type) {
    case FF_QUIT_EVENT:  //退出事件
    case SDL_QUIT:
      fprintf(stderr, "receive a QUIT event: %d\n", event.type);
      is->quit = 1;
      //SDL_Quit();
      //return 0;
      goto __QUIT;
      break;
    case FF_REFRESH_EVENT:  //视频刷新事件
      //fprintf(stderr, "receive a refresh event: %d\n", event.type);
      video_refresh_timer(event.user.data1);  //刷新视频，传入is对象信息
      break;
    default:
      break;
    }
  }

__QUIT:
  ret = 0;
  

__FAIL:
  SDL_Quit();
  if(audiofd){
    fclose(audiofd);
  }
  if(audiofd1){
    fclose(audiofd1);
  }
  return ret;

}
