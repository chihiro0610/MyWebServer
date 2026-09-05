#ifndef TIME_WHEEL_TIMER
#define TIME_WHEEL_TIMER

#include <time.h>
#include <netinet/in.h>
#include <stdio.h>

#define BUF_SIZE 64

class tw_timer;

struct client_data
{
    sockaddr_in address;
    int sockfd;
    char buf[BUF_SIZE];
    tw_timer* timer;
};

class tw_timer
{
    public:
        tw_timer(int rot, int ts) : next(NULL), prev(NULL), rotation(rot), time_slot(ts) {}
        int rotation; //记录定时器在时间轮转多少圈后生效
        int time_slot; //记录定时器属于时间轮上的哪个槽
        void (*cb_func) (client_data*);
        client_data* user_data;
        tw_timer* next;
        tw_timer* prev;
};

class time_wheel
{
    public:
        time_wheel() : cur_slot(0)
        {
            for(int i=0; i<N; i++)
            {
                slots[i] = NULL;
            }
        }
        ~time_wheel()
        {
            for(int i=0; i<N; i++)
            {
                tw_timer* tmp = slots[i];
                while(tmp)
                {
                    slots[i] = tmp->next;
                    delete tmp;
                    tmp = slots[i];
                }
            }
        }
        tw_timer* addtimer(int timeout)
        {
            if(timeout<0)
            {
                return NULL;
            }
            int ticks = 0;
            if(timeout<SI)
            {
                ticks = 1;
            }
            else
            {
                ticks = timeout/SI;
            }
            int rotation = ticks/N;
            int ts = (cur_slot+(ticks%N))%N;
            tw_timer* timer = new tw_timer(rotation, ts);
            printf("add timer, rotation is %d, ts is %d, cur_slot is %d\n",
                rotation, ts, cur_slot);
            if(!slots[ts])
            {
                slots[ts] = timer;
            }
            else
            {
                timer->next = slots[ts];
                slots[ts]->prev = timer;
                slots[ts] = timer;
            }
            return timer;
        }
        void del_timer(tw_timer* timer)
        {
            if(!timer)
            {
                return;
            }
            int ts = timer->time_slot;
            if(timer == slots[ts])
            {
                slots[ts] = slots[ts]->next;
                if(slots[ts])
                {
                    slots[ts]->prev = NULL;
                }
                delete timer;
            }
            else
            {
                timer->prev->next = timer->next;
                if(timer->next)
                {
                    timer->next->prev = timer->prev;
                }
                delete timer;
            }
        }

        void tick()
        {
            tw_timer* tmp = slots[cur_slot];
            printf("current slot is %d\n", cur_slot);
            while(tmp)
            {
                printf("tick the timer once\n");
                if(tmp->rotation>0)
                {
                    tmp->rotation--;
                    tmp=tmp->next;
                }
                else
                {
                    tmp->cb_func(tmp->user_data);
                    if(tmp == slots[cur_slot])
                    {
                        printf("delete header in cur_slot\n");
                        slots[cur_slot] = tmp->next;
                        delete tmp;
                        if(slots[cur_slot])
                        {
                            slots[cur_slot]->prev = NULL;
                        }
                        tmp = slots[cur_slot];
                    }
                    else
                    {
                        tmp->prev->next = tmp->next;
                        if(tmp->next)
                        {
                            tmp->next->prev = tmp->prev;
                        }
                        tw_timer* tmp2 = tmp->next;
                        delete tmp;
                        tmp = tmp2;
                    }
                }
            }
            cur_slot = ++cur_slot%N;
        }

    private:
        static const int N = 60; //槽个数
        static const int SI = 1; //槽间隔=1s
        tw_timer* slots[N]; //时间轮的槽，每个槽指向一个定时器链表，链表无序
        int cur_slot; //时间轮的当前槽
};

#endif