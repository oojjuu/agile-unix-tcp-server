#pragma once

#include <atomic>
#include <chrono>
#include <ctime>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace agile {
namespace unix_tcp_server {

using ReportSteadyTime = std::chrono::steady_clock::time_point;
// 数据回调函数
using UnixTcpDataCallBack = std::function<void(std::shared_ptr<std::string>&&)>;

/**
 *@brief tcp server 类
 */
class UnixTcpServer
{
   public:
    static UnixTcpServer& GetInstance()
    {
        static UnixTcpServer instance;
        return instance;
    }
    virtual ~UnixTcpServer() { Stop(); }

    /**
     * @brief 启动server处理线程
     * @param sock_file sock文件
     * @param cb 数据回调函数
     * @return bool 是否成功
     */
    bool Start(const std::string& sock_file, const UnixTcpDataCallBack& cb);

    /**
     * @brief 停止server处理线程
     */
    void Stop();

   private:
    /**
     * @brief 线程执行函数
     */
    void Run();

    /**
     * @brief 接收socket连接
     * @param socket_id socket id
     * @param on 是否打开非阻塞
     * @return 返回码 0:成功 其它失败
     */
    int SetNoBlocking(int socket_id, bool on);

    /**
     * @brief 设置socket option参数
     * @param socket_id socket id
     */
    void SetOption(int socket_id);

    /**
     * @brief 接收socket连接
     */
    void DoAccept();

    /**
     * @brief 读socket数据
     * @param cur_time 当前时间
     */
    void DoRead(const ReportSteadyTime& cur_time);

    /**
     * @brief socket读数据结构体
     */
    struct ReadData
    {
        // header str
        std::string header;
        // socket消息数据
        std::shared_ptr<std::string> msg;
        // 最近一次读取数据时间点
        ReportSteadyTime last_read_time_;

        ReadData()
        {
            msg = std::make_shared<std::string>();
        }

        void Reset()
        {
            header.clear();
            msg.reset();
        }
    };

    /**
     * @brief 读取socket数据
     * @param read_len 可读的字节数
     * @param cur_time 当前时间
     * @param read_data 读取数据结构
     * @return bool false:关闭socket true:正常
     */
    bool OnRead(uint32_t read_len, const ReportSteadyTime& cur_time, std::shared_ptr<ReadData>& read_data);

    /**
     * @brief 关闭socket
     * @param socket_id socket_id
     * @param reason 关闭socket原因
     */
    void CloseSocket(int socket_id, const std::string& reason);

    /**
     * @brief 检查连接的有效性
     * @param cur_time 当前时间
     */
    void CheckConnValid(const ReportSteadyTime& cur_time);

   private:
    // 线程是否运行标识
    std::atomic<bool> running_;
    // socket run 线程
    std::thread server_thread_;
    // lock
    std::mutex server_mtx_;
    // sock 文件
    std::string sock_file_;
    // 数据回调函数
    UnixTcpDataCallBack callback_ = nullptr;
    // 监听socket id
    int listen_socket_ = 0;
    // 接收缓存大小
    static constexpr int kTcpReadBufferSize = 10240;
    // 接收缓存
    unsigned char tcp_read_buffer_[kTcpReadBufferSize];
    // 连接列表
    std::unordered_map<int, std::shared_ptr<ReadData> > conns_;
};

}  // namespace unix_tcp_server
}  // namespace agile