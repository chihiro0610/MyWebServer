#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>

#include "./locker/locker.h"
#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000

extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);

void addsig(int sig, void (handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    if(restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void show_error(int connfd, const char* info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char* argv[])
{
    if(argc <= 2)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi(argv[2]);

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd>=0);
    struct linger tmp = {0, 0};  // {1,0}=close时RST会丢弃未确认的响应数据，压测必挂；{0,0}=正常四次挥手，优雅关闭连接
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));  //设置监听socket SO_LINGER，accept返回的connfd会继承对监听socket设置的这个选项

    int ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    //创建线程池
    threadpool<http_conn>* pool = NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch(...)
    {
        return 1;
    }
    //预先为每个可能的客户分配一个http_conn对象
    http_conn* users = new http_conn[MAX_FD];
    assert(users);
    int user_count = 0;

    addsig(SIGPIPE, SIG_IGN); //忽略SIGPIPE

    while(true)
    {
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if((num<0) && (errno!=EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        for(int i=0; i<num; i++)
        {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd)
            {
                while(true)
                {
                    //listenfd也注册了EPOLLET，所以为了防止高并发场景下同时到达的客户连接丢失，这里循环accept直到EAGAIN
                    struct sockaddr_in client_addr;
                    socklen_t client_addrlength = sizeof(client_addr);
                    int connfd = accept(listenfd, (struct sockaddr*)&client_addr, &client_addrlength);
                    if(connfd < 0)
                    {
                        if(errno==EAGAIN || errno==EWOULDBLOCK)
                        {
                            break;
                        }
                        break;
                    }
                    if(http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        continue;
                    }
                    users[connfd].init(connfd, client_addr);
                }
            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //如果有异常，直接关闭客户连接
                users[sockfd].close_conn();
            }
            else if(events[i].events & EPOLLIN)
            {
                //根据读的结果，决定将任务添加到线程池还是关闭连接
                if(users[sockfd].read())
                {
                    pool->append(users+sockfd);
                }
                else
                {
                    users[sockfd].close_conn();
                }
            }
            else if(events[i].events & EPOLLOUT)
            {
                if(!users[sockfd].write())
                {
                    users[sockfd].close_conn();
                }
            }
            else {}
        }
    }

    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;
    return 0;
}