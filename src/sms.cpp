#include "SignUtils.hpp"
#include "SmsReader.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

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
    std::cerr << "加载配置文件失败: " << e.what() << std::endl;
    return 1;
  }

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
      if (appConfig.debugEnabled) {
        std::cout << "[WebSocket] 连接已建立" << std::endl;
      }
      break;
    case ix::WebSocketMessageType::Message:
      // To-Do
      break;
    case ix::WebSocketMessageType::Error:
      if (appConfig.debugEnabled) {
        std::cout << "[WebSocket] 连接错误: " << msg->errorInfo.reason
                  << std::endl;
        // 以 json 形式打印 errorInfo
        json errorInfoJson;
        errorInfoJson["reason"] = msg->errorInfo.reason;
        errorInfoJson["retries"] = msg->errorInfo.retries;
        errorInfoJson["wait_time"] = msg->errorInfo.wait_time;
        errorInfoJson["http_status"] = msg->errorInfo.http_status;
        errorInfoJson["decompressionError"] = msg->errorInfo.decompressionError;
        std::cout << "errorInfo: " << errorInfoJson.dump() << std::endl;
      }
      break;
    case ix::WebSocketMessageType::Close:
      if (appConfig.debugEnabled) {
        std::cout << "[WebSocket] 连接关闭" << std::endl;
      }
      break;
    default:
      break;
    }
  });

  // 启动 WebSocket
  webSocket.start();

  // 初始化短信读取器
  QmiSmsReader reader(appConfig.devicePath);

  if (appConfig.debugEnabled) {
    std::cout << "\n启动异步监听，按 Ctrl+C 停止程序...\n" << std::endl;
  }

  // 每次监听到新短信时的回调
  reader.startListening(std::chrono::seconds(1), [&](const CompleteSMS &sms) {
    // 如果开启了debug，则打印收到的短信
    if (appConfig.debugEnabled) {
      std::cout << "-------------------------------------" << std::endl;
      std::cout << "[监听到新短信]" << std::endl;
      std::cout << "发件人: " << sms.sender << std::endl;
      std::cout << "时间戳: " << sms.timestamp << std::endl;
      std::cout << "完整内容: " << sms.fullText << std::endl;
      for (const auto &part : sms.parts) {
        std::cout << "  [索引 " << part.memoryIndex
                  << "] 分段号: " << part.partNumber << ", 内容: " << part.text
                  << std::endl;
      }
      std::cout << "-------------------------------------" << std::endl;
    }

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

  if (appConfig.debugEnabled) {
    std::cout << "\n接收到停止信号，停止监听..." << std::endl;
  }
  reader.stopListening();

  // 停止 WebSocket
  webSocket.stop();

  if (appConfig.debugEnabled) {
    std::cout << "程序退出" << std::endl;
  }

  return 0;
}
