#ifndef HEAP_TIMER
#define HEAP_TIMER

#include <iostream>
#include <netinet/in.h>
#include <time.h>
using std::exception;

#define BUF_SIZE 64

class heap_timer;

struct client_data
{
    sockaddr_in address;
    int sockfd;
    char buf[BUF_SIZE];
    heap_timer* timer;
};

class heap_timer
{
    public:
        heap_timer(int delay)
        {
            expire = time(NULL) + delay;
        }
        time_t expire; //定时器生效的绝对时间
        void (*cb_func) (client_data*);
        client_data* user_data;
};

class time_heap
{
    public:
        time_heap(int cap) throw (std::exception) :capacity(cap), cur_size(0)
        {
            array = new heap_timer* [capacity];
            if(!array)
            {
                throw std::exception();
            }
            for(int i=0; i<capacity; i++)
            {
                array[i] = NULL;
            }
        }
        time_heap(heap_timer** init_array, int size, int capacity) throw (std::exception)
            : cur_size(size), capacity(capacity)
        {
            if(capacity<size)
            {
                throw std::exception();
            }
            array = new heap_timer* [capacity];
            if(!array)
            {
                throw std::exception();
            }
            for(int i=0; i<capacity; i++)
            {
                array[i] = NULL;
            }
            if(size!=0)
            {
                for(int i=0; i<size; i++)
                {
                    array[i] = init_array[i];
                }
                for(int i=(cur_size-1)/2; i>=0; i--)
                {
                    //对数组中第(cur_size-1)/2到0个元素进行下虑操作
                    percolate_down(i);
                }
            }
        }
        ~time_heap()
        {
            for(int i=0;i<cur_size;i++)
            {
                delete array[i];
            }
            delete [] array;
        }

        void add_timer(heap_timer* timer) throw (std::exception)
        {
            if(!timer)
            {
                return;
            }
            if(cur_size>=capacity)
            {
                resize();
            }
            int hole = cur_size++;
            int parent = 0;
            //从空位到根节点进行上虑
            for(; hole>0; hole=parent)
            {
                parent = (hole-1)/2;
                if(array[parent]->expire<=timer->expire)
                {
                    break;
                }
                array[hole] = array[parent];
            }
            array[hole] = timer;
        }

        void del_timer(heap_timer* timer)
        {
            if(!timer)
                return;
            //仅将目标定时器的回调函数设置为空，即所谓的延迟销毁，这将节省真正删除该定时器的开销
            timer->cb_func = NULL;   
        }

        heap_timer* top() const
        {
            if(empty())
            {
                return NULL;
            }
            return array[0];
        }

        void pop_timer()
        {
            if(empty())
            {
                return;
            }
            if(array[0])
            {
                delete array[0];
                array[0] = array[--cur_size];
                array[cur_size] = NULL;
                percolate_down(0);
            }
        }

        void tick()
        {
            heap_timer* tmp = array[0];
            time_t cur = time(NULL);
            while(!empty())
            {
                if(!tmp)
                {
                    break;
                }
                if(tmp->expire>cur)
                {
                    break;
                }
                if(array[0]->cb_func)
                {
                    array[0]->cb_func(array[0]->user_data);
                }
                pop_timer();
                tmp = array[0];
            }
        }

        bool empty() const
        {
            return cur_size == 0;
        }

    private:
        heap_timer** array; //堆数组
        int capacity;  //堆数组容量
        int cur_size;  //堆数组当前包含元素的个数
        void percolate_down(int hole)
        {
            heap_timer* tmp = array[hole];
            int child = 0;
            for(; (hole*2+1)<=(cur_size-1); hole = child)
            {
                child = hole*2+1;
                if(child<(cur_size-1)&&(array[child+1]->expire<array[child]->expire))
                {
                    child++;
                }
                if(array[child]->expire<tmp->expire)
                {
                    array[hole] = array[child];
                }
                else
                {
                    break;
                }
            }
            array[hole] = tmp;
        }

        void resize() throw (std::exception)
        {
            heap_timer** tmp = new heap_timer* [2*capacity];
            for(int i=0; i<2*capacity; i++)
            {
                tmp[i] = NULL;
            }
            if(!tmp)
            {
                throw std::exception();
            }
            capacity = 2*capacity;
            for(int i=0; i<cur_size; i++)
            {
                tmp[i] = array[i];
            }
            delete [] array;
            array = tmp;
        }
};

#endif