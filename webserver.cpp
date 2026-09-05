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
#include "./timer/lst_timer.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5

extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);
extern int setnonblocking(int fd);

//定时器相关变量
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

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

void cb_func(client_data *user_data) //定时器回调函数，删除非活动连接的注册事件，并关闭fd
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}

void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
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
    epollfd = epoll_create(5);
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

    //创建sig管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    addsig(SIGPIPE, SIG_IGN); //忽略SIGPIPE
    addsig(SIGALRM, sig_handler, false);  //restart=false
    addsig(SIGINT, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);

    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD];
    alarm(TIMESLOT);
    bool timeout = false;

    while(!stop_server)
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
                    users_timer[connfd].sockfd = connfd;
                    users_timer[connfd].address = client_addr;
                    //创建定时器，设置其回调函数与超时时间，然后将定时器和users_timer绑定，最后将定时器添加到timer容器中
                    util_timer* timer = new util_timer;
                    timer->cb_func = cb_func;
                    timer->user_data = &users_timer[connfd];
                    time_t cur = time(NULL);
                    timer->expire = cur+3*TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //如果有异常，直接关闭客户连接,并移除定时器
                util_timer* timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);
                if(timer)
                {
                    timer_lst.del_timer(timer);
                }
            }
            else if((sockfd==pipefd[0]) && (events[i].events&EPOLLIN))
            {
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret<=0)
                {
                    continue;
                }
                else
                {
                    for(int i=0;i<ret;i++)
                    {
                        switch(signals[i])
                        {
                            case SIGALRM:
                            {
                                //SIGALRM放到最后处理，优先处理别的事件
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                stop_server = true;
                                break;
                            }
                        }
                    }
                }
            }
            else if(events[i].events & EPOLLIN)
            {
                //根据读的结果，决定将任务添加到线程池还是关闭连接
                util_timer* timer = users_timer[sockfd].timer;
                if(users[sockfd].read())
                {
                    pool->append(users+sockfd);
                    //如果有数据读写，将定时器调整延后3个TIMESLOT
                    if(timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3*TIMESLOT;
                        printf("adjust timer once\n");
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    timer_lst.del_timer(timer);
                }
            }
            else if(events[i].events & EPOLLOUT)
            {
                util_timer* timer = users_timer[sockfd].timer;
                if(!users[sockfd].write())
                {
                    timer->cb_func(&users_timer[sockfd]);
                    timer_lst.del_timer(timer);
                }
                else
                {
                    //如果有数据读写，将定时器调整延后3个TIMESLOT
                    if(timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3*TIMESLOT;
                        printf("adjust timer once\n");
                        timer_lst.adjust_timer(timer);
                    }
                }
            }

            if(timeout)
            {
                timer_handler();
                timeout = false;
            }
        }
    }

    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    delete [] users_timer;
    delete pool;
    return 0;
}