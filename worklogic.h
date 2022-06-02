#ifndef WORKLOGIC_H
#define WORKLOGIC_H

#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include "locker.h"

class CWorkLogic
{
public:
    static int m_epollfd;    // 所有socket上的事件都被注册到同一个epoll对象中
    static int m_user_count; // 统计用户数量

    CWorkLogic() {}

    ~CWorkLogic() {}

    // 处理客户端请求并响应的入口, 即工作逻辑的入口, 由线程池中的工作线程调用
    void process();                                
    void init(int sockfd, const sockaddr_in &addr); // 初始化新连接
    void close_conn();                              // 关闭连接
    bool read();                                    // 非阻塞读
    bool write();                                   // 非阻塞写

private:
    int m_sockfd;          // 当前连接的socket
    sockaddr_in m_address; // 连接的socket地址
};

#endif