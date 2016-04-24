/*
  exec に渡す realpath の処理をテストする。

 */
#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif /* defined(HAVE_CONFIG_H) */

#include <unistd.h>
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

int main(int argc , char* argv[] )
{
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
  return EXIT_SUCCESS;
}

 
