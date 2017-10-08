/**
   
   このプログラムは、 Google APP Engine の dev_appserver.sh のように、
   制御を戻さないプログラムをデーモンのように動作させるためのプログラ
   ムである。実行を行うと、このプログラム自体は、 fork(2) を使って、子
   プロセスを生成した後にすぐに制御を返す。
   
   子プロセスは、/usr/bin/logger を exec するプロセスと、ターゲットプ
   ログラムの実行をする子プロセスをフォークしたのち、ターゲットプログ
   ラムを制御し、PID ファイルを管理するプロセス（便宜上 コントロールプ
   ロセスと呼ぶ）がselect(2) でターゲットプロセスの停止と、コントロー
   ルプログラムに送られるシグナルを待つ。

   コントロールプロセスのPID は、PID ファイルに書き込まれ
   if [ -f /tmp/daemonlize.pid ] ; then kill -INT `cat /tmp/daemonlize.pid` ; fi
   でプロセスに INT シグナルをスクリプトを書きやすくする。

   ターゲットプロセスの標準入力は、/dev/null につなげられ、標準出力と
   標準エラー出力は、/usr/bin/logger へパイプでつなげられる。
   
   コントロールプロセスが、INT シグナル（と HUP シグナル）を受け取った
   時には、ターゲットプロセスに対して、INTシグナルを送り、プロセスの終
   了を待って終了する。

   ターゲットプロセスが、先に終了した場合にも本プロセスは終了する。


   "self-pipe technique": pipe に対してPOSIX.1-2001 では、 PIPE_BUF バ
   イト以下の write(2) は atomic に行われることを利用してselect(2) を
   起こす方法

*/

/*
  このプログラムはもともと daemonlize という名前であった。

  TODO:
  daemon(3) の使用を考慮
  getopt に対応させるべきかどうか考える
  子プロセスのプロセスのテスト
  sigaction のところのシグナルの動作 ちゃんと規格に準拠しているかどうかのチェック
   
  構造的には 30点ぐらいだけど、とりあえずここまでにしておきましょう。
*/

/* _XOPEN_SOURCE=700 が定義されると _POSIX_C_SOURCE が 200809L で定義される */

#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif /* defined(HAVE_CONFIG_H) */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <string.h>
#include <locale.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <errno.h>
#include <limits.h>
#include <assert.h>

#include "verify.h"

#if !defined( VERIFY )
#if defined( NDEBUG )
#define VERIFY( exp ) (exp)
#else /* defined( NDEBUG ) */
#define VERIFY( exp ) assert( exp )
#endif /* defined( NDEBUG ) */
#endif /* !defined( VERIFY ) */

#if ( _POSIX_C_SOURCE < 200809L )
#error you must use compiler option -D_XOPEN_SOURCE=700
#endif /* ( _POSIX_C_SOURCE < 200809L ) */

/* realpath(3) require */
/* _BSD_SOURCE || _XOPEN_SOURCE >= 500 || _XOPEN_SOURCE && _XOPEN_SOURCE_EXTENDED */

enum{
  READ_SIDE = 0,
  WRITE_SIDE = 1
};

/** SIGCHLD を通知する self-pipe で使うファイルディスクリプタを格納した sig_atomic_t */
static volatile sig_atomic_t sig_child_pipe = -1;
/** SIGINTR を通知する self-pipe で使うファイルディスクリプタを格納した sig_atomic_t */
static volatile sig_atomic_t sig_intr_pipe = -1;


#if ( 201112L <=__STDC_VERSION__ )
static_assert( sizeof(int) == sizeof(volatile sig_atomic_t ), "" );
#endif /* ( 201112L <=__STDC_VERSION__ ) */

/**
   パスの最大値となる値を返す
   POSIX では、 PATH_MAX もしくは pathconf( "." , _PC_PATH_MAX ) 
   でパスの最大値を得ることができる。ただし、 pathconf はリソースに制限を課していない場合に
   -1 を返すことがあり、pathconf の戻り値を malloc に渡すと、符号が落ちて、非常に巨大な値と
   なる場合がある。
   この関数では、リソースに制限を課していない場合のpathconf が負の値を返した時 引数 length に 4096 を戻す。
   戻り値は、pathconf の戻り値を返す。
   @return リソースに制限がかけられていない場合には、-1 が戻る。そうでない場合には 相対パス名の最大長を返す
   @param length 相対パスの最大長として妥当と思われる値を入れて返す。
*/
int pathconf_path_max( size_t* length );

/**
   path の示すファイルが存在した場合には、引数のパスを絶対パスに変換する。
   当該のパスが存在しない場合には NULL を返す。
   戻り値は malloc(3) で確保したメモリなので、free(3) で解放する必要がある。
   @param path 絶対パスに変換を行うパス
   @return 絶対パス path に対応するファイルが存在しない場合には、NULL を返す。
   この戻り値は、malloc(3) で取得したメモリであるので、 free(3) で解放をしなければならない
*/
char* get_absolute_path( const char* path );

/** 
    SIGCHLD: signal_handler 
*/
static void sig_child_handler(int sig);

/**
   SIGINT: signal_handler 
*/
static void sig_intr_handler(int sig);

/**
   sigaction に signal(2) 形式ののシグナルハンドラを設定する コンビニエンスメソッド
*/
static void set_signal_handler( struct sigaction* sigact , void (* const signal_handler)(int) );

/**
   start_process で使用するパラメータのパック
*/
struct process_param;
/**
   fork して、ターゲットプロセスの開始と、SIGINT をターゲットプロセスへ送るプロセスへ送る
   ターゲットプロセスが、終了するまで、この関数は制御を返さない

   @return 成功した場合は EXIT_SUCCESS を返す 失敗した場合はそれ以外の値を返す
   @param param start_process へ渡すパラメータをパックした構造体
   @param path 実行ファイルへのパス
   @param argv execvl へ渡す引数の配列。終端を表すために、配列の最後の要素は、NULL で終端されていなければならい
*/
int start_process( struct process_param param,  const char* path , char * argv[]);

/**
   最終的な 子プロセスを execvp(2) で実行する。
   この関数は、制御を戻さない
*/
void take_over_for_child_process( int logger_fd , const char* path , char* argv[] );

/** 
    実質的なエントリーポイント
*/
int entry_point( int argc , char* argv[] );

/************************* 実装 **************************/

int pathconf_path_max( size_t* length )
{
#if defined( PATH_MAX )
  return PATH_MAX;
#else /* defined( PATH_MAX ) */
  const int pathconf_result = pathconf("." , _PC_PATH_MAX );
  if( length ){
    *length = (size_t)(( pathconf_result ) < 0 ? 4098 : pathconf_result );
  }
  return pathconf_result;
#endif /* defined( PATH_MAX ) */
}

char* get_absolute_path( const char* path )
{
  int err = 0; // realpath に備えて、errno を用意しておく
  size_t path_max = 0;
  VERIFY( 0 < pathconf_path_max( &path_max ) );
  char* absolute_path = malloc( sizeof(char)  * path_max );
  if( absolute_path ){
    errno = 0; // エラー番号をクリアしておいて、
    {
      char* realpath_result = realpath( path , absolute_path );
      err = errno;
      /* realpath() の戻り値が、 引数 absolute_path と異なる場合
         例えば、 NULL が戻ってきた場合は NULL を返すので、
         absolute_path を 解放して、realpath の戻り値を
         absolute_path に入れてそれを返す */
      if( realpath_result != absolute_path ){
        free( absolute_path );
        absolute_path = realpath_result;
      }
    }
  }
  errno = err;
  return absolute_path;
}

static void sig_child_handler(int sig)
{
#if defined( __GNUC__ )
  __sync_synchronize(); 
#endif /* defined( __GNUC__ ) */
  int fd = (int)sig_child_pipe;
  if( 0 < fd ){
    char b[1] = {0};
    if( sizeof( b ) != write( fd , b , sizeof( b ) ) ){
      abort();
    }
  }
  return;
};

static void sig_intr_handler(int sig)
{
#if defined( __GNUC__ )
  __sync_synchronize(); 
#endif /* defined( __GNUC__ ) */
  int fd = (int)sig_intr_pipe;
  if( 0 < fd ){
    char b[1] = {0};
    if( sizeof( b ) != write( fd , b , sizeof( b ) )){
      abort();
    }
  }
  return;
};


/**
   最終的な 子プロセスを execvp(2) で実行する。
   この関数は、制御を戻さない
*/
void take_over_for_child_process( int logger_fd , const char* path , char* argv[] )
{
  int null_in = open( "/dev/null" , O_RDONLY );
  assert( 0 <= null_in );

  VERIFY( dup2( null_in , STDIN_FILENO ) == STDIN_FILENO );
  VERIFY( dup2( logger_fd , STDOUT_FILENO ) == STDOUT_FILENO );
  VERIFY( dup2( logger_fd , STDERR_FILENO ) == STDERR_FILENO );
  VERIFY( 0 == close(null_in ) );
  VERIFY( 0 == close(logger_fd) );

  if( -1 == execvp( path , argv ) ){
    int err = errno;
    syslog( LOG_ERR , "%m, execvp(2) faild , path = \"%s\"",path);
    errno = err;
    perror("execlp");
  }
  _exit( EXIT_FAILURE );
}

/**
   sigaction に signal(2) 形式ののシグナルハンドラを設定する コンビニエンスメソッド
*/
static void set_signal_handler( struct sigaction* sigact , void (* const signal_handler)(int) )
{
  assert( NULL != sigact );
  assert( NULL != signal_handler );
  sigact->sa_handler = signal_handler;
  VERIFY( 0 == sigemptyset( &(sigact->sa_mask ) ) );
  sigact->sa_flags = 0;
  return;
}

/**
   select(2) で fd_set を使用する際に、ファイルディスクリプタの最大値に+1をした数が必要なので
   それを算出するためのラッピング構造体
*/
typedef struct fd_set_wrap_tag{
  /** ファイルディスクリプタ集合へのポインタ */
  fd_set * const fds; 
  /** ファイルディスクリプタ集合 の中で最大の ファイルディスクリプタ 
      ファイルディスクリプタが空集合の場合は-1 である。*/
  int maxfd; 
} fd_set_wrap;

/**
   select(2) の第一引数 を得るために、引数 readfds , writefds ,
   exceptfds の三つの ファイルディスクリプタ集合のうち
   最大のファイルディスクリプタに１を加えたものを返す。
   
   @return 引数 readfds , writefds ,  exceptfds の三つの ファイルディスクリプタ集合のうち最大のファイルディスクリプタに１を加えたものを返す。 
*/
int fd_set_wrap_get_maximum_fd( const fd_set_wrap* readfds , const fd_set_wrap* writefds , const fd_set_wrap* exceptfds )
{
  int result = -1;
  if( readfds ){
    if( result < readfds->maxfd ){
      result = readfds->maxfd;
    }
  }
  if( writefds ){
    if( result < writefds->maxfd ){
      result = writefds->maxfd;
    }
  }
  if( exceptfds ){
    if( result < exceptfds->maxfd ){
      result = exceptfds->maxfd;
    }
  }
  // 一つも含まれていない場合は、負の数を返す
  return ( result < 0 ) ? -1 : ( result + 1 ); 
}

/**
   fd_set_wrap が指し示す ファイルディスクリプタ集合を消去して、一つも含まれていない状態にする。
   FD_ZERO を fd_set_wrap に合わせた関数
*/
void fd_set_wrap_clear( fd_set_wrap* fds )
{
  assert( fds );
  assert( fds->fds );
  fds->maxfd = -1;
  FD_ZERO( fds->fds  );
  return;
}

/**
   引数 ファイルディスクリプタ fd  を  fd_set_wrap が指し示すファイルディスクリプタ集合から消去する。
*/
void fd_set_wrap_set( int fd, fd_set_wrap* fds )
{
  assert( fds );
  assert( fds->fds );
  fds->maxfd = ( fds->maxfd < fd ) ? fd : fds->maxfd ;
  FD_SET( fd , fds->fds );
  return;
}

/**
   fd_set_wrap の内容を 第二引数 src から 第一引数 dst へディープコピーを行う
   ディープコピーなので、dst->fds のポインタ値は変更されず中身がコピーされる。
   dst と src が 同じポインタの場合には、何もせずに dst が返される
   
   @return 成功した時には、dst のポインタ値が戻される。
   @param dst コピー先の fd_set_wrap へのポインタ ( NULL は許容されない )
   @param src コピー元の fd_set_wrap へのポインタ
*/
fd_set_wrap* fd_set_wrap_copy(fd_set_wrap* dst ,const fd_set_wrap *src){
  assert( dst );
  assert( dst->fds );
  if( dst != src ){
    if( src &&  src->fds ){
      assert( dst->fds );
      assert( src->fds );
      *(dst->fds) = *(src->fds);
      dst->maxfd = src->maxfd;
    }else{
      fd_set_wrap_clear( dst );
    }
  }
  return dst;
}

/**
   デーモン化したプロセスをホストするメインループ
   この関数は、デーモン化した子プロセスが終了するまで、制御を返さない。

   @return 子プロセスの終了コード
   @param child_pid 子プロセスのプロセスID 
   @param sigchld_selfpipe SIGCHLD を受けた時に読み込み可能になるパイプのファイルディスクリプタ self-pipe テクニックを使う
   @param sigint_selfpipe SIGINT を受けた時に読み込み可能になるパイプのファイルディスクリプタ self-pipe テクニックを使う
*/
int host_daemonlize_process(pid_t const child_pid ,int const sigchld_selfpipe ,int const sigint_selfpipe)
{
  /*
    このプロセスを終了させようと、SIGINT が送られてきたときには、
    まずシグナルハンドラでsigint_selfpipe に 1byte が書き込まる。
    書き込まれると、 sigint_selfpipe が読み込み可能になり、 select(2) が制御を戻す。
    次に、kill( child_pid , SIGINT ) で子プロセスの終了が図られて、次のループへ入り、
    select(2) で 制御が一度止まる。
    
    子プロセスが終了した時には、 SIGCHLD が発生し、 sigchld_selfpipe に 1byte が書き込まれる
    すると、 sig_child_pipe が読み込み可能になり、select(2) が制御を返す。
    子プロセスが終了したので、この関数は waitpidで、子プロセスの終了状態を取得して、
    制御を返す。
  */
  fd_set next_readfds_v = {{0}};
  fd_set_wrap next_readfds = { &next_readfds_v , -1 };

  fd_set_wrap_clear( &next_readfds );
  fd_set_wrap_set( sigchld_selfpipe , &next_readfds );
  fd_set_wrap_set( sigint_selfpipe , &next_readfds );
  
  for(;;){
    fd_set readfds_v      = {{0}}; 
    fd_set writefds_v     = {{0}};
    fd_set exceptfds_v    = {{0}};
    fd_set_wrap readfds   = { &readfds_v , -1 };
    fd_set_wrap writefds  = { &writefds_v , -1 };
    fd_set_wrap exceptfds = { &exceptfds_v , -1 };

    fd_set_wrap_clear( &readfds );
    fd_set_wrap_clear( &writefds );
    fd_set_wrap_clear( &exceptfds );

    fd_set_wrap_copy( &readfds , &next_readfds );
    const int nfds = fd_set_wrap_get_maximum_fd( &readfds , &writefds , &exceptfds );
    if( select( nfds , readfds.fds , writefds.fds , exceptfds.fds , NULL ) < 0){
      /* select にエラーが起きた時には readfds の状態は不明なので ここで continue しないと、下の read がブロックする */
      const int err = errno;
      if( EINTR == errno ){
        continue;
      }
      syslog( LOG_ERR , "%m, select(2) faild" );
      errno = err;
      abort(); // なんかよくわからないことが起きた
    }

    fd_set_wrap_clear( &next_readfds );
    
    if( FD_ISSET( sigchld_selfpipe , readfds.fds ) ){ /* 子プロセスがお亡くなりになった */
      char b[1] = {0};
      VERIFY( sizeof(b) == read( sigchld_selfpipe , b , sizeof( b ) ) );
      int status = 0;
      waitpid( child_pid , &status , 0 );
      return status;
    }else{
      fd_set_wrap_set( sigchld_selfpipe , &next_readfds );
    }
    
    if( FD_ISSET( sigint_selfpipe , readfds.fds ) ){ /* 自分自身に終了要求が来ている */
      char b[1] = {0};
      /* TODO 
         もし、この下の read がブロックしてしまうような場合があった場合に備える必要がある
         と思われる。
      */
      VERIFY( sizeof(b) == read( sigint_selfpipe  , b , sizeof( b ) ) );
      VERIFY( 0 ==  kill( child_pid , SIGINT ) );
    }else{
      fd_set_wrap_set( sigint_selfpipe , &next_readfds );
    }
  } // end of for(;;)
  return 0;
}

struct process_param{
  int logger_pipe;
  const char* pid_file_path; // 出力するPID ファイルへのパス
};

/**
   fork して、ターゲットプロセスの開始と、SIGINT をターゲットプロセスへ送るプロセスへ送る
   ターゲットプロセスが、終了するまで、この関数は制御を返さない

   @return 成功した場合は EXIT_SUCCESS を返す 失敗した場合はそれ以外の値を返す
   @param param start_process へ渡すパラメータをパックした構造体
   @param path 実行ファイルへのパス
   @param argv execvl へ渡す引数の配列。終端を表すために、配列の最後の要素は、NULL で終端されていなければならい
*/
int start_process( struct process_param param,  const char* path , char * argv[])
{
  /* 自分自身のPID を 書き出して、kill -INT に備える ための PID ファイルを作成する */
  /* 書き出すファイルへのパス */
  const char* const pid_file_path = param.pid_file_path;
  {
    /* PID を 書き出すファイルへのファイルディスクリプタ */
    int fd = open( pid_file_path  , O_WRONLY | O_EXCL | O_CREAT , S_IRUSR | S_IWUSR | S_IWOTH );
    if( fd < 0 ){
      perror( "open( pid_file_path  , O_WRONLY | O_EXCL | O_CREAT , S_IRUSR | S_IWUSR | S_IWOTH )");
      return EXIT_FAILURE;
    }else{
      char pidnum[16] = {0}; // 多分 6桁あればいいと思う
      const size_t len = snprintf( pidnum , sizeof( pidnum ) , "%d\n" , (int)(getpid()) );
      if( 0 < len ){
        ssize_t write_result;
        if( len != ( write_result = write( fd , pidnum , len )) ){
          perror("write( fd , pidnum , len )" );
        }else{
          VERIFY(0 == fdatasync( fd ) );
        }
      }
      VERIFY( 0 == close( fd ) );
    }
  }

  int result = EXIT_SUCCESS;

  /* シグナルマスクして fork() してから、
     子 シグナルマスクの解除
     親 sigaction() して、シグナルマスクの解除 処置後に sigaction()
     したほうが多分よい  */
  // シグナルを禁止する。
  sigset_t oldset = {{0}};
  sigset_t sigset = {{0}};
  sigemptyset( &oldset );
  sigemptyset( &sigset );
  VERIFY( 0 == sigaddset(&sigset, SIGCHLD ) );

  if( -1 == sigprocmask( SIG_BLOCK , &sigset, &oldset ) ){
    return EXIT_FAILURE;
  }
  
  const pid_t child_pid = fork();
  if( -1 == child_pid ){
    //const int fork_errno = errno;
    perror( "fork" );
    VERIFY( 0 == sigprocmask( SIG_SETMASK , &oldset , NULL ) );
    result = EXIT_FAILURE;
  }else if( 0 == child_pid ){
    VERIFY( 0 == sigprocmask( SIG_SETMASK , &oldset , NULL ) );
    take_over_for_child_process( param.logger_pipe, path , argv );
    _exit( EXIT_FAILURE );
  }else{
    
    /* このパイプは、親プロセスの中で使うのみである。 */
    int child_pipe[2] = {-1,-1}; /* SIGCHLD をうける self-pipe */
    int intr_pipe[2]  = {-1,-1}; /* SIGINTR をうける self-pipe */
    
    VERIFY( 0 == pipe( child_pipe ) );
    VERIFY( 0 == pipe( intr_pipe ) );
    
    /* int は、 sig_atomic_t に納まる */
    struct type_static_assert{ int expression[ sizeof( sig_atomic_t ) <=  sizeof(int) ? 1 : -1 ]; };
    sig_child_pipe = (sig_atomic_t)child_pipe[WRITE_SIDE];
    sig_intr_pipe  = (sig_atomic_t)intr_pipe[WRITE_SIDE];
    
#if defined( __GNUC__ )
    /* sig_atomic_t への代入が終わったので、ダメ押しで、メモリバリアを張っておく 
       必要は無いはずである。*/
    __sync_synchronize(); 
#endif /* defined( __GNUC__ ) */
    
    /* シグナルハンドラの準備 */
    struct sigaction sig_child_act_store = {{0}};
    struct sigaction sig_intr_act_store = {{0}};
    struct sigaction sig_hup_act_store = {{0}};
    struct sigaction sig_term_act_store = {{0}};
    {
      struct sigaction sig_child_act = {{0}};
      set_signal_handler( & sig_child_act , sig_child_handler );
      VERIFY( 0 == sigaction( SIGCHLD , &sig_child_act , &sig_child_act_store  ));
    }
    {
      struct sigaction sig_intr_act = {{0}};
      set_signal_handler( &sig_intr_act , sig_intr_handler );
      VERIFY( 0 == sigaction( SIGINT , &sig_intr_act , &sig_intr_act_store ));
      VERIFY( 0 == sigaction( SIGHUP , &sig_intr_act , &sig_hup_act_store  ));
      VERIFY( 0 == sigaction( SIGTERM, &sig_intr_act , &sig_term_act_store )); 
    }
    VERIFY( 0 == sigprocmask( SIG_SETMASK , &oldset , NULL ) );
    
    host_daemonlize_process( child_pid , child_pipe[READ_SIDE] , intr_pipe[READ_SIDE] );
    
    VERIFY( 0 == sigaction( SIGHUP , &sig_hup_act_store , NULL ) );
    VERIFY( 0 == sigaction( SIGINT , &sig_intr_act_store ,NULL) );        
    VERIFY( 0 == sigaction( SIGCHLD , &sig_child_act_store ,NULL ) );
    VERIFY( 0 == sigaction( SIGTERM , &sig_term_act_store , NULL ));
    sig_child_pipe = (sig_atomic_t)-1;
    sig_intr_pipe  = (sig_atomic_t)-1;
#if defined( __GNUC__ )
    /* sig_atomic_t への代入が終わったので、ダメ押しで、メモリバリアを張っておく 
       必要は無いはずである。*/
    __sync_synchronize(); 
#endif /* defined( __GNUC__ ) */
    VERIFY( 0 == close( child_pipe[WRITE_SIDE] ) );
    VERIFY( 0 == close( child_pipe[READ_SIDE] ) );
    VERIFY( 0 == close( intr_pipe[WRITE_SIDE] ) );
    VERIFY( 0 == close( intr_pipe[READ_SIDE] ));
    result = EXIT_SUCCESS;
  }
  VERIFY( 0 == unlink( pid_file_path ) );
  return result;
}


void exec_logger_process( int readfd )
{
  int null_out = open( "/dev/null" , O_WRONLY );
  int err_fd = dup( STDERR_FILENO );
  if( -1 == fcntl( err_fd , F_SETFD , FD_CLOEXEC )){
    perror( "fcntl( err_fd , F_SETFD , FD_CLOEXEC )" );
  }
  VERIFY( dup2( readfd , STDIN_FILENO ) == STDIN_FILENO );
  VERIFY( dup2( null_out , STDOUT_FILENO ) ==STDOUT_FILENO );
  VERIFY( dup2( null_out , STDERR_FILENO ) == STDERR_FILENO );
  VERIFY( 0 == close( null_out ) );
  if( -1 == execl( "/usr/bin/logger" ,
                   "/usr/bin/logger" , "-t" , "daemonlize" , "-i" , NULL ) ){
    int err = errno;
    char buffer[80];
    strerror_r( err , buffer , sizeof( buffer )/ sizeof( buffer[0] ));
    write( err_fd , buffer , strlen( buffer ) );
    VERIFY( 0 == close( err_fd ) );
    abort();
  }
  return;
}

void print_help_text(const char* self_path)
{
  fprintf( stdout, "%s daemonlize_program [daemonlize_program_args...]\n" , self_path );
  return;
}

int entry_point( int argc , char* argv[] )
{
  if( ! ( 1 < argc ) ){
    /* オプションが足りない */
    print_help_text(argv[0]);
    return EXIT_SUCCESS;
  }

  /* まず一段階目のfork では SIGCHLD を 無視する  */
  {
    struct sigaction sa = {{0}}; 
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_NOCLDWAIT;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
      return EXIT_FAILURE;
    }
  }

  {
    const pid_t pid = fork();
    if( pid < 0 ){ // fork fail.
      perror( "fork faild" );
      return EXIT_FAILURE;
    }
    
    if( 0 != pid ){
      return EXIT_SUCCESS;
    }

    /* セッショングループを作り直して端末グループから外れる  */
    assert( 0 == pid && "the process is child process.");
    if( -1 == setsid() ){
      perror("create new session");
    }
  }

  /* パイプを作成する */
  int logger_pipes[2] = {-1,-1};
  if( pipe( logger_pipes ) ){
    perror( "pipe()" );
    return EXIT_FAILURE;
  }

  assert( 0 <=logger_pipes[0] );
  assert( 0 <=logger_pipes[1] );

  /* ログを書きだす先の プロセスを作成する */
  const pid_t logger_pid = fork();
  
  if( -1 == logger_pid ){
    perror( "fork() faild");
    VERIFY( 0 == close( logger_pipes[WRITE_SIDE] ));
    VERIFY( 0 == close( logger_pipes[READ_SIDE] ));
    abort();
  }

  if( 0 == logger_pid){
    VERIFY( 0 == close( logger_pipes[WRITE_SIDE] ));
    exec_logger_process( logger_pipes[READ_SIDE] );
    return EXIT_FAILURE;
  }else{
    VERIFY( 0 == close( logger_pipes[READ_SIDE] ));

    /* 標準入力を /dev/null に置き換える */
    {
      int null_in = open( "/dev/null" , O_RDONLY );
      if( -1 == null_in ){
        return EXIT_FAILURE;
      }
      int null_out = open( "/dev/null" , O_WRONLY );
      if( -1 == null_out ){
        return EXIT_FAILURE;
      }
      int stdin_dup  = dup( STDIN_FILENO );
      int stdout_dup = dup( STDOUT_FILENO );
      int stderr_dup = dup( STDERR_FILENO );
      VERIFY( dup2( null_in , STDIN_FILENO ) == STDIN_FILENO );
      VERIFY( dup2( null_out , STDOUT_FILENO ) == STDOUT_FILENO);
      VERIFY( dup2( null_out , STDERR_FILENO ) == STDERR_FILENO );
      
      VERIFY( 0 == close( stdin_dup  ) );
      VERIFY( 0 == close( stdout_dup ) );
      VERIFY( 0 == close( stderr_dup ) );
      
      VERIFY( 0 == close( null_in ));
      VERIFY( 0 == close( null_out ));
    }
    
    struct process_param param = { logger_pipes[WRITE_SIDE] ,NULL};

    char* pid_file_path = malloc( sizeof(char) * PATH_MAX );

    if( pid_file_path ){
      // TODO ここの PID_FILE_PATH の作り方、もうちょっと注意が必要 
      char *p = strrchr( argv[0] , '/' ); 
      if( p ){
        p++;
        p = (('\0' == *p) ? NULL : p);
      }else{
        p = argv[0];
      }
      VERIFY( 0 < snprintf( pid_file_path, sizeof( char ) * PATH_MAX , "/tmp/%s.pid" ,  (p)?(p): argv[0] ) );
      param.pid_file_path = pid_file_path;
      
      const size_t params_len = argc;
      char**params = malloc( sizeof(char*) * params_len );
      
      if( params ){
        // params に strdup で引数を積んでいく
        for( size_t i = 0 ; i < (params_len -1); ++i ){
          params[i] = strdup( argv[ i + 1] );
        }
        params[params_len - 1] = NULL;
        
        if( EXIT_SUCCESS != start_process(param, params[0], params ) ){
          // TODO spawn 失敗した
        }
        
        for( size_t i = 0;  i < params_len ; ++i ){
          if( params[i] ){
            free( params[i] );
          }
        }
        free( params );  
      }
      free( pid_file_path );
    }

    VERIFY( 0 == close( logger_pipes[WRITE_SIDE] ));
  }
  return EXIT_SUCCESS;
}

int main(int argc, char* argv[] )
{
  VERIFY( NULL != setlocale(LC_ALL, "" ) );
  return entry_point( argc , argv );
}
