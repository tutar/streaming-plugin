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

#include <QtWidgets/QApplication.h>
#include <QtWidgets/QWidget>
#include <QtWidgets/QDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QMessageBox>
#include <QtCore/QTemporaryFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtWidgets/QSlider>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QLabel>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtGui/QPalette>

#include <sherpa-onnx/c-api/c-api.h>
#include <sherpa-onnx/c-api/cxx-api.h>
#include <sherpa-onnx/cargs.h>
#include <SimpleIni.h>

#include "DouYin.h"

BOOL                    m_stop{ FALSE };
AudioParameter          g_ap1;
AudioParameter          g_ap2;
IPCDemo::AVDemuxer*     g_demuex1{ NULL };
IPCDemo::AVDemuxer*     g_demuex2{ NULL };
std::thread             g_task1;
std::thread             g_task2;
IPipeClient*			g_client{ NULL };
std::ofstream           g_logFile;
DouYin::LiveRoom*		g_live_room;

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
	 //std::filesystem::path path("D:\\open\\Livestreaming\\streaming-example.mp4");
	
	if (DouYin::Config::getInstance().getBgVideoPath().empty()) {
		// 弹框重新填
	}
	std::filesystem::path path(DouYin::Config::getInstance().getBgVideoPath());
	g_demuex1->Open(path.u8string());


	//std::string reply = "您们好\n\n1.我是今天的主播，欢迎大家来到这里。您们\n\n2. 我是今天的主播，欢迎大家来到这里。您们好\n\n3. 我是今天的主播，欢迎大家来到这里。您们好 \n\n 4. 我是今天的主播，欢迎大家来到这里。";
	////std::string reply = "您们好1.我是今天的主播，欢迎大家来到这里。您们2. 我是今天的主播，欢迎大家来到这里。您们好3. 我是今天的主播，欢迎大家来到这里。您们好4. 我是今天的主播，欢迎大家来到这里。";
	////std::string reply = "您们好 我是今天的主播，欢迎大家来到这里。您们好我是今天的主播，欢迎大家来到这里。";
	//
	//DouYin::TTS::getInstance().generate(reply, AudioGeneratedCallback);

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

//程序启动做些预处理
void OnApplicationStart() {

	// ffmpeg log
	//av_log_set_level(AV_LOG_ERROR);
	//av_log_set_callback([](void* ptr, int level, const char* fmt, va_list vargs) {
	//	// 打印日志消息
	//		vfprintf(stderr, fmt, vargs);
	//		fprintf(stderr, "\n");
	//	});

	// 加载配置文件
	CSimpleIniA& config = DouYin::Config::getInstance().getIniConfig();
	std::time_t t = time(nullptr);
	char   filetime[256];
	//strftime(filetime, sizeof(filetime), "%Y-%m-%d-%H-%M-%S", localtime(&t));
	strftime(filetime, sizeof(filetime), "%Y-%m-%d-%H", localtime(&t));
	std::string fileName = config.GetValue("log", "logFile")? config.GetValue("log", "logFile"):std::string(filetime).append(std::string(".log"));
	std::string filePathName = std::string(config.GetValue("log", "logPath")).append(std::string("/")).append(fileName);
	g_logFile.open(filePathName.c_str());

	g_live_room = new DouYin::LiveRoom();

}

void testContent() {
	std::string liveEventMessate = "\
	{\
  \"type\": \"EVENT_MESSAGE\",\
  \"eventName\": \"OPEN_LIVE_DATA\", \
  \"payload\": [\
    {\
      \"msg_id\": \"7352602227091444780\",\
      \"timestamp\": 1711939362000,\
      \"msg_type\": 2,\
      \"msg_type_str\": \"live_comment\",\
      \"sec_open_id\": \"2e441624-b99f-5f05-a632-d3f7c92df768\",\
      \"avatar_url\": \"https://p3.douyinpic.com/aweme/100x100/aweme-avatar/mosaic-legacy_3796_2975850990.jpeg?from=3067671334\",\
      \"nickname\": \"bor432\",\
      \"content\": \"主播厉害\",\
      \"user_privilege_level\": 1,\
      \"is_follow_anchor\": true,\
      \"fansclub_level\": 1\
	}\
	  ]\
	}\
";

	Json::Reader reader;
	Json::Value root;
	bool parsingSuccessful = reader.parse(liveEventMessate, root);
	std::string reply = g_live_room->liveDataHandler(root["payload"]);
	std::cout << reply << std::endl;
}
/*
void onMessageReceived() {
	// 接收到消息时，显示对话框
	QDialog dialog;
	dialog.setWindowTitle("配置页面");
	QVBoxLayout* layout = new QVBoxLayout(&dialog);

	QPushButton* button = new QPushButton("保存配置");
	layout->addWidget(button);
	QObject::connect(button, &QPushButton::clicked, [&]() {
		// 处理配置
		QMessageBox::information(&dialog, "信息", "配置已保存。");
		dialog.close();
		});

	dialog.setLayout(layout);
	dialog.exec(); // 显示模态对话框

	// 对话框关闭后，应用继续在后台运行
}
*/



void drawConfigDialog() {

	// 显示对话框
	QMessageBox msgBox;
	msgBox.setWindowTitle("事件通知");
	msgBox.setText("请填写以下信息：");

	// 添加自定义字段
	// 这里只是一个示例，你需要根据实际需求添加更多的字段
	msgBox.setInformativeText("时间: 数字: 文件: 文本: 音量:");

	// 用户点击确定后处理数据
	QObject::connect(&msgBox, &QMessageBox::accepted, [&]() {
		// 收集数据
		QJsonObject dataObject;
		// 假设你已经有了这些值
		dataObject["time"] = "12:00";
		dataObject["number"] = 123;
		dataObject["file"] = "/path/to/file";
		dataObject["text"] = "Some text";
		dataObject["volume"] = QJsonValue(70); // 转换为QJsonValue类型


		// 保存到临时文件
		QTemporaryFile file("temp_file.txt"); // 初始化QTemporaryFile对象
		if (file.open()) {
			QJsonDocument doc(dataObject);
			file.write(doc.toJson());
			file.close();
		}

		// 保存到C++配置对象（这里只是一个示例，你需要根据实际情况实现）
		// ConfigObject config;
		// config.setData(dataObject);
		});

	msgBox.exec(); // 显示模态对话框
}


//int main(int argc, char* argv[]) {
//	//优先异步加载模型
//	DouYin::TTS::getInstance();
//
//	DouYin::Dialog::showConfig(0, nullptr);
//	OnApplicationStart();
//
//	// 使用模型前等待加载完成
//	while (DouYin::TTS::getInstance().isInitialized() == false) {
//		Sleep(1);
//	}
//
//	//testContent();
//	//OnMessagePump1();
//	return 1;
//
//}
int main(int argc, char* argv[])
{
    if (argc <= 3)
    {
        std::cout << "input params invalid!" << std::endl;
        return -1;
    }
	//优先异步加载模型
	DouYin::TTS::getInstance();
	OnApplicationStart();

	logFile("starging.........:" + std::string(argv[1]) + " " + std::string(argv[2]) + " " + std::string(argv[3]));

    std::string firstArg(argv[1]);
    std::string frontStr = "--pipeName=";
    std::string pipeName = firstArg.substr(frontStr.length());
    std::string secondArg(argv[2]);
    std::string frontStr2 = "--maxChannels=";
    UINT32 maxChannels = std::stoi(secondArg.substr(frontStr2.length()));

	CreatePipeClient(&g_client);
	logFile("Pipe Client create success.........");

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
				// 弹框配置
				DouYin::Dialog::showConfig(0, nullptr);

				std::cout << std::string("EVENT_CONNECTED") << std::endl;

				g_task1 = std::thread(OnMessagePump1);
				//g_task2 = std::thread(OnMessagePump2);

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
				// 直播伴侣主动退出，则关闭插件
				logFile("直播伴侣主动退出，则关闭插件:" + std::string("EVENT_DISCONNECTED"));
				m_stop = TRUE;
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
						std::string reply = g_live_room->liveDataHandler(root["params"]["payload"]);
						
						// 跟视频语音合成
						//tts(reply);
						DouYin::TTS::getInstance().generate(reply, AudioGeneratedCallback);
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

	// 使用模型前等待加载完成
	while (DouYin::TTS::getInstance().isInitialized() == false) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	g_client->Open((to_wide_string(pipeName)).c_str(), maxChannels);


	std::string text;
	while (1)
	{
		if (m_stop){
			//m_stop = TRUE;
			if (g_task1.joinable())
				g_task1.join();
			if (g_task2.joinable())
				g_task2.join();
			if (g_client != NULL)
				g_client->Close();
			break;
		}

		if (std::string("DEBUG").compare(DouYin::Config::getInstance().getIniConfig().GetValue("log", "level"))) {
			std::getline(std::cin, text);
			if (text == "C") {
				m_stop = TRUE;
				if (g_task1.joinable())
					g_task1.join();
				if (g_task2.joinable())
					g_task2.join();
				if (g_client != NULL)
					g_client->Close();

				break;
			}
			if (g_client != NULL) {
				logFile("sub Msg:" + text);
				g_client->SendMessage(101, (const CHAR*)text.data(), text.size());
			}
		}
		Sleep(20);
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
