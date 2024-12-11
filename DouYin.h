#pragma once
#include "Common.h"
#include <mutex>
#include <chrono>
#include <SimpleIni.h>
#include <json/json.h>
#include <sherpa-onnx/c-api/c-api.h>

namespace DouYin
{

	// 回复POJO
	typedef struct tagReplay
	{
		//std::string like_reply_key = "like_reply";
		//std::string comment_reply_key = "comment_reply";
		//std::string gift_reply_key = "gift_reply";
		//事件时间 毫秒时间
		long long eventCreatedAt;
		//用户名 nickName
		std::string userName;
		//用户ID-openId
		std::string userId;
		// 回复类型
		std::string type;
		//提问/评论
		std::string content;

		//主播回复文字
		std::string replyContent;
		//回复生成时间
		long long replyCreatedAt;
		//主播回复语音

		//预计时长【3~5字/秒】

		//语音生成时间
		long long voiceCreatedAt;
	} Replay;


	class LiveRoom
	{
	public:
		LiveRoom();
		~LiveRoom();

	private:
		std::unordered_map<std::string, long> user_like_likes;
		std::string host;
		int port;
		std::string like_reply_type;
		std::string comment_reply_type;
		std::string gift_reply_type;

	public:
		std::string liveDataHandler(const Json::Value& payload);
		std::string replayForContent(const std::string& question, const std::string& session_id);

	private:
		std::string dispatch(std::map<std::string, std::list<DouYin::Replay>> live_replys);
	};

	// 语音合成 懒汉单例实现
	class TTS
	{
	private:
		TTS();
		~TTS();
		// 防止拷贝和赋值
		TTS(const TTS&) = delete;
		TTS& operator=(const TTS&) = delete;
		void doInitialization();

	private:
		static TTS* instance;
		static std::once_flag onceFlag;

		std::mutex initMutex;
		bool initialized;

		//tts 全局变量，预加载
		SherpaOnnxOfflineTtsConfig   tts_config{ nullptr };
		SherpaOnnxOfflineTts*        tts_model{ NULL };
		int						     tts_sample_rate{ -1 };
		int32_t sid = 0; //Speaker ID.Default to 0. Note it is not used for single-speaker models.

	public:
		static TTS& getInstance();
		bool isInitialized()const { return initialized; }

		void generate(std::string, SherpaOnnxGeneratedAudioCallbackWithArg callback);
	};



	// 插件配置
	class Config
	{
	private:
		Config();
		~Config();

		// 防止拷贝和赋值
		Config(const Config&) = delete;  
		Config& operator=(const Config&) = delete;

		static Config instance;


		// properties
		/////////// 运行时配置
		CSimpleIniA	iniConfig;

		///////// 用户配置 如配置框
		double aiVolume;// AI合成语音音量0~10 1位小数
		double bgVideoVolume;// 视频音量 0~10 1位小数
		BOOL volumeChanged;// 视频或AI语音是否有变
		std::string bgVideoPath;// 视频路径

	public:
		static Config& getInstance();
		CSimpleIniA& getIniConfig();

		double getAiVolume() { return aiVolume; }
		void setAiVolume(double aiVolume) { aiVolume = aiVolume; }
		double getBgVideoVolume() { return bgVideoVolume; }
		void setBgVideoVolume(double bgVideoVolume) { bgVideoVolume = bgVideoVolume; }
		BOOL getVolumeChanged() { return volumeChanged; }
		void setVolumeChanged(BOOL VolumeChanged) { volumeChanged = VolumeChanged; }
		std::string getBgVideoPath() { return bgVideoPath; }
		void setBgVideoPath(std::string BgVideoPath) { bgVideoPath = BgVideoPath; }

	} ;

	//插件对话框
	class Dialog
	{
	public:
		Dialog();
		~Dialog();

		static void showConfig(int argc, char* argv[]);
	};
}