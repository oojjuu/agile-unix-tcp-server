#include "report_server/tcp_server/UnixTcpServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <csignal>
#include <memory>

namespace agile {
namespace unix_tcp_server {

bool UnixTcpServer::Start(const std::string& sock_file, const UnixTcpDataCallBack& cb)
{
    std::lock_guard<std::mutex> lg(server_mtx_);
    if (running_.load())
    {
        std::cout << " server started already sock_file:" << sock_file_ << std::endl;
        return true;
    }
    sock_file_ = sock_file;
    callback_ = cb;
    unlink(sock_file_.c_str());

    listen_socket_ = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un server_sockaddr;
    server_sockaddr.sun_family = AF_UNIX;
    strcpy(server_sockaddr.sun_path, sock_file_.c_str());

    int ret = bind(listen_socket_, (struct sockaddr*)&server_sockaddr, sizeof(server_sockaddr));
    if (ret < 0)
    {
        std::cout << "fail to bind sock_file:" << sock_file_ << ", ret:" << ret
                                  << ", errno:" << (int)errno << ", err:" << strerror(errno) << std::endl;
        return false;
    }

    ret = SetNoBlocking(listen_socket_, true);
    if (0 != ret)
    {
        std::cout << "fail to SetNoBlocking ret:" << ret << ", sock_file:" << sock_file_
                                  << ", errno:" << (int)errno << ", err:" << strerror(errno) << std::endl;
        return false;
    }

    static constexpr int kMaxListenConns = 50;
    ret = listen(listen_socket_, kMaxListenConns);
    if (ret < 0)
    {
        std::cout << "fail to listen sock_file:" << sock_file_ << ", ret:" << ret
                                  << ", errno:" << (int)errno << ", err:" << strerror(errno) << std::endl;
        return false;
    }

    running_.store(true);
    server_thread_ = std::thread(&UnixTcpServer::Run, this);
    std::cout << " server started sock_file:" << sock_file_ << std::endl;
    return true;
}

void UnixTcpServer::Stop()
{
    std::lock_guard<std::mutex> lg(server_mtx_);

    if (!running_.load())
    {
        std::cout << " server stop already sock_file:" << sock_file_ << std::endl;
        return;
    }
    running_.store(false);

    if (server_thread_.joinable())
    {
        server_thread_.join();
    }
    conns_.clear();
    close(listen_socket_);

    std::cout << " server stop sock_file:" << sock_file_ << std::endl;
}

void UnixTcpServer::Run()
{
    static constexpr uint32_t kUsleepTimeVal = 5000;
    static constexpr uint32_t kCheckIntervalSec = 10;
    ReportSteadyTime trace_time = std::chrono::steady_clock::now();
    while (running_.load())
    {
        DoAccept();
        ReportSteadyTime cur_time = std::chrono::steady_clock::now();
        DoRead(cur_time);
        if (trace_time + std::chrono::seconds(kCheckIntervalSec) < cur_time)
        {
            trace_time = cur_time;
            CheckConnValid(cur_time);
        }
        usleep(kUsleepTimeVal);
    }
}

// 连接有效时间
static constexpr uint32_t kSocketConnInvalidTime = 60;

void UnixTcpServer::CheckConnValid(const ReportSteadyTime& cur_time)
{
    auto it = conns_.begin();
    while (it != conns_.end())
    {
        if (it->second->last_read_time_ + std::chrono::seconds(kSocketConnInvalidTime) < cur_time)
        {
            CloseSocket(it->first, "check time invalid");
            it = conns_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// unix socket server支持的最大连接数量
static constexpr uint32_t kUnixTcpServerMaxConns = 1000;

void UnixTcpServer::DoAccept()
{
    while (true)
    {
        struct sockaddr_in c_addr;
        socklen_t addr_len = sizeof(c_addr);
        int socket_id = accept(listen_socket_, (struct sockaddr*)&c_addr, &addr_len);
        if (socket_id > 0)
        {
            if (conns_.size() > kUnixTcpServerMaxConns)
            {
                CloseSocket(socket_id, "accept too many connections");
                continue;
            }
            SetOption(socket_id);
            SetNoBlocking(socket_id, true);
            conns_[socket_id] = std::make_shared<ReadData>();
            continue;
        }

        if (errno != EAGAIN && errno != EINTR)
        {
            std::cout << "fail to accept errno:" << (int)errno << ", err:" << strerror(errno) << std::endl;
        }
        return;
    }
}

bool UnixTcpServer::OnRead(uint32_t read_len, const ReportSteadyTime& cur_time, std::shared_ptr<ReadData>& read_data)
{
    static constexpr uint32_t kHeaderLen = sizeof(uint32_t);
    uint32_t read_index = 0;
    // 循环直到读取的字节数大于等于接收的字节数
    while (read_index < read_len)
    {
        // 可以消费的字节数
        uint32_t consume_size = read_len - read_index;

        uint32_t cur_len = read_data->header.length();

        uint32_t header_need_len = cur_len < kHeaderLen ? (kHeaderLen - cur_len) : 0;
        if (consume_size < header_need_len)
        {
            read_data->header.append((const char*)tcp_read_buffer_ + read_index, consume_size);
            return true;
        }
        else
        {
            if (0 < header_need_len)
            {
                read_data->header.append((const char*)tcp_read_buffer_ + read_index, header_need_len);
                read_index += header_need_len;
                consume_size -= header_need_len;
            }
        }
        uint32_t pk_len = 0;
        std::memcpy((char*)&pk_len, read_data->header.c_str(), sizeof(uint32_t));

        // 数据包的最大长度限制
        static constexpr uint32_t kSocketPacketSizeLimit = 10240;
        // 数据包长度过大主动断开连接
        if (kSocketPacketSizeLimit < pk_len)
        {
            std::cout << "socket_packet_size_limit < pk_len:" << pk_len  << std::endl;
            return false;
        }

        // ---------------处理包体数据开始--------------------------
        if (nullptr == read_data->msg)
        {
            read_data->msg = std::make_shared<std::string>();
        }
        cur_len = read_data->msg->length();

        size_t body_need_len = cur_len < pk_len ? (pk_len - cur_len) : 0;
        // 如果可消费的字节数不足以填充包体
        if (consume_size < body_need_len)
        {
            // 如果可消费的字节数不足以填充包体，先append当前可以消发的字节数，跳出循环等待下次数据
            read_data->msg->append((const char*)tcp_read_buffer_ + read_index, consume_size);
            return true;
        }
        else
        {
            // 如果包体仍需要数据
            if (body_need_len > 0)
            {
                // 填充包体需要的字节数
                read_data->msg->append((const char*)tcp_read_buffer_ + read_index, body_need_len);
                // 读数据下标往前移
                read_index += body_need_len;
            }
            // 已经获取到完整数据包
            if (callback_)
            {
                callback_(std::move(read_data->msg));
            }
            read_data->Reset();
        }
        // ---------------处理包体数据结束--------------------------
    }
    return true;
}

void UnixTcpServer::DoRead(const ReportSteadyTime& cur_time)
{
    auto it = conns_.begin();
    while (it != conns_.end())
    {
        int len = recv(it->first, tcp_read_buffer_, sizeof(tcp_read_buffer_), 0);
        if (len < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
            {
                ++it;
                continue;
            }
            CloseSocket(it->first, "recv len < 0");
            it = conns_.erase(it);
        }
        else if (0 == len)
        {
            CloseSocket(it->first, "recv len == 0");
            it = conns_.erase(it);
        }
        else
        {
            if (!OnRead(len, cur_time, it->second))
            {
                CloseSocket(it->first, "OnRead return false");
                it = conns_.erase(it);
            }
            else
            {
                it->second->last_read_time_ = cur_time;
                ++it;
            }
        }
    }
}

void UnixTcpServer::CloseSocket(int socket_id, const std::string& reason)
{
    close(socket_id);
    std::cout << "close socket:" << socket_id << ", reason:" << reason  << std::endl;
}

int UnixTcpServer::SetNoBlocking(int socket_id, bool on)
{
    int flags = fcntl(socket_id, F_GETFL, 0);
    if (-1 == flags)
    {
        return -1;
    }

    int b_val = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (-1 == fcntl(socket_id, F_SETFL, b_val))
    {
        return -2;
    }
    return 0;
}

void UnixTcpServer::SetOption(int socket_id)
{
    int optval = 1;
    int ret = setsockopt(socket_id, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
    if (ret != 0)
    {
        std::cout << "REUSEADDR errno:" << (int)errno << ", err:" << strerror(errno) << std::endl;
    }

    ret = setsockopt(socket_id, SOL_SOCKET, SO_REUSEPORT, (const char*)&optval, sizeof(int));
    if (ret != 0)
    {
        std::cout << "REUSEPORT errno:" << (int)errno << ", err:" << strerror(errno) << std::endl;
    }

    ret = setsockopt(socket_id, IPPROTO_TCP, TCP_NODELAY, (const char*)&optval, sizeof(int));
    if (ret != 0)
    {
        std::cout << "TCP_NODELAY errno:" << (int)errno << ", err:" << strerror(errno) << std::endl;
    }

    ret = setsockopt(socket_id, SOL_SOCKET, SO_KEEPALIVE, (const char*)&optval, sizeof(int));
    if (ret != 0)
    {
        std::cout << "KEEPALIVE errno:" << (int)errno << ", err:" << strerror(errno) << std::endl;
    }

    // 连接发送和接收缓存区大小
    static constexpr uint32_t kSocketSendRecvSize = 10240;
    size_t buffer_size = kSocketSendRecvSize;
    ret = setsockopt(socket_id, SOL_SOCKET, SO_SNDBUF, (const char*)&buffer_size, sizeof(size_t));
    if (ret != 0)
    {
        std::cout << "SNDBUF errno:" << (int)errno << ", err:" << strerror(errno) << std::endl;
    }

    ret = setsockopt(socket_id, SOL_SOCKET, SO_RCVBUF, (const char*)&buffer_size, sizeof(size_t));
    if (ret != 0)
    {
        std::cout << "RCVBUF errno:" << (int)errno << ", err:" << strerror(errno) << std::endl;
    }
}

}  // namespace unix_tcp_server
}  // namespace agile