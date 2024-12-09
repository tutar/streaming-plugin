// ConsoleApplication1.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#if _MSC_VER >= 1600
#pragma execution_character_set("utf-8")
#endif

#include <iostream>
#include <codecvt>
#include <locale>
#include<filesystem>
#include "AVDemuxer.h"
#include "PipeDef.h"
#include <fstream>
#include <time.h>
#include <json/json.h> // 使用 Jsoncpp 库处理 JSON
#include <chrono>
#include <string>
#include <sstream>
//#include <QtWidgets/QApplication>
//#include <QtWidgets/QDialog>
//#include <QtWidgets/QPushButton>
//#include <QtWidgets/QVBoxLayout>
// sherpa-onnx c-api
#include <sherpa-onnx/c-api/c-api.h>
#include <sherpa-onnx/c-api/cxx-api.h>
#include <sherpa-onnx/cargs.h>
//#include <sherpa-onnx/offline-tts.h>


//#include <SDL2/SDL.h>
//#include <iostream>


#define CHECK_CHANNELS_CONSISTENCY(frame) \
    av_assert2(!(frame)->channel_layout || \
               (frame)->channels == \
               av_get_channel_layout_nb_channels((frame)->channel_layout))


BOOL                    m_stop{ FALSE };
AudioParameter          g_ap1;
AudioParameter          g_ap2;
IPCDemo::AVDemuxer*     g_demuex1{ NULL };
IPCDemo::AVDemuxer*     g_demuex2{ NULL };
std::thread             g_task1;
std::thread             g_task2;
IPipeClient*			g_client{ NULL };
std::ofstream           g_logFile;
std::unordered_map<std::string, long> user_like_likes;//用户点赞数累计 临时客户端方案

std::wstring to_wide_string(const std::string& input)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.from_bytes(input);
}

BOOL LogMessageCallbackHandler(LogSeverity logLevel, LPCSTR szFile, INT32 line, LPCSTR szText)
{
    switch (logLevel)
    {
    case IPC_LOG_VERBOSE:
    case IPC_LOG_INFO:
    case IPC_LOG_WARNING:
    case IPC_LOG_ERROR:
    case IPC_LOG_FATAL:
    case IPC_LOG_NUM_SEVERITIES:
        if (g_logFile.is_open())
        {
            g_logFile << std::string(szText) << std::endl;
        }
        break;
    default:
        break;
    }

    return TRUE;
}

void OnMessagePump2()
{
	g_demuex2 = new IPCDemo::AVDemuxer();
	g_demuex2->AUDIO_START = [](const AudioParameter& ap)->void {
		g_ap2 = ap;
		};
	g_demuex2->AUDIO_PACKET = [](PipeSDK::IPC_PACKET& pkt)->void {
		pkt.channel = 1;
		if (g_client != NULL)
			g_client->WritePacket(&pkt, 1000);

		};
	g_demuex2->VIDEO_PACKET = [](PipeSDK::IPC_PACKET& pkt)->void {
		pkt.channel = 1;
		if (g_client != NULL)
			g_client->WritePacket(&pkt, 1000);
		};
	std::filesystem::path path("D:\\open\\Livestreaming\\tiger_natual.mp4");
	g_demuex2->Open(path.u8string());
	for (;;)
	{
		if (m_stop)
			break;
		Sleep(1);
	}
	g_demuex2->Close();
	delete g_demuex2;
}

static void logFile(std::string szText) {
	if (g_logFile.is_open())
	{
		g_logFile << szText << std::endl;
	}
}

// 生成UUID的函数
std::string generateUUID() {
	std::stringstream ss;
	auto t = std::chrono::system_clock::now().time_since_epoch().count();
	ss << std::hex << t;
	return ss.str();
}

// 发送订阅事件的函数
std::string constructSubscribeEvent(const std::string& eventName) {
	// 创建请求 JSON 对象
	Json::Value requestJson;
	requestJson["type"] = "request";
	requestJson["reqId"] = generateUUID(); // 生成唯一的 reqId
	requestJson["method"] = "x.subscribeEvent";
	Json::Value params;
	params["eventName"] = eventName;
	params["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()
	).count(); // 获取当前时间戳（毫秒级）
	requestJson["params"] = params;

	// 发送请求(const CHAR*)text.data()
	std::string text(requestJson.toStyledString());

	logFile("subscribeEvent: " + text);

	return text;	
}

void log_callback(void* ptr, int level, const char* fmt, va_list vargs) {
	// 忽略低于 av_log_get_level() 阈值的日志消息
	if (level > av_log_get_level()) {
		return;
	}

	// 打印日志消息
	vfprintf(stderr, fmt, vargs);
	fprintf(stderr, "\n");
}


static int32_t AudioGeneratedCallback(const float* s, int32_t n, void* arg) {
	// push samples
	int ret = 0;
	if (n > 0) {

		int sampleRate = *(static_cast<int*>(arg));

	/*	std::vector<float> samples = std::vector<float>{ s, s + n };
		float* frame_ptr = new float[samples.size()];*/


		IPCDemo::FF_AV_SAMPLES samples;
		samples.data = std::vector<float>{ s, s + n };
		samples.sampleRate = *(static_cast<int*>(arg));

		if (g_demuex1) {
			// 往播放队列里塞
			g_demuex1->pushMasterAudio(std::move(samples));
		}
	}
	
	return 1;
}

// 语音合成
void tts(std::string text) {
	SherpaOnnxOfflineTtsConfig config;
	memset(&config, 0, sizeof(config));

	int32_t sid = 0;
	std::string filename = "C:\\Users\\tutar\\generated.wav";

	std::string currentPath = __FILE__;
	std::string::size_type pos = currentPath.find_last_of("\\/");
	currentPath = currentPath.substr(0, pos + 1); // 获取当前文件的目录路径

	std::string resourceRoot = currentPath+"resource";

	std::string modelPath = resourceRoot + "/vits-melo-tts-zh_en/model.onnx";
	config.model.vits.model = modelPath.c_str();

	std::string lexiconPath = resourceRoot + "/vits-melo-tts-zh_en/lexicon.txt";
	config.model.vits.lexicon = lexiconPath.c_str();

	std::string tokensPath = resourceRoot + "/vits-melo-tts-zh_en/tokens.txt";
	config.model.vits.tokens = tokensPath.c_str();
	//config.model.vits.noise_scale = atof(value);
	//config.model.vits.noise_scale_w = atof(value);
	//config.model.vits.length_scale = atof(value);
	//config.model.num_threads = atoi(value);
	//config.model.provider = value;
	//config.model.debug = atoi(value);
	//config.max_num_sentences = atoi(value);
	//sid = atoi(value);

	// 释放文件
	//free((void*)(filename.c_str()));

	std::string fstsPath = resourceRoot + "/vits-melo-tts-zh_en/date.fst" +"," + resourceRoot + "/vits-melo-tts-zh_en/number.fst";
	config.rule_fsts = fstsPath.c_str();

	std::string dataPath = resourceRoot + "/vits-melo-tts-zh_en/dict";
	config.model.vits.dict_dir = dataPath.c_str();
	config.model.debug = 1;

	SherpaOnnxOfflineTts* tts = SherpaOnnxCreateOfflineTts(&config);

	const char* textChar = text.c_str();
	int sampleRate = SherpaOnnxOfflineTtsSampleRate(tts);

	const SherpaOnnxGeneratedAudio* audio = SherpaOnnxOfflineTtsGenerateWithCallbackWithArg(tts, textChar, sid, 1.0, AudioGeneratedCallback, &sampleRate);

	SherpaOnnxWriteWave(audio->samples, audio->n, audio->sample_rate, filename.c_str());

	SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
	SherpaOnnxDestroyOfflineTts(tts);

	// 删除生成文件
	//free((void*)(filename.c_str()));

}





// 直播间互动数据处理
std::string liveDataHandler(const Json::Value& payload) {

	std::map<std::string,std::list<std::string>> live_replys;
	std::string like_reply_key="like_reply";
	std::string comment_reply_key="comment_reply";
	std::string gift_reply_key="gift_reply";
	// 遍历payload内容
	for (const Json::Value& msg : payload) {
		// 语音优先顺序：
		// 业务问题必答，积极地评论感谢、非积极和超界限的忽略、恶劣的上报）-
		//（大礼物每次/空闲每次/非空闲忽略）礼物-
		//（空闲每次/非空闲忽略）进房欢迎-
		//（单人累计20各、感谢仅一次）点赞-
		
		// demo阶段：
		// 评论必答
		// 礼物感谢
		// 进房欢迎
		// 单人累计点赞感谢
		// 
		// 视频音频
		
		// 互动数据处理 1.点赞 2.评论 3.礼物 ？.进房
		if(msg["msg_type"].asInt() == 1) {
			// 优先级最低的回复，某人点赞累加到20感谢xxx点赞
			std::string open_id = msg["sec_open_id"].asString();
			auto it = user_like_likes.find(open_id);
			if (it == user_like_likes.end())
			{
				user_like_likes[open_id] = 1L;
			}
			else {
				// 重复点赞，忽略
				if (user_like_likes[open_id] != -1)
				{
					// 在点赞回复中添加点赞消息
					// 如果没有列表则创建一个
					if (live_replys.find(like_reply_key) == live_replys.end()) live_replys[like_reply_key] = std::list<std::string>();

					user_like_likes[open_id]++;
					if (user_like_likes[open_id] > 20) {
						// 有人点赞超过20次，发送点赞消息
						std::string like_reply = "感谢" + msg["nickname"].asString() + "的点赞。欢迎大家给主播点点赞~，你们的支持是我们最大的动力。";
						live_replys[like_reply_key].push_back(like_reply);

						// 仅回复一次
						user_like_likes[open_id] = -1;
					}
				}
			}
		}else if(msg["msg_type"].asInt() == 2) {
			// TODO 读取评论 分析 回复
		}
		else if (msg["msg_type"].asInt() == 3) {
			// 非常感谢 xx 送的礼物
			if (live_replys.find(gift_reply_key) == live_replys.end()) live_replys[gift_reply_key] = std::list<std::string>();

			std::string gift_reply = "非常感谢" + msg["nickname"].asString() + "送的礼物~，谢谢您的支持。不鼓励大家刷礼物，大家多看看商品，你们买到合意的商品才是主播最大的动力~";
			live_replys[gift_reply_key].push_back(gift_reply);
		}
	}

	// TODO 截取x秒进行
	// TODO tts输出语音
	std::string reply = "您们好 我是今天的主播，欢迎大家来到这里。";
	return reply;
}

//// 配置面板类
//class ConfigDialog : public QDialog {
//    Q_OBJECT
//	public:
//		ConfigDialog(QWidget *parent = nullptr) : QDialog(parent) {
//			// 设置对话框标题
//			setWindowTitle("互动工具配置");
//
//			// 创建一些配置选项
//			QPushButton *closeButton = new QPushButton("关闭", this);
//			connect(closeButton, &QPushButton::clicked, this, &ConfigDialog::accept);
//		}
//};
//
//// 函数：展示配置面板
//void showConfigDialog() {
//	//使用Qt创建一个弹框
//
//
//    // ConfigDialog configDialog;
//    // configDialog.exec(); // 显示配置面板
//}


void OnMessagePump1()
{
	g_demuex1 = new IPCDemo::AVDemuxer();
	g_demuex1->AUDIO_START = [](const AudioParameter& ap)->void {
		g_ap1 = ap;
		};
	g_demuex1->AUDIO_PACKET = [](PipeSDK::IPC_PACKET& pkt)->void {
		pkt.channel = 0;
		if (g_client != NULL)
			g_client->WritePacket(&pkt, 1000);

		};
	g_demuex1->VIDEO_PACKET = [](PipeSDK::IPC_PACKET& pkt)->void {
		pkt.channel = 0;
		if (g_client != NULL)
			g_client->WritePacket(&pkt, 1000);
		};
	//std::filesystem::path path("D:\\open\\Livestreaming\\tiger_natual.mp4");
	 std::filesystem::path path("D:\\open\\Livestreaming\\home.mp4");
	//std::filesystem::path path("D:\\open\\Livestreaming\\longLei.MP3");
	g_demuex1->Open(path.u8string());


	tts("您们好 我是今天的主播，欢迎大家来到这里。总结来说，这个宏定义整型列表选项。");

	for (;;)
	{
		if (m_stop)
			break;
		Sleep(1);
	}
	g_demuex1->Close();
	delete g_demuex1;
}

/*
int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        return 1;
    }

    // 加载WAV文件
    SDL_AudioSpec spec;
    Uint8* audioBuf;
    Uint32 audioLen;

    if (SDL_LoadWAV("C:\\Users\\tutar\\generated.wav", &spec, &audioBuf, &audioLen) == nullptr) {
        std::cerr << "Failed to load WAV: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    // 打开音频设备
    if (SDL_OpenAudio(&spec, nullptr) < 0) {
        std::cerr << "Failed to open audio: " << SDL_GetError() << std::endl;
        SDL_FreeWAV(audioBuf);
        SDL_Quit();
        return 1;
    }

    // 将音频数据放入音频设备
    SDL_QueueAudio(1, audioBuf, audioLen);

    // 播放音频
    SDL_PauseAudio(0);

    // 等待音频播放完成
    while (SDL_GetQueuedAudioSize(1) > 0) {
        SDL_Delay(100); // 简单的等待，实际应用中可能需要更好的方法
    }

    // 清理资源
    SDL_CloseAudio();
    SDL_FreeWAV(audioBuf);
    SDL_Quit();

    return 0;
}
*/



int main() {

	// ffmpeg log
	av_log_set_level(AV_LOG_ERROR);
	av_log_set_callback(log_callback);

	OnMessagePump1();
	return 0;
}

int mainbak(int argc, char* argv[])
{
    if (argc <= 3)
    {
        std::cout << "input params invalid!" << std::endl;
        return -1;
    }

	std::time_t t = time(nullptr);
	char   filetime[256];
	//strftime(filetime, sizeof(filetime), "%Y-%m-%d-%H-%M-%S", localtime(&t));
	strftime(filetime, sizeof(filetime), "%Y-%m-%d-%H", localtime(&t));
	std::string filename = std::string("D:\\open\\Livestreaming\\ConsoleApplication1\\bin\\").append(std::string("client_").append(filetime).append(std::string(".log")));
	g_logFile.open(filename.c_str());

	logFile("starging.........:" + std::string(argv[1]) + " " + std::string(argv[2]) + " " + std::string(argv[3]));

    std::string firstArg(argv[1]);
    std::string frontStr = "--pipeName=";
    std::string pipeName = firstArg.substr(frontStr.length());
    std::string secondArg(argv[2]);
    std::string frontStr2 = "--maxChannels=";
    UINT32 maxChannels = std::stoi(secondArg.substr(frontStr2.length()));

	CreatePipeClient(&g_client);
	logFile("Pipe Client create success.........");

	//std::time_t t = time(nullptr);
    //char   filetime[256];
    //strftime(filetime, sizeof(filetime), "%Y-%m-%d-%H-%M-%S", localtime(&t));
    //std::string filename = std::string("D:\\open\\Livestreaming\\ConsoleApplication1\\bin\\").append(std::string("client_").append(filetime).append(std::string(".log")));
    //g_logFile.open(filename.c_str());

    g_client->SetLogMessageCallback(LogMessageCallbackHandler);
	g_client->SetCallback([](IPC_EVENT_TYPE type, UINT32 msg, const CHAR* data, UINT32 size, void* args) -> void {
		IPipeClient* pThis = static_cast<IPipeClient*>(args);
		if (pThis != NULL)
		{
			logFile("IPC_EVENT_TYPE: " + std::to_string(type));
			switch (type)
			{
			case PipeSDK::EVENT_CONNECTED:
			{
				logFile("已连接，开始推送流....");
				std::cout << std::string("EVENT_CONNECTED") << std::endl;
				g_task1 = std::thread(OnMessagePump1);
				g_task2 = std::thread(OnMessagePump2);

				// 订阅直播间开放数据推送事件​
				std::string event_req = constructSubscribeEvent("OPEN_LIVE_DATA");
				BOOL sub_result = g_client->SendMessage(102, (const CHAR*)event_req.data(), event_req.size());
				logFile("sub_result Msg:" + sub_result);
			}
			break;
			case PipeSDK::EVENT_BROKEN:
				std::cout << std::string("EVENT_BROKEN") << std::endl;
				break;
			case PipeSDK::EVENT_DISCONNECTED:
				std::cout << std::string("EVENT_DISCONNECTED") << std::endl;
				break;
			case PipeSDK::EVENT_CONNECTION_RESET:
				std::cout << std::string("EVENT_CONNECTION_RESET") << std::endl;
				break;
			case PipeSDK::EVENT_MESSAGE:
			{
				logFile("SetCallback msg:"+ std::to_string(msg));

				std::string szText(data, size);
				logFile("Receive Msg:"+ szText);

				Json::Reader reader;
				Json::Value root;
				bool parsingSuccessful = reader.parse(szText, root);
				if (parsingSuccessful) {
					const Json::Value& type = root["type"];
					const Json::Value& eventName = root["eventName"];
					
					// OPEN_LIVE_DATA 直播数据处理
					if (type.isString() && eventName.isString() && type.asString() == "event" && eventName.asString() == "OPEN_LIVE_DATA")
					{
						logFile("replay generate based on live data");
						std::string reply = liveDataHandler(root["params"]["payload"]);
						
						// 跟视频语音合成
						tts(reply);
						logFile("tts finished:" + reply);
					}


					// 处理其他Message Event
				}
				else {
					std::cerr << "JSON parsing failed: " << reader.getFormattedErrorMessages();
				}

			/*	if (reader.parse(szText, root))
				{
					std::string eventName = root["eventName"].asString();
					if (eventName == "OPEN_WIN_FOCUS")
					{
						//logFile("Receive event name:" + eventName);
						// 当监听到 OPEN_WIN_FOCUS 事件时，调用 showConfigDialog 函数
						int argc;
						char* argv[] = { NULL };;
						QApplication app(argc,argv); // 创建 QApplication 实例
						// 创建 QDialog 实例
						QDialog dialog;
						// 标题使用 UTF-8 编码转化

						dialog.setWindowTitle("简单对话框"); // 设置对话框标题

						// 创建一个 QPushButton 实例，文本为“关闭”，父对象为当前 QDialog 实例
						QPushButton* closeButton = new QPushButton("关闭", &dialog);
						dialog.setLayout(new QVBoxLayout()); // 为对话框设置布局
						dialog.layout()->addWidget(closeButton); // 将按钮添加到布局中

						// 连接关闭按钮的 clicked 信号到 dialog 的 accept 槽
						QObject::connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);

						dialog.show(); // 显示对话框
						app.exec(); // 进入事件循环，并返回退出代码
					}
				}*/
				std::cout << "Receive Msg: " << szText << std::endl;
			}
			break;
			}
		};
		},
		g_client);

	g_client->Open((to_wide_string(pipeName)).c_str(), maxChannels);



	std::string text;
	while (1)
	{
		std::getline(std::cin, text);
		if (text == "C")

		{
			m_stop = TRUE;
			if (g_task1.joinable())
				g_task1.join();
			if (g_task2.joinable())
				g_task2.join();
			if (g_client != NULL)
				g_client->Close();

			break;
		}
		if (g_client != NULL)
			logFile("sub Msg:" + text);
			g_client->SendMessage(101, (const CHAR*)text.data(), text.size());
	}
	if (g_client != NULL)
		g_client->Close();
	if (g_task1.joinable())
		g_task1.join();
	if (g_task2.joinable())
		g_task2.join();
	if (g_client != NULL)
		g_client->Release();
    if (g_logFile.is_open())
        g_logFile.close();
}
