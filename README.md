
- 克隆项目
- vs打开sln文件
- 依赖工具配置
  - 安装vcpkg
  - VS中配置环境变量 [参考](https://learn.microsoft.com/zh-cn/vcpkg/get_started/get-started-msbuild?pivots=shell-powershell)
```
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
$env:PATH = "$env:VCPKG_ROOT;$env:PATH"
```

- sherpa-onnx环境【注意选择最新版本和对应环境包】
```
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.10.33/sherpa-onnx-v1.10.33-win-x64-static.tar.bz2
tar xvf sherpa-onnx-v1.10.33-win-x64-static.tar.bz2
mv sherpa-onnx-v1.10.32-win-x64-static/lib/*.lib lib
mv sherpa-onnx-v1.10.32-win-x64-static/include/* tts
```

- **模型准备**进入项目resource目录，下载解压 [参考](https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/vits.html#generate-speech-with-executable-compiled-from-c)
```
cd /path/to/resource
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-melo-tts-zh_en.tar.bz2
tar xvf vits-melo-tts-zh_en.tar.bz2
rm vits-melo-tts-zh_en.tar.bz2
```
- 

