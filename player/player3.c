/*************************************************************************
	> File Name: player3.c
	> Author:hzc 
    > briefly:加入音频
	> Mail: 
	> Created Time: 2021年01月07日 星期四 10时04分05秒
 ************************************************************************/

#include <stdio.h>
#include <assert.h>

#include <SDL.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 28, 1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

struct SwrContext *audio_convert_ctx = NULL;

//别名用法
typedef struct PacketQueue{
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;  //互斥锁
    SDL_cond *cond;  //SDL中线程用法，估计是和wait配合？？
}PacketQueue;

PacketQueue audioq;  //音频队列

int quit = 0;

void packet_queue_init(PacketQueue *q){
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}


//???
int packet_queue_put(PacketQueue *q, AVPacket *pkt){

    AVPacketList *pkt1;
    if(av_dup_packet(pkt) < 0){  //分配空间
        return -1;
    }

    pkt1 = av_malloc(sizeof(AVPacketList)); 
    if(!pkt1){
        return -1;
    }

    pkt1->pkt = *pkt;
    pkt1->next = NULL; 

    SDL_LockMutex(q->mutex);
    //??
    if(!q->last_pkt){
        q->first_pkt = pkt1;
    }
    else{
        q->last_pkt->next = pkt1;
    }

    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

//???
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for(;;){
        if(quit){
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if(pkt1){
            q->first_pkt = pkt1->next;
            if(!q->first_pkt)  q->last_pkt = NULL;
            q->size-= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        }else if(!block){
            ret = 0;
            break;
        }else{
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

/*@briefly:音频解码
  @para:aCodecCtx--音频解码上下文，audio_buf-音频缓冲区，buf_size--音频缓冲区大小
  @ret： 0 - suc， 1 - fault
*/
int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size)
{
    static AVPacket pkt;
    static uint8_t *audio_pkt_data = NULL;
    static int audio_pkt_size = 0;
    static AVFrame frame;

    int len1, data_size = 0;
    
    for(;;){
        while(audio_pkt_size > 0){
            int got_frame = 0;
            len1 = avcodec_decode_audio4(aCodecCtx, &frame, &got_frame, &pkt);  //packet解码成frame，返回解码成功数据的长度
            if(len1 < 0){
                audio_pkt_size = 0;
                break;
            }
            audio_pkt_data += len1;   
            audio_pkt_size -= len1;
            data_size = 0;
			//解码成功后进行重采样：將原数据转换成声卡识别的声音
            if(got_frame){
                data_size = 2 * 2 * frame.nb_samples;
                assert(data_size <= buf_size);
                swr_convert(audio_convert_ctx,
                           &audio_buf,
                           MAX_AUDIO_FRAME_SIZE * 3/2,
                           (const uint8_t **)frame.data,
                           frame.nb_samples);
            }
            if(data_size <= 0){
                continue;
            }
            return data_size;  //返回data_size的长度
        }
        if(pkt.data)    av_free_packet(&pkt);

        if(quit) return -1;
        
        if(packet_queue_get(&audioq, &pkt, 1) < 0){  //从音频队列中获取packet
            return -1;
        }

        audio_pkt_data = pkt.data;  //更新pkt的数据
        audio_pkt_size = pkt.size;  //更新pkt的大小
    }
}

/*@briefly:声卡的回调函数，有些操作可以理解为声卡自动执行
  @para:userdata--用户数据，stream-声卡存放数据的内存空间位置，len-内存空间大小
  @ret：void
*/
void audio_callback(void *userdata, uint8_t *stream, int len)
{
    AVCodecContext *aCodecCtx = (AVCodecContext *) userdata;  //音频编解码的上下文
    int len1, audio_size; 

    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];  //重采样的大小 -- audio数据的空间
    static unsigned int audio_buf_size = 0;  //初始化，刚开始缓冲区大小为0
    static unsigned int audio_buf_index = 0;  //初始化

    while(len > 0){   //如果内存中有数据
        if(audio_buf_index >= audio_buf_size){  //
            //we have already sent all our data; get more
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf)); //音频数据解码，放入audio_buf中
            if(audio_size < 0){
                //if error output silence，解码失败--设置静默音
                audio_buf_size = 1024;
                memset(audio_buf, 0, audio_buf_size);  
            }else{  //解码成功：更新音频数据缓冲区的数据大小
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;  //
        }
        len1 = audio_buf_size - audio_buf_index;
        if(len1 > len)
            len1 = len;
        fprintf(stderr, "index=%d, len1=%d, len=%d\n",
               audio_buf_index,
               len,
               len1);
        memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);  //將audio_buf的数据拷贝到stream中去
        len -= len1;  //len可以看做是声卡自动更新的一个值，此时声卡缓冲区已经满了，随着声音播放，len的值会逐渐变大
        stream += len1;； //更新下声卡缓冲区的位置，简单说并不是缓冲区只有len大小，而是声卡只能播放len大小的数据
        audio_buf_index += len1; //更新音频缓冲区的指针位置（音频缓冲区是固定大小的，因此如果超出大小就需要使得index置为0）
    }
    
}

//主函数
int main(int argc, char *argv[])
{
    int  ret = -1;
    int i,videostream,audiostream;

    AVFormatContext *pFormatCtx = NULL;


    //for video decode
    AVCodecContext *pCodecCtxOrig = NULL;
    AVCodecContext *pCodecCtx = NULL;
    AVCodec  *pCodec = NULL;

    struct SwsContext *sws_ctx = NULL;

    AVPicture  *pict = NULL;
    AVFrame  *pFrame = NULL;
    AVPacket  packet;
    int  frameFinished;

    //for audio decode
    AVCodecContext  *aCodecCtxOrig = NULL;
    AVCodecContext  *aCodecCtx = NULL;
    AVCodec  *aCodec = NULL;

    int64_t in_channel_layout;
    int64_t out_channel_layout;

    //for video rander
    int w_width = 640;
    int w_height =480;

    int pixformat;
    SDL_Rect rect;

    SDL_Window  *win;
    SDL_Renderer  *renderer;
    SDL_Texture  *texture;

    //for event
    SDL_Event event;

    //for audio
    SDL_AudioSpec  wanted_spec, spec;

    if(argc <  2){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Usage:command <file>");
        return ret;
    }

    //register all注册
    av_register_all();

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not initialize SDL - %s\n", SDL_GetError());
        return ret;
    }

    //open video file 打开多媒体文件
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL)!=0){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open multi-media file");
        goto __FAIL;
    }

    //retrieve stream information 获取多媒体信息
    if(avformat_find_stream_info(pFormatCtx, NULL)<0){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not find stream information");
        goto __FAIL;
    }

    av_dump_format(pFormatCtx, 0, argv[1], 0);  //dump出输入文件的信息

    //find the first video stream 寻找视频流和音频流
    videostream = -1;
    audiostream = -1;

    for(i = 0; i < pFormatCtx->nb_streams; ++i){
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO && videostream < 0){
            videostream = i;
        }
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && audiostream < 0){
            audiostream = i;
        }
    }

    if(videostream == -1){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Don't find a video stream");
        goto __FAIL;
    }


    if(audiostream == -1){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Don't find a audio stream");
        goto __FAIL;
    }

    //audio Codec Context
    aCodecCtxOrig = pFormatCtx->streams[audiostream]->codec;
    aCodec = avcodec_find_decoder(aCodecCtxOrig->codec_id); //找到根据原始文件解码器
    if(!aCodec){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Dont't find audio stream");
        goto __FAIL;
    }

    //Copy context :这一步是为了不再原CodecCtx进行操作，才复制一个
    aCodecCtx = avcodec_alloc_context3(aCodec);
    if(avcodec_copy_context(aCodecCtx, aCodecCtxOrig) != 0){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could't copy codec context! ");
        goto __FAIL;
    }

    //set audio setting from codec info 设置声卡参数
    wanted_spec.freq = aCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = aCodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = aCodecCtx;

    //打开音频设备，前一个参数是理想的，后一个参数是设备音频参数，会保留下来
    if(SDL_OpenAudio(&wanted_spec, &spec) < 0){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open audio device -%s!", SDL_GetError());
        goto __FAIL;
    }

    //打开音频解码器
    avcodec_open2(aCodecCtx, aCodec, NULL);

    //audioq这个参数哪里定义的？什么用？
    packet_queue_init(&audioq);

    in_channel_layout = av_get_default_channel_layout(aCodecCtx->channels);  //输入的通道数量
    out_channel_layout = in_channel_layout;  
    fprintf(stderr, "in layout:%lld, out layout:%lld \n", in_channel_layout, out_channel_layout);

    audio_convert_ctx = swr_alloc();  // 重采样分配一个SwrContext
    //根据原始数据设置参数
    if(audio_convert_ctx){
        swr_alloc_set_opts(audio_convert_ctx,
                          out_channel_layout,
                          AV_SAMPLE_FMT_S16,
                          aCodecCtx->sample_rate,
                          in_channel_layout,
                          aCodecCtx->sample_fmt,
                          aCodecCtx->sample_rate,
                          0,
                          NULL);
    }

    swr_init(audio_convert_ctx);

    //播放音频
    SDL_PauseAudio(0);

    //video部分
    //Get apointer to the codec context for the video stream
    pCodecCtxOrig = pFormatCtx->streams[videostream]->codec;

    //Find the decoder for the video stream
    pCodec = avcodec_find_decoder(pCodecCtxOrig->codec_id);
    if(pCodec == NULL){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unsupported codec!");
        goto __FAIL;
    }

    //Copy Context
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if(avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to copy context of codec!");
        goto __FAIL;
    }

    //Open codec
    if(avcodec_open2(pCodecCtx, pCodec, NULL) < 0){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open audio decoder!");
        goto __FAIL;
    }

    //Allocate video frame
    pFrame = av_frame_alloc();

    w_width = pCodecCtx->width;
    w_height = pCodecCtx->height;

    fprintf(stderr, "width %d, height:%d\n", w_width, w_height);

    //SDL WINDOW
    win = SDL_CreateWindow("Media Player",
                          SDL_WINDOWPOS_UNDEFINED,
                          SDL_WINDOWPOS_UNDEFINED,
                          w_width, w_height,
                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if(!win){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create window!");
        goto __FAIL;
    }

    //SDL renderer
    renderer = SDL_CreateRenderer(win, -1, 0);
    if(!renderer){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create renderer!");
        goto __FAIL;
    }

    //SDL renderer
    pixformat = SDL_PIXELFORMAT_IYUV;
    texture = SDL_CreateTexture(renderer,
                               pixformat,
                               SDL_TEXTUREACCESS_STREAMING,
                               w_width,
                               w_height);

    if(!texture){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create Texture!");
        goto __FAIL;
    }

    //初始化 SWS Context for software scaling
    sws_ctx = sws_getContext(pCodecCtx->width,
                            pCodecCtx->height,
                            pCodecCtx->pix_fmt,
                            pCodecCtx->width,
                            pCodecCtx->height,
                            AV_PIX_FMT_YUV420P,
                            SWS_BILINEAR,
                            NULL,
                            NULL,
                            NULL);

    pict = (AVPicture*)malloc(sizeof(AVPicture));
    avpicture_alloc(pict,
                   AV_PIX_FMT_YUV420P,
                   pCodecCtx->width,
                   pCodecCtx->height);

    //Read frames and save first five frames to disk
    while(av_read_frame(pFormatCtx, &packet) >= 0){
        //获取视频流
        if(packet.stream_index == videostream){
            //开始解码
            avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);

            //判断是否成功获取video frame
            if(frameFinished){
                sws_scale(sws_ctx, (uint8_t const *)pFrame->data, pFrame->linesize, 0, pCodecCtx->height, pict->data, pict->linesize);  //转换尺度

                SDL_UpdateYUVTexture(texture, NULL, 
                                     pict->data[0], pict->linesize[0],
                                    pict->data[1], pict->linesize[1],
                                    pict->data[2], pict->linesize[2]);

                rect.x = 0;
                rect.y = 0;
                rect.w = pCodecCtx->width;
                rect.h = pCodecCtx->height;

                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, NULL, &rect);
                SDL_RenderPresent(renderer);

                av_free_packet(&packet);
            }
		}
        else if(packet.stream_index == audiostream){
            packet_queue_put(&audioq, &packet);  //將音频packet加入队列
        }
        else{
            av_free_packet(&packet);
        }

            //free the packet that was allocated by av_read_frame，轮询事件
        SDL_PollEvent(&event);
        switch(event.type){
            case SDL_QUIT:
                quit = 1;
                goto __QUIT;
                break;
            default:
                break;
        }
    }

__QUIT:
    ret = 0;

__FAIL:
    //free the YUV frame
    if(pFrame){
        av_frame_free(pFrame);
    }
    
    //close the codecs
    if(pCodecCtxOrig){
        avcodec_close(pCodecCtxOrig);
    }

    if(pCodecCtx){
        avcodec_close(pCodecCtx);
    }

    if(aCodecCtxOrig){
        avcodec_close(aCodecCtxOrig);
    }

    if(aCodecCtx){
        avcodec_close(aCodecCtx);
	}
    
    //close the video File
    if(pFormatCtx){
        avformat_close_input(&pFormatCtx);
    }

    if(pict){
        avpicture_free(pict);
        free(pict);
    }

    if(win){
        SDL_DestroyWindow(win);        
    }

    if(renderer){
        SDL_DestroyRenderer(renderer);
    }

    if(texture){
        SDL_DestroyTexture(texture);
    }

    SDL_Quit();

    return ret;
}




