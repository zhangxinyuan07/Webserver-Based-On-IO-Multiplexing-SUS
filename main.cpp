#include "threadpool.h"
#include "worklogic.h"

#define MAX_FD 65535           // 最大文件描述符个数
#define MAX_EVENT_NUMBER 10000 // 监听的最大事件数量

// 添加文件描述符fd到epoll对象中
// 并根据one_shot注册EPOLLONESHOT事件
extern void addfd_to_epoll(int epollfd, int fd, bool one_shot);
// 从epoll对象中删除文件描述符fd
extern void rmfd_from_epoll(int epollfd, int fd);

// 添加信号捕捉
void addsig(int sig, void(handler)(int))
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

/*
之前用的是同步阻塞I/O(BIO)实现并发通信 :
服务端采用多线程，当 accept 一个请求后，开启线程进行 recv 和 send，可以完成并发处理
但随着请求数增加需要增加系统线程，大量的线程占用很大的内存空间，并且线程切换会带来很大的开销
10000个线程真正发生读写实际的线程数不会超过20 % ，每次accept都开一个线程也是一种资源浪费。

现在使用I/O多路复用实现并发通信, 使其能支持更多的并发连接请求 :
I/O 多路复用是一种同步I/O模型，实现一个线程可以监视多个文件句柄；
一旦某个文件句柄就绪，就能够通知应用程序进行相应的读写操作；
没有文件句柄就绪就会阻塞应用程序，交出CPU。
多路是指网络连接，复用指的是同一个线程

I/O多路复用有三种实现 : select, poll, epoll
这里用 epoll 实现 :
服务器采用单线程通过 epoll 系统调用获取 fd 列表，遍历有事件的 fd 进行 accept/recv/send
epoll可以理解为event poll，不同于忙轮询和无差别轮询
epoll会把哪个流发生了怎样的I/O事件通知我们
所以epoll实际上是 事件驱动（每个事件关联上fd) 的，此时我们对这些流的操作都是有意义的（复杂度降低到了O(1)）

epoll 有 EPOLLLT 和 EPOLLET 两种触发模式，LT 是默认的模式，ET 是 “高速” 模式。
LT 模式下，只要这个 fd 还有数据可读，每次 epoll_wait 都会返回它的事件，提醒用户程序去操作；
ET 模式下，它只会提示一次，直到下次再有数据流入之前都不会再提示了，无论 fd 中是否还有数据可读。
所以在 ET 模式下，read 一个 fd 的时候一定要把它的 buffer 读完，或者遇到 EAGIN 错误。
这里采用的是LT, 即水平触发模式来实现

注意 :
epoll是Linux目前大规模网络并发程序开发的首选模型。在绝大多数情况下性能远超select和poll。
目前流行的高性能web服务器Nginx正式依赖于epoll提供的高效网络套接字轮询服务。
但是，在并发连接不高的情况下，多线程 + 阻塞I/O方式可能性能更好。


服务器程序通常需要处理三类事件：I/O 事件、信号及定时事件。有两种高效的事件处理模式：Reactor和 Proactor
同步 I/O 模型通常用于实现 Reactor 模式，异步 I/O 模型通常用于实现 Proactor 模式。
Reactor模式 :
要求主线程（I/O处理单元）只负责监听文件描述符上是否有事件发生，有的话就立即将该事件通知工作
线程（逻辑单元），将 socket 可读可写事件放入请求队列，交给工作线程处理。除此之外，主线程不做
任何其他实质性的工作。读写数据，接受新的连接，以及处理客户请求均在工作线程中完成。
Proactor 模式:
将所有 I/O 操作都交给主线程和内核来处理（进行读、写），工作线程仅仅负责业务逻辑

这里使用同步 I/O 方式模拟出 Proactor 模式。
原理是：主线程执行数据读写操作，读写完成之后，主线程向工作线程通知这一”完成事件“。
那么从工作线程的角度来看，它们就直接获得了数据读写的结果，接下来要做的只是对读写的结果进行逻辑处理。

使用同步 I/O 模型（以 epoll_wait为例）模拟出的 Proactor 模式的工作流程如下：
1. 主线程往 epoll 内核事件表中注册 socket 上的读就绪事件。
2. 主线程调用 epoll_wait 等待 socket 上有数据可读。
3. 当 socket 上有数据可读时，epoll_wait 通知主线程。主线程从 socket 循环读取数据，直到没有更
多数据可读，然后将读取到的数据封装成一个请求对象并插入请求队列。
4. 睡眠在请求队列上的某个工作线程被唤醒，它获得请求对象并处理客户请求，然后往 epoll 内核事
件表中注册 socket 上的写就绪事件。
5. 主线程调用 epoll_wait 等待 socket 可写。
6. 当 socket 可写时，epoll_wait 通知主线程。主线程往 socket 上写入服务器处理客户请求的结果。

*/

int main(int argc, char *argv[])
{

    if (argc <= 1)
    {
        printf("按照如下格式运行 : %s prot_number\n", basename(argv[0]));
        exit(-1);
    }

    //获取端口号
    int port = atoi(argv[1]);

    // 对SIGPIE信号进行处理
    addsig(SIGPIPE, SIG_IGN);

    // 创建并初始化线程池
    CThreadPool<CWorkLogic> *pool = NULL;
    try
    {
        pool = new CThreadPool<CWorkLogic>;
    }
    catch (...)
    {
        exit(-1);
    }

    // 创建一个数组用于保存所有客户端信息
    CWorkLogic *users = new CWorkLogic[MAX_FD];

    // TCP通信流程: 创建, 复用(可选), 绑定, 监听
    // 创建监听套接字
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd == -1)
    {
        perror("socket");
        exit(-1);
    }

    // 设置端口复用, 要在绑定之前设置
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定监听套接字到本地端口
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    int ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    if (ret == -1)
    {
        perror("bind");
        close(listenfd);
        listenfd = 0;
        exit(-1);
    }

    // 监听
    int backlog = 128; // 客户端请求队列大小
    ret = listen(listenfd, backlog);
    if (ret == -1)
    {
        perror("listen");
        close(listenfd);
        listenfd = 0;
        exit(-1);
    }

    // 创建事件数组, 将来epoll_wait检测到发生改变的文件描述符(有事件发生)就会传入这里
    epoll_event events[MAX_EVENT_NUMBER];
    // 创建epoll对象, 直接在内核态创建, 没有从用户态到内核态的拷贝
    // 底层用一棵红黑树存放要遍历的fd
    // 相比select和poll效率大大提高
    int epollfd = epoll_create(5); // 括号里面的值只要不是0就行, 没影响

    // 将监听的文件描述符添加到epoll对象中
    // listenfd不需要注册EOPLLONESHOT事件
    addfd_to_epoll(epollfd, listenfd, false);
    CWorkLogic::m_epollfd = epollfd;

    // 循环检测epoll中的事件
    // 事件处理模式为 : Proactor模式
    //     - 将所有I/O操作都交给主线程和内核来处理，工作线程仅仅负责业务逻辑
    while (true)
    {
        // 监测epollfd上发生的事件, 返回值是有事件到达的文件描述符数量
        int fd_num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((fd_num < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        // 循环遍历事件数组
        for (int i = 0; i < fd_num; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd)
            {
                // 有客户的连接进来
                struct sockaddr_in client_addr;
                socklen_t client_addrlen = sizeof(client_addr);
                int connfd = accept(listenfd, (struct sockaddr *)&client_addr, &client_addrlen);
                if (connfd < 0)
                {
                    // 表明连接失败了
                    printf("errno is: %d\n", errno);
                    continue;
                }

                if (CWorkLogic::m_user_count >= MAX_FD)
                {
                    // 目前连接数满了
                    printf("客户端连接已满...\n");
                    close(connfd);
                    continue;
                }

                // 将新客户的数据初始化,并放入用户数组
                users[connfd].init(connfd, client_addr);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 对方异常断开或者发生错误则关闭连接
                users[sockfd].close_conn();
            }
            else if (events[i].events & EPOLLIN)
            {
                // 有读事件到达, 一次性读出所有数据
                if (users[sockfd].read())
                {
                    pool->append(users + sockfd);
                }
                else
                {
                    printf("读取数据失败...\n");
                    users[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                // 有写事件发生, 即有数据准备好了, 要向客户端发送
                if (!users[sockfd].write())
                {
                    printf("写入数据失败...\n");
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;

    return 0;
}