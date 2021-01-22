/*
** $Id: lmem.c,v 1.91 2015/03/06 19:45:54 roberto Exp $
** Interface to Memory Manager
** See Copyright Notice in lua.h
*/

#define lmem_c
#define LUA_CORE

#include "lprefix.h"


#include <stddef.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"



/*
** About the realloc function:
** void * frealloc (void *ud, void *ptr, size_t osize, size_t nsize);
** ('osize' is the old size, 'nsize' is the new size)
**
** * frealloc(ud, NULL, x, s) creates a new block of size 's' (no
** matter 'x').
**
** * frealloc(ud, p, x, 0) frees the block 'p'
** (in this specific case, frealloc must return NULL);
** particularly, frealloc(ud, NULL, 0, 0) does nothing
** (which is equivalent to free(NULL) in ISO C)
**
** frealloc returns NULL if it cannot create or reallocate the area
** (any reallocation to an equal or smaller size cannot fail!)
*/



#define MINSIZEARRAY	4


/*数组扩容*/
void *luaM_growaux_ (lua_State *L, void *block, int *size, size_t size_elems,
                     int limit, const char *what) {
  void *newblock;
  int newsize;
  /*当前容量已经超过最大限制的1/2*/
  if (*size >= limit/2) {  /* cannot double it? */
    if (*size >= limit)  /* cannot grow even a little? */
      luaG_runerror(L, "too many %s (limit is %d)", what, limit);
	/*扩容为最大限制*/
    newsize = limit;  /* still have at least one free place */
  }
  /*扩大为原来的两倍*/
  else {
    newsize = (*size)*2;
    if (newsize < MINSIZEARRAY)
      newsize = MINSIZEARRAY;  /* minimum size */
  }
  /*重新分配内存*/
  newblock = luaM_reallocv(L, block, *size, newsize, size_elems);
  /*更新容量*/
  *size = newsize;  /* update only when everything else is OK */
  return newblock;
}


l_noret luaM_toobig (lua_State *L) {
  luaG_runerror(L, "memory allocation error: block too big");
}



/*
** generic allocation routine.
*/
/*分配新的内存块，内存大小为nsize*/
void *luaM_realloc_ (lua_State *L, void *block, size_t osize, size_t nsize) {
  void *newblock;
  global_State *g = G(L);
  size_t realosize = (block) ? osize : 0;
  lua_assert((realosize == 0) == (block == NULL));
#if defined(HARDMEMTESTS)
  if (nsize > realosize && g->gcrunning)
    luaC_fullgc(L, 1);  /* force a GC whenever possible */
#endif
  /*实际调用l_alloc，分配nsize大小的内存块，释放老的内存块*/
  newblock = (*g->frealloc)(g->ud, block, osize, nsize);
  /*分配失败*/
  if (newblock == NULL && nsize > 0) {
    lua_assert(nsize > realosize);  /* cannot fail when shrinking a block */
    if (g->version) {  /* is state fully built? */
	  /*垃圾回收*/
      luaC_fullgc(L, 1);  /* try to free some memory... */
	  /*尝试重新分配内存*/
      newblock = (*g->frealloc)(g->ud, block, osize, nsize);  /* try again */
    }
	/*依然分配失败，异常处理*/
    if (newblock == NULL)
      luaD_throw(L, LUA_ERRMEM);
  }
  lua_assert((nsize == 0) == (newblock == NULL));
  /*更新内存统计*/
  g->GCdebt = (g->GCdebt + nsize) - realosize;
  return newblock;
}

