#pragma once
#if !defined( VERIFY_H_HEADER_GUARD )
#define VERIFY_H_HEADER_GUARD 1
/**
   VERIFY macro definition 

 */
#if defined(  __cplusplus )
#include <cassert>
#else /* defined(  __cplusplus ) */
#include <assert.h>
#endif /* defined(  __cplusplus ) */

#if !defined( VERIFY )
#if defined( NDEBUG )
#define VERIFY( exp ) (exp)
#else /* defined( NDEBUG ) */
#define VERIFY( exp ) assert( exp )
#endif /* defined( NDEBUG ) */
#endif /* !defined( VERIFY ) */

#endif /* defined( VERIFY_H_HEADER_GUARD ) */
