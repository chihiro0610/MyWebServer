#include "http_conn.h"

//定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

const char* doc_root = "/home/chihiro/桌面/practice/High performance Linux Server Programming/webserver/html"; //网站的根目录

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option|O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN|EPOLLET|EPOLLRDHUP;
    if(one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev|EPOLLET|EPOLLONESHOT|EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close)
{
    if(real_close && (m_sockfd!=-1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init(int sockfd, const sockaddr_in& addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //以下两行是为了避免TIME_WAIT状态，仅用于调试，实际使用时应去掉
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    init();
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE; //主状态机的状态初始化为REQUESTLINE请求行
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf, 0, READ_BUF_SIZE);
    memset(m_write_buf, 0, WRITE_BUF_SIZE);
    memset(m_real_file, 0, FILENAME_LEN);
}

http_conn::LINE_STATUS http_conn::parse_line() //从状态机，解析出一行内容
{
    char temp;
    for(; m_checked_idx<m_read_idx; m_checked_idx++)
    {
        //获取当前要分析的字节
        temp = m_read_buf[m_checked_idx];
        if(temp=='\r') //读到\r，说明可能读到一个完整的行
        {
            //如果\r是buffer中最后一个数据，说明此次分析没有读到一个完整的行
            if((m_checked_idx+1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            else if(m_read_buf[m_checked_idx+1] == '\n')
            {
                m_read_buf[m_checked_idx++] = 0;
                m_read_buf[m_checked_idx++] = 0;
                return LINE_OK;
            }
            return LINE_BAD; //若非以上情况，说明客户发送的HTTP请求有语法问题
        }
        else if(temp=='\n')
        {
            if((m_checked_idx>1) && m_read_buf[m_checked_idx-1]=='\r')
            {
                m_read_buf[m_checked_idx-1] = 0;
                m_read_buf[m_checked_idx++] = 0;
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN; //所有内容分析完没遇到\r，返回LINE_OPEN
}

bool http_conn::read() //循环读取客户数据，直到无数据可读或对方关闭连接
{
    if(m_read_idx >= READ_BUF_SIZE)
    {
        return false;
    }
    int bytes_read = 0;
    while(true)
    {
        bytes_read = recv(m_sockfd, m_read_buf+m_read_idx, READ_BUF_SIZE-m_read_idx, 0);
        if(bytes_read == -1)
        {
            if(errno==EAGAIN || errno==EWOULDBLOCK)
            {
                break;
            }
            return false;
        }
        else if(bytes_read == 0)
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

//解析HTTP请求行，获得请求方法、目标URL，以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    //例：GET http://example.com/index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); //匹配字符串中第一个" "或"\t"
    if(!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = 0;

    char* method = text;
    if(strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else
    {
        return BAD_REQUEST;
    }

    m_url += strspn(m_url, " \t"); //返回m_url中第一个既不是' '也不是'\t'的字符的位置，跳过多余的分隔符
    m_version = strpbrk(m_url, " \t");
    if(!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = 0;
    m_version += strspn(m_version, " \t");
    if(strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }
    if(strncasecmp(m_url, "http://", 7) == 0) //检查URL是否合法
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if(!m_url || m_url[0]!='/')
    {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; //HTTP请求行处理完成，状态转移到分析头部
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char* text) //解析HTTP请求的头部信息
{
    if(text[0] == '\0') //如果遇到空行，表示头部字段解析完成
    {
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，状态转移到CHECK_STATE_CONTENT
        if(m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    else if(strncasecmp(text, "Connection:", 11) == 0) //处理Connection头部字段
    {
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if(strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if(strncasecmp(text, "Host:", 5) == 0)
    {
        text  += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
}

//我们没有真正解析HTTP请求的消息体，只是判断它是否被完整地读入了
http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    if(m_read_idx >= (m_content_length+m_checked_idx))  //这里的m_checked_idx应为content的起始位置，判断m_read_idx >= content长度+content起始位置，条件为真即content被完整读入
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn:: HTTP_CODE http_conn::process_read() //主状态机
{
    LINE_STATUS linestatus = LINE_OK; //记录当前行的读取状态
    HTTP_CODE ret = NO_REQUEST; //记录HTTP请求的处理结果
    char* text = 0;

    while(((m_check_state == CHECK_STATE_CONTENT) && (linestatus == LINE_OK))
        || ((linestatus = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        printf("got 1 http line: %s\n", text);
        
        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if(ret == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if(ret == GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if(ret == GET_REQUEST)
                {
                    return do_request();
                }
                linestatus = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    //如果没读到一个完整的行，则返回NO_REQUEST，表示请求不完整，需要继续读取客户数据
    if(linestatus == LINE_OPEN)
    {
        return NO_REQUEST;
    }
    else
    {
        return BAD_REQUEST;
    }
}

/*当得到一个完整、正确的HTTP请求时，我们分析目标文件的属性，如果目标文件存在，
对所有用户可读，且不是目录，则使用mmap将其映射到m_file_address，并返回FILE_REQUEST*/
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root); //将m_real_file设置为网站根目录
    int len = strlen(doc_root);
    strncpy(m_real_file+len, m_url, FILENAME_LEN - len -1);
    if(stat(m_real_file, &m_file_stat)<0)
    {
        return NO_RESOURCE;
    }
    if(!(m_file_stat.st_mode & S_IROTH)) //S_IROTH：其他用户可读
    {
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST;
    }

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, 
        MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::add_response(const char* format, ...) //往写缓存中写入待发送数据
{
    if(m_write_idx >= WRITE_BUF_SIZE)
    {
        return false;
    }
    va_list arg_list; //可变参数列表
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf+m_write_idx, WRITE_BUF_SIZE-1-m_write_idx, format, arg_list);  //将可变参数按照format的格式写入m_write_buf
    if(len>=(WRITE_BUF_SIZE-1-m_write_idx))
    {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger==true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

bool http_conn::add_content(const char* content)
{
    return add_response("%s", content);
}

//根据服务器处理的HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
        case INTERNAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form))
            {
                return false;
            }
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form))
            {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form))
            {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form))
            {
                return false;
            }
            break;
        }
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            if(m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                return true;
            }
            else
            {
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string))
                {
                    return false;
                }
                break;
            }
        }
        default:
        {
            return false;
        }
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

bool http_conn::write() //写http响应
{
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;
    if(bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while(1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if(temp <= -1)
        {
            //如果TCP写缓存没有空间，则等待下一轮EPOLLOUT事件，在此期间，
            //服务器无法立即接收同一客户的下一个请求，但这可以保证连接的完整性
            if(errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;
        if(bytes_to_send <= bytes_have_send)
        {
            //发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger)
            {
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            else 
            {
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return false;
            }
        }
    }
}

void http_conn::process() //由线程池中的工作线程调用，这是处理HTTP请求的入口函数
{
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if(!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT); //处理完返回给客户端的内容，通知主线程m_sockfd的写就绪
}