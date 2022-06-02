#include "worklogic.h"

int CWorkLogic::m_epollfd = -1;
int CWorkLogic::m_user_count = 0;

void setnonblocking(int fd)
{
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

void addfd_to_epoll(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    // EPOLLRDHUP可以检测对方连接断开, 监听读事件
    event.events = EPOLLIN | EPOLLRDHUP;
    // 注册EPOLLONESHOT事件, 以保证一个socket连接在任一时刻都只被一个线程处理
    // 每次该事件触发, 在当前socket处理完后, 还要及时重置该事件, 以保证下次还能触发
    if (one_shot)
    {
        event.events | EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);
}

void rmfd_from_epoll(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改epoll中文件描述符fd监听的事件为ev
void modfd_in_epoll(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    // 重置一下EPOLLONESHOT和EPOLLRDHUP事件
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void CWorkLogic::init(int sockfd, const sockaddr_in &addr)
{
    // 通信sockfd的信息传递进来
    m_sockfd = sockfd;
    m_address = addr;

    // 设置客户端连接进来的sockfd端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 把通信sockfd添加到epoll对象中
    addfd_to_epoll(m_epollfd, m_sockfd, true);
    m_user_count++; // 用户数+1
}

void CWorkLogic::close_conn()
{
    if (m_sockfd != -1)
    {
        rmfd_from_epoll(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
        printf("客户端连接已断开...\n");
    }
}

bool CWorkLogic::read()
{
    printf("读取数据...\n");
    return true;
}

bool CWorkLogic::write()
{
    printf("写入数据...\n");
    return true;
}

void CWorkLogic::process()
{
    // 处理读入的数据
    printf("working...\n");

    // 准备好即将发送的数据
}