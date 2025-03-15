#include "SmsReader.hpp"
#include "gio/gio.h"
#include "glibconfig.h"
#include "libqmi-glib.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

// =======================
// 设备初始化相关
// =======================
struct DeviceInitContext {
  GMainLoop *loop;
  QmiDevice *device;
  bool success;
};

static void openCallback(QmiDevice *dev, GAsyncResult *res,
                         gpointer user_data) {
  DeviceInitContext *ctx = static_cast<DeviceInitContext *>(user_data);
  g_autoptr(GError) error = nullptr;
  if (!qmi_device_open_finish(dev, res, &error)) {
    std::cerr << "无法打开设备: " << error->message << std::endl;
    ctx->success = false;
    g_main_loop_quit(ctx->loop);
    return;
  }
  ctx->device = dev;
  ctx->success = true;
  g_main_loop_quit(ctx->loop);
}

static void deviceNewCallback(GObject * /*source*/, GAsyncResult *res,
                              gpointer user_data) {
  DeviceInitContext *ctx = static_cast<DeviceInitContext *>(user_data);
  g_autoptr(GError) error = nullptr;
  QmiDevice *dev = qmi_device_new_finish(res, &error);
  if (!dev) {
    std::cerr << "无法创建 QmiDevice: " << error->message << std::endl;
    ctx->success = false;
    g_main_loop_quit(ctx->loop);
    return;
  }
  // 打开设备
  qmi_device_open(dev,
                  (QmiDeviceOpenFlags)(QMI_DEVICE_OPEN_FLAGS_PROXY |
                                       QMI_DEVICE_OPEN_FLAGS_AUTO),
                  10, /* timeout 秒 */
                  nullptr, (GAsyncReadyCallback)openCallback, ctx);
}

// 构造函数
QmiSmsReader::QmiSmsReader(const std::string &devicePath)
    : devicePath_(devicePath) {
  if (!initDevice()) {
    std::cerr << "设备初始化失败！" << std::endl;
    throw std::runtime_error("设备初始化失败");
  }
}

// 析构函数
QmiSmsReader::~QmiSmsReader() {
  stopListening();
  closeDevice();
}

bool QmiSmsReader::initDevice() {
  DeviceInitContext ctx;
  ctx.loop = g_main_loop_new(nullptr, FALSE);
  ctx.device = nullptr;
  ctx.success = false;

  g_autoptr(GFile) file = g_file_new_for_path(devicePath_.c_str());
  qmi_device_new(file, nullptr, deviceNewCallback, &ctx);

  g_main_loop_run(ctx.loop);
  g_main_loop_unref(ctx.loop);

  if (!ctx.success) {
    return false;
  }
  device_ = ctx.device;
  return true;
}

void closeCallback(QmiDevice *dev, GAsyncResult *res, gpointer user_data) {
  GMainLoop *loop = static_cast<GMainLoop *>(user_data);
  g_autoptr(GError) error = nullptr;
  if (!qmi_device_close_finish(dev, res, &error)) {
    std::cerr << "关闭设备失败: " << error->message << std::endl;
  }
  g_main_loop_quit(loop);
}

void QmiSmsReader::closeDevice() {
  if (device_) {
    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    qmi_device_close_async(device_, 10, nullptr,
                           (GAsyncReadyCallback)closeCallback, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    g_object_unref(device_);
    device_ = nullptr;
  }
}

// =======================
// 同步 WMS Client 创建／释放封装
// =======================
void QmiSmsReader::synchronousAllocateClientCallback(QmiDevice *device,
                                                     GAsyncResult *res,
                                                     gpointer user_data) {
  auto *ctx = static_cast<SynchronousClientContext *>(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(QmiClient) client =
      qmi_device_allocate_client_finish(device, res, &error);
  if (error && error->message &&
      strstr(error->message, "Transaction timed out")) {
    ctx->client = nullptr;
    ctx->success = false;
    g_main_loop_quit(ctx->loop);
    return;
  }
  if (!client) {
    std::cerr << "无法分配 WMS 客户端: "
              << (error ? error->message : "未知错误") << std::endl;
    ctx->client = nullptr;
    ctx->success = false;
    g_main_loop_quit(ctx->loop);
    return;
  }
  ctx->client = QMI_CLIENT_WMS(g_object_ref(client));
  ctx->success = true;
  g_main_loop_quit(ctx->loop);
}

QmiClientWms *QmiSmsReader::createWmsClientSync() {
  const int maxRetries = 3;
  int retries = 0;
  QmiClientWms *client = nullptr;
  while (retries < maxRetries) {
    SynchronousClientContext ctx;
    ctx.loop = g_main_loop_new(nullptr, FALSE);
    ctx.client = nullptr;
    ctx.success = false;
    qmi_device_allocate_client(
        device_, QMI_SERVICE_WMS, QMI_CID_NONE, 10, nullptr,
        (GAsyncReadyCallback)synchronousAllocateClientCallback, &ctx);
    g_main_loop_run(ctx.loop);
    g_main_loop_unref(ctx.loop);
    if (ctx.success && ctx.client != nullptr) {
      client = ctx.client;
      break;
    }
    ++retries;
  }
  return client;
}

void QmiSmsReader::synchronousReleaseClientCallback(QmiDevice *device,
                                                    GAsyncResult *res,
                                                    gpointer user_data) {
  auto *ctx = static_cast<ReleaseClientContext *>(user_data);
  g_autoptr(GError) error = nullptr;
  if (!qmi_device_release_client_finish(device, res, &error)) {
    std::cerr << "关闭客户端失败: " << (error ? error->message : "未知错误")
              << std::endl;
    ctx->success = false;
  } else {
    ctx->success = true;
  }
  g_main_loop_quit(ctx->loop);
}

void QmiSmsReader::releaseWmsClientSync(QmiClientWms *client) {
  ReleaseClientContext ctx;
  ctx.loop = g_main_loop_new(nullptr, FALSE);
  ctx.success = false;
  qmi_device_release_client(
      device_, QMI_CLIENT(client), QMI_DEVICE_RELEASE_CLIENT_FLAGS_NONE, 10,
      nullptr, (GAsyncReadyCallback)synchronousReleaseClientCallback, &ctx);
  g_main_loop_run(ctx.loop);
  g_main_loop_unref(ctx.loop);
}

// =======================
// 短信读取（同步）
// =======================
std::vector<CompleteSMS> QmiSmsReader::readAllMessages() {
  return performSyncRead();
}

// 创建列表消息回调
static void listCallback(QmiClientWms *client, GAsyncResult *res,
                         gpointer user_data) {
  auto *listCtx = static_cast<ListContext *>(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(QmiMessageWmsListMessagesOutput) output =
      qmi_client_wms_list_messages_finish(client, res, &error);

  if (!output ||
      !qmi_message_wms_list_messages_output_get_result(output, &error)) {
    std::cerr << "列出短信列表失败: " << error->message << std::endl;
    g_main_loop_quit(listCtx->loop);
    return;
  }

  GArray *message_list = nullptr;
  qmi_message_wms_list_messages_output_get_message_list(output, &message_list,
                                                        nullptr);
  if (message_list) {
    for (guint i = 0; i < message_list->len; i++) {
      auto *msg = &g_array_index(
          message_list, QmiMessageWmsListMessagesOutputMessageListElement, i);
      listCtx->messageIndices->push_back(msg->memory_index);
    }
    listCtx->success = true;
  }
  g_main_loop_quit(listCtx->loop);
};

std::vector<int> QmiSmsReader::listAllMessages(bool alreadyLocked) {
  // 只有在未持有锁时才获取锁
  std::unique_lock<std::mutex> opLock(clientOperationMutex_, std::defer_lock);
  if (!alreadyLocked) {
    opLock.lock();
  }

  std::vector<int> messageIndices;

  auto *ctx = new MessageSyncContext;
  ctx->loop = g_main_loop_new(nullptr, FALSE);
  ctx->device = device_;

  // 若已有持久 client，则复用；否则创建临时 client
  {
    std::unique_lock lock(persistentClientMutex_);
    if (persistentClient_) {
      ctx->client = persistentClient_;
      ctx->temporaryClient = false;
    }
  }

  if (ctx->client == nullptr) {
    ctx->client = createWmsClientSync();
    ctx->temporaryClient = true;
    if (!ctx->client) {
      std::cerr << "无法分配临时 WMS 客户端" << std::endl;
      g_main_loop_unref(ctx->loop);
      delete ctx;
      return messageIndices;
    }
  }

  g_autoptr(GError) error = nullptr;
  // 输入参数对象
  QmiMessageWmsListMessagesInput *input =
      qmi_message_wms_list_messages_input_new();

  // 设置存储类型
  if (!qmi_message_wms_list_messages_input_set_storage_type(
          input,
          QMI_WMS_STORAGE_TYPE_UIM, // SIM/UIM 卡
          &error)) {
    g_printerr("Error setting storage type: %s\n", error->message);
    qmi_message_wms_list_messages_input_unref(input);
    if (ctx->temporaryClient)
      releaseWmsClientSync(ctx->client);
    g_main_loop_unref(ctx->loop);
    delete ctx;
    return messageIndices;
  }

  // QMI_WMS_MESSAGE_MODE_GSM_WCDMA
  if (!qmi_message_wms_list_messages_input_set_message_mode(
          input, QMI_WMS_MESSAGE_MODE_GSM_WCDMA, &error)) {
    g_printerr("Error setting message mode: %s\n", error->message);
    qmi_message_wms_list_messages_input_unref(input);
    if (ctx->temporaryClient)
      releaseWmsClientSync(ctx->client);
    g_main_loop_unref(ctx->loop);
    delete ctx;
    return messageIndices;
  }

  // QMI_WMS_MESSAGE_TAG_TYPE_MT_READ
  if (!qmi_message_wms_list_messages_input_set_message_tag(
          input, QMI_WMS_MESSAGE_TAG_TYPE_MT_NOT_READ, &error)) {
    g_printerr("Error setting message tag: %s\n", error->message);
    qmi_message_wms_list_messages_input_unref(input);
    if (ctx->temporaryClient)
      releaseWmsClientSync(ctx->client);
    g_main_loop_unref(ctx->loop);
    delete ctx;
    return messageIndices;
  }

  // 创建单独的GMainLoop用于同步等待列表请求完成
  GMainLoop *listLoop = g_main_loop_new(nullptr, FALSE);

  ListContext listCtx;
  listCtx.loop = listLoop;
  listCtx.messageIndices = &messageIndices;
  listCtx.success = false;

  // 发起列表消息请求
  qmi_client_wms_list_messages(QMI_CLIENT_WMS(ctx->client), input, 10, nullptr,
                               (GAsyncReadyCallback)listCallback, &listCtx);

  qmi_message_wms_list_messages_input_unref(input);

  // 等待列表请求完成
  g_main_loop_run(listLoop);
  g_main_loop_unref(listLoop);

  if (ctx->temporaryClient)
    releaseWmsClientSync(ctx->client);

  g_main_loop_unref(ctx->loop);
  delete ctx;

  return messageIndices;
}

std::vector<CompleteSMS> QmiSmsReader::performSyncRead() {
  // 序列化对 client 的操作
  std::unique_lock opLock(clientOperationMutex_);
  auto *ctx = new MessageSyncContext;
  ctx->loop = g_main_loop_new(nullptr, FALSE);
  ctx->device = device_;

  // 若已有持久 client，则复用；否则创建临时 client
  {
    std::unique_lock lock(persistentClientMutex_);
    if (persistentClient_) {
      ctx->client = persistentClient_;
      ctx->temporaryClient = false;
    }
  }

  if (ctx->client == nullptr) {
    ctx->client = createWmsClientSync();
    ctx->temporaryClient = true;
    if (!ctx->client) {
      std::cerr << "无法分配临时 WMS 客户端" << std::endl;
      g_main_loop_unref(ctx->loop);
      delete ctx;
      return std::vector<CompleteSMS>();
    }
  }

  // 先获取所有短信索引
  std::vector<int> messageIndices;
  {
    // 临时释放当前锁，以便让 listAllMessages 可以获取锁
    opLock.unlock();
    messageIndices = listAllMessages();
    opLock.lock();
  }

  if (messageIndices.empty()) {
    if (ctx->temporaryClient)
      releaseWmsClientSync(ctx->client);
    g_main_loop_unref(ctx->loop);
    delete ctx;
    return std::vector<CompleteSMS>();
  }

  // 设置总数量，用于跟踪处理进度
  ctx->totalSMSCount = messageIndices.size();

  // 将所有短信索引添加到待处理队列
  for (int memoryIndex : messageIndices) {
    ctx->pendingSmsIndices.push(memoryIndex);
  }

  // 开始处理第一条短信
  processNextSms(ctx);

  // 等待所有短信读取完成
  g_main_loop_run(ctx->loop);

  // 处理所有短信（例如多段短信拼接）
  processAllSMS(ctx);

  if (ctx->temporaryClient)
    releaseWmsClientSync(ctx->client);

  std::vector<CompleteSMS> result = std::move(ctx->completeSMSList);
  g_main_loop_unref(ctx->loop);
  delete ctx;
  return result;
}

// 处理队列中的下一条短信
void QmiSmsReader::processNextSms(MessageSyncContext *ctx) {
  // 如果队列为空，表示所有短信已处理完毕
  if (ctx->pendingSmsIndices.empty()) {
    return;
  }

  // 取出队列中的下一个短信索引
  int memoryIndex = ctx->pendingSmsIndices.front();
  ctx->pendingSmsIndices.pop();

  QmiMessageWmsRawReadInput *read_input = qmi_message_wms_raw_read_input_new();
  g_autoptr(GError) error = nullptr;

  if (!qmi_message_wms_raw_read_input_set_message_mode(
          read_input, QMI_WMS_MESSAGE_MODE_GSM_WCDMA, &error)) {
    std::cerr << "设置短信模式失败: " << error->message << std::endl;
    qmi_message_wms_raw_read_input_unref(read_input);
    // 处理下一条短信
    processNextSms(ctx);
    return;
  }

  if (!qmi_message_wms_raw_read_input_set_message_memory_storage_id(
          read_input, QMI_WMS_STORAGE_TYPE_UIM, memoryIndex, &error)) {
    std::cerr << "设置短信存储ID失败: " << error->message << std::endl;
    qmi_message_wms_raw_read_input_unref(read_input);
    // 处理下一条短信
    processNextSms(ctx);
    return;
  }

  qmi_message_wms_raw_read_input_ref(read_input);
  auto *data = new RawReadUserData{ctx, memoryIndex, read_input};

  qmi_client_wms_raw_read(QMI_CLIENT_WMS(ctx->client), read_input, 10, nullptr,
                          (GAsyncReadyCallback)rawReadReadyCallback, data);

  qmi_message_wms_raw_read_input_unref(read_input);
}

void QmiSmsReader::rawReadReadyCallback(QmiClientWms *client, GAsyncResult *res,
                                        gpointer user_data) {
  auto *data = static_cast<RawReadUserData *>(user_data);
  auto *ctx = data->ctx;
  int mem_index = data->memoryIndex;
  // 取出 read_input 后删除 data
  QmiMessageWmsRawReadInput *read_input = data->read_input;
  delete data;

  g_autoptr(GError) error = nullptr;
  g_autoptr(QmiMessageWmsRawReadOutput) output =
      qmi_client_wms_raw_read_finish(client, res, &error);

  // 超时则不释放 read_input
  if (error && strstr(error->message, "Transaction timed out")) {
    std::cout << "读取短信超时，重试中..." << std::endl;
    auto *retryData = new RawReadUserData{ctx, mem_index, read_input};
    qmi_client_wms_raw_read(client, read_input, 10, nullptr,
                            (GAsyncReadyCallback)rawReadReadyCallback,
                            retryData);
    return;
  }

  if (!output) {
    std::cerr << "读取短信内容（索引 " << mem_index
              << "）失败: " << (error ? error->message : "未知错误")
              << std::endl;
    ctx->processedSMSCount++;
    qmi_message_wms_raw_read_input_unref(read_input);
  } else {
    GArray *raw_data = nullptr;
    QmiWmsMessageTagType msg_tag;
    QmiWmsMessageFormat msg_format;
    if (!qmi_message_wms_raw_read_output_get_raw_message_data(
            output, &msg_tag, &msg_format, &raw_data, &error)) {
      std::cerr << "获取短信原始数据（索引 " << mem_index
                << "）失败: " << error->message << std::endl;
      ctx->processedSMSCount++;
    } else if (raw_data && raw_data->len > 0) {
      std::ostringstream oss;
      for (guint i = 0; i < raw_data->len; i++) {
        char buf[3];
        sprintf(buf, "%02X", ((guint8 *)raw_data->data)[i]);
        oss << buf;
      }
      SMSPart part;
      part.memoryIndex = mem_index;
      part.hexPDU = oss.str();
      part.rawData.resize(raw_data->len);
      memcpy(part.rawData.data(), raw_data->data, raw_data->len);
      ctx->rawSMSMap[mem_index] = part;
      ctx->processedSMSCount++;
    } else {
      std::cout << "短信索引 " << mem_index << " 无内容或读取为空。"
                << std::endl;
      ctx->processedSMSCount++;
    }
  }

  qmi_message_wms_raw_read_input_unref(read_input);

  // 如果所有短信都已处理完，退出主循环
  if (ctx->processedSMSCount >= ctx->totalSMSCount) {
    g_main_loop_quit(ctx->loop);
  } else {
    // 否则处理下一条短信
    processNextSms(ctx);
  }
}

void QmiSmsReader::startSyncListMessages(MessageSyncContext *ctx) {
  // 获取短信索引列表 - 告知函数已持有锁
  std::vector<int> messageIndices = listAllMessages(true);

  if (messageIndices.empty()) {
    // 没有短信，直接退出循环
    g_main_loop_quit(ctx->loop);
    return;
  }

  // 设置总数量，用于跟踪处理进度
  ctx->totalSMSCount = messageIndices.size();

  // 将所有短信索引添加到待处理队列
  for (int memoryIndex : messageIndices) {
    ctx->pendingSmsIndices.push(memoryIndex);
  }

  // 开始处理第一条短信
  processNextSms(ctx);
}

// =======================
// 短信删除（同步）
// =======================
bool QmiSmsReader::deleteMessage(int memoryIndex) {
  std::unique_lock lock(seenMutex_);
  if (!performMessageDelete(memoryIndex)) {
    return false;
  }
  return seenMessages_.erase(memoryIndex);
}

bool QmiSmsReader::performMessageDelete(int memoryIndex) {
  std::unique_lock opLock(clientOperationMutex_);
  auto *ctx = new DeleteSMSContext;
  ctx->loop = g_main_loop_new(nullptr, FALSE);
  ctx->device = device_;
  {
    std::unique_lock lock(persistentClientMutex_);
    if (persistentClient_) {
      ctx->client = persistentClient_;
      ctx->temporaryClient = false;
    }
  }
  if (ctx->client == nullptr) {
    ctx->client = createWmsClientSync();
    ctx->temporaryClient = true;
    if (!ctx->client) {
      std::cerr << "无法分配临时 WMS 客户端" << std::endl;
      g_main_loop_quit(ctx->loop);
      delete ctx;
      return false;
    }
  }
  QmiMessageWmsDeleteInput *input = qmi_message_wms_delete_input_new();
  g_autoptr(GError) error = nullptr;
  if (!qmi_message_wms_delete_input_set_memory_storage(
          input, QMI_WMS_STORAGE_TYPE_UIM, &error)) {
    std::cerr << "设置删除短信存储位置失败: " << error->message << std::endl;
    qmi_message_wms_delete_input_unref(input);
    if (ctx->temporaryClient)
      releaseWmsClientSync(ctx->client);
    g_main_loop_quit(ctx->loop);
    delete ctx;
    return false;
  }
  if (!qmi_message_wms_delete_input_set_memory_index(input, memoryIndex,
                                                     &error)) {
    std::cerr << "设置删除短信 index 失败: " << error->message << std::endl;
    qmi_message_wms_delete_input_unref(input);
    if (ctx->temporaryClient)
      releaseWmsClientSync(ctx->client);
    g_main_loop_quit(ctx->loop);
    delete ctx;
    return false;
  }
  if (!qmi_message_wms_delete_input_set_message_mode(
          input, QMI_WMS_MESSAGE_MODE_GSM_WCDMA, &error)) {
    std::cerr << "设置删除短信模式失败: " << error->message << std::endl;
    qmi_message_wms_delete_input_unref(input);
    if (ctx->temporaryClient)
      releaseWmsClientSync(ctx->client);
    g_main_loop_quit(ctx->loop);
    delete ctx;
    return false;
  }
  qmi_client_wms_delete(QMI_CLIENT_WMS(ctx->client), input, 10, nullptr,
                        (GAsyncReadyCallback)deleteMessageReadyCallback, ctx);
  qmi_message_wms_delete_input_unref(input);
  g_main_loop_run(ctx->loop);
  if (ctx->temporaryClient)
    releaseWmsClientSync(ctx->client);
  bool success = ctx->promise.get_future().get();
  delete ctx;
  return success;
}

// =======================
// 异步回调函数
// =======================
void QmiSmsReader::listMessagesReadyCallback(QmiClientWms *client,
                                             GAsyncResult *res,
                                             gpointer user_data) {
  auto *ctx = static_cast<MessageSyncContext *>(user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(QmiMessageWmsListMessagesOutput) output =
      qmi_client_wms_list_messages_finish(client, res, &error);
  if (!output ||
      !qmi_message_wms_list_messages_output_get_result(output, &error)) {
    std::cerr << "列出短信列表失败: " << error->message << std::endl;
    if (ctx->temporaryClient)
      releaseClient(QMI_CLIENT(client), ctx);
    g_main_loop_quit(ctx->loop);
    return;
  }
  GArray *message_list = nullptr;
  qmi_message_wms_list_messages_output_get_message_list(output, &message_list,
                                                        nullptr);
  if (message_list) {
    ctx->totalSMSCount = message_list->len;
    for (guint i = 0; i < message_list->len; i++) {
      auto *msg = &g_array_index(
          message_list, QmiMessageWmsListMessagesOutputMessageListElement, i);
      QmiMessageWmsRawReadInput *read_input =
          qmi_message_wms_raw_read_input_new();
      if (!qmi_message_wms_raw_read_input_set_message_mode(
              read_input, QMI_WMS_MESSAGE_MODE_GSM_WCDMA, &error)) {
        std::cerr << "设置短信模式失败: " << error->message << std::endl;
        qmi_message_wms_raw_read_input_unref(read_input);
        continue;
      }
      if (!qmi_message_wms_raw_read_input_set_message_memory_storage_id(
              read_input, QMI_WMS_STORAGE_TYPE_UIM, msg->memory_index,
              &error)) {
        std::cerr << "设置短信存储ID失败: " << error->message << std::endl;
        qmi_message_wms_raw_read_input_unref(read_input);
        continue;
      }
      qmi_message_wms_raw_read_input_ref(read_input);
      auto *data = new RawReadUserData{ctx, static_cast<int>(msg->memory_index),
                                       read_input};
      qmi_client_wms_raw_read(QMI_CLIENT_WMS(ctx->client), read_input, 10,
                              nullptr,
                              (GAsyncReadyCallback)rawReadReadyCallback, data);
      qmi_message_wms_raw_read_input_unref(read_input);
    }
    while (ctx->processedSMSCount < ctx->totalSMSCount) {
      g_main_context_iteration(nullptr, TRUE);
    }
    // 处理所有短信（例如多段短信拼接）
    processAllSMS(ctx);
    if (ctx->temporaryClient)
      releaseClient(QMI_CLIENT(client), ctx);
    g_main_loop_quit(ctx->loop);
  } else {
    std::cout << "未找到短信。" << std::endl;
    if (ctx->temporaryClient)
      releaseClient(QMI_CLIENT(client), ctx);
    g_main_loop_quit(ctx->loop);
  }
}

void QmiSmsReader::deleteMessageReadyCallback(QmiClientWms *client,
                                              GAsyncResult *res,
                                              gpointer user_data) {
  auto *ctx = static_cast<DeleteSMSContext *>(user_data);
  g_autoptr(GError) error = nullptr;
  if (!qmi_client_wms_delete_finish(client, res, &error)) {
    std::cerr << "删除短信失败: " << error->message << std::endl;
    ctx->promise.set_value(false);
  } else {
    ctx->promise.set_value(true);
  }
  g_main_loop_quit(ctx->loop);
}

void QmiSmsReader::releaseClient(QmiClient *client, gpointer user_data) {
  auto *ctx = static_cast<MessageSyncContext *>(user_data);
  qmi_device_release_client(
      ctx->device, client, QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID, 10,
      nullptr, (GAsyncReadyCallback)releaseClientReadyCallback, ctx);
}

void QmiSmsReader::releaseClientReadyCallback(QmiDevice *device,
                                              GAsyncResult *res,
                                              gpointer user_data) {
  auto *ctx = static_cast<MessageSyncContext *>(user_data);
  g_autoptr(GError) error = nullptr;
  if (!qmi_device_release_client_finish(device, res, &error)) {
    std::cerr << "释放 WMS client 失败: " << error->message << std::endl;
  }
  // 此处不调用 g_main_loop_quit(ctx->loop)，由上层逻辑统一 quit
}

// =======================
// 处理短信
// =======================
void QmiSmsReader::processAllSMS(MessageSyncContext *ctx) {
  std::vector<CompleteSMS> completeSMSList;
  // 用于分段短信拼接的 map，key 为分段短信的参考号
  std::unordered_map<int, std::vector<SMSPart>> multipartGroups;

  // 遍历所有读取到的短信原始数据
  for (const auto &kv : ctx->rawSMSMap) {
    int mem_index = kv.first;
    const SMSPart &rawPart = kv.second;
    // 使用 PDUlib 封装的 PDU 类进行解析
    PDU pdu;
    if (!pdu.decodePDU(rawPart.hexPDU.c_str())) {
      std::cerr << "PDU解析失败，索引 " << mem_index << std::endl;
      continue;
    }
    const char *text = pdu.getText();
    const char *sender = pdu.getSender();
    const char *timestamp = pdu.getTimeStamp();
    const int *concatInfo = pdu.getConcatInfo();
    // 如果存在分段信息（当前分段号大于0且总分段数大于1）
    if (concatInfo && concatInfo[1] > 0 && concatInfo[2] > 1) {
      int ref = concatInfo[0];
      SMSPart part;
      part.memoryIndex = mem_index;
      part.partNumber = concatInfo[1];
      part.text = text;
      part.sender = sender;
      part.timestamp = timestamp;
      part.hexPDU = rawPart.hexPDU;
      part.rawData = rawPart.rawData;
      multipartGroups[ref].push_back(part);
    } else {
      // 单条短信
      SMSPart part;
      part.memoryIndex = mem_index;
      part.partNumber = 1;
      // 对于单条短信，使用 pdu.getText()（原代码中单条短信直接采用 getText()
      // 的结果）
      part.text = pdu.getText();
      part.sender = sender;
      part.timestamp = timestamp;
      part.hexPDU = rawPart.hexPDU;
      part.rawData = rawPart.rawData;
      CompleteSMS csms;
      csms.sender = sender;
      csms.timestamp = timestamp;
      csms.fullText = part.text;
      csms.parts.push_back(part);
      completeSMSList.push_back(csms);
    }
  }
  // 对所有分段短信进行拼接：同一参考号下的各分段先按 partNumber
  // 升序排序，然后依次拼接文本
  for (auto &groupEntry : multipartGroups) {
    int ref = groupEntry.first;
    auto &parts = groupEntry.second;
    std::sort(parts.begin(), parts.end(),
              [](const SMSPart &a, const SMSPart &b) {
                return a.partNumber < b.partNumber;
              });
    CompleteSMS csms;
    csms.sender = parts.front().sender;
    csms.timestamp = parts.front().timestamp;
    for (const auto &p : parts) {
      csms.fullText += p.text;
      csms.parts.push_back(p);
    }
    completeSMSList.push_back(csms);
  }
  ctx->completeSMSList = std::move(completeSMSList);
  g_main_loop_quit(ctx->loop);
}

// =======================
// 短信监听（异步）
// =======================
void QmiSmsReader::startListening(
    std::chrono::seconds interval,
    std::function<void(const CompleteSMS &)> callback) {
  std::unique_lock lock(persistentClientMutex_);
  if (/* 正在监听 */ persistentClient_ != nullptr && interval.count() <= 0) {
    return;
  }
  // 标记为正在监听
  // 创建持久 client（同步方式）
  persistentClient_ = createWmsClientSync();
  if (!persistentClient_) {
    throw std::runtime_error("无法分配持久化 WMS 客户端");
  }
  listening_ = true;
  // 启动监听线程
  listenerThread_ =
      std::thread(&QmiSmsReader::pollingLoop, this, interval, callback);
}

void QmiSmsReader::stopListening() {
  // 停止轮询线程
  listening_ = false;
  if (listenerThread_.joinable())
    listenerThread_.join();
  // 释放持久 client
  std::unique_lock lock(persistentClientMutex_);
  if (persistentClient_) {
    releaseWmsClientSync(persistentClient_);
    persistentClient_ = nullptr;
  }
}

void QmiSmsReader::pollingLoop(
    std::chrono::seconds interval,
    std::function<void(const CompleteSMS &)> callback) {
  while (listening_) {
    std::vector<CompleteSMS> newMessages;
    {
      std::unique_lock opLock(clientOperationMutex_);
      MessageSyncContext ctx;
      ctx.loop = g_main_loop_new(nullptr, FALSE);
      ctx.device = device_;
      {
        std::unique_lock lock(persistentClientMutex_);
        ctx.client = persistentClient_;
        ctx.temporaryClient = false;
      }

      // 启动短信读取过程
      startSyncListMessages(&ctx);

      // 等待所有短信读取完成
      while (ctx.processedSMSCount < ctx.totalSMSCount) {
        g_main_context_iteration(nullptr, TRUE);
      }

      // 处理所有短信（例如多段短信拼接）
      processAllSMS(&ctx);

      // 查找新短信并存储到临时列表（在持有锁的情况下）
      for (const auto &sms : ctx.completeSMSList) {
        std::unique_lock lock(seenMutex_);
        if (seenMessages_.find(sms.parts.front().memoryIndex) ==
            seenMessages_.end()) {
          seenMessages_.insert(sms.parts.front().memoryIndex);
          newMessages.push_back(sms);  // 存储到临时列表
        }
      }
      g_main_loop_unref(ctx.loop);
    } // 在这里释放 clientOperationMutex_

    // 在释放锁之后处理新消息
    for (const auto &sms : newMessages) {
      callback(sms);  // 现在调用回调是安全的，因为已经释放了锁
    }

    std::this_thread::sleep_for(interval);
  }
}
