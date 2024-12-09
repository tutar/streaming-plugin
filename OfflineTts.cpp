// c-api-examples/offline-tts-c-api.c
//
// Copyright (c)  2023  Xiaomi Corporation

// This file shows how to use sherpa-onnx C API
// to convert text to speech using an offline model.

#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "cargs.h"
#include "c-api.h"


int32_t mainb(int32_t argc, char* argv[]) {

    SherpaOnnxOfflineTtsConfig config;
    memset(&config, 0, sizeof(config));

    int32_t sid = 0;
    std::string filename = strdup("./generated.wav");

    config.model.vits.model = "sherpa-onnx/tts/vits-melo-tts-zh_en/model.onnx";
    config.model.vits.lexicon = "vits-melo-tts-zh_en/lexicon.txt";
    config.model.vits.tokens = "vits-melo-tts-zh_en/tokens.txt";
    //config.model.vits.noise_scale = atof(value);
    //config.model.vits.noise_scale_w = atof(value);
    //config.model.vits.length_scale = atof(value);
    //config.model.num_threads = atoi(value);
    //config.model.provider = value;
    //config.model.debug = atoi(value);
    //config.max_num_sentences = atoi(value);
    //sid = atoi(value);
    
    // 释放文件
    free((void*)const_cast<char*>(filename.data()));

    config.rule_fsts = "./vits-melo-tts-zh_en/date.fst,./vits-melo-tts-zh_en/number.fst";
    
    config.model.vits.data_dir = "./vits-melo-tts-zh_en/dict";


    std::string text = "您们好 我是今天的主播，欢迎大家来到这里。";

    SherpaOnnxOfflineTts* tts = SherpaOnnxCreateOfflineTts(&config);

    const SherpaOnnxGeneratedAudio* audio =
        SherpaOnnxOfflineTtsGenerate(tts, const_cast<char*>(text.data()), sid, 1.0);

    SherpaOnnxWriteWave(audio->samples, audio->n, audio->sample_rate, const_cast<char*>(filename.data()));

    SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
    SherpaOnnxDestroyOfflineTts(tts);

    // 删除生成文件
    free((void*)const_cast<char*>(filename.data()));

    return 0;
}