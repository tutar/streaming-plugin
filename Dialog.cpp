
#include "DouYin.h"

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

namespace DouYin
{
	class VolumeWidget : public QWidget {
	public:
		VolumeWidget(QWidget* parent = nullptr) : QWidget(parent) {
			// 设置窗口标题和默认大小
			setWindowTitle("配置");
			resize(750, 450); // 设置窗口大小为宽750像素和高450像素
			Config& config = Config::getInstance();

			// 设置背景色和文字颜色
			setStyleSheet("QWidget {"
				"background-color: rgb(34,34,34);"
				"color: rgb(255,255,255); }"
				"QLineEdit {"
				"background-color: rgb(42,42,42);"
				"color: rgb(255,255,255); }"
				"QPushButton {"
				"background-color: rgb(51,51,51);"
				"color: rgb(255,255,255); }"
				"QPushButton {"
				"background-color: rgb(229, 50, 86);"
				"color: rgb(255,255,255);"
				"font-weight: bold; }" // 设置按钮的字体为粗体
				"QPushButton:pressed {"
				"background-color: rgb(117, 42, 69); }"
				"QSlider {"
				"background-color: rgb(42,42,42);"
				"color: rgb(255,255,255); }"
				"QLabel {"
				"color: rgb(255,255,255); }"
				"QTitleBar {"
				"background-color: rgb(51,51,51); }"); // 设置标题栏颜

			// 音量控制
			double defaultVolume = config.getAiVolume();
			volumeSlider = new QSlider(Qt::Horizontal);
			volumeSlider->setMinimum(0.0);
			volumeSlider->setMaximum(50);
			volumeSlider->setSingleStep(1); 
			volumeSlider->setValue(defaultVolume * 10);
			volumeEdit = new QLineEdit();
			volumeEdit->setReadOnly(true);
			volumeEdit->setText(QString::number(defaultVolume, 'f', 1)); // 初始值
			connect(volumeSlider, &QSlider::valueChanged, this, [=](int value) {
				double volume = value / 10.0;
				volumeEdit->setText(QString::number(volume, 'f', 1));
				});

			// 视频文件输入框
			videoInputEdit = new QLineEdit();
			videoInputButton = new QPushButton("选择视频文件");
			connect(videoInputButton, &QPushButton::clicked, this, &VolumeWidget::selectVideoFile);

			// 视频音量控制
			double defaultAudioVolume = config.getBgVideoVolume();
			audioSlider = new QSlider(Qt::Horizontal);
			audioSlider->setMinimum(1);
			audioSlider->setMaximum(50);
			volumeSlider->setSingleStep(1);
			audioSlider->setValue(defaultAudioVolume * 10);
			audioEdit = new QLineEdit();
			audioEdit->setReadOnly(true);
			audioEdit->setText(QString::number(defaultAudioVolume, 'f', 1)); // 初始值
			connect(audioSlider, &QSlider::valueChanged, this, [=](int value) { 
				double volume = value / 10.0;
				audioEdit->setText(QString::number(volume, 'f', 1));
				});

			// 提交和取消按钮
			submitButton = new QPushButton("提交");
			cancelButton = new QPushButton("取消");
			submitButton->setDefault(true); // 设置为默认选中按钮
			connect(submitButton, &QPushButton::clicked, this, &VolumeWidget::onSubmit);
			connect(cancelButton, &QPushButton::clicked, this, &VolumeWidget::onCancel);

			// 知识库文件输入框
			knowledgeBaseEdit = new QLineEdit();
			knowledgeBaseEdit->setReadOnly(true);
			knowledgeBaseButton = new QPushButton("选择知识库文件");
			connect(knowledgeBaseButton, &QPushButton::clicked, this, &VolumeWidget::selectKnowledgeBaseFiles);


			// 创建网格布局
			QGridLayout* gridLayout = new QGridLayout(this);

			// 添加控件到网格布局
			gridLayout->addWidget(new QLabel("AI音量:"), 0, 0);
			gridLayout->addWidget(volumeEdit, 0, 1);
			gridLayout->addWidget(volumeSlider, 0, 2);

			gridLayout->addWidget(new QLabel("视频文件:"), 1, 0);
			gridLayout->addWidget(videoInputEdit, 1, 1);
			gridLayout->addWidget(videoInputButton, 1, 2);

			gridLayout->addWidget(new QLabel("视频音量:"), 2, 0);
			gridLayout->addWidget(audioEdit, 2, 1);
			gridLayout->addWidget(audioSlider, 2, 2);


			gridLayout->addWidget(new QLabel("知识库文件(AI回答知识来源):"), 3, 0);
			gridLayout->addWidget(knowledgeBaseEdit, 3, 1, 1, 2);
			gridLayout->addWidget(knowledgeBaseButton, 3, 3);

			gridLayout->addWidget(submitButton, 4, 2);
			gridLayout->addWidget(cancelButton, 4, 1);

			setLayout(gridLayout);
		}

	private slots:
		void updateVolume(int value) {
			// 将滑动条的值转换为小数并更新文本框
			double volume = value / 10.0; // 转换为0.0到10.0
			volumeLineEdit->setText(QString::number(volume, 'f', 1)); // 保留1位小数
		}
		void selectVideoFile() {
			QString fileName = QFileDialog::getOpenFileName(this, "选择视频文件", "", "Video Files (*.mp4 *.avi *.mov)");
			if (!fileName.isEmpty()) {
				videoInputEdit->setText(fileName);
				// 假设configObject是存储配置的QObject派生对象
				// configObject->setProperty("videoFilePath", fileName);
			}
		}
		void selectKnowledgeBaseFiles() {
			// TODO: 添加选择知识库文件的逻辑
			QStringList fileNames = QFileDialog::getOpenFileNames(this, "选择知识库文件", "", "Text Files (*.txt);;Word Documents (*.doc *.docx);;Excel Files (*.xls *.xlsx)");
			if (!fileNames.isEmpty()) {
				if (!fileNames.isEmpty()) {
					// 您需要在这里处理文件名列表
					// 例如，您可以将它们连接成一个字符串
					knowledgeBaseEdit->setText(fileNames.join(", "));
				}
			}
		}
		void onSubmit() {
			int g_volume = volumeSlider->value() / 10.0;
			std::string g_videoFilePath = videoInputEdit->text().toStdString();
			int g_audioTime = audioSlider->value();
			// 可以在这里添加其他提交逻辑，比如验证数据或保存到文件
			//qDebug() << "提交表单数据：" << g_volume << g_videoFilePath << g_audioTime;

			Config& config = Config::getInstance();
			config.setAiVolume(volumeEdit->text().toDouble()/10.0);
			config.setBgVideoVolume(audioEdit->text().toDouble()/10.0);
			config.setVolumeChanged(true);
			config.setBgVideoPath(g_videoFilePath);
			VolumeWidget::close();
		}

		void onCancel() {
			VolumeWidget::close();
			// 可以在这里添加取消逻辑，比如重置表单或关闭窗口
		}
	private:
		QSlider* volumeSlider;
		QLineEdit* volumeLineEdit;
		QLineEdit* volumeEdit;
		QLineEdit* videoInputEdit;
		QPushButton* videoInputButton;
		QSlider* audioSlider;
		QLineEdit* audioEdit;
		QLineEdit* knowledgeBaseEdit;
		QPushButton* knowledgeBaseButton;
		QPushButton* submitButton;
		QPushButton* cancelButton;
	};


	Dialog::Dialog(){}
	Dialog::~Dialog() {}
	void Dialog::showConfig(int argc, char* argv[]) {
		QApplication app(argc,argv);
		VolumeWidget window;
		window.show();
		//drawConfigDialog();
		app.exec();
	}
}