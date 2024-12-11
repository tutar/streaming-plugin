#include "AVDemuxer.h"
#include <iostream>
#include "libswresample/swresample_internal.h"
#include "DouYin.h"

#define LOG_FILTER_ERROR(ret) \
    do { \
        char errbuf[AV_ERROR_MAX_STRING_SIZE]; \
        av_strerror(ret, errbuf, sizeof(errbuf)); \
        fprintf(stderr, "Filter error: %s\n", errbuf); \
    } while(0)

namespace IPCDemo
{
	static AVPacket    g_packet;
	static const char* g_wanted_stream_spec[AVMEDIA_TYPE_NB] = { 0 };
	static FF_AV_BUFFER g_av_buffer;

	static inline INT64 get_valid_channel_layout(INT64 channel_layout, int channels)
	{
		if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
			return channel_layout;
		else
			return 0;
	}

	AVPacketQueue::AVPacketQueue()
		: m_stop(TRUE)
		, m_serial(0)
		, m_size(0)
		, m_duration(0)
	{
	}

	AVPacketQueue::~AVPacketQueue()
	{
	}

	void AVPacketQueue::Unlock()
	{
		AutoLock autolock(m_mutex);
		m_stop = TRUE;
		m_cv.Unlock(FALSE);
	}

	BOOL AVPacketQueue::Push(INT32 index)
	{
		AVPacket pkt;
		av_init_packet(&pkt);
		pkt.data = NULL;
		pkt.size = 0;
		pkt.stream_index = index;
		return Push(pkt);
	}

	BOOL AVPacketQueue::Pop(AVPacket& pkt, INT32* serial)
	{
		AutoLock autolock(m_mutex);
		for (;;)
		{
			if (m_stop)
				return FALSE;

			if (m_linkqueue.size() > 0)
			{
				FF_AV_PACKET& pkt1 = m_linkqueue.front();
				m_size -= pkt1.pkt.size;
				m_duration -= pkt1.pkt.duration;
				pkt = pkt1.pkt;
				if (serial)
					*serial = pkt1.serial;
				m_linkqueue.pop_front();
				return TRUE;
			}
			else
			{
				m_cv.Lock(m_mutex);
			}
		}
		return TRUE;
	}

	int AVPacketQueue::GetBufferedAudioPacketDurationMS()
	{
		int          durationMS = 0;
		AutoLock autolock(m_mutex);

		if (m_linkqueue.size() == 0)
		{
			return 0;
		}
		else if (m_linkqueue.size() == 1)
		{
			return 0; // before decode,we can't calculate audio duration
		}
		else
		{
			FF_AV_PACKET& firstTSPacket = m_linkqueue.front();
			FF_AV_PACKET& lastTSPacket = m_linkqueue.back();
			durationMS = lastTSPacket.pkt.pts - firstTSPacket.pkt.pts;
			return durationMS;
		}
	}

	int AVPacketQueue::PacketCount()
	{
		return m_linkqueue.size();
	}

	BOOL AVPacketQueue::Push(AVPacket& pkt)
	{
		AutoLock autolock(m_mutex);
		if (m_stop)
			goto _FINISH;
		if (&pkt == &g_packet)
			m_serial++;
		FF_AV_PACKET value;
		value.pkt = pkt;
		value.serial = m_serial;
		m_size += value.pkt.size;
		m_duration += value.pkt.duration;
		m_linkqueue.push_back(value);
		m_cv.Unlock(FALSE);
		return TRUE;
	_FINISH:
		if (&pkt != &g_packet)
			av_packet_unref(&pkt);
		return FALSE;
	}

	void AVPacketQueue::Flush()
	{
		AutoLock autolock(m_mutex);
		while (!m_linkqueue.empty())
		{
			FF_AV_PACKET& value = m_linkqueue.front();
			av_packet_unref(&value.pkt);
			m_linkqueue.pop_front();
		}
		m_size = 0;
		m_duration = 0;
	}

	//////////////////////////////////////////////////////////////////////////
	AVFrameQueue::AVFrameQueue()
		: m_indexR(0)
		, m_indexW(0)
		, m_size(0)
		, m_maxsize(0)
		, m_indexshow(0)
	{
		ZeroMemory(m_frames, sizeof(FF_AV_FRAME) * FRAME_QUEUE_SIZE);
	}

	AVFrameQueue::~AVFrameQueue()
	{
	}

	INT32 AVFrameQueue::remaining() const
	{
		return m_size - m_indexshow;
	}

	FF_AV_FRAME* AVFrameQueue::PeekNext()
	{
		return &m_frames[(m_indexR + m_indexshow + 1) % m_maxsize];
	}

	FF_AV_FRAME* AVFrameQueue::PeakLast()
	{
		return &m_frames[m_indexR];
	}

	FF_AV_FRAME* AVFrameQueue::Peek(BOOL bREAD)
	{
		AutoLock autolock(m_mutex);
		if (!bREAD)
		{
			while (m_size >= m_maxsize && !m_packets.m_stop)
				m_cv.Lock(m_mutex);
			if (m_packets.m_stop)
				return NULL;
			return &m_frames[m_indexW];
		}
		else
		{
			while (m_size - m_indexshow <= 0 && !m_packets.m_stop)
				m_cv.Lock(m_mutex);
			if (m_packets.m_stop)
				return NULL;
			return &m_frames[(m_indexR + m_indexshow) % m_maxsize];
		}
	}

	void AVFrameQueue::Push()
	{
		if (++m_indexW == m_maxsize)
			m_indexW = 0;
		AutoLock autolock(m_mutex);
		m_size++;
		m_cv.Unlock(FALSE);
	}

	BOOL AVFrameQueue::Next()
	{
		if (!m_indexshow)
		{
			m_indexshow = 1;
			return FALSE;
		}
		av_frame_unref(m_frames[m_indexR].frame);
		if (++m_indexR == m_maxsize)
			m_indexR = 0;
		AutoLock autolock(m_mutex);
		m_size--;
		m_cv.Unlock(FALSE);
		return TRUE;
	}

	INT64 AVFrameQueue::GetLastPos()
	{
		FF_AV_FRAME& value = m_frames[m_indexR];
		if (m_indexshow && value.serial == m_packets.m_serial)
			return value.pos;
		return -1;
	}

	void AVFrameQueue::Unlock()
	{
		AutoLock autolock(m_mutex);
		m_cv.Unlock(FALSE);
	}

	BOOL AVFrameQueue::Create(INT32 maxsize)
	{
		Destroy();
		m_packets.m_stop = TRUE;
		m_maxsize = FFMIN(maxsize, FRAME_QUEUE_SIZE);
		for (INT32 i = 0; i < m_maxsize; i++)
		{
			m_frames[i].frame = av_frame_alloc();
			if (!m_frames[i].frame)
				goto _ERROR;
		}
		return TRUE;
	_ERROR:
		Destroy();
		return FALSE;
	}

	void AVFrameQueue::Destroy()
	{
		for (INT32 i = 0; i < m_maxsize; i++)
		{
			FF_AV_FRAME& value = m_frames[i];
			if (value.frame != NULL)
			{
				av_frame_unref(value.frame);
				av_frame_free(&value.frame);
			}
		}
		m_size = 0;
		m_indexR = 0;
		m_indexW = 0;
		m_maxsize = 0;
		m_indexshow = 0;
	}

	//////////////////////////////////////////////////////////////////////////
	Clock::Clock()
		: speed(1.0)
		, pts_drift(0.0)
		, last_updateds(0.0)
		, pts(0)
		, paused(FALSE)
		, queueserial(NULL)
	{
	}

	void Clock::SetupClock(INT32* queueserial)
	{
		this->speed = 1.0;
		this->paused = FALSE;
		this->queueserial = queueserial;
		SetClock(NAN, -1);
	}

	Clock::~Clock()
	{
	}

	DOUBLE Clock::GetClock()
	{
		if (!queueserial)
			return 0;
		if (*queueserial != serial)
			return NAN;
		if (this->paused)
		{
			return this->pts;
		}
		else
		{
			double time = av_gettime_relative() / 1000000.0;
			return this->pts_drift + time - (time - this->last_updateds) * (1.0 - this->speed);
		}
	}

	void Clock::SetClock(double pts, int serial)
	{
		double time = av_gettime_relative() / 1000000.0;
		SetClockAt(pts, serial, time);
	}

	void Clock::SetClockAt(double pts, int serial, double time)
	{
		this->pts = pts;
		this->last_updateds = time;
		this->pts_drift = this->pts - time;
		this->serial = serial;
	}

	void Clock::SetClockSpeed(double speed)
	{
		SetClock(GetClock(), serial);
		this->speed = speed;
	}
	//////////////////////////////////////////////////////////////////////////
	FF_AV::FF_AV()
		: m_finished(0)
		, m_serial(0)
		, m_bPending(FALSE)
		, m_bEnableHW(FALSE)
		, m_pts(0)
		, m_context(NULL)
		, m_contextHW(NULL)
		, m_continueCV(NULL)
		, m_index(-1)
		, m_stream(NULL)
	{
		ZeroMemory(&m_pkt, sizeof(m_pkt));
		ZeroMemory(&m_rational, sizeof(m_rational));
	}

	FF_AV::~FF_AV()
	{
	}

	AVStream* FF_AV::stream()
	{
		return m_stream;
	}

	BOOL FF_AV::Open(AVFormatContext* context, INT32 index, BOOL& bEnableHW)
	{
		INT32          iRes = 0;
		const AVCodec* codec = NULL;
		{
			m_context = avcodec_alloc_context3(NULL);
			if (!m_context)
			{
				goto _ERROR;
			}
			iRes = avcodec_parameters_to_context(m_context, context->streams[index]->codecpar);
			if (iRes < 0)
			{
				goto _ERROR;
			}
			m_context->pkt_timebase = context->streams[index]->time_base;
			if ((m_context->codec_id == AV_CODEC_ID_VP8) || (m_context->codec_id == AV_CODEC_ID_VP9))
			{
				AVDictionaryEntry* tag = NULL;
				tag = av_dict_get(context->streams[index]->metadata, "alpha_mode", tag, 0);
				if (tag && (strcmp(tag->value, "1") == 0))
				{
					const char* codec_name = (m_context->codec_id == AV_CODEC_ID_VP8) ? "libvpx" : "libvpx-vp9";
					codec = avcodec_find_decoder_by_name(codec_name);
				}
			}
			if (codec == NULL)
			{
				codec = avcodec_find_decoder(m_context->codec_id);
			}
			if (!codec)
			{
				goto _ERROR;
			}
			m_context->codec_id = codec->id;
			m_context->flags2 |= AV_CODEC_FLAG2_FAST;
			AVDictionary* opts = NULL;
			if (!av_dict_get(opts, "threads", NULL, 0))
				av_dict_set(&opts, "threads", "auto", 0);
			av_dict_set(&opts, "refcounted_frames", "1", 0);
			iRes = avcodec_open2(m_context, codec, &opts);
			if (iRes < 0)
			{
				goto _ERROR;
			}
			context->streams[index]->discard = AVDISCARD_DEFAULT;
			m_stream = context->streams[index];
			m_index = index;
			m_queue.m_packets.m_stop = FALSE;
			m_queue.m_packets.Push(g_packet);
		}
		return TRUE;
	_ERROR:
		Close();
		return FALSE;
	}

	void FF_AV::Close()
	{
		Stop();
		if (m_task.joinable())
			m_task.join();
		m_queue.m_packets.Flush();
		m_queue.Destroy();
		av_packet_unref(&m_pkt);
		if (m_context != NULL)
			avcodec_free_context(&m_context);
		m_context = NULL;
		if (m_contextHW != NULL)
			av_buffer_unref(&m_contextHW);
		m_contextHW = NULL;
		m_index = -1;
		m_stream = NULL;
	}

	void FF_AV::Stop()
	{
		m_bPending = FALSE;
		m_queue.m_packets.Unlock();
		m_queue.Unlock();
	}

	void FF_AV::Unlock()
	{
		m_bPending = FALSE;
		m_queue.m_packets.Unlock();
		m_queue.Unlock();
	}

	//////////////////////////////////////////////////////////////////////////
	static INT32 decode_interrupt_cb(void* ctx)
	{
		AVDemuxer* pThis = (AVDemuxer*)ctx;
		return pThis->IsExiting();
	}

	//////////////////////////////////////////////////////////////////////////
	AVDemuxer::AVDemuxer()
		: m_context(NULL)
		, m_exiting(FALSE)
		, m_bEOF(FALSE)
		, m_bStep(FALSE)
		, m_bEnableLoop(FALSE)
		, m_bEnableHW(FALSE)
		, m_bPause(FALSE)
		, m_bPreviousPause(FALSE)
		, m_bAttachment(FALSE)
		, m_bPresent(FALSE)
		, m_timestamp(0)
		, m_seekPos(0)
		, m_seekRel(0)
		, m_seekFlag(0)
		, m_beginTS(0)
		, m_maxduration(0.0)
		, m_previousPos(0.0)
		, m_videoPTS(0.0)
		, m_audioPTS(NAN)
		, m_lasttime(0)
		, m_lastdelay(0)
		, m_buffersize(1024 * 1024 * 3)
		, m_reconnects(3)
		, m_swscontext(NULL)
		, m_graph(NULL)
		, m_bufferFilter(NULL)
		, m_buffersinkFilter(NULL)
		, m_yadifFilter(NULL)
		, a_graph(NULL)
		, a_bufferFilter(NULL)
		, a_bufferMasterFilter(NULL)
		, a_buffersinkFilter(NULL)
		, a_mixFilter(NULL)
		, m_bestFMT(AV_PIX_FMT_NONE)
	{

	}

	AVDemuxer::~AVDemuxer()
	{
	}

	BOOL AVDemuxer::IsExiting() const
	{
		return m_exiting;
	}
	BOOL AVDemuxer::Open(const std::string& szNAME)
	{
		m_szNAME = szNAME;
		m_bEOF = FALSE;
		m_bStep = FALSE;
		m_bPause = FALSE;
		m_bPreviousPause = FALSE;
		m_bAttachment = FALSE;
		m_bPresent = FALSE;
		m_bestFMT = AV_PIX_FMT_NONE;
		av_init_packet(&g_packet);
		g_packet.data = (UINT8*)&g_packet;
		if (!m_audio.m_queue.Create(AUDIO_QUEUE_SIZE))
		{
			goto _ERROR;
		}
		if (!m_video.m_queue.Create(VIDEO_QUEUE_SIZE))
		{
			goto _ERROR;
		}
		m_audioclock.SetupClock(&(m_audio.m_queue.m_packets.m_serial));
		m_videoclock.SetupClock(&(m_video.m_queue.m_packets.m_serial));
		m_readTask = std::thread(&AVDemuxer::OnReadPump, this);
		return TRUE;
	_ERROR:
		Close();
		return FALSE;
	}

	void AVDemuxer::UninitializeFilter()
	{
		if (m_graph != NULL)
			avfilter_graph_free(&m_graph);
		m_graph = NULL;
		m_bufferFilter = NULL;
		m_buffersinkFilter = NULL;
		m_yadifFilter = NULL;
	}

	BOOL AVDemuxer::InitializeFilter(AVFrame* video)
	{
		INT32 iRes = 0;
		CHAR       bufferargs[256];
		AVRational tb = av_guess_frame_rate(m_context, m_video.m_stream, NULL);
		UninitializeFilter();
		m_graph = avfilter_graph_alloc();
		if (!m_graph)
		{
			iRes = AVERROR(ENOMEM);
			goto _ERROR;
		}
		snprintf(bufferargs, sizeof(bufferargs), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
			video->width, video->height, video->format, m_video.stream()->time_base.num, m_video.stream()->time_base.den,
			m_video.stream()->codecpar->sample_aspect_ratio.num,
			FFMAX(m_video.stream()->codecpar->sample_aspect_ratio.den, 1));
		if (tb.num && tb.den)
			av_strlcatf(bufferargs, sizeof(bufferargs), ":frame_rate=%d/%d", tb.num, tb.den);
		iRes = avfilter_graph_create_filter(&m_bufferFilter, avfilter_get_by_name("buffer"), "FFDemuxer_buffer", bufferargs,
			nullptr, m_graph);
		if (iRes < 0)
		{
			goto _ERROR;
		}
		iRes = avfilter_graph_create_filter(&m_buffersinkFilter, avfilter_get_by_name("buffersink"), "FFDemuxer_buffersink",
			nullptr, nullptr, m_graph);
		if (iRes < 0)
		{
			goto _ERROR;
		}
		iRes = avfilter_graph_create_filter(&m_yadifFilter, avfilter_get_by_name("yadif"), "FFDemuxer_yadif", nullptr,
			nullptr, m_graph);
		if (iRes < 0)
		{
			goto _ERROR;
		}
		iRes = avfilter_link(m_bufferFilter, 0, m_yadifFilter, 0);
		if (iRes < 0)
		{
			goto _ERROR;
		}
		iRes = avfilter_link(m_yadifFilter, 0, m_buffersinkFilter, 0);
		if (iRes < 0)
		{
			goto _ERROR;
		}
		iRes = avfilter_graph_config(m_graph, NULL);
		if (iRes < 0)
		{
			goto _ERROR;
		}
		return TRUE;
	_ERROR:
		UninitializeFilter();
		return FALSE;
	}

	int AVDemuxer::pushMasterAudio(FF_AV_SAMPLES samples) {

		std::lock_guard<std::mutex> lock(g_av_buffer.mutex);
		g_av_buffer.samples.push(std::move(samples));

		return 1;
	}


	// 音频过滤
	void AVDemuxer::UninitializeAFilter()
	{
		if (a_graph != NULL)
			avfilter_graph_free(&a_graph);
		a_graph = NULL;
		a_bufferFilter = NULL;
		a_bufferMasterFilter = NULL;
		a_buffersinkFilter = NULL;
		a_mixFilter = NULL;
	}
	
	int transformMasterAudioFrame(AVFrame* masterFrame, const FF_AV_SAMPLES& samples)
	{
		if (!masterFrame || samples.data.empty()) {
			return -1;
		}

		// 设置基本参数
		masterFrame->format = AV_SAMPLE_FMT_FLTP;  // 平面浮点格式
		masterFrame->channel_layout = AV_CH_LAYOUT_MONO;  // 单声道输入
		masterFrame->channels = 1;
		masterFrame->sample_rate = samples.sampleRate;  // 使用samples中的采样率

		// 设置合理的采样数，考虑已消耗的样本
		const int available_samples = static_cast<int>(samples.data.size()) - samples.consumed;
		const int target_nb_samples = 1024;  // 使用标准的帧大小
		masterFrame->nb_samples = min(target_nb_samples, available_samples);

		// 分配内存
		int ret = av_frame_get_buffer(masterFrame, 0);
		if (ret < 0) {
			char errbuf[1024];
			av_strerror(ret, errbuf, sizeof(errbuf));
			std::cerr << "Failed to allocate master frame buffer: " << errbuf << std::endl;
			return ret;
		}

		// 确保帧可写
		ret = av_frame_make_writable(masterFrame);
		if (ret < 0) {
			char errbuf[1024];
			av_strerror(ret, errbuf, sizeof(errbuf));
			std::cerr << "Failed to make master frame writable: " << errbuf << std::endl;
			return ret;
		}

		// 复制音频数据，考虑已消耗的样本
		float* dst = (float*)masterFrame->data[0];
		memcpy(dst, samples.data.data() + samples.consumed,
			masterFrame->nb_samples * sizeof(float));

		// 设置正确的时间戳
		masterFrame->pts = samples.consumed;  // 使用已消耗的样本数作为时间戳基准

		return masterFrame->nb_samples;  // 返回实际处理的样本数
	}

	int getMasterFrameIfExist()
	{
		std::lock_guard<std::mutex> lock(g_av_buffer.mutex);

		if (g_av_buffer.samples.empty()) {
			return 0;
		}

		const FF_AV_SAMPLES& current_samples = g_av_buffer.samples.front();

		// 检查是否还有未消耗的数据
		int remaining_samples = static_cast<int>(current_samples.data.size()) - current_samples.consumed;
		if (remaining_samples < 1024) {  // 如果剩余样本不足一帧
			if (current_samples.consumed > 0) {  // 如果这个buffer已经部分消耗
				g_av_buffer.samples.pop();  // 移除已处理完的buffer
			}
			return 0;
		}

		return 1;  // 有足够的数据可用
	}

	BOOL AVDemuxer::InitializeAFilter(AVFrame* audio,AVFrame* master)
	{
		if (a_graph) {
			avfilter_graph_free(&a_graph);
			a_graph = nullptr;
		}

		int ret = 0;
		a_graph = avfilter_graph_alloc();
		if (!a_graph) {
			return false;
		}

		// 创建buffer source filter (音乐输入)
		char args_audio[512];
		snprintf(args_audio, sizeof(args_audio),
			"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%llx",
			1, audio->sample_rate,
			audio->sample_rate,
			av_get_sample_fmt_name((AVSampleFormat)audio->format),
			audio->channel_layout);
		ret = avfilter_graph_create_filter(&a_bufferFilter,
			avfilter_get_by_name("abuffer"), "audio_src",
			args_audio, nullptr, a_graph);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}

		// 创建buffer source filter (语音输入)
		char args_master[512];
		snprintf(args_master, sizeof(args_master),
			"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%llx",
			1, master->sample_rate,
			master->sample_rate,
			av_get_sample_fmt_name((AVSampleFormat)master->format),
			(uint64_t)master->channel_layout);
		ret = avfilter_graph_create_filter(&a_bufferMasterFilter,
			avfilter_get_by_name("abuffer"), "master_src",
			args_master, nullptr, a_graph);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}

		// 创建 volume filter (用于提高主播音量)
		const char* volumeArgs = std::string("volume=").append(std::to_string(DouYin::Config::getInstance().getAiVolume())).c_str();
		AVFilterContext* volume_ctx = nullptr;
		ret = avfilter_graph_create_filter(&volume_ctx,
			avfilter_get_by_name("volume"), "volume",
			volumeArgs, nullptr, a_graph);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}

		// 创建 volume filter (用于提高主播音量)
		const char* bgVolumeArgs = std::string("volume=").append(std::to_string(DouYin::Config::getInstance().getBgVideoVolume())).c_str();
		AVFilterContext* bg_volume_ctx = nullptr;
		ret = avfilter_graph_create_filter(&bg_volume_ctx,
			avfilter_get_by_name("volume"), "volume",
			bgVolumeArgs, nullptr, a_graph);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}

		// 创建 amix filter
		AVFilterContext* amix_ctx = nullptr;
		ret = avfilter_graph_create_filter(&amix_ctx,
			avfilter_get_by_name("amix"), "amix",
			"inputs=2:duration=longest:dropout_transition=3:weights=\"0.25 1\"", nullptr, a_graph);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}

		// 创建 buffersink filter
		ret = avfilter_graph_create_filter(&a_buffersinkFilter,
			avfilter_get_by_name("abuffersink"), "sink",
			nullptr, nullptr, a_graph);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}

		// 设置输出格式
		enum AVSampleFormat out_sample_fmts[] = { AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_NONE };
		ret = av_opt_set_int_list(a_buffersinkFilter, "sample_fmts", out_sample_fmts, -1,
			AV_OPT_SEARCH_CHILDREN);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}
		int64_t out_channel_layouts[] = { AV_CH_LAYOUT_STEREO, -1 };
		ret = av_opt_set_int_list(a_buffersinkFilter, "channel_layouts", out_channel_layouts, -1,
			AV_OPT_SEARCH_CHILDREN);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}
		int out_sample_rates[] = { audio->sample_rate, -1 };
		ret = av_opt_set_int_list(a_buffersinkFilter, "sample_rates", out_sample_rates, -1,
			AV_OPT_SEARCH_CHILDREN);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}

		// 连接 filters
		// 音乐: src ->bgVolume -> amix
		ret = avfilter_link(a_bufferFilter, 0, bg_volume_ctx, 0);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}
		ret = avfilter_link(bg_volume_ctx, 0, amix_ctx, 0);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}

		// 语音: src -> volume-> amix
		ret = avfilter_link(a_bufferMasterFilter, 0, volume_ctx, 0);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}
		ret = avfilter_link(volume_ctx, 0, amix_ctx, 1);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}

		// amix -> sink
		ret = avfilter_link(amix_ctx, 0, a_buffersinkFilter, 0);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}

		// 配置 graph
		ret = avfilter_graph_config(a_graph, nullptr);
		if (ret < 0) {
			LOG_FILTER_ERROR(ret);
			return false;
		}

		return true;
	}

	void AVDemuxer::Close()
	{
		m_exiting = TRUE;
		m_audio.Stop();
		m_video.Stop();
		if (m_readTask.joinable())
			m_readTask.join();
		if (m_audioTask.joinable())
			m_audioTask.join();
		if (m_audio.m_task.joinable())
			m_audio.m_task.join();
		if (m_videoTask.joinable())
			m_videoTask.join();
		if (m_video.m_task.joinable())
			m_video.m_task.join();

		m_video.Close();
		m_audio.Close();

		m_resampler.Close();

		m_bEOF = FALSE;
		m_seekFlag = 0;
		m_bStep = FALSE;
		m_bPause = FALSE;
		m_bPreviousPause = FALSE;
		m_bAttachment = FALSE;
		m_bPresent = FALSE;
		m_exiting = FALSE;
	}

	void AVDemuxer::OnReadPump()
	{
		if (m_exiting)
		{
			return;
		}
		int count = 0;
		BOOL     bRes = FALSE;
		INT32    iRes = 0;
		AVPacket pkt;
		av_init_packet(&pkt);
		AVDictionary* opts = NULL;
		av_dict_set(&opts, "stimeout", "30000000", 0);
		if (!m_buffersize)
			av_dict_set(&opts, "fflags", "nobuffer", 0);
		else
			av_dict_set_int(&opts, "buffer_size", m_buffersize, 0);
		if (!av_dict_get(opts, "threads", NULL, 0))
			av_dict_set(&opts, "threads", "auto", 0);
		m_context = avformat_alloc_context();
		if (!m_context)
		{
			goto _ERROR;
		}
		if (!m_buffersize)
			m_context->flags |= AVFMT_FLAG_NOBUFFER;
		m_context->interrupt_callback.callback = decode_interrupt_cb;
		m_context->interrupt_callback.opaque = this;
		iRes = avformat_open_input(&m_context, m_szNAME.c_str(), NULL, &opts);
		if (iRes < 0)
		{
			goto _ERROR;
		}
		if (m_context->pb && (!strncmp(m_context->url, "http:", 5)))
			av_dict_set(&opts, "timeout", "5000", 0);
		av_format_inject_global_side_data(m_context);
		iRes = avformat_find_stream_info(m_context, NULL);
		if (iRes < 0)
		{
			goto _ERROR;
		}
		if (m_context->pb != NULL)
			m_context->pb->eof_reached = 0;
		m_maxduration = (m_context->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
		INT32 indexs[AVMEDIA_TYPE_NB];
		memset(indexs, -1, sizeof(indexs));
		for (INT32 i = 0; i < m_context->nb_streams; i++)
		{
			AVStream*        stream = m_context->streams[i];
			enum AVMediaType type = (enum AVMediaType)stream->codecpar->codec_type;
			stream->discard = AVDISCARD_ALL;
			if (type >= 0 && g_wanted_stream_spec[type] && indexs[type] == -1)
				if (avformat_match_stream_specifier(m_context, stream, g_wanted_stream_spec[type]) > 0)
					indexs[type] = i;
		}
		for (INT32 i = 0; i < AVMEDIA_TYPE_NB; i++)
		{
			if (g_wanted_stream_spec[i] && indexs[i] == -1)
			{
				indexs[i] = INT_MAX;
			}
		}
		indexs[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(m_context, AVMEDIA_TYPE_AUDIO, indexs[AVMEDIA_TYPE_AUDIO],
			indexs[AVMEDIA_TYPE_VIDEO], NULL, 0);
		indexs[AVMEDIA_TYPE_VIDEO] =
			av_find_best_stream(m_context, AVMEDIA_TYPE_VIDEO, indexs[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
		if (indexs[AVMEDIA_TYPE_AUDIO] >= 0)
		{
			BOOL bEnableHW = FALSE;
			if (!m_audio.Open(m_context, indexs[AVMEDIA_TYPE_AUDIO], bEnableHW))
			{
				goto _ERROR;
			}
			m_audio.m_serial = -1;
			m_audio.m_continueCV = &m_continueCV;
			AVCodecContext* context = m_audio.m_context;
			INT32           channels = context->channels;
			UINT64          channellayout = context->channel_layout;
			if (!channellayout || channels != av_get_channel_layout_nb_channels(channellayout))
			{
				channellayout = av_get_default_channel_layout(channels);
				channellayout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
			}
			m_audioParameterO.SetChannels(2);
			m_audioParameterO.SetChannelLayout(IPC_CHANNEL_STEREO);
			m_audioParameterO.SetSamplesPerSec(context->sample_rate);
			m_audioParameterO.SetAudioFormat(IPC_AUDIO_FORMAT_16BIT);
			m_audioParameterO.SetBitsPerSample(16);
			if (AUDIO_START != nullptr)
				AUDIO_START(m_audioParameterO);
			m_audioTask = std::thread(&AVDemuxer::OnAudioRenderPump, this);
			m_audio.m_task = std::thread(&AVDemuxer::OnAudioDecodePump, this);
		}
		if (indexs[AVMEDIA_TYPE_VIDEO] >= 0)
		{
			if (!m_video.Open(m_context, indexs[AVMEDIA_TYPE_VIDEO], m_bEnableHW))
				goto _ERROR;
			if (m_video.m_context->codec_id == AV_CODEC_ID_RAWVIDEO)
			{
				goto _ERROR;
			}
			m_video.m_serial = -1;
			m_video.m_continueCV = &m_continueCV;
			AVCodecContext* context = m_video.m_context;
			m_videoParameter.SetCS(MS_AVCOL_SPC_TO_COLOR_SPACE(context->colorspace));
			m_videoParameter.SetCT(MS_AVCOL_SPC_TO_COLOR_TRANSFER(context->color_trc));
			m_videoParameter.SetRange(context->color_range == AVCOL_RANGE_JPEG ? IPC_VIDEO_RANGE_FULL : IPC_VIDEO_RANGE_PARTIAL);
			m_videoParameter.SetSize({ context->width, context->height });
			m_videoParameter.SetFormat(MS_AV_PIX_FMT_TO_VIDEO_PIXEL_FORMAT(context->pix_fmt));
			m_video.m_task = std::thread(&AVDemuxer::OnVideoDecodePump, this);
			m_videoTask = std::thread(&AVDemuxer::OnVideoRenderPump, this);
			m_bAttachment = TRUE;
		}
		for (;;)
		{
			if (m_exiting)
				break;
			if (m_bPause != m_bPreviousPause)
			{
				m_bPreviousPause = m_bPause;
				if (m_bPause)
					av_read_pause(m_context);
				else
					av_read_play(m_context);
			}
			if (m_bPause &&
				(!strcmp(m_context->iformat->name, "rtsp") || (m_context->pb && !strncmp(m_szNAME.c_str(), "mmsh:", 5))))
			{
				Sleep(10);
				continue;
			}
			if (m_bAttachment)
			{
				if (m_video.m_stream != NULL && m_video.m_stream->disposition & AV_DISPOSITION_ATTACHED_PIC)
				{
					AVPacket copy;
					if ((iRes = av_packet_ref(&copy, &m_video.m_stream->attached_pic)) < 0)
					{
						bRes = TRUE;
						goto _ERROR;
					}
					m_video.m_queue.m_packets.Push(copy);
					m_video.m_queue.m_packets.Push(m_video.m_index);
				}
				m_bAttachment = FALSE;
			}
			if (((m_audio.m_queue.m_packets.m_size + m_video.m_queue.m_packets.m_size) > MAX_QUEUE_SIZE ||
				(CheckPackets(m_audio.m_stream, m_audio.m_index, &m_audio.m_queue.m_packets) &&
					CheckPackets(m_video.m_stream, m_video.m_index, &m_video.m_queue.m_packets))))
			{
				AutoLock autolock(m_mutexCV);
				m_continueCV.TryLock(m_mutexCV, 10);
				continue;
			}
			iRes = av_read_frame(m_context, &pkt);
			if (iRes < 0)
			{
				if ((iRes == AVERROR_EOF || avio_feof(m_context->pb)) && !m_bEOF)
				{
					if (m_video.m_index >= 0)
						m_video.m_queue.m_packets.Push(m_video.m_index);
					if (m_audio.m_index >= 0)
						m_audio.m_queue.m_packets.Push(m_audio.m_index);

					if (count > 0) {
						m_bEOF = TRUE;
						m_exiting = true;
					}
					else {
						// 重新播放
						count++;
						avformat_seek_file(m_context, -1, 0, 0, 0, 0);
					}
					

				}
				if (m_context->pb != NULL && m_context->pb->error != NULL)
				{
					bRes = TRUE;
					break;
				}
				AutoLock autolock(m_mutexCV);
				m_continueCV.TryLock(m_mutexCV, 10);
				continue;
			}
			else
			{
				m_bEOF = FALSE;
			}
			if (pkt.stream_index == m_audio.m_index)
			{
				m_audio.m_queue.m_packets.Push(pkt);
			}
			else if (pkt.stream_index == m_video.m_index && !(m_video.m_stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
			{
				m_video.m_queue.m_packets.Push(pkt);

			}
			else
			{
				av_packet_unref(&pkt);
			}
		}
	_ERROR:
		if (opts != NULL)
			av_dict_free(&opts);
		opts = NULL;
		BOOL already_in = m_exiting.exchange(TRUE);
		m_audio.Stop();
		if (m_audioTask.joinable())
			m_audioTask.join();
		if (m_audio.m_task.joinable())
			m_audio.m_task.join();
		m_video.Stop();
		if (m_videoTask.joinable())
			m_videoTask.join();
		if (m_video.m_task.joinable())
			m_video.m_task.join();
		if (m_context != NULL)
			avformat_close_input(&m_context);
		m_context = NULL;
		if (m_swscontext != NULL)
		{
			sws_freeContext(m_swscontext);
			av_freep(&m_pointers[0]);
		}
		m_swscontext = NULL;
		UninitializeFilter();
		UninitializeAFilter();
	}
	void AVDemuxer::OnAudioDecodePump()
	{
		BOOL         bRes = FALSE;
		INT32       iRes = 0;
		INT32        got = 0;
		FF_AV_FRAME* value = NULL;
		AVRational   tb;
		AVFrame* audio = NULL;
		AVFrame* masterAudioFrame = NULL;
		SwrContext* mix_swr_ctx = nullptr;



		AVFormatContext* fmt_ctx = nullptr;
		int ret;
		AVStream* out_stream=nullptr;
		const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MP3);
		AVCodecContext* encoder_ctx = nullptr;
		SwrContext* swr_ctx = nullptr;
		BOOL first = TRUE;  // 标志first
		int64_t total_samples = 0;  // 用于跟踪已处理的采样数
		AVPacket pkt;
		AVFrame* output_frame = nullptr;
		AVFrame* resampled_master = nullptr;
		CSimpleIniA& iniConfig = DouYin::Config::getInstance().getIniConfig();
		if ("DEBUG" == iniConfig.GetValue("log", "DEBUG")) {
			fmt_ctx = nullptr;
			const AVOutputFormat* ofmt = av_guess_format("mp3", nullptr, nullptr);
			ret = avformat_alloc_output_context2(&fmt_ctx, ofmt, ofmt->name, iniConfig.GetValue("log", "amixAudioFilePath"));
			if (!fmt_ctx || ret < 0) {
				std::cerr << "Could not create output context\n";
				return;
			}
			// 添加音频流
			out_stream = avformat_new_stream(fmt_ctx, nullptr);
			if (!out_stream) {
				std::cerr << "Failed to create new stream\n";
				return;
			}
			//初始化编码器
			//const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MP3);
			if (!encoder) {
				fprintf(stderr, "Codec not found\n");
				return;
			}
			// 分配并初始化编码器上下文
			encoder_ctx = avcodec_alloc_context3(encoder);
			if (!encoder_ctx) {
				fprintf(stderr, "Could not allocate audio codec context\n");
				return;
			}
			//
			// 1. 首先确保编码器参数正确设置
			encoder_ctx->bit_rate = 128000;
			encoder_ctx->channel_layout = AV_CH_LAYOUT_STEREO;  // 3 表示立体声布局
			encoder_ctx->channels = 2;
			encoder_ctx->sample_rate = 44100;
			encoder_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;  // MP3编码器需要浮点格式
			tb = { 1, out_stream->codecpar->sample_rate };
			encoder_ctx->time_base = tb;
			// 打开编码器
			if (avcodec_open2(encoder_ctx, encoder, NULL) < 0) {
				fprintf(stderr, "Could not open codec\n");
				return;
			}
			// 7. 从编码器上下文复制参数到输出流
			ret = avcodec_parameters_from_context(out_stream->codecpar, encoder_ctx);
			if (ret < 0) {
				std::cerr << "Could not copy codec params to stream\n";
				return;
			}
			// 打开输出文件
			if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
				if (avio_open(&fmt_ctx->pb, iniConfig.GetValue("log", "amixAudioFilePath"), AVIO_FLAG_WRITE) < 0) {
					std::cerr << "Could not open output file\n";
					return;
				}
			}
			// 写入文件头
			ret = avformat_write_header(fmt_ctx, nullptr);
			if (ret < 0) {
				std::cerr << "Error occurred when opening output file\n";
				return;
			}
			// 设置音频流参数
			// 2. 确保输出流参数与编码器一致
			out_stream->codecpar->codec_id = AV_CODEC_ID_MP3; // PCM = AV_CODEC_ID_PCM_SLE; // PCM 16-bit little-endian
			out_stream->codecpar->codec_type = encoder->type;// AVMEDIA_TYPE_AUDIO;
			out_stream->codecpar->channel_layout = encoder_ctx->channel_layout;
			out_stream->codecpar->channels = encoder_ctx->channels;
			out_stream->codecpar->sample_rate = encoder_ctx->sample_rate;
			out_stream->codecpar->format = encoder_ctx->sample_fmt;
			// 创建 SwrContext 并设置参数
			// 3. 使用新的方式初始化重采样器
			swr_ctx = swr_alloc();
			if (!swr_ctx) {
				std::cerr << "Could not allocate SwrContext\n";
				return;
			}
			// 3. 明确设置重采样器参数
			av_opt_set_channel_layout(swr_ctx, "in_channel_layout", m_audio.m_context->channel_layout, 0);
			av_opt_set_channel_layout(swr_ctx, "out_channel_layout", encoder_ctx->channel_layout, 0);
			av_opt_set_int(swr_ctx, "in_sample_rate", m_audio.m_context->sample_rate, 0);
			av_opt_set_int(swr_ctx, "out_sample_rate", encoder_ctx->sample_rate, 0);
			av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", m_audio.m_context->sample_fmt, 0);
			av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", encoder_ctx->sample_fmt, 0);
			// 5. 初始化重采样器
			ret = swr_init(swr_ctx);
			if (ret < 0) {
				char errbuf[1024];
				av_strerror(ret, errbuf, sizeof(errbuf));
				std::cerr << "Failed to initialize resampler: " << errbuf << std::endl;
				swr_free(&swr_ctx);
				return;
			}
			// 添加采样计数器
			av_init_packet(&pkt);
			output_frame = av_frame_alloc();
			resampled_master = av_frame_alloc();
			if (!output_frame || !resampled_master) {
				goto _END;
			}
		}

		

		audio = av_frame_alloc();
		masterAudioFrame = av_frame_alloc();
		if (!audio || !masterAudioFrame)
		{
			goto _END;
		}

		for (;;)
		{
			if (m_exiting)
				goto _END;
			if ((got = OnDecodeFrame(m_audio, audio)) < 0)
			{
				bRes = TRUE;
				goto _END;
			}

			if (got)
			{
				tb = { 1, audio->sample_rate };
				value = m_audio.m_queue.Peek();
				if (!value)
					goto _END;
				value->pts = (audio->pts == AV_NOPTS_VALUE) ? NAN : audio->pts * av_q2d(tb);
				value->pos = audio->pkt_pos;
				value->serial = m_audio.m_serial;
				value->duration = av_q2d({ audio->nb_samples, audio->sample_rate });
				
				// 有主音则开启滤镜
				if (getMasterFrameIfExist() > 0) {
					// 打印输入帧信息用于调试
					std::cout << "Before mixing:" << std::endl;
					std::cout << "Audio frame info"
						<< " - samples: " << audio->nb_samples
						<< ", pts: " << audio->pts
						<< ", linesize: " << audio->linesize[0]
						<< " - rate: " << audio->sample_rate
						<< ", format: " << audio->format
						<< ", channels: " << audio->channels
						<< ", layout: " << audio->channel_layout << std::endl;
					// 转换 master frame
					{
						std::lock_guard<std::mutex> lock(g_av_buffer.mutex);
						FF_AV_SAMPLES& s = g_av_buffer.samples.front();
						int processed_samples = transformMasterAudioFrame(masterAudioFrame, s);
						if (processed_samples < 0) {
							std::cerr << "Failed to transform master frame\n";
							goto _END;
						}

						// 更新已消耗的样本数
						s.consumed += processed_samples;

						// 如果当前buffer已经完全消耗，移除它
						if (s.consumed >= static_cast<int>(s.data.size())) {
							g_av_buffer.samples.pop();
						}
					}

					std::cout << "Master frame info"
						<< " - samples: " << masterAudioFrame->nb_samples
						<< ", pts: " << masterAudioFrame->pts
						<< ", linesize: " << masterAudioFrame->linesize[0]
						<< " - rate: " << masterAudioFrame->sample_rate
						<< ", format: " << masterAudioFrame->format
						<< ", channels: " << masterAudioFrame->channels
						<< ", layout: " << masterAudioFrame->channel_layout << std::endl;

					// 确保两个输入帧的格式一致
					if (masterAudioFrame->format != audio->format ||
						masterAudioFrame->sample_rate != audio->sample_rate ||
						masterAudioFrame->channel_layout != audio->channel_layout) {
						// 需要重采样对齐 TODO
						
						if (!mix_swr_ctx) {
							// 1.主语音和背景音混合
							mix_swr_ctx = swr_alloc();
							if (!mix_swr_ctx) {
								std::cerr << "Could not allocate MixSwrContext\n";
								goto _END;
							}
							// 2. 明确设置重采样器参数
							av_opt_set_channel_layout(mix_swr_ctx, "in_channel_layout", masterAudioFrame->channel_layout, 0);
							av_opt_set_channel_layout(mix_swr_ctx, "out_channel_layout", audio->channel_layout, 0);
							av_opt_set_int(mix_swr_ctx, "in_sample_rate", masterAudioFrame->sample_rate, 0);
							av_opt_set_int(mix_swr_ctx, "out_sample_rate", audio->sample_rate, 0);
							av_opt_set_sample_fmt(mix_swr_ctx, "in_sample_fmt", (enum AVSampleFormat)masterAudioFrame->format, 0);
							av_opt_set_sample_fmt(mix_swr_ctx, "out_sample_fmt", (enum AVSampleFormat)audio->format, 0);
							// 3. 初始化重采样器
							ret = swr_init(mix_swr_ctx);
							if (ret < 0) {
								char errbuf[1024];
								av_strerror(ret, errbuf, sizeof(errbuf));
								std::cerr << "Failed to initialize resampler: " << errbuf << std::endl;
								swr_free(&mix_swr_ctx);
								goto _END;
							}
						}
						// 重采样MasterAudioFrame
						resampled_master->channel_layout = audio->channel_layout;
						resampled_master->channels = audio->channel_layout;
						resampled_master->sample_rate = audio->sample_rate;
						resampled_master->format = audio->format;
						resampled_master->nb_samples = masterAudioFrame->nb_samples;  // 使用输入帧的采样数
						// 分配输出帧缓冲区
						ret = av_frame_get_buffer(resampled_master, 0);
						if (ret < 0) {
							char errbuf[1024];
							av_strerror(ret, errbuf, sizeof(errbuf));
							std::cerr << "Failed to allocate output frame buffer: " << errbuf << std::endl;
							goto _END;
						}
						// 确保帧数据可写
						ret = av_frame_make_writable(resampled_master);
						if (ret < 0) {
							char errbuf[1024];
							av_strerror(ret, errbuf, sizeof(errbuf));
							std::cerr << "Failed to make frame writable: " << errbuf << std::endl;
							goto _END;
						}
						// 执行重采样
						ret = swr_convert_frame(mix_swr_ctx, resampled_master, masterAudioFrame);
						if (ret < 0) {
							char errbuf[1024];
							av_strerror(ret, errbuf, sizeof(errbuf));
							std::cerr << "Failed to convert frame: " << errbuf << std::endl;

							// 打印更多调试信息
							std::cout << "Failed frame details:" << std::endl;
							std::cout << "mix_swr_ctx state:"
								<< "\n  in_channel_layout: " << mix_swr_ctx->in_ch_layout
								<< "\n  out_channel_layout: " << mix_swr_ctx->out_ch_layout
								<< "\n  in_sample_rate: " << mix_swr_ctx->in_sample_rate
								<< "\n  out_sample_rate: " << mix_swr_ctx->out_sample_rate
								<< std::endl;
							goto _END;
						}
						// 设置正确的时间戳
						resampled_master->pts = masterAudioFrame->pts;
					}
					else {
						resampled_master = masterAudioFrame;
					}


					// 初始化或重新初始化 filter graph
					if (!a_graph && !InitializeAFilter(audio, resampled_master)) {
						goto _END;
					}
					// 推送帧到 filter graph
					ret = av_buffersrc_add_frame_flags(a_bufferFilter, audio,AV_BUFFERSRC_FLAG_KEEP_REF);
					if (ret < 0) {
						char errbuf[AV_ERROR_MAX_STRING_SIZE];
						av_strerror(ret, errbuf, sizeof(errbuf));
						fprintf(stderr, "Error feeding audio: %s\n", errbuf);
						goto _END;
					}
					ret = av_buffersrc_add_frame_flags(a_bufferMasterFilter, resampled_master,AV_BUFFERSRC_FLAG_KEEP_REF);
					if (ret < 0) {
						// 错误处理
						goto _END;
					}
					// 获取混音后的帧
					while ((ret = av_buffersink_get_frame(a_buffersinkFilter, audio)) >= 0) {
						// 在混音后添加
						std::cout << "Mixed frame info - samples: " << audio->nb_samples
							<< ", pts: " << audio->pts
							<< ", linesize: " << audio->linesize[0] 
							<< " - rate: " << audio->sample_rate
							<< ", format: " << audio->format
							<< ", channels: " << audio->channels
							<< ", layout: " << audio->channel_layout << std::endl;
						//av_frame_unref(audio);
						av_frame_unref(resampled_master);
					}
					if (ret == AVERROR(EAGAIN))
						ret = 0; // 正常结束
					else if (ret == AVERROR_EOF)
						ret = 0;
				}
				
			
				if ("DEBUG" == iniConfig.GetValue("log", "DEBUG")) {

					// 重采样
					// 分配输出 AVFrame
					// 设置输出帧的参数，直接从 out_stream 的 codecpar 获取
					output_frame->channel_layout = encoder_ctx->channel_layout;
					output_frame->channels = encoder_ctx->channel_layout;
					output_frame->sample_rate = encoder_ctx->sample_rate;
					output_frame->format = encoder_ctx->sample_fmt;
					output_frame->nb_samples = encoder_ctx->frame_size;  // 使用输入帧的采样数
					// 分配输出帧缓冲区
					ret = av_frame_get_buffer(output_frame, 0);
					if (ret < 0) {
						char errbuf[1024];
						av_strerror(ret, errbuf, sizeof(errbuf));
						std::cerr << "Failed to allocate output frame buffer: " << errbuf << std::endl;
						goto _END;
					}
					// 确保帧数据可写
					ret = av_frame_make_writable(output_frame);
					if (ret < 0) {
						char errbuf[1024];
						av_strerror(ret, errbuf, sizeof(errbuf));
						std::cerr << "Failed to make frame writable: " << errbuf << std::endl;
						goto _END;
					}


					// 执行重采样
					ret = swr_convert_frame(swr_ctx, output_frame, audio);
					if (ret < 0) {
						char errbuf[1024];
						av_strerror(ret, errbuf, sizeof(errbuf));
						std::cerr << "Failed to convert frame: " << errbuf << std::endl;

						// 打印更多调试信息
						std::cout << "Failed frame details:" << std::endl;
						std::cout << "SwrContext state:"
							<< "\n  in_channel_layout: " << swr_ctx->in_ch_layout
							<< "\n  out_channel_layout: " << swr_ctx->out_ch_layout
							<< "\n  in_sample_rate: " << swr_ctx->in_sample_rate
							<< "\n  out_sample_rate: " << swr_ctx->out_sample_rate
							<< std::endl;
						goto _END;
					}
					// 设置输出帧的时间戳
					tb = { 1, encoder_ctx->sample_rate };
					output_frame->pts = av_rescale_q(total_samples, tb, encoder_ctx->time_base);
					total_samples += output_frame->nb_samples;
					// 将 AVFrame 转换为 AVPacket
					// 将 AVFrame 发送给编码器，并将 AVFrame 转换为 AVPacket
					ret = avcodec_send_frame(encoder_ctx, output_frame);
					if (ret < 0) {
						char errbuf[1024];
						av_strerror(ret, errbuf, sizeof(errbuf));
						std::cerr << "Error sending frame to encoder: " << errbuf << std::endl;
						std::cerr << "Frame details:" << std::endl;
						std::cerr << "  format: " << output_frame->format << std::endl;
						std::cerr << "  channels: " << output_frame->channels << std::endl;
						std::cerr << "  samples: " << output_frame->nb_samples << std::endl;
						std::cerr << "  linesize[0]: " << output_frame->linesize[0] << std::endl;
						goto _END;
					}
					// 接收编码后的 AVPacket
					while (avcodec_receive_packet(encoder_ctx, &pkt) == 0) {
						// 设置 packet 的流索引
						pkt.stream_index = out_stream->index;

						// 将 packet 写入输出文件
						if (av_interleaved_write_frame(fmt_ctx, &pkt) < 0) {
							fprintf(stderr, "Error muxing packet\n");
							goto _END;
						}
						//// 释放 packet
						//av_packet_unref(&pkt);
					}
				}

				av_frame_move_ref(value->frame, audio);
				m_audio.m_queue.Push();
			}
		}

	_END:
		
		if ("DEBUG" == iniConfig.GetValue("log", "DEBUG")) {
			// 清理...
			av_frame_free(&output_frame);
			av_frame_free(&resampled_master);
			swr_free(&swr_ctx);
			avcodec_free_context(&encoder_ctx);
			// 写入文件尾
			av_packet_unref(&pkt); // 清理最后的pkt
			av_write_trailer(fmt_ctx);
			// 清理资源
			if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
				avio_close(fmt_ctx->pb);
			}
			avformat_free_context(fmt_ctx);
		}


		swr_free(&mix_swr_ctx);

		av_frame_free(&audio);
		av_frame_free(&masterAudioFrame);
	}



	double AVDemuxer::CalculateDuration(FF_AV_FRAME* vp, FF_AV_FRAME* nextvp)
	{
		if (vp->serial == nextvp->serial)
		{
			double duration = nextvp->pts - vp->pts;
			if (isnan(duration) || duration <= 0 || duration > m_maxduration)
				return vp->duration;
			else
				return duration;
		}
		else
		{
			return 0.0;
		}
	}

	double AVDemuxer::CalculateDelay(double delay)
	{
		double sync_threshold, diff = 0;
		diff = m_videoclock.GetClock() - m_audioclock.GetClock();
		sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
		if (!isnan(diff) && fabs(diff) < m_maxduration)
		{
			if (diff <= -sync_threshold)
				delay = FFMAX(0, delay + diff);
			else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
				delay = delay + diff;
			else if (diff >= sync_threshold)
				delay = 2 * delay;
		}
		return delay;
	}

	BOOL AVDemuxer::OnVideoRender(double* remaining)
	{
		if (!m_video.m_stream)
			return FALSE;
		double time = 0;
	_RETRY:
		if (m_video.m_queue.remaining() != 0)
		{
			double       lastduration, duration, delay = 0;
			FF_AV_FRAME* currentvp = NULL;
			FF_AV_FRAME* lastvp = NULL;
			lastvp = m_video.m_queue.PeakLast();
			currentvp = m_video.m_queue.Peek(TRUE);
			if (!currentvp || !lastvp)
				return FALSE;
			if (currentvp->serial != m_video.m_queue.m_packets.m_serial)
			{
				m_video.m_queue.Next();
				goto _RETRY;
			}
			if (lastvp->serial != currentvp->serial)
				m_videoPTS = av_gettime_relative() / 1000000.0;
			if (m_bPause)
				goto _DISPLAY;
			lastduration = CalculateDuration(lastvp, currentvp);
			delay = CalculateDelay(lastduration);
			time = av_gettime_relative() / 1000000.0;
			if (time < m_videoPTS + delay)
			{
				*remaining = FFMIN(m_videoPTS + delay - time, *remaining);
				goto _DISPLAY;
			}
			m_videoPTS += delay;
			if (delay > 0 && time - m_videoPTS > AV_SYNC_THRESHOLD_MAX)
				m_videoPTS = time;
			m_video.m_queue.m_mutex.Lock();
			if (!isnan(currentvp->pts))
			{
				m_videoclock.SetClock(currentvp->pts, currentvp->serial);
				m_previousPos = m_videoclock.GetClock();
			}
			m_video.m_queue.m_mutex.Unlock();
			if (m_video.m_queue.remaining() > 1)
			{
				FF_AV_FRAME* nextvp = m_video.m_queue.PeekNext();
				duration = CalculateDuration(currentvp, nextvp);
				if (!m_bStep && time > (m_videoPTS + duration))
				{
					m_video.m_queue.Next();
					goto _RETRY;
				}
			}
			m_video.m_queue.Next();
			m_bPresent = TRUE;
			if (m_bStep && !m_bPause)
			{
				m_bPause = m_videoclock.paused = m_audioclock.paused = !m_bPause;
			}
		}
	_DISPLAY:
		if (m_video.m_queue.m_indexshow && m_bPresent)
		{
			FF_AV_FRAME* vp = NULL;
			vp = m_video.m_queue.PeakLast();
			if (vp != NULL && vp->frame != NULL)
			{
				INT32              iRes = 0;
				enum AVPixelFormat format = (enum AVPixelFormat)vp->frame->format;
				enum AVPixelFormat bestFMT = AV_PIX_FMT_RGBA;
				if (bestFMT != format)
				{
					if (m_bestFMT != bestFMT)
					{
						if (m_swscontext != NULL)
						{
							sws_freeContext(m_swscontext);
							av_freep(&m_pointers[0]);
						}
						m_swscontext = NULL;
						INT32      space = MS_AV_CS_TO_SWS_CS(vp->frame->colorspace);
						INT32      range = vp->frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
						const int* coeff = sws_getCoefficients(space);
						m_swscontext = sws_getCachedContext(NULL, vp->frame->width, vp->frame->height, format, vp->frame->width,
							vp->frame->height, bestFMT, SWS_POINT, NULL, NULL, NULL);
						if (!m_swscontext)
						{
							return FALSE;
						}
						iRes = av_image_alloc(m_pointers, m_linesizes, m_video.m_context->width, m_video.m_context->height, bestFMT, 32);
						if (iRes < 0)
						{
							return FALSE;
						}
#define FIXED_1_0 (1 << 16)
						sws_setColorspaceDetails(m_swscontext, coeff, range, coeff, range, 0, FIXED_1_0, FIXED_1_0);
						m_bestFMT = bestFMT;
					}
				}
				IPC_VIDEO_PIXEL_FORMAT vFORMAT = MS_AV_PIX_FMT_TO_VIDEO_PIXEL_FORMAT(bestFMT);
				m_videoParameter.SetFormat(vFORMAT);
				IPC_PACKET vpk;
				ZeroMemory(&vpk, sizeof(vpk));
				if (m_swscontext != NULL)
				{
					iRes = sws_scale(m_swscontext, (const UINT8* const*)vp->frame->data, vp->frame->linesize, 0, vp->frame->height,
						m_pointers, m_linesizes);
					if (iRes < 0)
					{
						return FALSE;
					}
					for (UINT32 i = 0; i < 4; i++)
					{
						vpk.data[i] = m_pointers[i];
						vpk.linesize[i] = abs(m_linesizes[i]);
					}
				}
				else
				{
					for (UINT32 i = 0; i < 4; i++)
					{
						vpk.data[i] = vp->frame->data[i];
						vpk.linesize[i] = abs(vp->frame->linesize[i]);
					}
				}

				vpk.type = IPC_VIDEO;
				vpk.timestamp = PipeSDK::GetTimeNS();
				vpk.video.cx = vp->frame->width;
				vpk.video.cy = vp->frame->height;
				vpk.video.format = vFORMAT;
				vpk.video.cs = MS_AVCOL_SPC_TO_COLOR_SPACE(vp->frame->colorspace);
				vpk.video.ct = MS_AVCOL_SPC_TO_COLOR_TRANSFER(vp->frame->color_trc);
				vpk.video.range = vp->frame->color_range == AVCOL_RANGE_JPEG ? IPC_VIDEO_RANGE_FULL : IPC_VIDEO_RANGE_PARTIAL;
				vpk.video.angle = 0;
				vpk.video.bFlipH = FALSE;
				vpk.video.bFlipV = FALSE;
				if (VIDEO_PACKET != nullptr)
					VIDEO_PACKET(vpk);

			}
		}
		m_bPresent = FALSE;
		return TRUE;
	}

	void AVDemuxer::OnVideoRenderPump()
	{
		if (!m_video.m_stream)
			return;
		double remaining = 0.0;
		for (;;)
		{
			if (m_exiting)
				break;
			if (remaining > 0.0)
				av_usleep((int64_t)(remaining * 1000000.0));
			remaining = REFRESH_RATE;
			if (!m_bPause || m_bPresent)
			{
				OnVideoRender(&remaining);
			}
		}
	}

	void AVDemuxer::OnVideoDecodePump()
	{
		BOOL     bRes = FALSE;
		INT32    iRes = 0;
		INT32    lastcx = 0;
		INT32    lastcy = 0;
		INT32    lastformat = 0;
		INT32    lastserial = 0;
		DOUBLE   pts = 0;
		DOUBLE   duration = 0;
		AVFrame* video = NULL;
		video = av_frame_alloc();
		if (!video)
			return;
		AVRational tb = m_video.m_stream->time_base;
		AVRational tb2 = av_guess_frame_rate(m_context, m_video.m_stream, NULL);
		for (;;)
		{
			if (m_exiting)
				goto _END;
			iRes = GetVideoFrame(video);
			if (iRes < 0)
			{
				bRes = TRUE;
				goto _END;
			}
			if (!iRes)
				continue;
			if (video->interlaced_frame)
			{
				if (lastcx != video->width || lastcy != video->height || lastformat != video->format ||
					lastserial != m_video.m_serial)
				{
					if (!InitializeFilter(video))
						goto _END;
					lastcx = video->width;
					lastcy = video->height;
					lastformat = video->format;
					lastserial = m_video.m_serial;
				}
				iRes = av_buffersrc_add_frame(m_bufferFilter, video);
				if (iRes < 0)
				{
					goto _END;
				}
				while (iRes >= 0)
				{
					m_lasttime = av_gettime_relative() / 1000000.0;
					iRes = av_buffersink_get_frame_flags(m_buffersinkFilter, video, 0);
					if (iRes < 0)
					{
						if (iRes == AVERROR_EOF)
							m_video.m_finished = m_video.m_serial;
						iRes = 0;
						break;
					}
					m_lastdelay = av_gettime_relative() / 1000000.0 - m_lasttime;
					if (fabs(m_lastdelay) > AV_NOSYNC_THRESHOLD / 10.0)
						m_lastdelay = 0;
					tb = av_buffersink_get_time_base(m_buffersinkFilter);
					duration = (tb2.num && tb2.den ? av_q2d({ tb2.den, tb2.num }) : 0);
					pts = (video->pts == AV_NOPTS_VALUE) ? NAN : video->pts * av_q2d(tb);
					iRes = OnQueuePicture(video, pts, duration, video->pkt_pos, m_video.m_serial);
					if (video != NULL)
						av_frame_unref(video);
					if (m_video.m_queue.m_packets.m_serial != m_video.m_serial)
						break;
				}
			}
			else
			{
				duration = (tb2.num && tb2.den ? av_q2d({ tb2.den, tb2.num }) : 0);
				pts = (video->pts == AV_NOPTS_VALUE) ? NAN : video->pts * av_q2d(tb);
				iRes = OnQueuePicture(video, pts, duration, video->pkt_pos, m_video.m_serial);
				if (video != NULL)
					av_frame_unref(video);
			}
			if (iRes < 0)
				goto _END;
		}
	_END:
		if (m_graph != NULL)
			avfilter_graph_free(&m_graph);
		m_graph = NULL;
		if (video != NULL)
			av_frame_unref(video);
		av_frame_free(&video);
	}
	inline UINT32 CalculateSamples(UINT32 samplesPerSec)
	{
		return (std::max)(1024, 2 << av_log2(samplesPerSec / 30));
	}
	void AVDemuxer::OnAudioRenderPump()
	{
		INT32  serial = 0;
		UINT32 avgBytesPerSec = m_audioParameterO.GetBlockSize() * m_audioParameterO.GetSamplesPerSec();
		INT32  chunksize = CalculateSamples(m_audio.m_context->sample_rate) *
			((m_audioParameterO.GetChannels() * m_audioParameterO.GetBitsPerSample()) / 8);
		DOUBLE            audioPTS = NAN;
		std::vector<UINT8> buffer;
		buffer.resize(chunksize);
		for (;;)
		{
			if (m_exiting)
				break;;
			m_timestamp = av_gettime_relative();
			UINT8* output[MAX_AV_PLANES];
			ZeroMemory(output, sizeof(output));
			INT32 iRes = GetAudioFrame(output, serial, audioPTS, chunksize);
			if (iRes > 0)
			{
				if (m_aq.size() <= MAX_AUDIO_BUFFER_SIZE)
				{
					m_aq.Push(output[0], iRes);
					const UINT8* data = NULL;
					INT32 size = 0;
					m_aq.Peek(&data, &size);
					if (size >= chunksize)
					{
						memcpy(buffer.data(), data, chunksize);
						m_aq.Pop(chunksize);
						PipeSDK::IPC_PACKET pkt;
						ZeroMemory(&pkt, sizeof(pkt));
						pkt.type = IPC_AUDIO;
						pkt.audio.blocksize = static_cast<UINT32>(m_audioParameterO.GetChannels() * (m_audioParameterO.GetBitsPerSample() / 8));
						pkt.audio.count = chunksize / pkt.audio.blocksize;
						pkt.audio.sampleRate = m_audioParameterO.GetSamplesPerSec();
						pkt.audio.layout = m_audioParameterO.GetChannelLayout();
						pkt.audio.format = m_audioParameterO.GetAudioFormat();
						pkt.audio.planes = m_audioParameterO.GetPlanes();
						pkt.timestamp = PipeSDK::GetTimeNS();
						pkt.size = pkt.audio.count * pkt.audio.blocksize;
						pkt.data[0] = (UINT8*)buffer.data();
						pkt.linesize[0] = buffer.size();
						if (AUDIO_PACKET != nullptr)
							AUDIO_PACKET(pkt);
						if (!isnan(audioPTS))
						{
							double diff = static_cast<DOUBLE>((double)audioPTS - static_cast<DOUBLE>((double)chunksize / (double)avgBytesPerSec));
							m_audioclock.SetClockAt(diff, serial, static_cast<DOUBLE>(static_cast<double>(m_timestamp) / 1000000.0));
						}
					}
					continue;
				}
				else
				{
					m_aq.Reset();
				}
			}
			else
				av_usleep(1000);
			buffer.resize(chunksize);
			memset(buffer.data(), 0, chunksize);
			PipeSDK::IPC_PACKET pkt;
			ZeroMemory(&pkt, sizeof(pkt));
			pkt.type = IPC_AUDIO;
			pkt.audio.blocksize = static_cast<UINT32>(m_audioParameterO.GetChannels() * (m_audioParameterO.GetBitsPerSample() / 8));
			pkt.audio.count = chunksize / pkt.audio.blocksize;
			pkt.audio.sampleRate = m_audioParameterO.GetSamplesPerSec();
			pkt.audio.layout = m_audioParameterO.GetChannelLayout();
			pkt.audio.format = m_audioParameterO.GetAudioFormat();
			pkt.audio.planes = m_audioParameterO.GetPlanes();
			pkt.timestamp = PipeSDK::GetTimeNS();
			pkt.size = pkt.audio.count * pkt.audio.blocksize;
			pkt.data[0] = (UINT8*)buffer.data();
			pkt.linesize[0] = buffer.size();
			if (AUDIO_PACKET != nullptr)
				AUDIO_PACKET(pkt);
		}
	}

	INT32 AVDemuxer::GetAudioFrame(UINT8* output[], INT32& serial, DOUBLE& audioPTS, INT32 chunksize)
	{
		if (m_bPause || m_bStep)
			return -1;
		BOOL         bRes = FALSE;
		FF_AV_FRAME* value = NULL;
		UINT32       avgBytesPerSec = m_audioParameterO.GetSamplesPerSec() * m_audioParameterO.GetBlockSize();
		do
		{
			while (m_audio.m_queue.remaining() == 0 && !m_exiting)
			{
				if ((av_gettime_relative() - m_timestamp) > (1000000LL * chunksize / avgBytesPerSec / 2))
					return -1;
				av_usleep(100);
			}
			value = m_audio.m_queue.Peek(TRUE);
			if (!value)
				return -1;
			m_audio.m_queue.Next();
		} while (value->serial != m_audio.m_queue.m_packets.m_serial);
		INT32          size = av_samples_get_buffer_size(NULL, value->frame->channels, value->frame->nb_samples,
			(enum AVSampleFormat)value->frame->format, 1);
		INT64          channellayout = (value->frame->channel_layout &&
			value->frame->channels == av_get_channel_layout_nb_channels(value->frame->channel_layout))
			? value->frame->channel_layout
			: av_get_default_channel_layout(value->frame->channels);
		AudioParameter cur;
		cur.SetAudioFormat(MS_AV_SAMPLE_FMT_TO_AUDIO_FORMAT((enum AVSampleFormat)value->frame->format));
		cur.SetBitsPerSample(MS_AV_SAMPLE_FMT_TO_SIZE((enum AVSampleFormat)value->frame->format));
		cur.SetChannelLayout(MS_AV_CH_LAYOUT_TO_CHANNEL_LAYOUT(channellayout));
		cur.SetSamplesPerSec(value->frame->sample_rate);
		cur.SetChannels(value->frame->channels);
		if (cur != m_audioParameterI)
		{
			m_resampler.Close();
			m_audioParameterI = cur;
		}

		if (m_audioParameterO != cur)
		{
			if (m_resampler.IsEmpty())
			{
				if (!m_resampler.Open(m_audioParameterI, m_audioParameterO))
				{
					return -1;
				}
			}
		}
		if (!m_resampler.IsEmpty())
		{
			UINT64        delay = 0;
			const UINT8** input = (const UINT8**)value->frame->extended_data;
			UINT32        countO = value->frame->nb_samples + 256;
			bRes = m_resampler.Resample(output, &countO, &delay, input, value->frame->nb_samples);
			if (!bRes)
			{
				return -1;
			}
			size = countO * m_audioParameterO.GetChannels() * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
		}
		else
		{
			output[0] = value->frame->data[0];
		}
		if (!isnan(value->pts))
			audioPTS = value->pts + (double)value->frame->nb_samples / value->frame->sample_rate;
		else
			audioPTS = NAN;
		serial = value->serial;
		return size;
	}

	INT32 AVDemuxer::GetVideoFrame(AVFrame* video)
	{
		INT32 got = 0;
		if ((got = OnDecodeFrame(m_video, video)) < 0)
			return -1;
		if (got)
		{
			DOUBLE dpts = NAN;
			if (video->pts != AV_NOPTS_VALUE)
				dpts = av_q2d(m_video.m_stream->time_base) * video->pts;
			video->sample_aspect_ratio = av_guess_sample_aspect_ratio(m_context, m_video.m_stream, video);
			if (video->pts != AV_NOPTS_VALUE)
			{
				DOUBLE clock = m_audioclock.GetClock();
				DOUBLE diff = dpts - clock;
				if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD && ((diff - m_lastdelay) < 0) &&
					(m_video.m_serial == m_videoclock.serial) && m_video.m_queue.m_packets.m_linkqueue.size())
				{
					av_frame_unref(video);
					got = 0;
				}
			}
		}
		return got;
	}

	INT32 AVDemuxer::OnQueuePicture(AVFrame* video, DOUBLE pts, DOUBLE duration, INT64 pos, INT32 serial)
	{
		FF_AV_FRAME* value = NULL;
		value = m_video.m_queue.Peek();
		if (!value)
			return -1;
		value->rational = video->sample_aspect_ratio;
		value->width = video->width;
		value->height = video->height;
		value->format = video->format;
		value->pts = pts;
		value->duration = duration;
		value->pos = pos;
		value->serial = serial;
		av_frame_move_ref(value->frame, video);
		m_video.m_queue.Push();
		return 0;
	}

	INT32 AVDemuxer::OnDecodeFrame(FF_AV& av, AVFrame* frame)
	{
		INT32 iRes = AVERROR(EAGAIN);
		for (;;)
		{
			if (m_exiting)
				return -1;
			AVPacket pkt;
			if (av.m_queue.m_packets.m_serial == av.m_serial)
			{
				do
				{
					if (m_exiting)
						return -1;
					if (av.m_queue.m_packets.m_stop)
						return -1;
					switch (av.m_context->codec_type)
					{
					case AVMEDIA_TYPE_VIDEO:
					{
						iRes = avcodec_receive_frame(av.m_context, frame);
						if (iRes >= 0)
						{
							frame->pts = frame->best_effort_timestamp;
						}
					}
					break;
					case AVMEDIA_TYPE_AUDIO:
					{
						iRes = avcodec_receive_frame(av.m_context, frame);
						if (iRes >= 0)
						{
							AVRational tb = { 1, frame->sample_rate };
							if (frame->pts != AV_NOPTS_VALUE)
								frame->pts = av_rescale_q(frame->pts, av.m_context->pkt_timebase, tb);
							else if (av.m_pts != AV_NOPTS_VALUE)
								frame->pts = av_rescale_q(av.m_pts, av.m_rational, tb);
							if (frame->pts != AV_NOPTS_VALUE)
							{
								av.m_pts = frame->pts + frame->nb_samples;
								av.m_rational = tb;
							}
						}
					}
					break;
					}
					if (iRes == AVERROR_EOF)
					{
						av.m_finished = av.m_serial;
						avcodec_flush_buffers(av.m_context);
						return 0;
					}
					if (iRes >= 0)
						return 1;
				} while (iRes != AVERROR(EAGAIN));
			}
			do
			{
				if (av.m_queue.m_packets.m_linkqueue.size() == 0)
				{
					if (av.m_continueCV != NULL)
						av.m_continueCV->Unlock(FALSE);
				}
				if (av.m_bPending)
				{
					av_packet_move_ref(&pkt, &av.m_pkt);
					av.m_bPending = FALSE;
				}
				else
				{
					if (!av.m_queue.m_packets.Pop(pkt, &av.m_serial))
						return -1;
				}
				if (av.m_queue.m_packets.m_serial == av.m_serial)
					break;
				av_packet_unref(&pkt);
			} while (1);
			if (pkt.data == g_packet.data)
			{
				avcodec_flush_buffers(av.m_context);
				av.m_finished = 0;
				av.m_pts = AV_NOPTS_VALUE;
				av.m_rational = { 0, 0 };
			}
			else
			{
				if (avcodec_send_packet(av.m_context, &pkt) == AVERROR(EAGAIN))
				{
					av.m_bPending = TRUE;
					av_packet_move_ref(&av.m_pkt, &pkt);
				}
				av_packet_unref(&pkt);
			}
		}
	}

	BOOL AVDemuxer::CheckPackets(AVStream* stream, INT32 streamID, AVPacketQueue* queue)
	{
		return streamID < 0 || queue->m_stop || (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
			queue->m_linkqueue.size() > MIN_FRAMES &&
			(!queue->m_duration || av_q2d(stream->time_base) * queue->m_duration > 1.0);
	}
} // namespace MediaSDK
