/*
  exec に渡す realpath の処理をテストする。

  TODO：
  fork_and_exec したときの SIGCHLD の取扱を追加するべき。
 */
#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif /* defined(HAVE_CONFIG_H) */

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "verify.h"

#if ( _POSIX_C_SOURCE < 200809L )
#error you must use compiler option -D_XOPEN_SOURCE=700
#endif /* ( _POSIX_C_SOURCE < 200809L ) */

/* realpath(3) require */
/* _BSD_SOURCE || _XOPEN_SOURCE >= 500 || _XOPEN_SOURCE && _XOPEN_SOURCE_EXTENDED */

/* パイプの読み出し側 書き込み側のシンボル */
enum{
  READ_SIDE = 0,
  WRITE_SIDE = 1,
};

/**
   pathの最大長( 通常は、PATH_MAX の値 ) を問い合わせる
   注意する点は二点あって、 
   1) pathconf( "." , _PC_PATH_MAX ) は無制限を意味する負の値を返すことがある
   2) POSIX.1-2001 以前は、終端の NULL文字について言及されていないので、余裕をもってNULL文字分を追加する必要がある。
   

   @return pathconf(3) の戻り値の値 無制限を意味する -1 が返されることがある。
   @param lentgh パスの最大長あるいは最大値として適当な値を戻す size_t へのポインタ この値には、NULL 文字が含まれた値が返る。
   この値は、malloc に食わせることを目的とする。pathconf が実際に無制限を意味する -1を返したかどうかの確認は、
   戻り値が -1 を返したかどうかを確認すること。
 */
int pathconf_path_max( size_t* length );

/**
   正規化されたパスを返す。
   この戻り値は、malloc(3) で確保されたヒープを指しているので、free(3)で解放する必要がある。
 */
char* get_canonical_path( const char* path );

/**
   引数 fd で渡された ファイルディスクリプタに対して、
   O_CLOEXEC を追加で付与する。
   戻り値は、成功時には、0 を戻し、失敗時には、-1 になる。
   失敗して、-1 を返した時には、 errno に失敗の理由が保持される。
 */
int fcntl_set_close_exec( int fd );

/**
   perror の ファイルディスクリプタとエラー番号指定したバージョン
   @return 成功時には 0 を返す、失敗時はそれ以外を返す。 失敗時の理由は errno に保存される。
   @param fd 書き込み先のファイルディスクリプタ
   @param errnum エラー番号
   @param msg 追加のメッセージ
 */
int fdperror( int fd ,int errnum ,const char* msg );

/************************************************
 * 実装
 ************************************************/

int pathconf_path_max( size_t* length )
{
#if defined( PATH_MAX )
  const int pathconf_result = PATH_MAX ;
#else /* defined( PATH_MAX ) */
  const int pathconf_result = pathconf("." , _PC_PATH_MAX );
#endif /* defined( PATH_MAX ) */
  if( length ){
    const size_t require_size = (size_t)( ( pathconf_result < 0 ) ? 4096 : pathconf_result );
    /* 
       Before POSIX.1-2001 , we aren't guaranteed that PATH_MAX includees the terminating NULL byte('\0'),
       Same goes for XPG3 ,
       see APUE 2.5 Limits. , 2.5.5 Indeterminate RuntimeLimits. 
    */
    *length =  ( ( ( sysconf( _SC_VERSION ) < 200112L ) && ( sysconf( _SC_XOPEN_VERSION ) < 4 ) ) ?
                 require_size + 1 :
                 require_size  );
    
  }
  return pathconf_result;
}

char* get_canonical_path( const char* path )
{
  size_t path_max = 0; /* PATH_MAX の値が保持されるsize_t 値 */

  const int pathconf_result = pathconf_path_max( &path_max );
  assert( 0 < pathconf_result );
  (void)( pathconf_result );
  
  if( path_max < 0 ){ // 取得に失敗したので
    /* pathconf_path_max() が設定した errno をそのまま使う */
    return NULL;
  }
  
  /* pathconf_path_max() は、終端NULL文字を含む数をかえす */
  char* canonical_path = malloc( sizeof(char)  * path_max );
  if( canonical_path == NULL ){ /* malloc に失敗した */
    return NULL;
  }

  if( NULL == realpath( path , canonical_path ) ){
    const int errno_realpath = errno; 
    free( canonical_path );
    errno = errno_realpath;
    return NULL;
  }
  return canonical_path;
}

int fcntl_set_close_exec( int fd )
{ 
  assert( 0 <= fd );
  const int arg = fcntl( fd , F_GETFD );
  if( arg == -1 ){
#if !defined( NDEBUG )
    const int stored_errno = errno ;
    perror( "fcntl( fd , F_GETFD )");
    errno = stored_errno;
#endif /* !defined( NDEBUG ) */
    return -1;
  }
  unsigned int mod_arg = (unsigned int) arg;
  mod_arg |= O_CLOEXEC;
  const int fcntl_result = fcntl( fd , F_SETFD , (int)mod_arg );
  if( -1 == fcntl_result ){
#if !defined( NDEBUG )
    const int stored_errno = errno;
    perror( "fcntl( fd , F_SETFD , O_CLOEXEC )" );
    errno = stored_errno;
#endif /* !defined( NDEBUG ) */
    return -1;
  }
  return fcntl_result;
}

int fdperror( int fd ,int errnum ,const char* msg )
{
  assert( 0 <= fd );
  if( NULL == msg ){
    static const char emptystr[] = "";
    msg = emptystr;
  }
  const int errno_store = errno;
  errno = 0;
#if (_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600) && ! _GNU_SOURCE

  size_t error_message_len = 1024;
  char*  error_message = malloc( sizeof( char )* error_message_len );
  if( error_message ){
    for(;;){
      /* strerror_r は仕様が特殊で、成功時に 0 を返し、失敗時には 
         glibc 2.13 より前では、-1を戻し、errno を設定して戻る
         glibc 2.13 より後では、「戻り値に」 エラー番号を正の値として返す。 */
      const int strerr_errno = strerror_r( errnum , error_message , error_message_len ) ;
      if( 0 == strerr_errno ){ // 成功
        break;  
      }else{ // 失敗
        assert( ERANGE == ( strerr_errno == -1 ? errno : strerr_errno ) );
        if( ERANGE == ( strerr_errno == -1 ? errno : strerr_errno ) ){
          error_message_len *= 2;
          char * const realloc_error_message = realloc( error_message , error_message_len);
          const int realloc_errno = errno;
          VERIFY( realloc_error_message );
          if( realloc_error_message ){
            error_message = realloc_error_message;
          }else{
            free( error_message );
            errno = realloc_errno;
            return -1;
          }
        }else{
          const int store = errno;
          free( error_message );
          errno = ( strerr_errno == -1 ? store : strerr_errno );
          return -1;
        }
      }
    }
    
    static const char space[] = " ";
    static const char terminate[] = "\n";
    const size_t write_size =  strlen( error_message ) + strlen( space ) + strlen( msg ) +  strlen( terminate );
    const size_t bufsize = write_size+1;
    char* buf = malloc( sizeof( char ) * bufsize );
    if( buf ){
      VERIFY( buf == strncpy( buf , error_message, bufsize ) );
      VERIFY( buf == strncat( buf , space , bufsize - strlen( buf ) ));
      VERIFY( buf == strncat( buf , msg , bufsize-strlen( buf ) ) );
      VERIFY( buf == strncat( buf , terminate , bufsize - strlen( buf ) ));
      assert( write_size == strlen( buf ) );
      VERIFY( write_size == write( fd , buf , write_size ));
      free( buf );
    }
    free( error_message );
  }
#else /* (_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600) && ! _GNU_SOURCE */
#error use strerror_r(3) XSI-comiliant version. see strerror_r(3) manual.
#endif /* (_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600) && ! _GNU_SOURCE */
  errno = errno_store;
  return 0;
}

/**
   fork_and_exec{v,vp} の実装本体
 */
static pid_t fork_and_exec( int (*exec)( const char* , char * const [] ) , const char* path ,  char* const argv[] );
static pid_t fork_and_exec_do( int (*exec)( const char* , char * const [] ) , const char* path , char* const argv[] );


/**
   fork して、exec する

 */
static pid_t fork_and_exec_do( int (*exec)( const char* , char * const [] ) , const char* path , char* const argv[] )
{
  int pipefd[2] = { -1 , -1 };
  if( -1 == pipe( pipefd  ) ){
    perror( "pipe()" );
  }
  assert( 0 <= pipefd[READ_SIDE] );
  assert( 0 <= pipefd[WRITE_SIDE] );
  
  /* 
     書き込み側 は、O_CLOEXEC を付けておく
     こうすることで、exec ファミリを呼び出したときに、
     自動的に、書き込み側が閉じられるので、
     親プロセス側では、read(2) で待っていると、
     
     exec が成功したら0byteのread に成功し、
     （つまり 書き込み側が exec に成功して、閉じたことを意味し）
     exec が失敗した時には、そのerrno が親プロセス側で検知することができる。
  */
  int fcntl_result = -1;
  VERIFY( 0 == ( fcntl_result = fcntl_set_close_exec( pipefd[ WRITE_SIDE ] ) ));
  
  pid_t result = -1; /* この関数の戻り値 */

  if( 0 != fcntl_result ){
    goto ERROR_HANDLE_PIPE;
  }
  
  sigset_t oldset = {{0}};
  VERIFY( 0 == sigemptyset( &oldset ) ); // TODO 既にブロックされているシグナルを考慮していないので修正が必要

  { /* SIGCHLD をマスクして fork() に備える */
    sigset_t sigset = {{0}};
    VERIFY( 0 == sigemptyset( &sigset ) );
    VERIFY( 0 == sigaddset( &sigset , SIGCHLD ) );
    if( 0 != sigprocmask( SIG_BLOCK , &sigset , &oldset ) ){
      perror("sigprocmask( SIG_BLOCK , &sigset , &oldset )");
      goto ERROR_HANDLE_PIPE;
    }
  }

  const pid_t pid = fork();
  if( -1 == pid ){ /* fork() error */
    perror("fork()");
    goto ERROR_HANDLE_SIGNAL;
  }

  /********************** TODO **********************/
  /* 
    何をするのか 
    このfork() と関係のない子プロセスが終了した時に
    SIGCHLD が発生するが、それを確実に補足するように変更する責任がある。
    あれ？ 回数数えるだけでいいんだから、self-pipe 使う必要なくない？ 
  */
#if 0 /* 多分こんなかんじ */
  do{
    int suspended_sigchld_pipefd[2] = {-1,-1};
    if( 0 == pipe( suspended_sigchld_pipefd ) ){
      assert( -1 != suspended_sigchld_pipefd[WRITE_SIDE] ); 
      assert( -1 != suspended_sigchld_pipefd[READ_SIDE]  );

      for(;;){
        int status;
        const pid_t waitpid_result = waitpid( pid , &status , WNOHANG );
        if( -1 == waitpid_result && EINTR == errno ){
          /* waitpid(2) でプロセスが止まっているときに、シグナルを受信し、waitpid() が制御を戻した */
          continue;
        }
        
        if( -1 == waitpid_result ){ // エラーが発生した。
          const int waitpid_errno = errno; // waitpid() が -1 を返して エラーを通知してきたときの errno 
          VERIFY( EINVAL != waitpid_errno );
          VERIFY( ECHILD != waitpid_errno );
          abort(); // 関知しないエラー？ どうなっているのか確認する必要がある。
        }else if( waitpid_result == pid  ){  // 当該のプロセスが終了した。
          // ファイルディスクリプタのチェック
          break;
        }else if( 0 == waitpid_result ){ // 当該のプロセスはまだ生きている
          // ということは、後で、raise(SIGCHLD) を発行する必要がある。
          char b = 0;
          assert( -1 != suspended_sigchld_pipefd[ WRITE_SIDE ] );
          write( suspended_sigchld_pipefd[WRITE_SIDE] , &b , sizeof( b ) );
        }
      } // end of for(;;)

      /************************************************/
      /******  TODO シグナルハンドラをもとに外す ******/
      /******  わすれないようにしましょう        ******/
      /************************************************/
      
      VERIFY( 0 == close( suspended_sigchld_pipefd[WRITE_SIDE] )) ; // 書き込み側を閉じる
      suspended_sigchld_pipefd[WRITE_SIDE] = -1; /* パイプのファイルディスクリプタ が後で閉じたことを確認するために-1 を指定しておく*/
      
      // サスペンドされたシグナルの回数分 raise する
      char b = 0;
      while( 0 < read( suspended_sigchld_pipefd[READ_SIDE] , &b , sizeof( b ) ) ){ // おかしい。 -1 を返してきたときのことが考えられていない。
        if( 0 != raise( SIGCHLD ) ){
          // raise(3) に失敗した時に注意 まだ、読み取ってないSIGCHLD がありますよ
          while( 0 < read( suspended_sigchld_pipefd[READ_SIDE] , &b , sizeof( b ) )){
            ;
          }
          break;
        }
      }
      VERIFY( 0 == close( suspended_sigchld_pipefd[READ_SIDE]  )); // 読み込み側も閉じる

      suspended_sigchld_pipefd[READ_SIDE] = -1; /* 安全のため */
    }

    assert( -1 == suspended_sigchld_pipefd[WRITE_SIDE] );
    assert( -1 == suspended_sigchld_pipefd[READ_SIDE] );

    if( -1 != suspended_sigchld_pipefd[WRITE_SIDE] ){
      VERIFY( 0 == close( suspended_sigchld_pipefd[WRITE_SIDE] ) );
    }
    if( -1 != suspended_sigchld_pipefd[READ_SIDE] ){
      VERIFY( 0 == close( suspended_sigchld_pipefd[READ_SIDE] ) );
    }
  }while( (void)0, 0 ); // end of do-while 
#endif /* 多分こんなかんじ */
  
  if( 0 == pid ){
    VERIFY( 0 == close( pipefd[ READ_SIDE ] ) );
    /* 子プロセス側の シグナルブロックを解除する */
    VERIFY( 0 == sigprocmask( SIG_SETMASK , &oldset , NULL ) );

    if( -1 == exec( path , argv) ){
      /* execl 失敗した時には、失敗した理由 errno を パイプに書き込んで終了する。*/
      const int b = errno;
      VERIFY( sizeof( b ) == write( pipefd[WRITE_SIDE] , &b , sizeof( b ) ) );
      VERIFY( 0 == close( pipefd[WRITE_SIDE] ) );
      _exit(EXIT_SUCCESS);
    }
  }else{
    VERIFY( 0 == close( pipefd[ WRITE_SIDE ] ) );
    /* 親プロセス側のシグナルブロックを解除する */
    VERIFY( 0 == sigprocmask( SIG_SETMASK , &oldset , NULL ) );

    /* TODO sigaction() */
    for(;;){
      /** select で、処理する必要がある */
      int read_errno = 0; /* パイプから読み取ったエラー番号値 */
      const ssize_t read_v = read( pipefd[READ_SIDE] , &read_errno  , sizeof(read_errno) ); 
      // read_v は 負を返す場合があるのに注意
      if( -1 == read_v ){
        if( EINTR == errno ){
          continue;
        }
        perror("read( pipefd[READ_SIDE] , &b  , sizeof(b) )");
        abort();
      }
      
      if( read_v == 0 ){
        errno = 0;
      }else if( read_v == sizeof( read_errno ) ){
        /* exec に失敗した時には、関数内で プロセスの回収を行う */
        {
          int status = 0;
          VERIFY( pid == waitpid( pid , &status , 0 ) );
          VERIFY( EXIT_SUCCESS == status );
        }
        /* エラーメッセージの出力 */
        fdperror( STDERR_FILENO , read_errno, "exec()" );
        errno = read_errno;
      } /* end of if ( read_v == sizeof( b ) ) */
      break;
    } /* end of for(;;) */
    
    // TODO  sigaction を元に戻す
    // raise() を使用して、 suspend された sigaction をもっかい投げる 
    VERIFY( 0 == close( pipefd[ READ_SIDE ] ) );
  }
  return result;

 ERROR_HANDLE_SIGNAL:
  VERIFY( 0 == sigprocmask( SIG_SETMASK , &oldset , NULL ) );
 ERROR_HANDLE_PIPE:
  VERIFY( 0 == close( pipefd[WRITE_SIDE] ) ); 
  VERIFY( 0 == close( pipefd[READ_SIDE] ) );
  return -1;
}


static pid_t fork_and_exec( int (* const exec)( const char* , char * const [] ) ,
                            const char* exec_path , char* const argv[] )
{
  /*
     TODO: 
     * 関数の戻り値についての仕様をもうはっきりさせる必要がある。
     * 関数自体が長すぎる 
     * path が不適当なときにどうするかのチェック
     * argv のチェック？ できる？
     * 作成中に、戻り値が、int だったり pid_t だったりして、安定していない。
     多分 fork の戻り値 pid_t をベースに考えるのが良いと思う。
     * SIGCHLD の動作を考慮していない。 sigaction の動作を加えて考えること。
     * 既に開いている ファイルディスクリプタの操作がまだ。
     * 戻り値の pid_t の意味と、errno の状態についてまとめておくこと。
   */
  assert( exec );
  if( exec ){
    return -1;
  }

  /* 
     引数のパスを絶対パスに変換して、exec_path へ
     この引数パス展開措置は、もうすこし考慮が必要
   */

  char * path = get_canonical_path( exec_path );
  if( NULL == path ){
    return -1;
  }else{
    pid_t pid = fork_and_exec_do( exec , path , argv );
    free( path );
    return pid;
  }
}


/**

 */
int fork_and_execv( const char* path , char* const argv[] );

int fork_and_execv( const char* path , char* const argv[] )
{
  return fork_and_exec( execv , path , argv );
}

#if 0
/**
   環境変数 PATH の方は考えなくてもいいかもというか多分これ使わないほうがいい。
 */
int fork_and_execvp( const char* path , char* const argv[] );

int fork_and_execvp( const char* path , char* const argv[] )
{
  return fork_and_exec( execvp , path , argv );
}
#endif /* 0 */


#include <locale.h>

pid_t sample_fork_and_exec(){
  char* argv[] = {"/bin/true" , NULL };
  //TODO 標準入出力の処理 とシグナルの処理 を追加する
  return fork_and_exec( execv , "/bin/true" , argv );
}

int main(int argc , char* argv[] )
{
  VERIFY( NULL != setlocale(LC_ALL , ""  ) );

  printf( "sysconf( _SC_VERSION )       = %ldL (%ld)\n" ,sysconf( _SC_VERSION) , _POSIX_VERSION );
  printf( "sysconf( _SC_XOPEN_VERSION ) = %ldL \n" , sysconf( _SC_XOPEN_VERSION ) );
  VERIFY( 0 == fdperror( STDERR_FILENO , 0 , "fdperror()" ) );
  {
    // CHECK pathconf は 負の数を返すことがある。
    size_t path_max = 
      ( ( ( sysconf( _SC_VERSION ) < 200112L ) && ( sysconf( _SC_XOPEN_VERSION ) < 4 ) )?
        ( pathconf( "/" , _PC_PATH_MAX ) + 1 ) :
        ( pathconf( "/" , _PC_PATH_MAX ) ) );
    (void) path_max ;
  }
  
  for( size_t i = 0 ; i < argc ; ++i ){
    {
      char *fullpath = get_canonical_path( argv[i] );
      if( fullpath ){
        printf( "[%zd] %-20s %s\n", i , argv[i], (fullpath)?fullpath : "?(null)" );
        free( fullpath );
        continue;
      }
    }
    const int err = errno;
    
    {
      static const char separator[] = ". : ";
      {
        int length = 0;
        {
          char buffer[80];
          length = strerror_r( err , buffer , sizeof( buffer ) / sizeof( buffer[0] ) );
          if( length < ( sizeof( buffer ) / sizeof(buffer[0] )) ){
            strncat( buffer , separator , sizeof( buffer )/ sizeof( buffer[0]) );
            strncat( buffer , argv[i] , sizeof( buffer )/ sizeof( buffer[0]) );
            printf( "[%zd] %-20s %s\n", i , argv[i], buffer );
            continue;
          }
        }
          
        {
          const size_t alloc_size = sizeof( separator ) + strlen( argv[i] ) + length   ;
          char* alloc_buffer = malloc( alloc_size +1);
          if( alloc_buffer ){
            strerror_r( err , alloc_buffer , alloc_size );
            strncat( alloc_buffer , separator , alloc_size);
            strncat( alloc_buffer , argv[i] , alloc_size );
            printf( "[%zd] %-20s %s\n", i , argv[i], alloc_buffer );
            free( alloc_buffer );
          }
          continue;
        }
      }
    }
  }

  // fork_and_exec() って関数にしよう
  
  {
    int pipefd[2] = { -1 , -1 };
    if( -1 == pipe( pipefd  ) ){
      perror( "pipe()" );
    }
    assert( 0 <= pipefd[READ_SIDE] );
    assert( 0 <= pipefd[WRITE_SIDE] );

    /* 
       書き込み側 は、O_CLOEXEC を付けておく
       こうすることで、exec ファミリを呼び出したときに、
       自動的に、書き込み側が閉じられるので、
       親プロセス側では、read(2) で待っていると、
       
       exec が成功したら0byteのread に成功されて、
       （つまり 書き込み側が exec に成功して、閉じたことを意味し）
       exec が失敗した時には、そのerrno が親プロセス側で検知することができる。
     */
    int fcntl_result = -1;
    VERIFY( 0 == ( fcntl_result = fcntl_set_close_exec( pipefd[ WRITE_SIDE ] ) ));
    if( 0 != fcntl_result ){
      perror( "fcntl()" );
      abort();
    }

    const char path[] = "/bin/true";
    const pid_t pid = fork();
    switch( pid ){
    case -1 :
      {
        abort();
        break;
      }
    case 0:
      {
        if( 0 != close( pipefd[ READ_SIDE ] )  ){
          perror( "close()" );
        }
        if( -1 == execl( path , path , NULL ) ){
          /* execl 失敗した時には、失敗した理由 errno を パイプに書き込んで終了する。*/
          int b = errno;
          VERIFY( sizeof( b ) == write( pipefd[WRITE_SIDE] , &b , sizeof( b ) ) );
          VERIFY( 0 == close( pipefd[WRITE_SIDE] ) );
          _exit(EXIT_SUCCESS);
        }
        break;
      }
    default:
      {
        if( 0 != close( pipefd[ WRITE_SIDE ] ) ){
          perror( "close()" );
        }
        do{
          // select で、
          int b = 0;
          const ssize_t read_v = read( pipefd[READ_SIDE] , &b  , sizeof(b) );
          //printf( "read_v = %zd\n" , read_v );
          if( read_v == sizeof( b ) ){
            { // 終了ステータスを得る。
              int status = 0;
              VERIFY( pid == waitpid( pid , &status , 0 ) );
              if( WIFEXITED( status ) ){
                VERIFY( EXIT_SUCCESS == WEXITSTATUS(status) );
              }
            }
            fdperror( STDERR_FILENO , b , "" );
            errno = b;
          }
        }while( (void)0, 0 );
        VERIFY( 0 == close( pipefd[ READ_SIDE ] ) );
        break;
      }
    }
  }
  return EXIT_SUCCESS;
}
