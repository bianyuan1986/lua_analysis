/*
** $Id: ltable.c,v 2.118 2016/11/07 12:38:35 roberto Exp $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/

#define ltable_c
#define LUA_CORE

#include "lprefix.h"


/*
** Implementation of tables (aka arrays, objects, or hash tables).
** Tables keep its elements in two parts: an array part and a hash part.
** Non-negative integer keys are all candidates to be kept in the array
** part. The actual size of the array is the largest 'n' such that
** more than half the slots between 1 and n are in use.
** Hash uses a mix of chained scatter table with Brent's variation.
** A main invariant of these tables is that, if an element is not
** in its main position (i.e. the 'original' position that its hash gives
** to it), then the colliding element is in its own main position.
** Hence even when the load factor reaches 100%, performance remains good.
*/

#include <math.h>
#include <limits.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lvm.h"


/*
** Maximum size of array part (MAXASIZE) is 2^MAXABITS. MAXABITS is
** the largest integer such that MAXASIZE fits in an unsigned int.
*/
#define MAXABITS	cast_int(sizeof(int) * CHAR_BIT - 1)
#define MAXASIZE	(1u << MAXABITS)

/*
** Maximum size of hash part is 2^MAXHBITS. MAXHBITS is the largest
** integer such that 2^MAXHBITS fits in a signed int. (Note that the
** maximum number of elements in a table, 2^MAXABITS + 2^MAXHBITS, still
** fits comfortably in an unsigned int.)
*/
#define MAXHBITS	(MAXABITS - 1)


#define hashpow2(t,n)		(gnode(t, lmod((n), sizenode(t))))

#define hashstr(t,str)		hashpow2(t, (str)->hash)
#define hashboolean(t,p)	hashpow2(t, p)
#define hashint(t,i)		hashpow2(t, i)


/*
** for some types, it is better to avoid modulus by power of 2, as
** they tend to have many 2 factors.
*/
#define hashmod(t,n)	(gnode(t, ((n) % ((sizenode(t)-1)|1))))


#define hashpointer(t,p)	hashmod(t, point2uint(p))


#define dummynode		(&dummynode_)

static const Node dummynode_ = {
  {NILCONSTANT},  /* value */
  {{NILCONSTANT, 0}}  /* key */
};


/*
** Hash for floating-point numbers.
** The main computation should be just
**     n = frexp(n, &i); return (n * INT_MAX) + i
** but there are some numerical subtleties.
** In a two-complement representation, INT_MAX does not has an exact
** representation as a float, but INT_MIN does; because the absolute
** value of 'frexp' is smaller than 1 (unless 'n' is inf/NaN), the
** absolute value of the product 'frexp * -INT_MIN' is smaller or equal
** to INT_MAX. Next, the use of 'unsigned int' avoids overflows when
** adding 'i'; the use of '~u' (instead of '-u') avoids problems with
** INT_MIN.
*/
#if !defined(l_hashfloat)
static int l_hashfloat (lua_Number n) {
  int i;
  lua_Integer ni;
  n = l_mathop(frexp)(n, &i) * -cast_num(INT_MIN);
  if (!lua_numbertointeger(n, &ni)) {  /* is 'n' inf/-inf/NaN? */
    lua_assert(luai_numisnan(n) || l_mathop(fabs)(n) == cast_num(HUGE_VAL));
    return 0;
  }
  else {  /* normal case */
    unsigned int u = cast(unsigned int, i) + cast(unsigned int, ni);
    return cast_int(u <= cast(unsigned int, INT_MAX) ? u : ~u);
  }
}
#endif


/*
** returns the 'main' position of an element in a table (that is, the index
** of its hash value)
*/
static Node *mainposition (const Table *t, const TValue *key) {
  /*按照不同类型计算出hash值并获取对应的hash节点*/
  switch (ttype(key)) {
    case LUA_TNUMINT:
      return hashint(t, ivalue(key));
    case LUA_TNUMFLT:
      return hashmod(t, l_hashfloat(fltvalue(key)));
    case LUA_TSHRSTR:
      return hashstr(t, tsvalue(key));
    case LUA_TLNGSTR:
      return hashpow2(t, luaS_hashlongstr(tsvalue(key)));
    case LUA_TBOOLEAN:
      return hashboolean(t, bvalue(key));
    case LUA_TLIGHTUSERDATA:
      return hashpointer(t, pvalue(key));
    case LUA_TLCF:
      return hashpointer(t, fvalue(key));
    default:
      lua_assert(!ttisdeadkey(key));
      return hashpointer(t, gcvalue(key));
  }
}


/*
** returns the index for 'key' if 'key' is an appropriate key to live in
** the array part of the table, 0 otherwise.
*/
static unsigned int arrayindex (const TValue *key) {
  if (ttisinteger(key)) {
  	/*获取key值*/
    lua_Integer k = ivalue(key);
	/*可以存放到数组中*/
    if (0 < k && (lua_Unsigned)k <= MAXASIZE)
      return cast(unsigned int, k);  /* 'key' is an appropriate array index */
  }
  return 0;  /* 'key' did not match some condition */
}


/*
** returns the index of a 'key' for table traversals. First goes all
** elements in the array part, then elements in the hash part. The
** beginning of a traversal is signaled by 0.
*/
static unsigned int findindex (lua_State *L, Table *t, StkId key) {
  unsigned int i;
  if (ttisnil(key)) return 0;  /* first iteration */
  i = arrayindex(key);
  if (i != 0 && i <= t->sizearray)  /* is 'key' inside array part? */
    return i;  /* yes; that's the index */
  else {
    int nx;
    Node *n = mainposition(t, key);
    for (;;) {  /* check whether 'key' is somewhere in the chain */
      /* key may be dead already, but it is ok to use it in 'next' */
      if (luaV_rawequalobj(gkey(n), key) ||
            (ttisdeadkey(gkey(n)) && iscollectable(key) &&
             deadvalue(gkey(n)) == gcvalue(key))) {
        i = cast_int(n - gnode(t, 0));  /* key index in hash table */
        /* hash elements are numbered after array ones */
        return (i + 1) + t->sizearray;
      }
      nx = gnext(n);
      if (nx == 0)
        luaG_runerror(L, "invalid key to 'next'");  /* key not found */
      else n += nx;
    }
  }
}


int luaH_next (lua_State *L, Table *t, StkId key) {
  unsigned int i = findindex(L, t, key);  /* find original element */
  for (; i < t->sizearray; i++) {  /* try first array part */
    if (!ttisnil(&t->array[i])) {  /* a non-nil value? */
      setivalue(key, i + 1);
      setobj2s(L, key+1, &t->array[i]);
      return 1;
    }
  }
  for (i -= t->sizearray; cast_int(i) < sizenode(t); i++) {  /* hash part */
    if (!ttisnil(gval(gnode(t, i)))) {  /* a non-nil value? */
      setobj2s(L, key, gkey(gnode(t, i)));
      setobj2s(L, key+1, gval(gnode(t, i)));
      return 1;
    }
  }
  return 0;  /* no more elements */
}


/*
** {=============================================================
** Rehash
** ==============================================================
*/

/*
** Compute the optimal size for the array part of table 't'. 'nums' is a
** "count array" where 'nums[i]' is the number of integers in the table
** between 2^(i - 1) + 1 and 2^i. 'pna' enters with the total number of
** integer keys in the table and leaves with the number of keys that
** will go to the array part; return the optimal size.
*/
static unsigned int computesizes (unsigned int nums[], unsigned int *pna) {
  int i;
  unsigned int twotoi;  /* 2^i (candidate for optimal size) */
  unsigned int a = 0;  /* number of elements smaller than 2^i */
  unsigned int na = 0;  /* number of elements to go to array part */
  unsigned int optimal = 0;  /* optimal size for array part */
  /* loop while keys can fill more than half of total size */
  /*twotoi每次扩大一倍，pna大于twotoi的一般时循环终止*/
  for (i = 0, twotoi = 1; *pna > twotoi / 2; i++, twotoi *= 2) {
    if (nums[i] > 0) {
      a += nums[i];
	  /*要添加到数组部分的节点数大于最大容量的一半则为最佳选择*/
      if (a > twotoi/2) {  /* more than half elements present? */
        optimal = twotoi;  /* optimal size (till now) */
		/*要添加到数组部分的节点数目*/
        na = a;  /* all elements up to 'optimal' will go to array part */
      }
    }
  }
  lua_assert((optimal == 0 || optimal / 2 < na) && na <= optimal);
  /*更新pna*/
  *pna = na;
  /*数组部分大小*/
  return optimal;
}


static int countint (const TValue *key, unsigned int *nums) {
  unsigned int k = arrayindex(key);
  /*可以存放到数组中，统计到对应的区间中[2^(lg-1),2^lg]*/
  if (k != 0) {  /* is 'key' an appropriate array index? */
    nums[luaO_ceillog2(k)]++;  /* count as such */
    return 1;
  }
  else
    return 0;
}


/*
** Count keys in array part of table 't': Fill 'nums[i]' with
** number of keys that will go into corresponding slice and return
** total number of non-nil keys.
*/
static unsigned int numusearray (const Table *t, unsigned int *nums) {
  int lg;
  unsigned int ttlg;  /* 2^lg */
  unsigned int ause = 0;  /* summation of 'nums' */
  unsigned int i = 1;  /* count to traverse all array keys */
  /* traverse each slice */
  /*统计数组中元素在2^(lg-1)和2^lg范围内的个数*/
  for (lg = 0, ttlg = 1; lg <= MAXABITS; lg++, ttlg *= 2) {
    unsigned int lc = 0;  /* counter */
    unsigned int lim = ttlg;
  	/*统计完毕*/
    if (lim > t->sizearray) {
      lim = t->sizearray;  /* adjust upper limit */
      if (i > lim)
        break;  /* no more elements to count */
    }
    /* count elements in range (2^(lg - 1), 2^lg] */
	/*当前区间的元素个数*/
    for (; i <= lim; i++) {
      if (!ttisnil(&t->array[i-1]))
        lc++;
    }
    nums[lg] += lc;
    ause += lc;
  }
  /*数组中总的元素个数*/
  return ause;
}


static int numusehash (const Table *t, unsigned int *nums, unsigned int *pna) {
  int totaluse = 0;  /* total number of elements */
  int ause = 0;  /* elements added to 'nums' (can go to array part) */
  int i = sizenode(t);
  while (i--) {
    Node *n = &t->node[i];
    if (!ttisnil(gval(n))) {
	  /*统计hash节点到对应的区间[2^(lg-1),2^lg]*/
      ause += countint(gkey(n), nums);
      totaluse++;
    }
  }
  *pna += ause;
  return totaluse;
}


static void setarrayvector (lua_State *L, Table *t, unsigned int size) {
  unsigned int i;
  /*分配size*sizeof(TValue)大小的内存*/
  luaM_reallocvector(L, t->array, t->sizearray, size, TValue);
  /*初始化每个节点*/
  for (i=t->sizearray; i<size; i++)
     setnilvalue(&t->array[i]);
  t->sizearray = size;
}


static void setnodevector (lua_State *L, Table *t, unsigned int size) {
  /*size为0，暂时不分配hash节点*/
  if (size == 0) {  /* no elements to hash part? */
  	/*暂时指向全局的dummynode_*/
    t->node = cast(Node *, dummynode);  /* use common 'dummynode' */
    t->lsizenode = 0;
    t->lastfree = NULL;  /* signal that it is using dummy node */
  }
  /*指定了size*/
  else {
    int i;
	/*查表获取2^lsize>=size*/
    int lsize = luaO_ceillog2(size);
    if (lsize > MAXHBITS)
      luaG_runerror(L, "table overflow");
	/*获取hash表容量大小，该值大于等于传递进来的size*/
    size = twoto(lsize);
	/*分配size*sizeof(Node)大小的内存*/
    t->node = luaM_newvector(L, size, Node);
	/*初始化每个hash节点*/
    for (i = 0; i < (int)size; i++) {
      Node *n = gnode(t, i);
      gnext(n) = 0;
      setnilvalue(wgkey(n));
      setnilvalue(gval(n));
    }
    t->lsizenode = cast_byte(lsize);
    t->lastfree = gnode(t, size);  /* all positions are free */
  }
}

/*重新调整Table大小，nasize为指定的数组部分大小，nhsize为指定的hash部分大小*/
void luaH_resize (lua_State *L, Table *t, unsigned int nasize,
                                          unsigned int nhsize) {
  unsigned int i;
  int j;
  /*获取数组大小*/
  unsigned int oldasize = t->sizearray;
  /*获取hash部分大小*/
  int oldhsize = allocsizenode(t);
  Node *nold = t->node;  /* save old hash ... */
  /*数组部分需要扩大，重新分配内存*/
  if (nasize > oldasize)  /* array part must grow? */
    setarrayvector(L, t, nasize);
  /* create new hash part with appropriate size */
  /*分配hash部分内存并初始化*/
  setnodevector(L, t, nhsize);
  /*需要缩减数组部分*/
  if (nasize < oldasize) {  /* array part must shrink? */
    t->sizearray = nasize;
    /* re-insert elements from vanishing slice */
    for (i=nasize; i<oldasize; i++) {
      if (!ttisnil(&t->array[i]))
        luaH_setint(L, t, i + 1, &t->array[i]);
    }
    /* shrink array */
    luaM_reallocvector(L, t->array, oldasize, nasize, TValue);
  }
  /* re-insert elements from hash part */
  for (j = oldhsize - 1; j >= 0; j--) {
    Node *old = nold + j;
    if (!ttisnil(gval(old))) {
      /* doesn't need barrier/invalidate cache, as entry was
         already present in the table */
      setobjt2t(L, luaH_set(L, t, gkey(old)), gval(old));
    }
  }
  if (oldhsize > 0)  /* not the dummy node? */
    luaM_freearray(L, nold, cast(size_t, oldhsize)); /* free old hash */
}


void luaH_resizearray (lua_State *L, Table *t, unsigned int nasize) {
  int nsize = allocsizenode(t);
  luaH_resize(L, t, nasize, nsize);
}

/*
** nums[i] = number of keys 'k' where 2^(i - 1) < k <= 2^i
*/
static void rehash (lua_State *L, Table *t, const TValue *ek) {
  unsigned int asize;  /* optimal size for array part */
  unsigned int na;  /* number of keys in the array part */
  unsigned int nums[MAXABITS + 1];
  int i;
  int totaluse;
  for (i = 0; i <= MAXABITS; i++) nums[i] = 0;  /* reset counts */
  /*统计数组中元素在各个[2^(lg-1),2^lg]区间的个数并保存到nums中，返回数组中总元素个数*/
  na = numusearray(t, nums);  /* count keys in array part */
  totaluse = na;  /* all those keys are integer keys */
  /*把hash部分节点划分到不同的[2^(lg-1),2^lg]区间中*/
  totaluse += numusehash(t, nums, &na);  /* count keys in hash part */
  /* count extra key */
  na += countint(ek, nums);
  totaluse++;
  /* compute new size for array part */
  /*重新计算数组部分大小，nums中保存了节点在各个区间段的分布，asize为计算后的
  数组部分容量，na为要添加到数组部分的节点数目*/
  asize = computesizes(nums, &na);
  /* resize the table to new computed sizes */
  /*asize为数组部分容量大小，totaluse-na为总结点数减去要添加到数组部分的节点数目*/
  luaH_resize(L, t, asize, totaluse - na);
}



/*
** }=============================================================
*/


Table *luaH_new (lua_State *L) {
  /*分配一个Table对象，转换为GCObject对象添加到链表global_State->allgc链表*/
  GCObject *o = luaC_newobj(L, LUA_TTABLE, sizeof(Table));
  /*转换为Table类型对象*/
  Table *t = gco2t(o);
  t->metatable = NULL;
  t->flags = cast_byte(~0);
  t->array = NULL;
  t->sizearray = 0;
  /*初始化Table*/
  setnodevector(L, t, 0);
  return t;
}


void luaH_free (lua_State *L, Table *t) {
  if (!isdummy(t))
    luaM_freearray(L, t->node, cast(size_t, sizenode(t)));
  luaM_freearray(L, t->array, t->sizearray);
  luaM_free(L, t);
}


static Node *getfreepos (Table *t) {
  if (!isdummy(t)) {
  	/*从后向前查找最后一个空闲位置*/
    while (t->lastfree > t->node) {
      t->lastfree--;
	  /*该位置为空*/
      if (ttisnil(gkey(t->lastfree)))
        return t->lastfree;
    }
  }
  return NULL;  /* could not find a free place */
}



/*
** inserts a new key into a hash table; first, check whether key's main
** position is free. If not, check whether colliding node is in its main
** position or not: if it is not, move colliding node to an empty place and
** put new key in its main position; otherwise (colliding node is in its main
** position), new key goes to an empty position.
*/
/*添加新的节点到hash表中*/
TValue *luaH_newkey (lua_State *L, Table *t, const TValue *key) {
  Node *mp;
  TValue aux;
  if (ttisnil(key)) luaG_runerror(L, "table index is nil");
  /*float类型*/
  else if (ttisfloat(key)) {
    lua_Integer k;
    if (luaV_tointeger(key, &k, 0)) {  /* does index fit in an integer? */
      setivalue(&aux, k);
      key = &aux;  /* insert it as an integer */
    }
    else if (luai_numisnan(fltvalue(key)))
      luaG_runerror(L, "table index is NaN");
  }
  /*根据不同类型计算出hash值并返回对应的hash节点*/
  mp = mainposition(t, key);
  /*主位置被占用*/
  if (!ttisnil(gval(mp)) || isdummy(t)) {  /* main position is taken? */
    Node *othern;
	/*从后向前查找最后一个空闲位置*/
    Node *f = getfreepos(t);  /* get a free place */
  	/*找不到空闲位置，扩大hash表然后再次插入*/
    if (f == NULL) {  /* cannot find a free place? */
      rehash(L, t, key);  /* grow table */
      /* whatever called 'newkey' takes care of TM cache */
	  /*插入后直接返回*/
      return luaH_set(L, t, key);  /* insert key into grown table */
    }
    lua_assert(!isdummy(t));
	/*找到空闲位置，然后根据目前主位置中存储的节点mp的key查找对应的主位置othern*/
    othern = mainposition(t, gkey(mp));
	/*othern不等于mp，表明冲突节点不在主位置上*/
    if (othern != mp) {  /* is colliding node out of its main position? */
      /* yes; move colliding node into free position */
	  /*当前主位置存储的冲突节点不在对应的主位置，说明该冲突节点的主位置被占用了，而冲突节点
	  被链接到该链表中，此时从链表头othern开始查找，找到当前冲突节点的前一个节点。为什么不直接
	  在结构体中添加prev成员，减少每次的遍历*/
      while (othern + gnext(othern) != mp)  /* find previous */
        othern += gnext(othern);
	  /*把找到的空闲位置插入到链表中，即mp前一个节点和mp之间*/
      gnext(othern) = cast_int(f - othern);  /* rechain to point to 'f' */
	  /*把冲突节点的内容拷贝到空闲位置*/
      *f = *mp;  /* copy colliding node into free pos. (mp->next also goes) */
	  /*mp不是尾节点，还需要校正f的next值*/
      if (gnext(mp) != 0) {
	  	/*因为next为偏移值，此时的f保存了从mp拷贝过来的next值，此时只需再添加f相对于mp的偏移即可*/
        gnext(f) += cast_int(mp - f);  /* correct 'next' */
		/*冲突节点已经移动到空闲位置，主位置节点可用*/
        gnext(mp) = 0;  /* now 'mp' is free */
      }
      setnilvalue(gval(mp));
    }
	/*冲突节点在主位置上*/
    else {  /* colliding node is in its own main position */
      /* new node will go into free position */
	  /*存在下一个节点*/
      if (gnext(mp) != 0)
	  	/*当前节点插入到f，计算f相对于mp节点的下一个节点的偏移，相当于把f链接到
	    mp和mp下一个节点之间*/
        gnext(f) = cast_int((mp + gnext(mp)) - f);  /* chain new position */
      else 
	  	/*不存在下一个节点，相当于把f插入到尾部，next为0*/
	  	lua_assert(gnext(f) == 0);
	  /*mp的下一个节点为f，把冲突节点链接起来，next为下个节点相对于当前节点的偏移值*/
      gnext(mp) = cast_int(f - mp);
	  /*返回的mp指向f，相当于把新插入节点插入到了上边找到的空闲位置*/
      mp = f;
    }
  }
  /*拷贝key中value到mp->i_key中，此时的mp可能是在冲突节点存在于主位置的情况下找到的空闲节点，也可能是
  该key对应的主位置节点*/
  setnodekey(L, &mp->i_key, key);
  luaC_barrierback(L, t, key);
  lua_assert(ttisnil(gval(mp)));
  /*返回该节点中保存的TValue类型的值*/
  return gval(mp);
}


/*
** search function for integers
*/
/*根据key在table中查找返回value*/
const TValue *luaH_getint (Table *t, lua_Integer key) {
  /* (1 <= key && key <= t->sizearray) */
  /*直接作为索引从数组中访问对应节点*/
  if (l_castS2U(key) - 1 < t->sizearray)
    return &t->array[key - 1];
  else {
  	/*计算hash值，key%t->lsizenode*/
    Node *n = hashint(t, key);
    for (;;) {  /* check whether 'key' is somewhere in the chain */
	  /*找到该节点，key相等*/
      if (ttisinteger(gkey(n)) && ivalue(gkey(n)) == key)
	  	/*返回TValue类型的值*/
        return gval(n);  /* that's it */
      else {
	  	/*继续向后查找，nx为偏移*/
        int nx = gnext(n);
        if (nx == 0) break;
        n += nx;
      }
    }
    return luaO_nilobject;
  }
}


/*
** search function for short strings
*/
const TValue *luaH_getshortstr (Table *t, TString *key) {
  Node *n = hashstr(t, key);
  lua_assert(key->tt == LUA_TSHRSTR);
  for (;;) {  /* check whether 'key' is somewhere in the chain */
    const TValue *k = gkey(n);
    if (ttisshrstring(k) && eqshrstr(tsvalue(k), key))
      return gval(n);  /* that's it */
    else {
      int nx = gnext(n);
      if (nx == 0)
        return luaO_nilobject;  /* not found */
      n += nx;
    }
  }
}


/*
** "Generic" get version. (Not that generic: not valid for integers,
** which may be in array part, nor for floats with integral values.)
*/
static const TValue *getgeneric (Table *t, const TValue *key) {
  Node *n = mainposition(t, key);
  for (;;) {  /* check whether 'key' is somewhere in the chain */
  	/*判断两个key对象是否相等*/
    if (luaV_rawequalobj(gkey(n), key))
	  /*找到节点，返回value*/
      return gval(n);  /* that's it */
	/*匹配下一个节点*/
    else {
      int nx = gnext(n);
      if (nx == 0)
        return luaO_nilobject;  /* not found */
      n += nx;
    }
  }
}


const TValue *luaH_getstr (Table *t, TString *key) {
  /*key为短字符串*/
  if (key->tt == LUA_TSHRSTR)
    return luaH_getshortstr(t, key);
  /*key为长字符串*/
  else {  /* for long strings, use generic case */
    TValue ko;
	/*使用key初始化一个TString类型的对象ko*/
    setsvalue(cast(lua_State *, NULL), &ko, key);
    return getgeneric(t, &ko);
  }
}


/*
** main search function
*/
const TValue *luaH_get (Table *t, const TValue *key) {
  switch (ttype(key)) {
    case LUA_TSHRSTR: return luaH_getshortstr(t, tsvalue(key));
    case LUA_TNUMINT: return luaH_getint(t, ivalue(key));
    case LUA_TNIL: return luaO_nilobject;
    case LUA_TNUMFLT: {
      lua_Integer k;
      if (luaV_tointeger(key, &k, 0)) /* index is int? */
        return luaH_getint(t, k);  /* use specialized version */
      /* else... */
    }  /* FALLTHROUGH */
    default:
      return getgeneric(t, key);
  }
}


/*
** beware: when using this function you probably need to check a GC
** barrier and invalidate the TM cache.
*/
/*使用key查找，未找到则插入新节点*/
TValue *luaH_set (lua_State *L, Table *t, const TValue *key) {
  const TValue *p = luaH_get(t, key);
  if (p != luaO_nilobject)
    return cast(TValue *, p);
  else return luaH_newkey(L, t, key);
}


void luaH_setint (lua_State *L, Table *t, lua_Integer key, TValue *value) {
  /*根据key从table中查找元素*/
  const TValue *p = luaH_getint(t, key);
  TValue *cell;
  /*找到*/
  if (p != luaO_nilobject)
    cell = cast(TValue *, p);
  /*未找到*/
  else {
    TValue k;
	/*把整数值key保存到k中*/
    setivalue(&k, key);
  	/*添加新节点到hash表中*/
    cell = luaH_newkey(L, t, &k);
  }
  /**cell=*value，保存value到节点中*/
  setobj2t(L, cell, value);
}


static int unbound_search (Table *t, unsigned int j) {
  unsigned int i = j;  /* i is zero or a present index */
  j++;
  /* find 'i' and 'j' such that i is present and j is not */
  while (!ttisnil(luaH_getint(t, j))) {
    i = j;
    if (j > cast(unsigned int, MAX_INT)/2) {  /* overflow? */
      /* table was built with bad purposes: resort to linear search */
      i = 1;
      while (!ttisnil(luaH_getint(t, i))) i++;
      return i - 1;
    }
    j *= 2;
  }
  /* now do a binary search between them */
  while (j - i > 1) {
    unsigned int m = (i+j)/2;
    if (ttisnil(luaH_getint(t, m))) j = m;
    else i = m;
  }
  return i;
}


/*
** Try to find a boundary in table 't'. A 'boundary' is an integer index
** such that t[i] is non-nil and t[i+1] is nil (and 0 if t[1] is nil).
*/
int luaH_getn (Table *t) {
  unsigned int j = t->sizearray;
  if (j > 0 && ttisnil(&t->array[j - 1])) {
    /* there is a boundary in the array part: (binary) search for it */
    unsigned int i = 0;
    while (j - i > 1) {
      unsigned int m = (i+j)/2;
      if (ttisnil(&t->array[m - 1])) j = m;
      else i = m;
    }
    return i;
  }
  /* else must find a boundary in hash part */
  else if (isdummy(t))  /* hash part is empty? */
    return j;  /* that is easy... */
  else return unbound_search(t, j);
}



#if defined(LUA_DEBUG)

Node *luaH_mainposition (const Table *t, const TValue *key) {
  return mainposition(t, key);
}

int luaH_isdummy (const Table *t) { return isdummy(t); }

#endif
