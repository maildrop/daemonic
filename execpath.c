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

int pathconf_path_max( size_t* length );
char* get_absolute_path( const char* path );

/************************************************

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

// なんか名前がいけてないな。
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

/* 
   引数 fd で渡された ファイルディスクリプタに対して、
   O_CLOEXEC を追加で付与する。
   戻り値は、成功時には、0 を戻し、失敗時には、-1 になる。
   失敗して、-1 を返した時には、 errno に失敗の理由が保持される。
 */
int fcntl_set_close_exec( int fd );

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


/**
   fork_and_exec{v,vp} の実装本体
   
 */
static pid_t fork_and_exec( int (*exec)( const char* , char * const [] ) , const char* path ,  char* const argv[] );

static pid_t fork_and_exec( int (* const exec)( const char* , char * const [] ) , const char* path ,  char* const argv[] )
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
  if( exec ){ return -1; } // もうちょい 引数チェックを実行的に realpath 使うとか ?
  
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
  
  pid_t result = -1; // この関数の戻り値

  if( 0 != fcntl_result ){
    return -1;
  }
  const pid_t pid = fork();
  
  switch( pid ){
  case -1 :
    {
      abort(); // TODO:  abort() すると pipefd がリークする。
      break;
    }
  case 0:
    {
      if( 0 != close( pipefd[ READ_SIDE ] )  ){
        perror( "close()" );
      }
      if( -1 == exec( path , argv) ){
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
        if( read_v == 0 ){
          errno = 0;
        }else if( read_v == sizeof( b ) ){
          /* exec に失敗した時には、関数内で プロセスの回収を行う */
          {
            int status = 0;
            VERIFY( pid == waitpid( pid , &status , 0 ) );
            VERIFY( EXIT_SUCCESS == status );
          }
          /* エラーメッセージの出力 */
#if defined(  _GNU_SOURCE )
#error use strerror_r(3) XSI-comiliant version. see strerror_r(3) manual.
#endif/* defined(  _GNU_SOURCE ) */
          char err_message[80];
          if( 0 == strerror_r( b , err_message , sizeof( err_message ) / sizeof( err_message[0] ) ) ){
            do{
              {
                if( -1 == write( STDERR_FILENO , err_message , strlen( err_message ) ) ){
                  break;
                }
              }
              {
                const char space[] = " ";
                if( -1 == write( STDERR_FILENO , space , strlen( space ) ) ){
                  break;
                }
              }
              {
                if( -1 == write( STDERR_FILENO , path , strlen( path ) ) ){
                  break;
                }
              }
              {
                const char terminate[] = "\n";
                if( -1 == write( STDERR_FILENO , terminate , strlen( terminate ) ) ){
                  break;
                }
              }
            }while( (void)0,0 );
            sync();
          }
          errno = b;
        } /* end of if ( read_v == sizeof( b ) ) */
      }while( (void)0, 0 );
      VERIFY( 0 == close( pipefd[ READ_SIDE ] ) );
      break; 
    }
  } // end of switch( pid )
  return result;
}



/**

 */
int fork_and_execv( const char* path , char* const argv[] );

int fork_and_execv( const char* path , char* const argv[] )
{
  return fork_and_exec( execv , path , argv );
}

/**

 */
int fork_and_execvp( const char* path , char* const argv[] );

int fork_and_execvp( const char* path , char* const argv[] )
{
  return fork_and_exec( execvp , path , argv );
}


#include <locale.h>

int main(int argc , char* argv[] )
{
  VERIFY( NULL != setlocale(LC_ALL , ""  ) );

  printf( "sysconf( _SC_VERSION )       = %ldL (%ld)\n" ,sysconf( _SC_VERSION) , _POSIX_VERSION );
  printf( "sysconf( _SC_XOPEN_VERSION ) = %ldL \n" , sysconf( _SC_XOPEN_VERSION ) );

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
      char *fullpath = get_absolute_path( argv[i] );
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

    if( 0 == fcntl_result ){
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
              /* exec に失敗した時には、関数内で プロセスの回収を行う */
              {
                int status = 0;
                VERIFY( pid == waitpid( pid , &status , 0 ) );
                VERIFY( EXIT_SUCCESS == status );
              }
              /* エラーメッセージの出力 */
#if defined(  _GNU_SOURCE )
#error use strerror_r(3) XSI-comiliant version. see strerror_r(3) manual.
#endif/* defined(  _GNU_SOURCE ) */
              char err_message[80];
              if( 0 == strerror_r( b , err_message , sizeof( err_message ) / sizeof( err_message[0] ) ) ){
                do{
                  {
                    if( -1 == write( STDERR_FILENO , err_message , strlen( err_message ) ) ){
                      break;
                    }
                  }
                  {
                    const char space[] = " ";
                    if( -1 == write( STDERR_FILENO , space , strlen( space ) ) ){
                      break;
                    }
                  }
                  {
                    if( -1 == write( STDERR_FILENO , path , strlen( path ) ) ){
                      break;
                    }
                  }
                  {
                    const char terminate[] = "\n";
                    if( -1 == write( STDERR_FILENO , terminate , strlen( terminate ) ) ){
                      break;
                    }
                  }
                }while( (void)0,0 );
                sync();
              }
              errno = b;
            }
          }while( (void)0, 0 );
          VERIFY( 0 == close( pipefd[ READ_SIDE ] ) );
          break;
        }
      }
    }
    
  }
  
  return EXIT_SUCCESS;
}

 
