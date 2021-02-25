/*
 * (C) Radim Kolar 1997-2004
 * This is free software, see GNU Public License version 2 for
 * details.
 *
 * Simple forking WWW Server benchmark:
 *
 * Usage:
 *   webbench --help
 *
 * Return codes:
 *    0 - sucess
 *    1 - benchmark failed (server is not on-line)
 *    2 - bad param
 *    3 - internal error, fork failed
 * 
 */
#include "socket.c"
#include <unistd.h>
#include <sys/param.h>
//make运行报错，找不到头文件
//直接gcc并指定文件位置即可
//gcc webbench.c -I /usr/include/tirpc
#include <rpc/types.h>
#include <getopt.h>
#include <strings.h>
#include <time.h>
#include <signal.h>

/* values */
//根据命令行参数-t指定的测试时间判断是否超时
volatile int timerexpired = 0;
//子进程成功得到服务器响应的总数
int speed = 0;
//子进程请求失败总数
int failed = 0;
//读取到的字节数
int bytes = 0;
/* globals */
//HTTP协议版本定义
int http10 = 1; /* 0 - http/0.9, 1 - http/1.0, 2 - http/1.1 */
/* Allow: GET, HEAD, OPTIONS, TRACE */
#define METHOD_GET 0
#define METHOD_HEAD 1
#define METHOD_OPTIONS 2
#define METHOD_TRACE 3
#define PROGRAM_VERSION "1.5"
//定义HTTP请求方法GET 此外还支持OPTIONS、HEAD、TRACE方法，在main函数中用switch判断
int method = METHOD_GET;
//默认并发数为1，也就是子进程个数 可以由命令行参数-c指定
int clients = 1;
//是否等待从服务器获取数据 0为获取
int force = 0;
//是否使用cache  0为使用
int force_reload = 0;
//代理服务器端口 80
int proxyport = 80;
//代理服务器IP 默认为NULL
char *proxyhost = NULL;
//测试时间  默认为30秒  可由命令行参数-t指定
int benchtime = 30;
/* internal */
//创建管道(半双工) 父子进程间通信，读取/写入数据
int mypipe[2];
//定义服务器IP
char host[MAXHOSTNAMELEN];
#define REQUEST_SIZE 2048
//HTTP请求信息
char request[REQUEST_SIZE];

//构造长选项与短选项的对应
static const struct option long_options[] =
    {
        {"force", no_argument, &force, 1},
        {"reload", no_argument, &force_reload, 1},
        {"time", required_argument, NULL, 't'},
        {"help", no_argument, NULL, '?'},
        {"http09", no_argument, NULL, '9'},
        {"http10", no_argument, NULL, '1'},
        {"http11", no_argument, NULL, '2'},
        {"get", no_argument, &method, METHOD_GET},
        {"head", no_argument, &method, METHOD_HEAD},
        {"options", no_argument, &method, METHOD_OPTIONS},
        {"trace", no_argument, &method, METHOD_TRACE},
        {"version", no_argument, NULL, 'V'},
        {"proxy", required_argument, NULL, 'p'},
        {"clients", required_argument, NULL, 'c'},
        {NULL, 0, NULL, 0}};

/* prototypes */
static void benchcore(const char *host, const int port, const char *request);
//
static int bench(void);
//函数声明，根据选项构建HTTP请求
static void build_request(const char *url);

static void alarm_handler(int signal){
   timerexpired = 1;
}

static void usage(void){
   fprintf(stderr,
           "webbench [option]... URL\n"
           "  -f|--force               Don't wait for reply from server.\n"
           "  -r|--reload              Send reload request - Pragma: no-cache.\n"
           "  -t|--time <sec>          Run benchmark for <sec> seconds. Default 30.\n"
           "  -p|--proxy <server:port> Use proxy server for request.\n"
           "  -c|--clients <n>         Run <n> HTTP clients at once. Default one.\n"
           "  -9|--http09              Use HTTP/0.9 style requests.\n"
           "  -1|--http10              Use HTTP/1.0 protocol.\n"
           "  -2|--http11              Use HTTP/1.1 protocol.\n"
           "  --get                    Use GET request method.\n"
           "  --head                   Use HEAD request method.\n"
           "  --options                Use OPTIONS request method.\n"
           "  --trace                  Use TRACE request method.\n"
           "  -?|-h|--help             This information.\n"
           "  -V|--version             Display program version.\n");
};
int main(int argc, char *argv[]){
   int opt = 0;
   int options_index = 0;
   char *tmp = NULL;

   if (argc == 1){
      usage();
      return 2;
   }

   while ((opt = getopt_long(argc, argv, "912Vfrt:p:c:?h", long_options, &options_index)) != EOF){
      switch (opt){
         case 0: break;
         case 'f': force = 1; break;
         case 'r': force_reload = 1; break;
         case '9': http10 = 0; break;
         case '1': http10 = 1; break;
         case '2': http10 = 2; break;
         case 'V': 
            printf(PROGRAM_VERSION "\n"); 
            exit(0);
         case 't': benchtime = atoi(optarg); break;
         case 'p':
            /* proxy server parsing server:port */
            tmp = strrchr(optarg, ':');
            proxyhost = optarg;
            if (tmp == NULL){
               break;
            }
            if (tmp == optarg){
               fprintf(stderr, "Error in option --proxy %s: Missing hostname.\n", optarg);
               return 2;
            }
            if (tmp == optarg + strlen(optarg) - 1){
               fprintf(stderr, "Error in option --proxy %s Port number is missing.\n", optarg);
               return 2;
            }
            *tmp = '\0';
            proxyport = atoi(tmp + 1);
            break;
         case ':':
         case 'h':
         case '?': usage(); return 2; break;
         case 'c': clients = atoi(optarg); break;
      }
   }

   //argv[optind]是第1个非选项参数
   //optind指选项参数的个数（包括程序名）
   //argc指全部参数的个数
   //url不是选项
   if (optind == argc){
      fprintf(stderr, "webbench: Missing URL!\n");
      usage();
      return 2;
   }

   //用户的选项设置可能有问题，容错处理
   if (clients == 0)
      clients = 1;
   if (benchtime == 0)
      benchtime = 60;
   /* Copyright */
   fprintf(stderr, "Webbench - Simple Web Benchmark " PROGRAM_VERSION "\n"
                   "Copyright (c) Radim Kolar 1997-2004, GPL Open Source Software.\n");
   //构建http
   build_request(argv[optind]);
   /* print bench info */
   printf("\nBenchmarking: ");
   switch (method){
      case METHOD_GET:
      default: 
         printf("GET");
         break;
      case METHOD_OPTIONS: 
         printf("OPTIONS"); 
         break;
      case METHOD_HEAD: 
         printf("HEAD"); 
         break;
      case METHOD_TRACE: 
         printf("TRACE"); 
         break;
   }
   printf(" %s", argv[optind]);
   switch (http10){
      case 0:
         printf(" (using HTTP/0.9)");
         break;
      case 2:
         printf(" (using HTTP/1.1)");
         break;
   }
   printf("\n");
   if (clients == 1)
      printf("1 client");
   else
      printf("%d clients", clients);

   printf(", running %d sec", benchtime);
   if (force)
      printf(", early socket close");
   if (proxyhost != NULL)
      printf(", via proxy server %s:%d", proxyhost, proxyport);
   if (force_reload)
      printf(", forcing reload");
   //换行不能少！库函数是默认行缓冲，子进程会复制整个缓冲区，
   //若不换行刷新缓冲区，子进程会把缓冲区的的也打出来！
   printf(".\n");
   return bench();
}

void build_request(const char *url)
{
   char tmp[10];
   int i;

   bzero(host, MAXHOSTNAMELEN);
   bzero(request, REQUEST_SIZE);

   //缓存和代理都是http1.0后才有的
   //无缓存和代理都要在http1.0以上才能使用
   //因此这里要处理一下，不然可能会出问题
   if (force_reload && proxyhost != NULL && http10 < 1)
      http10 = 1;
   //HEAD请求是http1.0后才有
   if (method == METHOD_HEAD && http10 < 1)
      http10 = 1;
   //OPTIONS和TRACE都是http1.1才有
   if (method == METHOD_OPTIONS && http10 < 2)
      http10 = 2;
   if (method == METHOD_TRACE && http10 < 2)
      http10 = 2;

   switch (method){
      default:
      case METHOD_GET:
         strcpy(request, "GET");
         break;
      case METHOD_HEAD:
         strcpy(request, "HEAD");
         break;
      case METHOD_OPTIONS:
         strcpy(request, "OPTIONS");
         break;
      case METHOD_TRACE:
         strcpy(request, "TRACE");
         break;
   }

   strcat(request, " ");

   //url不存在://则打印错误信息，并退出
   if (NULL == strstr(url, "://")){
      fprintf(stderr, "\n%s: is not a valid URL.\n", url);
      exit(2);
   }
   if (strlen(url) > 1500){
      fprintf(stderr, "URL is too long.\n");
      exit(2);
   }
   //没有代理服务器却填写错误，容错处理
   if (proxyhost == NULL){
      //忽略大小写，比较两个字符串的前7个字符，保证url是http://前缀
      if (0 != strncasecmp("http://", url, 7)){
         fprintf(stderr, "\nOnly HTTP protocol is directly supported, set --proxy for others.\n");
         exit(2);
      }
   }
   /* protocol/host delimiter */
   //将i指向url中://后第一个字符的位置
   i = strstr(url, "://") - url + 3;
   /* printf("%d\n",i); */

   //保证剩余字符中有'/'字符，否则报错退出
   if (strchr(url + i, '/') == NULL){
      fprintf(stderr, "\nInvalid URL syntax - hostname don't ends with '/'.\n");
      exit(2);
   }
   if (proxyhost == NULL){
      /* get port from hostname */
      //index类似strchr，找出第一个出现位置
      //index系列函数把字符串中最后的结束字符也当是字符串的内容处理
      if (index(url + i, ':') != NULL && index(url + i, ':') < index(url + i, '/')){
         strncpy(host, url + i, strchr(url + i, ':') - url - i);
         bzero(tmp, 10);
         strncpy(tmp, index(url + i, ':') + 1, strchr(url + i, '/') - index(url + i, ':') - 1);
         /* printf("tmp=%s\n",tmp); */
         proxyport = atoi(tmp);
         if (proxyport == 0)
            proxyport = 80;
      }
      else{
         //将"/"之前的字符拷贝到host中
         strncpy(host, url + i, strcspn(url + i, "/"));
      }
      // printf("Host=%s\n",host);
      //将从资源服务器获取的资源名称拼接进请求中
      strcat(request + strlen(request), url + i + strcspn(url + i, "/"));
   }
   
   else { //有代理服务器就简单了，直接填就行，不用自己处理
      // printf("ProxyHost=%s\nProxyPort=%d\n",proxyhost,proxyport);
      strcat(request, url);
   }
   if (http10 == 1)
      strcat(request, " HTTP/1.0");
   else if (http10 == 2)
      strcat(request, " HTTP/1.1");
   strcat(request, "\r\n");
   if (http10 > 0)
      strcat(request, "User-Agent: WebBench " PROGRAM_VERSION "\r\n");
   if (proxyhost == NULL && http10 > 0) {
      strcat(request, "Host: ");
      strcat(request, host);
      strcat(request, "\r\n");
   }

   //若选择强制重新加载，则填写无缓存
   if (force_reload && proxyhost != NULL) {
      strcat(request, "Pragma: no-cache\r\n");
   }
   //我们目的是构造请求给网站，不需要传输任何内容，当然不必用长连接
   //否则太多的连接维护会造成太大的消耗，大大降低可构造的请求数与客户端数
   //http1.1后是默认keep-alive的
   if (http10 > 1)
      strcat(request, "Connection: close\r\n");
   /* add empty line at end */
   if (http10 > 0)
      strcat(request, "\r\n");
   // printf("Req=%s\n",request);
}

/* vraci system rc error kod */
//父进程的作用：创建子进程，读子进程测试到的数据，然后处理
static int bench(void)
{
   int i, j, k;
   pid_t pid = 0;
   FILE *f;

   /* check avaibility of target server */
   //判断是否使用代理服务器，将Socket函数的返回值进行判断
   //尝试建立连接一次
   i = Socket(proxyhost == NULL ? host : proxyhost, proxyport);
   if (i < 0) {
      fprintf(stderr, "\nConnect to server failed. Aborting benchmark.\n");
      return 1;
   }
   close(i);
   /* create pipe */
   //创建管道及错误处理
   if (pipe(mypipe)) {
      perror("pipe failed.");
      return 3;
   }

   /* not needed, since we have alarm() in childrens */
   /* wait 4 next system clock tick */
   /*
  cas=time(NULL);
  while(time(NULL)==cas)
        sched_yield();
  */

   /* fork childs */
   for (i = 0; i < clients; i++) {
      pid = fork();
      
      if (pid <= (pid_t)0) {
         /* child process or error*/
         sleep(1); /* make childs faster */
         break;//创建子进程失败则结束循环
      }
   }

   //错误处理，fork调用失败返回负数
   if (pid < (pid_t)0) {
      fprintf(stderr, "problems forking worker no. %d\n", i);
      perror("fork failed.");
      return 3;
   }
   //判断pid是否为0，为0则进入子进程执行相应的代码
   if (pid == (pid_t)0) {
      /* I am a child */
      //判断代理，进入benchcore函数 
      if (proxyhost == NULL)
         benchcore(host, proxyport, request);
      else
         benchcore(proxyhost, proxyport, request);

      /* write results to pipe */
      //打开管道，子进程往管道写数据 
      f = fdopen(mypipe[1], "w");
      if (f == NULL) {
         perror("open pipe for writing failed.");
         return 3;
      }
      /* fprintf(stderr,"Child - %d %d\n",speed,failed); */
      fprintf(f, "%d %d %d\n", speed, failed, bytes);
      fclose(f);
      return 0;
   }
   //判断pid是否大于0，大于0进入父进程，执行相应代码
   else {
      //打开管道，父进程从管道读数据，一个管道只能进行半双工的工作(一端读，一端写) 
      f = fdopen(mypipe[0], "r");
      if (f == NULL) {
         perror("open pipe for reading failed.");
         return 3;
      }
      //fopen标准IO函数是自带缓冲区的，
      //我们的输入数据非常短，并且要求数据要及时，
      //因此没有缓冲是最合适的
      //我们不需要缓冲区
      //因此把缓冲类型设置为_IONBF
      setvbuf(f, NULL, _IONBF, 0);
      speed = 0;
      failed = 0;
      bytes = 0;

      while (1) {
         //从管道读数据 
         pid = fscanf(f, "%d %d %d", &i, &j, &k);
         if (pid < 2) {
            fprintf(stderr, "Some of our childrens died.\n");
            break;
         }
         //全局计数器  speed failed bytes
         speed += i;
         failed += j;
         bytes += k;
         /* fprintf(stderr,"*Knock* %d %d read=%d\n",speed,failed,pid); */
         //子进程为0，数据读完后，退出循环
         if (--clients == 0)
            break;
      }
      fclose(f);
      
      //打印结果
      printf("\nSpeed=%d pages/min, %d bytes/sec.\nRequests: %d susceed, %d failed.\n",
             (int)((speed + failed) / (benchtime / 60.0f)),
             (int)(bytes / (float)benchtime),
             speed,
             failed);
   }
   return i;
}

void benchcore(const char *host, const int port, const char *req) {
   int rlen;
   char buf[1500];
   int s, i;
   struct sigaction sa;

   /* setup alarm signal handler */
   //加载信号处理函数 
   sa.sa_handler = alarm_handler;
   sa.sa_flags = 0;
   if (sigaction(SIGALRM, &sa, NULL))
      exit(3);
   //计时开始 
   alarm(benchtime);

   rlen = strlen(req);
   //带go-to语句的while循环 
nexttry:
   while (1) {
      //超时则退出函数
      //闹钟信号处理函数设置该值为1
      if (timerexpired) {
         if (failed > 0) {
            /* fprintf(stderr,"Correcting failed by signal\n"); */
            failed--;
         }
         return;
      }
      //建立socket连接，获取socket描述符
      s = Socket(host, port);
      //连接建立失败,failed++
      if (s < 0) {
         failed++;
         continue;
      }
      //往服务器发送请求；如果请求失败，failed++,
      //关闭当前子进程socket描述符，go-to到while循环再来一次 
      if (rlen != write(s, req, rlen)) {
         failed++;
         close(s);
         continue;
      }
      //针对HTTP0.9的处理办法 
      //http0.9的特殊处理
      //因为http0.9是在服务器回复后自动断开连接的，不keep-alive
      //在此可以提前先彻底关闭套接字的写的一半，如果失败了那么肯定是个不正常的状态,
      //如果关闭成功则继续往后，因为可能还有需要接收服务器的恢复内容
      //但是写这一半是一定可以关闭了，作为客户端进程上不需要再写了
      //因此我们主动破坏套接字的写端，但是这不是关闭套接字，关闭还是得close
      //事实上，关闭写端后，服务器没写完的数据也不会再写了，这个就不考虑了
      if (http10 == 0) {
         if (shutdown(s, 1)) {
            failed++;
            close(s);
            continue;
         }
      }
      //判断是否等待服务器回复
      if (force == 0) {
         /* read all available data from socket */
         while (1) {
            //判断超时 
            if (timerexpired)
               break;
            //从服务器读取数据，保存到buff数组中
            i = read(s, buf, 1500);
            /* fprintf(stderr,"%d\n",i); */
            //读取数据失败的处理，返回while循环开始处，重新来一次 
            if (i < 0) {
               failed++;
               close(s);
               goto nexttry;
            }
            //读取成功后，bytes统计数据大小 
            else {
               if (i == 0) break;//读完
               else bytes += i;
            }
         }
      }
      if (close(s)) {
         failed++;
         continue;
      }
      speed++;
   }
}