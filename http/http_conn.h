#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <sys/types.h>
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
#include <signal.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <sys/uio.h>
#include "../locker/locker.h"

class http_conn
{
    public:
        static const int FILENAME_LEN = 200; //文件名的最大长度
        static const int READ_BUF_SIZE = 2048; //读缓存大小
        static const int WRITE_BUF_SIZE = 1024; //写缓存大小
        enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE,
            OPTIONS, CONNECT, PATCH}; //HTTP请求方法，这里的实现只支持GET
        enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER,
            CHECK_STATE_CONTENT}; //解析客户请求时，主状态机的状态
        enum LINE_STATUS {LINE_OK = 0, LINE_BAD, LINE_OPEN}; //从状态机，行的读取状态
        enum HTTP_CODE {NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE,
            FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSE_CONNECTION}; //服务器处理HTTP请求可能的结果
        
        http_conn() {}
        ~http_conn() {}

        void init(int sockfd, const sockaddr_in& addr); //初始化新接受的连接
        void close_conn(bool real_close = true); //关闭连接
        void process(); //；处理客户请求
        bool read(); //非阻塞读操作
        bool write(); //非阻塞写操作

        static int m_epollfd; //所有socket上的事件都被注册到同一个epoll内核事件表中，所以将epoll文件描述符设置为静态的
        static int m_user_count; //用户数量

    private:
        //该http连接的socket和对方的sock地址
        int m_sockfd;
        sockaddr_in m_address;
        
        char m_read_buf[READ_BUF_SIZE]; //读缓存区
        int m_read_idx; //标识读缓存中已经读入的客户数据的最后一个字节的下一个位置
        int m_checked_idx; //当前正在分析的字符在读缓存中的位置
        int m_start_line; //当前正在解析的行的起始位置
        char m_write_buf[WRITE_BUF_SIZE]; //写缓存
        int m_write_idx; //写缓存中待发送的字节数

        CHECK_STATE m_check_state; //主状态机当前的状态
        METHOD m_method; //请求方法

        char m_real_file[FILENAME_LEN]; //客户请求的目标文件的完整路径
        char* m_url; //客户请求的目标文件名
        char* m_version; //HTTP协议版本号，这里只支持HTTP/1.1
        char* m_host; //主机名
        int m_content_length; //HTTP请求的消息体长度
        bool m_linger; //HTTP请求是否要求保持连接

        char* m_file_address; //客户请求的目标文件被mmap到内存中的起始位置
        struct stat m_file_stat; //目标文件的状态
        //采用writev来进行写操作
        struct iovec m_iv[2];
        int m_iv_count;

        void init(); //初始化连接
        HTTP_CODE process_read(); //解析HTTP请求
        bool process_write(HTTP_CODE ret); //填充HTTP应答

        //下面这组函数被process_read调用以分析HTTP请求
        HTTP_CODE parse_request_line(char* text);
        HTTP_CODE parse_headers(char* text);
        HTTP_CODE parse_content(char* text);
        HTTP_CODE do_request();
        char* get_line() {return m_read_buf+m_start_line;}
        LINE_STATUS parse_line();

        //下面这组函数被process_write调用以填充HTTP应答
        void unmap();
        bool add_response(const char* format, ...);
        bool add_content(const char* content);
        bool add_status_line(int status, const char* title);
        bool add_headers(int content_length);
        bool add_content_length(int content_length);
        bool add_linger();
        bool add_blank_line();
};

#endif