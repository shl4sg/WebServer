#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <iostream>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include "tools/locker.h"
#include "http/threadpool.h"
#include "http/http_conn.h"
#include "./tools/lst_timer.h"
#include "mysql_cgi/sql_connection_pool.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5
static int epollfd;
#define LISTEN_MODE_ET
#define SYNLOG  //同步写日志
//#define ASYNLOG //异步写日志

//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;

void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}
void cb_func( client_data* user_data)
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0 );
    assert( user_data );
    close( user_data->sockfd );
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}

void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}
void addsig( int sig, void( handler )(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart )
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void show_error( int connfd, const char* info )
{
    LOG_ERROR("%s", "Internal server busy");
    send( connfd, info, strlen( info ), 0 );
    close( connfd );
}


int main( int argc, char* argv[] )
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif
    if( argc <= 2 )
    {
        LOG_INFO( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi( argv[2] );


//忽略SIGPIPE信号
    addsig(SIGPIPE, SIG_IGN);

//创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "123456", "webDB", 3306, 8);
    initmysql_result(connPool);

//创建线程池
    threadpool< http_conn >* pool = NULL;
    try
    {
        pool = new threadpool< http_conn >(connPool);
    }
    catch( ... )
    {
        return 1;
    }
//创建连接池
    http_conn* users = new http_conn[ MAX_FD ];
    assert( users );
    int user_count = 0;
    int ret;

//添加信号
    addsig( SIGALRM,sig_handler);
    addsig( SIGTERM,sig_handler);
//添加定时器所用的数据
    client_data *users_timer = new client_data[MAX_FD];

//设置监听端口
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );
    struct linger tmp = { 1, 0 };
    setsockopt( listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof( tmp ) );

    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret >= 0 );

    ret = listen( listenfd, 5 );
    assert( ret >= 0 );
//创建epoll
    epoll_event events[ MAX_EVENT_NUMBER ];
    epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
#ifdef LISTEN_MODE_ET
    addfd( epollfd, listenfd, false ,1);
#else
    addfd( epollfd, listenfd, false ,0);
#endif
    http_conn::m_epollfd = epollfd;
    bool stop_server=1;
    //    设置定时器
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false,0);
    //初始化定时
    bool timeout = false;
    alarm(TIMESLOT);
    while(stop_server)
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            LOG_ERROR( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( sockfd == listenfd )
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
#ifdef LISTEN_MODE_ET
                while (1){
                    int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                    if ( connfd < 0 )
                    {
                        LOG_WARN( "accept errno is: %d\n", errno );
                        break;
                    }
                    if( http_conn::m_user_count >= MAX_FD )
                    {
                        show_error( connfd, "Internal server busy" );
                        break;
                    }
                    LOG_INFO("%s:%d connected ...\n",inet_ntoa(client_address.sin_addr),ntohs(client_address.sin_port));
                    users[connfd].init( connfd, client_address );
                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
#else
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );

                if ( connfd < 0 )
                {
                    LOG_WARN( "accept errno is: %d\n", errno );
                    continue;
                }
                if( http_conn::m_user_count >= MAX_FD )break
                {
                    show_error( connfd, "Internal server busy" );
                    break;
                }
                LOG_INFO("%s:%d connected ...\n",inet_ntoa(client_address.sin_addr),ntohs(client_address.sin_port));
                users[connfd].init( connfd, client_address );
                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
#endif

            }
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) )
            {
                users[sockfd].close_conn();
                util_timer *timer = users_timer[sockfd].timer;
                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }
//处理信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                            case SIGALRM:
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            else if( events[i].events & EPOLLIN )
            {

                util_timer *timer = users_timer[sockfd].timer;
                if( users[sockfd].read() )
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].m_address.sin_addr));
                    Log::get_instance()->flush();
                    pool->append( users + sockfd );
//调整定时器
                    if( timer )
                    {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        Log::get_instance()->flush();
                        LOG_INFO( "adjust tools once\n" );
                        timer_lst.adjust_timer( timer );
                    }
                }
                else
                {
                    users[sockfd].close_conn();
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if( events[i].events & EPOLLOUT )
            {
                util_timer *timer = users_timer[sockfd].timer;
                if( users[sockfd].write() )
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].m_address.sin_addr));
                    Log::get_instance()->flush();
                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else{
                    users[sockfd].close_conn();
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else
            {}
            if (timeout)
            {
                timer_handler();
                timeout = false;
            }
        }
    }

    close( epollfd );
    close( listenfd );
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
