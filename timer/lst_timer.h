#ifndef LST_TIMER
#define LST_TIMER

#include <time.h>
#include <netinet/in.h>
#include <stdio.h>
#define BUF_SIZE 64
class util_timer;  //前向声明

//用户数据结构：客户端socket地址、socketfd、读缓存和定时器
struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer* timer;
};

//定时器类
class util_timer
{
    public:
        util_timer() : prev(NULL), next(NULL){}
        time_t expire; //任务超时时间，这里使用绝对时间
        void (*cb_func) (client_data*); //任务回调函数
        //回调函数处理的客户数据，由定时器的执行者传递给回调函数
        client_data* user_data;
        util_timer* prev;
        util_timer* next;  
};

//定时器链表，一个升序、双向链表，带有头节点和尾节点
class sort_timer_lst
{
    public:
        sort_timer_lst() : head(NULL), tail(NULL) {}

        ~sort_timer_lst()
        {
            //销毁链表时，删除其中所有定时器
            util_timer* tmp = head;
            while(tmp)
            {
                head = tmp->next;
                delete tmp;
                tmp = head;
            }
        }

        void add_timer(util_timer* timer)
        {
            if(!timer)
            {
                return;
            }
            if(!head)
            {
                head = tail = timer;
                return;
            }
            /*如果目标定时器的超时时间小于当前链表所有定时器的超时时间，
            则把该定时器插入链表头部，作为链表新的头部节点，否则调用add_timer重载函数，
            把它插入到链表中合适的位置，保证链表的升序特性*/
            if(timer->expire<head->expire)
            {
                timer->next = head;
                head->prev = timer;
                head = timer;
                return;
            }
            add_timer(timer, head);
        }

        void adjust_timer(util_timer* timer)
        {
            /*当某个定时任务发生变化时，调整对应的定时器在链表中的位置，
            这个函数只考虑被调整定时器的超时时间延长的情况，即该定时器需要往链表尾部移动*/
            if(!timer)
            {
                return;
            }
            util_timer* tmp = timer->next;
            if(!tmp||(timer->expire<tmp->expire))
            {
                return;
            }
            //如果目标定时器三链表的头节点，则将该定时器从链表中取出并重新插入
            if(timer==head)
            {
                head = head->next;
                head->prev = NULL;
                timer->next = NULL;
                add_timer(timer, head);
            }
            //如果不是链表的头节点，则将该定时器从链表中取出，插入原来位置之后的部分链表中
            else
            {
                timer->prev->next = timer->next;
                timer->next->prev = timer->prev;
                add_timer(timer, timer->next);
            }
        }

        void del_timer(util_timer* timer)
        {
            if(!timer)
            {
                return;
            }
            if((timer==head)&&(timer==tail))
            {
                //表示链表只有一个定时器
                delete timer;
                head = NULL;
                tail = NULL;
                return;
            }
            if(timer == head)
            {
                head = head->next;
                head->prev = NULL;
                delete timer;
                return;
            }
            if(timer == tail)
            {
                tail = tail->prev;
                tail->next = NULL;
                delete timer;
                return;
            }
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            delete timer;
        }

        /*SIGALARM信号每次被触发就在其信号处理函数（或统一事件源的主函数）中
        执行一次tick，以处理链表上到期的任务*/
        void tick()
        {
            if(!head)
            {
                return;
            }
            printf("timer tick\n");
            time_t cur = time(NULL); //获得系统当前时间
            util_timer* tmp = head;
            //从头节点开始依次处理每个定时器，直到遇到一个尚未到期的定时器
            while(tmp)
            {
                if(cur<tmp->expire)
                {
                    break;
                }
                tmp->cb_func(tmp->user_data);
                head = tmp->next;
                if(head)
                {
                    head->prev=NULL;
                }
                delete tmp;
                tmp = head;
            }
        }
    
    private:
        util_timer* head;
        util_timer* tail;

        /*一个重载的辅助函数，将timer添加到lst_head之后的链表部分中*/
        void add_timer(util_timer* timer, util_timer* lst_head)
        {
            util_timer* prev = lst_head;
            util_timer* tmp = prev->next;
            while(tmp)
            {
                if(timer->expire < tmp->expire)
                {
                    prev->next = timer;
                    timer->next = tmp;
                    tmp->prev = timer;
                    timer->prev = prev;
                    break;
                }
                prev = tmp;
                tmp = tmp->next;
            }
            if(!tmp)
            {
                prev->next = timer;
                timer->prev = prev;
                timer->next = NULL;
                tail = timer;
            }
        }
};

#endif