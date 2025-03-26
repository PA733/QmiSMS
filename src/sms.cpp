#include "SignUtils.hpp"
#include "SmsReader.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include <glog/logging.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

using json = nlohmann::json;

namespace {
std::atomic<bool> g_running{true}; // 全局运行状态标志
}

// 信号处理
void stop_signal_handler(int /*signal*/) { g_running.store(false); }

// 配置结构体
struct AppConfig {
  std::string devicePath;
  std::string wsUrl;
  std::string caCertPath;
  std::string secret;
  bool deleteAfterRead;
  bool debugEnabled;
};

// 加载配置
AppConfig loadConfig(const std::string &configPath) {
  AppConfig config;
  YAML::Node root = YAML::LoadFile(configPath);
  config.devicePath = root["device_path"].as<std::string>();
  config.wsUrl = root["websocket_url"].as<std::string>();
  config.caCertPath = root["ca_cert_path"].as<std::string>();
  config.secret = root["secret_key"].as<std::string>();
  config.deleteAfterRead = root["delete_after_read"].as<bool>();
  config.debugEnabled = root["debug"].as<bool>();
  return config;
}

void init_logger(bool enable_debug) {
  FLAGS_logtostderr = 1;
  if (enable_debug) {
    FLAGS_v = 1;
  }
  google::InitGoogleLogging("QmiSms");
}

int main() {
  ix::initNetSystem();

  // 注册停止信号处理
  std::signal(SIGINT, stop_signal_handler);
  std::signal(SIGTERM, stop_signal_handler);

  // 加载配置
  AppConfig appConfig = {};
  try {
    appConfig = loadConfig("config.yaml");
  } catch (const std::exception &e) {
    LOG(ERROR) << "加载配置文件失败: " << e.what();
    return 1;
  }

  // 初始化日志
  init_logger(appConfig.debugEnabled);

  // 创建 WebSocket 对象
  ix::WebSocket webSocket;
  webSocket.setUrl(appConfig.wsUrl);

  if (appConfig.wsUrl.find("wss://") == 0) {
    ix::SocketTLSOptions tlsOptions;
    tlsOptions.tls = true;
    tlsOptions.caFile = appConfig.caCertPath;
    webSocket.setTLSOptions(tlsOptions);
  }

  // 设置回调函数，处理连接事件、接收消息和错误
  webSocket.setOnMessageCallback([&webSocket, &appConfig](
                                     const ix::WebSocketMessagePtr &msg) {
    switch (msg->type) {
    case ix::WebSocketMessageType::Open:
      LOG(INFO) << "[WebSocket] 连接已建立";
      break;
    case ix::WebSocketMessageType::Message:
      // To-Do
      break;
    case ix::WebSocketMessageType::Error:
      LOG(WARNING) << "[WebSocket] 连接错误: " << msg->errorInfo.reason;
      // 以 json 形式打印 errorInfo
      {
        json errorInfoJson;
        errorInfoJson["reason"] = msg->errorInfo.reason;
        errorInfoJson["retries"] = msg->errorInfo.retries;
        errorInfoJson["wait_time"] = msg->errorInfo.wait_time;
        errorInfoJson["http_status"] = msg->errorInfo.http_status;
        errorInfoJson["decompressionError"] = msg->errorInfo.decompressionError;
        VLOG(1) << "errorInfo: " << errorInfoJson.dump();
      }
      break;
    case ix::WebSocketMessageType::Close:
      LOG(INFO) << "[WebSocket] 连接关闭";
      break;
    default:
      break;
    }
  });

  // 启动 WebSocket
  webSocket.start();

  while (webSocket.getReadyState() != ix::ReadyState::Open) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // 初始化短信读取器
  QmiSmsReader reader(appConfig.devicePath);

  LOG(INFO) << "\n启动异步监听，按 Ctrl+C 停止程序...\n" << std::endl;

  // 每次监听到新短信时的回调
  reader.startListening(std::chrono::seconds(1), [&](const CompleteSMS &sms) {
    VLOG(1) << "-------------------------------------" << std::endl
            << "[监听到新短信]" << std::endl
            << "发件人: " << sms.sender << std::endl
            << "时间戳: " << sms.timestamp << std::endl
            << "完整内容: " << sms.fullText;
    for (const auto &part : sms.parts) {
      VLOG(1) << "  [索引 " << part.memoryIndex
              << "] 分段号: " << part.partNumber << ", 内容: " << part.text;
    }
    VLOG(1) << "-------------------------------------";

    // 签名
    std::string currentTimestamp = sms.timestamp;
    std::string sign = generateSign(currentTimestamp, appConfig.secret);

    // payload
    json msgPayload;
    msgPayload["sender"] = sms.sender;
    msgPayload["text"] = sms.fullText;
    msgPayload["timestamp"] = currentTimestamp;
    msgPayload["sign"] = sign;

    // 外层 JSON
    json wsMessage;
    wsMessage["action"] = "send_message";
    wsMessage["payload"] = msgPayload;

    webSocket.send(wsMessage.dump());

    if (appConfig.deleteAfterRead) {
      for (const auto &part : sms.parts) {
        reader.deleteMessage(part.memoryIndex);
      }
    }
  });

  // 主循环，等待退出信号
  while (g_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  LOG(INFO) << "\n接收到停止信号，停止监听..." << std::endl;
  reader.stopListening();

  // 停止 WebSocket
  webSocket.stop();
  LOG(INFO) << "程序退出" << std::endl;

  return 0;
}
