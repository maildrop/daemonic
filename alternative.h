#if ! defined( ALTERNATIVE_H_HEADER_GUARD )
#define ALTERNATIVE_H_HEADER_GUARD 1

/**
   fdatasync の OS 依存wrapper
 */
int x_fdatasync( int fd );

#endif /* ALTERNATIVE_H_HEADER_GUARD */
