#ifndef SMS_READER_HPP
#define SMS_READER_HPP

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>

// C Headers
extern "C" {
#include "pdulib.h"
#include <glib-unix.h>
#include <glib.h>
#include <libqmi-glib.h>
}

// 单个短信分段结构
struct SMSPart {
  int memoryIndex;              // 短信在设备存储中的索引
  int partNumber;               // 分段号
  std::string hexPDU;           // 原始 PDU 十六进制文本
  std::vector<uint8_t> rawData; // 原始 PDU 二进制数据
  std::string text;             // 解码后的短信文本
  std::string sender;           // 发件人号码
  std::string timestamp;        // 时间戳
};

struct CompleteSMS {
  std::string sender;
  std::string timestamp;
  std::string fullText;       // 完整消息
  std::vector<SMSPart> parts; // 消息分段
};

// 用于同步列出短信的上下文
struct ListContext {
  GMainLoop *loop;
  std::vector<int> *messageIndices;
  bool success;
};

// 用于同步读取短信的上下文
struct MessageSyncContext {
  GMainLoop *loop;
  std::vector<CompleteSMS> completeSMSList;
  // 按 memoryIndex 存储原始短信（实际应用中可能需要按分段参考号分组）
  std::unordered_map<int, SMSPart> rawSMSMap;
  int totalSMSCount = 0;
  int processedSMSCount = 0;
  std::promise<std::vector<CompleteSMS>> promise;
  QmiDevice *device = nullptr;
  QmiClientWms *client = nullptr;
  bool temporaryClient = false; // 若为临时 client，则操作完成后需释放
  
  // 添加待处理的短信索引队列
  std::queue<int> pendingSmsIndices;
  
  // 存储需要删除的重复短信索引
  std::vector<int> toDeleteIndices;
};

// 用于同步删除短信的上下文
struct DeleteSMSContext {
  GMainLoop *loop;
  uint memoryIndex;
  std::promise<bool> promise;
  QmiDevice *device = nullptr;
  QmiClientWms *client = nullptr;
  bool temporaryClient = false; // 若为临时 client，则操作完成后需释放
};

// 用于原始读取操作的上下文
struct RawReadUserData {
  MessageSyncContext *ctx;
  int memoryIndex;
  QmiMessageWmsRawReadInput *read_input;
};

// 用于同步 client 分配的上下文
struct SynchronousClientContext {
  GMainLoop *loop;
  QmiClientWms *client;
  bool success;
};

// 用于同步释放 client 的上下文
struct ReleaseClientContext {
  GMainLoop *loop;
  bool success;
};

class QmiSmsReader {
public:
  // 构造时指定设备路径，默认"/dev/cdc-wdm0"
  explicit QmiSmsReader(const std::string &devicePath = "/dev/cdc-wdm0");
  ~QmiSmsReader();

  // 同步方式一次性读取全部短信，返回一个 CompleteSMS 数组
  std::vector<CompleteSMS> readAllMessages();

  // 异步监听：启动监听进程，每隔 interval 调用一次；新短信通过 callback
  // 单条传出
  void startListening(std::chrono::seconds interval,
                      std::function<void(const CompleteSMS &)> callback);

  // 同步删除短信
  bool deleteMessage(int memoryIndex);

  // 停止监听，释放所有资源
  void stopListening();

  // 列出所有短信的索引，新增参数表示是否已持有锁
  std::vector<int> listAllMessages(bool alreadyLocked = false);

private:
  std::string devicePath_;
  QmiDevice *device_ = nullptr;

  std::atomic<bool> listening_{false};
  std::thread listenerThread_;

  // 持久化异步监听中使用的 WMS Client 与相关互斥锁
  std::mutex persistentClientMutex_;
  QmiClientWms *persistentClient_ = nullptr;
  std::mutex clientOperationMutex_;

  // 用于异步监听时记录已处理短信，防止重复通知
  std::mutex seenMutex_;
  std::unordered_set<int> seenMessages_; // 用 memoryIndex 标记

  // 内部同步读取接口（复用同步上下文实现）
  std::vector<CompleteSMS> performSyncRead();

  // 同步短信删除
  bool performMessageDelete(int memoryIndex);

  // 开始同步短信读取（前提：ctx->client 已就绪）
  void startSyncListMessages(MessageSyncContext *ctx);

  // 以下为各个异步回调函数，全部为静态成员函数，user_data 中传入 SyncContext*
  // 或其他上下文
  static void listMessagesReadyCallback(QmiClientWms *client, GAsyncResult *res,
                                        gpointer user_data);
  static void rawReadReadyCallback(QmiClientWms *client, GAsyncResult *res,
                                   gpointer user_data);
  static void releaseClientReadyCallback(QmiDevice *device, GAsyncResult *res,
                                         gpointer user_data);
  static void deleteMessageReadyCallback(QmiClientWms *client,
                                         GAsyncResult *res, gpointer user_data);

  // 用于释放客户端（异步调用封装，由同步接口调用）
  static void releaseClient(QmiClient *client, gpointer user_data);

  // 对所有短信进行后续处理（例如多段短信拼接等）
  static void processAllSMS(MessageSyncContext *ctx);

  // 对短信 PDU 解析，返回 SMSMessage（仅用于提取文本，此处可自定义实现）
  struct SMSMessage {
    std::string sender;
    std::string text;
  };

  // 异步监听线程主循环：定时调用同步读取，并将新短信通过 callback 传出
  void pollingLoop(std::chrono::seconds interval,
                   std::function<void(const CompleteSMS &)> callback);

  // 构造和释放 WMS Client 的同步封装
  QmiClientWms *createWmsClientSync();
  void releaseWmsClientSync(QmiClientWms *client);

  // 用于同步 client 分配的回调
  static void synchronousAllocateClientCallback(QmiDevice *device,
                                                GAsyncResult *res,
                                                gpointer user_data);
  // 用于同步释放 client 的回调
  static void synchronousReleaseClientCallback(QmiDevice *device,
                                               GAsyncResult *res,
                                               gpointer user_data);

  // 设备初始化和关闭
  bool initDevice();
  void closeDevice();

  // 处理队列中的下一条短信
  static void processNextSms(MessageSyncContext *ctx);
};

#endif // SMS_READER_HPP
