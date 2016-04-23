/**
   daemonlize の動作テストのための SIGINT を受けると、プロセスを
   終了させる簡単なプログラム
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>

#if !defined( VERIFY )
#if defined( NDEBUG )
#define VERIFY( exp ) (exp)
#else /* defined( NDEBUG ) */
#define VERIFY( exp ) assert( exp )
#endif /* defined( NDEBUG ) */
#endif /* !defined( VERIFY ) */

static void sig_interrupt(int sig){
  return;
};

int main(int argc ,char* argv[]){
  struct sigaction sig_intterupt_act = {{0}};
  struct sigaction sig_intterupt_act_store = {{0}};
  sig_intterupt_act.sa_handler = sig_interrupt;
  // sig_intterupt_act.sa_sigaction = NULL;
  VERIFY( 0 == sigemptyset( & sig_intterupt_act.sa_mask ) );
  sig_intterupt_act.sa_flags = 0;

  if(! sigaction( SIGINT, &sig_intterupt_act , &sig_intterupt_act_store  ) ){
    puts( "enter pause()" );
    VERIFY( 0 == fflush( stdout ) );
    if( -1 == pause() ){
      if( EINTR == errno ){
        printf( "EINTR\n" );
        VERIFY( 0 == fflush( stdout ) );
      }
    }
    puts( "leave pause()" );
    VERIFY( 0 == fflush( stdout ) );
    VERIFY( 0 == sigaction( SIGINT , &sig_intterupt_act_store , NULL ) );
  }
  return 0;
}
