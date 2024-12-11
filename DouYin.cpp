//#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include "DouYin.h"
#include <json/json.h> // 使用 Jsoncpp 库处理 JSON
#include <chrono>
#include <SimpleIni.h>

#include <sherpa-onnx/c-api/c-api.h>
#include <sherpa-onnx/c-api/cxx-api.h>
#include <sherpa-onnx/cargs.h>

namespace DouYin
{


	// 定义一个函数来初始化结构体
	static Replay createReplay(long long eventCreatedAt, const std::string& userName, const std::string& userId, const std::string& type) {
		Replay p;
		p.eventCreatedAt = eventCreatedAt;
		p.userName = userName;
		p.userId = userId;
		p.type = type;

		return p;
	}



	////////////////////LiveRoom class//////////////////////////////////////////////////////////////
	LiveRoom::LiveRoom()
		: user_like_likes(),
			host(""),
			port(80),
			like_reply_type("like_reply"),
			comment_reply_type("comment_reply"),
			gift_reply_type("gift_reply")
		{

		}
	LiveRoom::~LiveRoom() {}

		// 通过ajax请求服务端评论的应答
	std::string LiveRoom::replayForContent(const std::string& question, const std::string& session_id) {
		try {
			//  从配置文件中读取
			if (host.empty()) {
				CSimpleIniA& iniConfig= Config::getInstance().getIniConfig();
				host = iniConfig.GetValue("server", "host");
				port = std::stoi(iniConfig.GetValue("server", "port", "80"));
			}
			std::string token = "Token 6433bf97ce23487ab15371af07609675";
			httplib::Client httpClient(host, port);
			httpClient.set_default_headers({ {"Authorization", token} });

			std::string requestBody = std::string("{\"question\":\"").append(question).append("\",\"session_id\": \"").append(session_id).append("\"}");

			std::string updateCallRecord = "/qa/ask";

			auto response = httpClient.Post(updateCallRecord, requestBody, "application/json");

			if (response && response->status == 200) {
				std::cout << "Request URL:" << updateCallRecord << "RequestBody:" << requestBody
					<< ",Response" << response->body << std::endl;

				Json::Reader reader;
				Json::Value root;
				bool parsingSuccessful = reader.parse(response->body, root);
				/* 响应示例	
				{
					"id": 2,
						"question" : "价格多少",
						"answer" : "文档中提到的汽车脚垫价格如下：\n\n1. 平面脚垫：通常在100以内，高端特殊材质会贵一些，约1000左右。\n2. 半包围脚垫：价格在1000~2000左右。\n3. 全包围脚垫：价格约200左右。\n4. 360软包脚垫：价格在300~1000，包安装。\n\n另外，团购产品的具体价格为：\n\n1. 288软包脚垫团购：288元\n2. 688软包脚垫团购：688元\n3. 799软包脚垫团购：799元\n4. 88大包围脚垫团购：88元\n5. 49尾箱垫团购：49元\n\n后备箱垫需要单独下单，价格未在文档中具体提及。",
						"session_id" : "2323-fe232"
				}*/
				// 获取响应体，并返回
				return std::string(root["answer"].asString());
			}
			else if (response && response->status == 401) {
				std::cout << "(updateCustomerRecord) Auth 401" << std::endl;
			}
			else {
				std::cout << "OutboundService::updateCallRecord error: "
					<< response.error() << "No response" << std::endl;
			}
		}
		catch (const std::exception& e) {
			std::cout << "Exception occurred while calling the API to query survey details by phone number: "
				<< e.what() << std::endl;
		}
		throw std::runtime_error("请求服务器失败");
	}



	// 直播间互动数据处理
	std::string LiveRoom::liveDataHandler(const Json::Value& payload) {

		std::map<std::string, std::list<DouYin::Replay>> live_replys;
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
			if (msg["msg_type"].asInt() == 1) {
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
						if (live_replys.find(like_reply_type) == live_replys.end()) live_replys[like_reply_type] = std::list<DouYin::Replay>();

						user_like_likes[open_id]++;
						if (user_like_likes[open_id] > 20) {
							// 有人点赞超过20次，发送点赞消息
							DouYin::Replay like_reply = DouYin::createReplay(msg["timestamp"].asInt64(), msg["nickname"].asString(), msg["sec_open_id"].asString(), like_reply_type);
							like_reply.replyContent = std::string("感谢").append(msg["nickname"].asString()).append("的点赞。欢迎大家给主播点点赞~，你们的支持是我们最大的动力。");
							live_replys[like_reply_type].push_back(like_reply);

							// 仅回复一次
							user_like_likes[open_id] = -1;
						}
					}
				}
			}
			else if (msg["msg_type"].asInt() == 2) {
				// TODO 读取评论 分析 回复
				std::string content = msg["content"].asString();
				std::string sessionId = msg["sec_open_id"].asString();
				// 请求服务端获取应答
				try {
					std::string reply = replayForContent(content, sessionId);

					DouYin::Replay comment_reply = DouYin::createReplay(msg["timestamp"].asInt64(), msg["nickname"].asString(), msg["sec_open_id"].asString(), comment_reply_type);
					comment_reply.content=content;
					comment_reply.replyContent = reply;
					live_replys[gift_reply_type].push_back(comment_reply);
				}
				catch (const std::runtime_error& e) {
					std::cerr << "Exception caught: " << e.what() << std::endl;
				}


			}
			else if (msg["msg_type"].asInt() == 3) {
				// 非常感谢 xx 送的礼物
				if (live_replys.find(gift_reply_type) == live_replys.end()) live_replys[gift_reply_type] = std::list<DouYin::Replay>();

				DouYin::Replay gift_reply = DouYin::createReplay(msg["timestamp"].asInt64(), msg["nickname"].asString(), msg["sec_open_id"].asString(), gift_reply_type);
				gift_reply.replyContent = std::string("非常感谢").append(msg["nickname"].asString()).append("送的礼物~，谢谢您的支持。不鼓励大家刷礼物，大家多看看商品，你们买到合意的商品才是主播最大的动力~");
				live_replys[gift_reply_type].push_back(gift_reply);
			}
			else {
				std::cout << "unknown msg_type(可能是进房类型):" + std::to_string(msg["msg_type"].asInt()) << std::endl;
			}
		}

		std::string reply = dispatch(live_replys);
		return reply;
	}


	//static  CSimpleIniA	g_config;

	//LiveRoom::LiveRoom()
	//	: user_like_likes(),
	//	host(""),
	//	like_reply_type("like_reply"),
	//	comment_reply_type("comment_reply"),
	//	gift_reply_type("gift_reply")
	//{
	//	g_config.SetUnicode();
	//	SI_Error rc = g_config.LoadFile("settings.ini");
	//	if (rc < 0) { throw std::invalid_argument("setting.ini load error."); }
	//}
	//LiveRoom::~LiveRoom() {}



	std::string  LiveRoom::dispatch(std::map<std::string, std::list<DouYin::Replay>> live_replys) {
		// TODO 挑重点回复：1.回复评论问题 2.评论问题没有时答谢送礼 3.送礼没有时欢饮进房 4.进房没有时点赞感谢 5.都没有时定期引导点赞 
		// TODO 2、3、4、5 需要构建回答库，随机选择回答库回答。
		// TODO 6.超时n秒后的不再回复
		// 平均语速：3~5字/秒，超过则触发丢弃操作。【源头控制】
		// TODO 答复列表 ==队列削峰==》 tts输出语音 ===》amix混入视频
		// live_replys 遍历


		// 调度方式一：全部有回应，不考虑延迟
		std::string replays = "";
		std::list<DouYin::Replay> contentReplys = live_replys[comment_reply_type];
		for (DouYin::Replay replay : contentReplys)
		{
			replays.append(replay.replyContent);
		}
		std::list<DouYin::Replay> giftReplys = live_replys[gift_reply_type];
		for (DouYin::Replay replay : giftReplys)
		{
			replays.append(replay.replyContent);
		}
		std::list<DouYin::Replay> likeReplys = live_replys[like_reply_type];
		for (DouYin::Replay replay : likeReplys)
		{
			replays.append(replay.replyContent);
		}

		return replays;
	}


	///////////////////////TTS//////////////////////////////////////////////////////////////////////
	
	// 定义静态实例
	TTS* TTS::instance = nullptr;
	std::once_flag TTS::onceFlag;
	
	TTS::TTS() : initialized(false) {
	}
	TTS::~TTS() {
		// 释放tts_model
		SherpaOnnxDestroyOfflineTts(tts_model);
	}

	void TTS::doInitialization() {
		// 在这里初始化内部状态
		std::lock_guard<std::mutex> lock(initMutex);
		


		// 加载tts模型
		memset(&tts_config, 0, sizeof(tts_config));
		std::string currentPath = __FILE__;
		std::string::size_type pos = currentPath.find_last_of("\\/");
		currentPath = currentPath.substr(0, pos + 1); // 获取当前文件的目录路径

		std::string resourceRoot = currentPath + "resource";

		std::string modelPath = resourceRoot + "/vits-melo-tts-zh_en/model.onnx";
		tts_config.model.vits.model = modelPath.c_str();

		std::string lexiconPath = resourceRoot + "/vits-melo-tts-zh_en/lexicon.txt";
		tts_config.model.vits.lexicon = lexiconPath.c_str();

		std::string tokensPath = resourceRoot + "/vits-melo-tts-zh_en/tokens.txt";
		tts_config.model.vits.tokens = tokensPath.c_str();
		//config.model.vits.noise_scale = atof(value);
		//config.model.vits.noise_scale_w = atof(value);
		//config.model.vits.length_scale = atof(value);
		//config.model.num_threads = atoi(value);
		//config.model.provider = value;
		//config.model.debug = atoi(value);
		//config.max_num_sentences = atoi(value);
		//sid = atoi(value);
		std::string fstsPath = resourceRoot + "/vits-melo-tts-zh_en/date.fst" + "," + resourceRoot + "/vits-melo-tts-zh_en/number.fst";
		tts_config.rule_fsts = fstsPath.c_str();
		std::string dataPath = resourceRoot + "/vits-melo-tts-zh_en/dict";
		tts_config.model.vits.dict_dir = dataPath.c_str();
		tts_config.model.debug = 1;

		tts_model = SherpaOnnxCreateOfflineTts(&tts_config);
		tts_sample_rate = SherpaOnnxOfflineTtsSampleRate(tts_model);


		initialized = true;
	}

	TTS& TTS::getInstance() {
		if (instance == nullptr) {
			std::call_once(onceFlag, []() {
				instance = new TTS();
				// 启动异步线程进行初始化
				std::thread t([]() {
					// 模拟耗时的初始化过程
					std::this_thread::sleep_for(std::chrono::seconds(2));
					TTS::getInstance().doInitialization();
					});
				t.detach(); // 分离线程，允许它在后台运行
				});
		}
		return *instance;
	}




	void TTS::generate(std::string text, SherpaOnnxGeneratedAudioCallbackWithArg callback) {

		const char* textChar = text.c_str();

		const SherpaOnnxGeneratedAudio* audio = SherpaOnnxOfflineTtsGenerateWithCallbackWithArg(tts_model, textChar, sid, 1.0, callback, &tts_sample_rate);

		// debug 时写出生成的音频
		std::string filename = "C:\\Users\\tutar\\generated.wav";
		SherpaOnnxWriteWave(audio->samples, audio->n, audio->sample_rate, filename.c_str());


		// 释放audio内存
		SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);

	}

	//////////////////////////////Config//////////////////////////////////////////////////////////
	//static  CSimpleIniA	ini_config;

	Config::~Config(){
		iniConfig.Reset();
	}
	Config::Config()
		:volumeChanged(false),
		aiVolume(2.0),
		bgVideoVolume(1.0),
		bgVideoPath("")
	{
		iniConfig.SetUnicode();
		SI_Error rc = iniConfig.LoadFile("settings.ini");
		if (rc < 0) { throw std::invalid_argument("setting.ini load error."); }
	}

	Config Config::instance;

	Config& Config::getInstance() {
		return instance;
	}

	CSimpleIniA& Config::getIniConfig() {
		return iniConfig;
	}
}