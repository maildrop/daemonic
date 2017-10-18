#include "config.h"
#include <unistd.h>
#include "alternative.h"

/**
   fdatasync は、プラットホーム依存なところがあるので、それの吸収用のプロシージャ
   

   _POSIX_SYNCHRONIZED_IO が定義されていて、 0 より大きい場合
   fdatasync が使えるならば、fdatasync(fd) を呼び出す そうでない場合は fsync(fd) を呼び出す
   
   _POSIX_SYNCHRONIZED_IO がそれ以外の場合は、
   fsync( fd ) を呼び出す

   TODO: 正しいやり方をしているかどうかチェックが必要
   macOS の場合 fcntl で F_FULLSYNC をかける必要がある。
 */
int x_fdatasync( int fd )
{
  int result = 0;
#if ( defined( _POSIX_SYNCHRONIZED_IO ) && ( 0 < _POSIX_SYNCHRONIZED_IO ) )

# if defined( HAVE_FDATASYNC )
  result = fdatasync( fd );
# else /* defined( HAVE_FDATASYNC ) */
  result = fsync( fd );
# endif /* defined( HAVE_FDATASYNC ) */

#else /* ( defined( _POSIX_SYNCHRONIZED_IO ) && ( _POSIX_SYNCHRONIZED_IO > 0 ) ) */
  result = fsync( fd );
#endif /* ( defined( _POSIX_SYNCHRONIZED_IO ) && ( _POSIX_SYNCHRONIZED_IO > 0 ) ) */
  return result;
}


