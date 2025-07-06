#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量
#define TIMESLOT 5      //超时时间，每5秒检查一下是否有不活跃的连接并关闭

static int pipefd[2];     //管道，用来处理信号
static sort_timer_lst timer_lst;     // 定时器链表，用来处理定时任务
static int epollfd = 0;        // epoll对象，用来监听事件

// 添加文件描述符
extern void setnonblocking( int fd );
extern void addfd( int epollfd, int fd, bool one_shot );
extern void removefd( int epollfd, int fd );

// 添加信号捕捉
void addsig(int sig, void( handler )(int)){
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}
// 信号处理函数
// 该函数会被信号处理函数调用，向管道发送信号
void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}
// 添加信号处理函数
void addsig( int sig )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

// 定时器处理函数，epoll中调用来处理超时任务
void timer_handler()
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func( http_conn* user_data )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->m_sockfd, 0 );
    assert( user_data );
    printf( "close fd %d\n", user_data->m_sockfd );
    close( user_data->m_sockfd );
}


int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    // 获取端口号
    int port = atoi( argv[1] );

    // 对SIGPIE信号进行处理
    addsig( SIGPIPE, SIG_IGN );
    
    // 创建和初始化线程池
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }

    // 创建一个数组 用于保存所有的客户端信息
    http_conn* users = new http_conn[ MAX_FD ];


    // 创建监听套接字
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );

    // 监听
    ret = listen( listenfd, 5 );

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create1( 0 );
    // 添加到epoll对象中
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0] ,false);

    // 设置信号处理函数
    addsig( SIGALRM );
    addsig( SIGTERM );
    bool stop_server = false;
    
    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号

    while(!stop_server) {
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            if( sockfd == listenfd ) {
                // 有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {
                    // 目前连接数满了
                    // 给客户端写一个信息： 服务器正忙
                    close(connfd);
                    continue;
                }
                // 将新的客户的数据初始化， 放入数组中
                users[connfd].init( connfd, client_address);

                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time( NULL );
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].timer = timer;
                timer_lst.add_timer( timer );
            
            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                // 对方异常断开或错误等事件
                cb_func( &users[sockfd] );
                users[sockfd].close_conn();
                
                if( users[sockfd].timer )
                {
                    timer_lst.del_timer( users[sockfd].timer );
                }

            } else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) {
                // 处理信号
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {
                    for( int i = 0; i < ret; ++i ) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
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
            } else if(events[i].events & EPOLLIN) {
                // 一次性把全部数据读完
                ret = users[sockfd].read();
                util_timer* timer = users[sockfd].timer;
                if(ret > 0) {
                    pool->append(users + sockfd);
                    // 如果读数据成功，则调整定时器的超时时间
                    // 这里假设每次读数据都需要延长3个时间片的
                    if( timer ) {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        printf( "adjust timer once\n" );
                        timer_lst.adjust_timer( timer );
                    }
                } else {
                    cb_func( &users[sockfd] );
                    users[sockfd].close_conn();
                    
                    if( timer )
                    {
                        timer_lst.del_timer( timer );
                    }
                }

            }  else if( events[i].events & EPOLLOUT ) {
                // 一次性把全部数据写完
                if( !users[sockfd].write() ) {
                    cb_func( &users[sockfd] );
                    users[sockfd].close_conn();
                }

            }
        }
        //超时任务最后处理，在epoll检测到的事件处理完成之后
        if( timeout ) {
            timer_handler();
            timeout = false;
        }
    }
    
    close( epollfd );
    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] users;
    delete pool;
    return 0;
}