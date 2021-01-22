/*
** $Id: lapi.c,v 2.259 2016/02/29 14:27:14 roberto Exp $
** Lua API
** See Copyright Notice in lua.h
*/

#define lapi_c
#define LUA_CORE

#include "lprefix.h"


#include <stdarg.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"



const char lua_ident[] =
  "$LuaVersion: " LUA_COPYRIGHT " $"
  "$LuaAuthors: " LUA_AUTHORS " $";


/* value at a non-valid index */
#define NONVALIDVALUE		cast(TValue *, luaO_nilobject)

/* corresponding test */
#define isvalid(o)	((o) != luaO_nilobject)

/* test for pseudo index */
#define ispseudo(i)		((i) <= LUA_REGISTRYINDEX)

/* test for upvalue */
#define isupvalue(i)		((i) < LUA_REGISTRYINDEX)

/* test for valid but not pseudo index */
#define isstackindex(i, o)	(isvalid(o) && !ispseudo(i))

#define api_checkvalidindex(l,o)  api_check(l, isvalid(o), "invalid index")

#define api_checkstackindex(l, i, o)  \
	api_check(l, isstackindex(i, o), "index not in the stack")


/*
<case 1>
(gdb) print o[0]    //实际为压入栈中做为pmain参数的argc
$51 = {value_ = {gc = 0x2, p = 0x2, b = 2, f = 0x2, i = 2, n = 9.8813129168249309e-324}, tt_ = 19}
(gdb) bt
#0  index2addr (L=0x644018, idx=1) at lapi.c:65
#1  0x0000000000405d78 in lua_tointegerx (L=0x644018, idx=1, pisnum=0x0) at lapi.c:358
#2  0x0000000000404f67 in pmain (L=0x644018) at lua.c:555
#3  0x000000000040aedf in luaD_precall (L=0x644018, func=0x644640, nresults=1) at ldo.c:434
#4  0x000000000040b26c in luaD_call (L=0x644018, func=0x644640, nResults=1) at ldo.c:498
#5  0x000000000040b2d9 in luaD_callnoyield (L=0x644018, func=0x644640, nResults=1) at ldo.c:509
#6  0x000000000040791e in f_call (L=0x644018, ud=0x7fffffffe300) at lapi.c:943
#7  0x000000000040a284 in luaD_rawrunprotected (L=0x644018, f=0x4078e9 <f_call>, ud=0x7fffffffe300) at ldo.c:142
#8  0x000000000040ba89 in luaD_pcall (L=0x644018, func=0x4078e9 <f_call>, u=0x7fffffffe300, old_top=16, ef=0) at ldo.c:729
#9  0x00000000004079ec in lua_pcallk (L=0x644018, nargs=2, nresults=1, errfunc=0, ctx=0, k=0x0) at lapi.c:969
#10 0x0000000000405206 in main (argc=2, argv=0x7fffffffe448) at lua.c:606

*/
/*根据索引从栈中获取对象*/
static TValue *index2addr (lua_State *L, int idx) {
  /*获取当前的函数调用*/
  CallInfo *ci = L->ci;
  /*<case 1>，获取传递给函数的参数*/
  if (idx > 0) {
  	/*获取参数，idx相对于func的偏移，实际相当于获取压栈的参数*/
    TValue *o = ci->func + idx;
    api_check(L, idx <= ci->top - (ci->func + 1), "unacceptable index");
    if (o >= L->top) return NONVALIDVALUE;
    else return o;
  }
  /*LUA_REGISTRYINDEX < idx <= 0，因为idx小于0，所以top+idx向低地址处走，idx为相对于栈顶的索引*/
  else if (!ispseudo(idx)) {  /* negative index */
    api_check(L, idx != 0 && -idx <= L->top - (ci->func + 1), "invalid index");
    return L->top + idx;
  }
  /*idx == LUA_REGISTRYINDEX，返回全局表*/
  else if (idx == LUA_REGISTRYINDEX)
  	/*直接返回Table*/
    return &G(L)->l_registry;
  /*idx < LUA_REGISTRYINDEX*/
  else {  /* upvalues */
  	/*idx小于LUA_REGISTRYINDEX，所以相减后得到一个正的偏移，获取upvalue*/
    idx = LUA_REGISTRYINDEX - idx;
    api_check(L, idx <= MAXUPVAL + 1, "upvalue index too large");
    if (ttislcf(ci->func))  /* light C function? */
      return NONVALIDVALUE;  /* it has no upvalues */
    else {
      CClosure *func = clCvalue(ci->func); /*闭包函数*/
      return (idx <= func->nupvalues) ? &func->upvalue[idx-1] : NONVALIDVALUE; /*获取upvalue值*/
    }
  }
}


/*
** to be called by 'lua_checkstack' in protected mode, to grow stack
** capturing memory errors
*/
static void growstack (lua_State *L, void *ud) {
  int size = *(int *)ud;
  luaD_growstack(L, size);
}


LUA_API int lua_checkstack (lua_State *L, int n) {
  int res;
  CallInfo *ci = L->ci;
  lua_lock(L);
  api_check(L, n >= 0, "negative 'n'");
  if (L->stack_last - L->top > n)  /* stack large enough? */
    res = 1;  /* yes; check is OK */
  else {  /* no; need to grow stack */
    int inuse = cast_int(L->top - L->stack) + EXTRA_STACK;
    if (inuse > LUAI_MAXSTACK - n)  /* can grow without overflow? */
      res = 0;  /* no */
    else  /* try to grow stack */
      res = (luaD_rawrunprotected(L, &growstack, &n) == LUA_OK);
  }
  if (res && ci->top < L->top + n)
    ci->top = L->top + n;  /* adjust frame top */
  lua_unlock(L);
  return res;
}


LUA_API void lua_xmove (lua_State *from, lua_State *to, int n) {
  int i;
  if (from == to) return;
  lua_lock(to);
  api_checknelems(from, n);
  api_check(from, G(from) == G(to), "moving among independent states");
  api_check(from, to->ci->top - to->top >= n, "stack overflow");
  from->top -= n;
  for (i = 0; i < n; i++) {
    setobj2s(to, to->top, from->top + i);
    to->top++;  /* stack already checked by previous 'api_check' */
  }
  lua_unlock(to);
}


LUA_API lua_CFunction lua_atpanic (lua_State *L, lua_CFunction panicf) {
  lua_CFunction old;
  lua_lock(L);
  old = G(L)->panic;
  G(L)->panic = panicf;
  lua_unlock(L);
  return old;
}


LUA_API const lua_Number *lua_version (lua_State *L) {
  static const lua_Number version = LUA_VERSION_NUM;
  if (L == NULL) return &version;
  else return G(L)->version;
}



/*
** basic stack manipulation
*/


/*
** convert an acceptable stack index into an absolute index
*/
/*把可接受的栈索引转换为绝对索引*/
LUA_API int lua_absindex (lua_State *L, int idx) {
  return (idx > 0 || ispseudo(idx))
         ? idx /*idx > 0 || idx <= LUA_REGISTRINDEX，如果是伪索引或者索引大于零则不需要转换，比如获取全局表的索引LUA_REGISTRYINDEX*/
         : cast_int(L->top - L->ci->func) + idx; /*LUA_REGISTRYINDEX < idx <= 0，计算相对于ci->func的相对索引，idx可能是相对于top的一个相对索引*/
}


LUA_API int lua_gettop (lua_State *L) {
  return cast_int(L->top - (L->ci->func + 1));
}


/*修改栈顶*/
LUA_API void lua_settop (lua_State *L, int idx) {
  StkId func = L->ci->func;
  lua_lock(L);
  /*idx大于等于0*/
  if (idx >= 0) {
    api_check(L, idx <= L->stack_last - (func + 1), "new top too large");
    while (L->top < (func + 1) + idx)
      setnilvalue(L->top++);
    L->top = (func + 1) + idx;
  }
  /*idx小于0*/
  else {
    api_check(L, -(idx+1) <= (L->top - (func + 1)), "invalid new top");
    L->top += idx+1;  /* 'subtract' index (index is negative) */
  }
  lua_unlock(L);
}


/*
** Reverse the stack segment from 'from' to 'to'
** (auxiliary to 'lua_rotate')
*/
static void reverse (lua_State *L, StkId from, StkId to) {
  for (; from < to; from++, to--) {
    TValue temp;
    setobj(L, &temp, from);
    setobjs2s(L, from, to);
    setobj2s(L, to, &temp);
  }
}


/*
** Let x = AB, where A is a prefix of length 'n'. Then,
** rotate x n == BA. But BA == (A^r . B^r)^r.
*/
LUA_API void lua_rotate (lua_State *L, int idx, int n) {
  StkId p, t, m;
  lua_lock(L);
  t = L->top - 1;  /* end of stack segment being rotated */
  p = index2addr(L, idx);  /* start of segment */
  api_checkstackindex(L, idx, p);
  api_check(L, (n >= 0 ? n : -n) <= (t - p + 1), "invalid 'n'");
  m = (n >= 0 ? t - n : p - n - 1);  /* end of prefix */
  reverse(L, p, m);  /* reverse the prefix with length 'n' */
  reverse(L, m + 1, t);  /* reverse the suffix */
  reverse(L, p, t);  /* reverse the entire segment */
  lua_unlock(L);
}


LUA_API void lua_copy (lua_State *L, int fromidx, int toidx) {
  TValue *fr, *to;
  lua_lock(L);
  fr = index2addr(L, fromidx);
  to = index2addr(L, toidx);
  api_checkvalidindex(L, to);
  setobj(L, to, fr);
  if (isupvalue(toidx))  /* function upvalue? */
    luaC_barrier(L, clCvalue(L->ci->func), fr);
  /* LUA_REGISTRYINDEX does not need gc barrier
     (collector revisits it before finishing collection) */
  lua_unlock(L);
}


/*idx为索引，相对于栈顶或者函数栈底，根据idx从栈中获取TValue对象然后拷贝内容到栈顶，相当于
根据idx获取参数或者寄存器表或者栈中局部变量拷贝到栈顶*/
LUA_API void lua_pushvalue (lua_State *L, int idx) {
  lua_lock(L);
  /*如果idx为-1则把当前栈顶节点拷贝到L->top中*/
  setobj2s(L, L->top, index2addr(L, idx));
  /*递增栈顶索引*/
  api_incr_top(L);
  lua_unlock(L);
}



/*
** access functions (stack -> C)
*/


LUA_API int lua_type (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (isvalid(o) ? ttnov(o) : LUA_TNONE);
}


LUA_API const char *lua_typename (lua_State *L, int t) {
  UNUSED(L);
  api_check(L, LUA_TNONE <= t && t < LUA_NUMTAGS, "invalid tag");
  return ttypename(t);
}


LUA_API int lua_iscfunction (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (ttislcf(o) || (ttisCclosure(o)));
}


LUA_API int lua_isinteger (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return ttisinteger(o);
}


LUA_API int lua_isnumber (lua_State *L, int idx) {
  lua_Number n;
  const TValue *o = index2addr(L, idx);
  return tonumber(o, &n);
}


LUA_API int lua_isstring (lua_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return (ttisstring(o) || cvt2str(o));
}


LUA_API int lua_isuserdata (lua_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return (ttisfulluserdata(o) || ttislightuserdata(o));
}


LUA_API int lua_rawequal (lua_State *L, int index1, int index2) {
  StkId o1 = index2addr(L, index1);
  StkId o2 = index2addr(L, index2);
  return (isvalid(o1) && isvalid(o2)) ? luaV_rawequalobj(o1, o2) : 0;
}


LUA_API void lua_arith (lua_State *L, int op) {
  lua_lock(L);
  if (op != LUA_OPUNM && op != LUA_OPBNOT)
    api_checknelems(L, 2);  /* all other operations expect two operands */
  else {  /* for unary operations, add fake 2nd operand */
    api_checknelems(L, 1);
    setobjs2s(L, L->top, L->top - 1);
    api_incr_top(L);
  }
  /* first operand at top - 2, second at top - 1; result go to top - 2 */
  luaO_arith(L, op, L->top - 2, L->top - 1, L->top - 2);
  L->top--;  /* remove second operand */
  lua_unlock(L);
}


LUA_API int lua_compare (lua_State *L, int index1, int index2, int op) {
  StkId o1, o2;
  int i = 0;
  lua_lock(L);  /* may call tag method */
  o1 = index2addr(L, index1);
  o2 = index2addr(L, index2);
  if (isvalid(o1) && isvalid(o2)) {
    switch (op) {
      case LUA_OPEQ: i = luaV_equalobj(L, o1, o2); break;
      case LUA_OPLT: i = luaV_lessthan(L, o1, o2); break;
      case LUA_OPLE: i = luaV_lessequal(L, o1, o2); break;
      default: api_check(L, 0, "invalid option");
    }
  }
  lua_unlock(L);
  return i;
}


LUA_API size_t lua_stringtonumber (lua_State *L, const char *s) {
  size_t sz = luaO_str2num(s, L->top);
  if (sz != 0)
    api_incr_top(L);
  return sz;
}


LUA_API lua_Number lua_tonumberx (lua_State *L, int idx, int *pisnum) {
  lua_Number n;
  const TValue *o = index2addr(L, idx);
  int isnum = tonumber(o, &n);
  if (!isnum)
    n = 0;  /* call to 'tonumber' may change 'n' even if it fails */
  if (pisnum) *pisnum = isnum;
  return n;
}

/*返回整数值，并且把是否为整数的判断结果保存到pisnum中*/
LUA_API lua_Integer lua_tointegerx (lua_State *L, int idx, int *pisnum) {
  lua_Integer res;
  /*根据idx获取对应的TValue*/
  const TValue *o = index2addr(L, idx);
  /*获取整数值保存到res中*/
  int isnum = tointeger(o, &res);
  /*非整数*/
  if (!isnum)
    res = 0;  /* call to 'tointeger' may change 'n' even if it fails */
  /*返回是否是整数*/
  if (pisnum) *pisnum = isnum;
  /*返回整数值*/
  return res;
}


LUA_API int lua_toboolean (lua_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return !l_isfalse(o);
}


LUA_API const char *lua_tolstring (lua_State *L, int idx, size_t *len) {
  StkId o = index2addr(L, idx);
  if (!ttisstring(o)) {
    if (!cvt2str(o)) {  /* not convertible? */
      if (len != NULL) *len = 0;
      return NULL;
    }
    lua_lock(L);  /* 'luaO_tostring' may create a new string */
    luaO_tostring(L, o);
    luaC_checkGC(L);
    o = index2addr(L, idx);  /* previous call may reallocate the stack */
    lua_unlock(L);
  }
  if (len != NULL)
    *len = vslen(o);
  return svalue(o);
}


LUA_API size_t lua_rawlen (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttype(o)) {
    case LUA_TSHRSTR: return tsvalue(o)->shrlen;
    case LUA_TLNGSTR: return tsvalue(o)->u.lnglen;
    case LUA_TUSERDATA: return uvalue(o)->len;
    case LUA_TTABLE: return luaH_getn(hvalue(o));
    default: return 0;
  }
}


LUA_API lua_CFunction lua_tocfunction (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  if (ttislcf(o)) return fvalue(o);
  else if (ttisCclosure(o))
    return clCvalue(o)->f;
  else return NULL;  /* not a C function */
}


LUA_API void *lua_touserdata (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttnov(o)) {
    case LUA_TUSERDATA: return getudatamem(uvalue(o));
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}


LUA_API lua_State *lua_tothread (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (!ttisthread(o)) ? NULL : thvalue(o);
}


LUA_API const void *lua_topointer (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttype(o)) {
    case LUA_TTABLE: return hvalue(o);
    case LUA_TLCL: return clLvalue(o);
    case LUA_TCCL: return clCvalue(o);
    case LUA_TLCF: return cast(void *, cast(size_t, fvalue(o)));
    case LUA_TTHREAD: return thvalue(o);
    case LUA_TUSERDATA: return getudatamem(uvalue(o));
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}



/*
** push functions (C -> stack)
*/


LUA_API void lua_pushnil (lua_State *L) {
  lua_lock(L);
  setnilvalue(L->top);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushnumber (lua_State *L, lua_Number n) {
  lua_lock(L);
  setfltvalue(L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushinteger (lua_State *L, lua_Integer n) {
  lua_lock(L);
  setivalue(L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}


/*
** Pushes on the stack a string with given length. Avoid using 's' when
** 'len' == 0 (as 's' can be NULL in that case), due to later use of
** 'memcmp' and 'memcpy'.
*/
LUA_API const char *lua_pushlstring (lua_State *L, const char *s, size_t len) {
  TString *ts;
  lua_lock(L);
  ts = (len == 0) ? luaS_new(L, "") : luaS_newlstr(L, s, len);
  setsvalue2s(L, L->top, ts);
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
  return getstr(ts);
}


LUA_API const char *lua_pushstring (lua_State *L, const char *s) {
  lua_lock(L);
  if (s == NULL)
    setnilvalue(L->top);
  else {
    TString *ts;
	/*获取一个字符串对象*/
    ts = luaS_new(L, s);
  	/*入栈*/
    setsvalue2s(L, L->top, ts);
	/*返回字符串内容*/
    s = getstr(ts);  /* internal copy's address */
  }
  api_incr_top(L); /*递增栈顶指针*/
  luaC_checkGC(L);
  lua_unlock(L);
  return s;
}


LUA_API const char *lua_pushvfstring (lua_State *L, const char *fmt,
                                      va_list argp) {
  const char *ret;
  lua_lock(L);
  ret = luaO_pushvfstring(L, fmt, argp);
  luaC_checkGC(L);
  lua_unlock(L);
  return ret;
}


LUA_API const char *lua_pushfstring (lua_State *L, const char *fmt, ...) {
  const char *ret;
  va_list argp;
  lua_lock(L);
  va_start(argp, fmt);
  ret = luaO_pushvfstring(L, fmt, argp);
  va_end(argp);
  luaC_checkGC(L);
  lua_unlock(L);
  return ret;
}


LUA_API void lua_pushcclosure (lua_State *L, lua_CFunction fn, int n) {
  lua_lock(L);
  /*如果n为0，则保存到栈顶元素中*/
  if (n == 0) {
  	/*把fn保存在L->top中，top指向栈顶，栈中节点类型为TValue，即LUA_TLCF，c函数类型*/
    setfvalue(L->top, fn);
  }
  /*如果n不为0*/
  else {
    CClosure *cl;
	/*检查栈中剩余节点是否大于n*/
    api_checknelems(L, n);
    api_check(L, n <= MAXUPVAL, "upvalue index too large");
	/*创建一个CClosure对象*/
    cl = luaF_newCclosure(L, n);
	/*保存函数指针*/
    cl->f = fn;
	/*把栈中upvalue值移动到闭包函数cl后面，此处的top指向upvalue首节点*/
    L->top -= n;
	/*把upvalue值从栈中拷贝到闭包函数后面*/
    while (n--) {
	  /*拷贝stack中对应TValue节点的内容到CClosure中upvalue节点*/
      setobj2n(L, &cl->upvalue[n], L->top + n);
      /* does not need barrier because closure is white */
    }
	/*把cl对象保存在栈顶，后面接跟着n个upvalue*/
    setclCvalue(L, L->top, cl);
  }
  /*L->top递增*/
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
}


LUA_API void lua_pushboolean (lua_State *L, int b) {
  lua_lock(L);
  setbvalue(L->top, (b != 0));  /* ensure that true is 1 */
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushlightuserdata (lua_State *L, void *p) {
  lua_lock(L);
  setpvalue(L->top, p);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API int lua_pushthread (lua_State *L) {
  lua_lock(L);
  setthvalue(L, L->top, L);
  api_incr_top(L);
  lua_unlock(L);
  return (G(L)->mainthread == L);
}



/*
** get functions (Lua -> stack)
*/

/*从table t中查找key k，并把返回的结果入栈保存*/
static int auxgetstr (lua_State *L, const TValue *t, const char *k) {
  const TValue *slot;
  /*查找k字符串是否存在，如果不存在则创建并加入cache，实际作用是根据字符串k创建字符串对象*/
  TString *str = luaS_new(L, k);
  /*在hash表t中查找字符串对象str是否存在，即key k是否存在，slot保存查找结果*/
  if (luaV_fastget(L, t, str, slot, luaH_getstr)) {
	/*k存在，保存slot到L->top中，即入栈*/
    setobj2s(L, L->top, slot);
	/*递增栈顶*/
    api_incr_top(L);
  }
  /*未找到str*/
  else {
  	/*把str保存到L->top中*/
    setsvalue2s(L, L->top, str);
	/*递增栈顶*/
    api_incr_top(L);
  	/*
  	(gdb) print L->top[0]
	$17 = {value_ = {gc = 0x0, p = 0x0, b = 0, f = 0x0, i = 0, n = 0}, tt_ = 0}
	(gdb) print L->top[0]
	$18 = {value_ = {gc = 0x6456e0, p = 0x6456e0, b = 6575840, f = 0x6456e0, i = 6575840, n = 3.2488966365487027e-317}, tt_ = 68}
	(gdb) print str
	$19 = (TString *) 0x6456e0
  	*/
    luaV_finishget(L, t, L->top - 1, L->top - 1, slot);
  }
  lua_unlock(L);
  /*返回对象类型*/
  return ttnov(L->top - 1);
}


LUA_API int lua_getglobal (lua_State *L, const char *name) {
  Table *reg = hvalue(&G(L)->l_registry);
  lua_lock(L);
  return auxgetstr(L, luaH_getint(reg, LUA_RIDX_GLOBALS), name);
}


LUA_API int lua_gettable (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  luaV_gettable(L, t, L->top - 1, L->top - 1);
  lua_unlock(L);
  return ttnov(L->top - 1);
}


/*
(gdb) bt
#0  index2addr (L=0x644018, idx=-1001000) at lapi.c:73
#1  0x0000000000406917 in lua_getfield (L=0x644018, idx=-1001000, k=0x4357a4 "_LOADED") at lapi.c:624
#2  0x0000000000420e23 in luaL_getsubtable (L=0x644018, idx=-1001000, fname=0x4357a4 "_LOADED") at lauxlib.c:952
#3  0x0000000000420ec4 in luaL_requiref (L=0x644018, modname=0x435b00 "_G", openf=0x426a46 <luaopen_base>, glb=1) at lauxlib.c:973
#4  0x000000000042129e in luaL_openlibs (L=0x644018) at linit.c:64
#5  0x000000000040505c in pmain (L=0x644018) at lua.c:571
#6  0x000000000040aedf in luaD_precall (L=0x644018, func=0x644640, nresults=1) at ldo.c:434
#7  0x000000000040b26c in luaD_call (L=0x644018, func=0x644640, nResults=1) at ldo.c:498
#8  0x000000000040b2d9 in luaD_callnoyield (L=0x644018, func=0x644640, nResults=1) at ldo.c:509
#9  0x000000000040791e in f_call (L=0x644018, ud=0x7fffffffe300) at lapi.c:943
#10 0x000000000040a284 in luaD_rawrunprotected (L=0x644018, f=0x4078e9 <f_call>, ud=0x7fffffffe300) at ldo.c:142
#11 0x000000000040ba89 in luaD_pcall (L=0x644018, func=0x4078e9 <f_call>, u=0x7fffffffe300, old_top=16, ef=0) at ldo.c:729
#12 0x00000000004079ec in lua_pcallk (L=0x644018, nargs=2, nresults=1, errfunc=0, ctx=0, k=0x0) at lapi.c:969
#13 0x0000000000405206 in main (argc=2, argv=0x7fffffffe448) at lua.c:606
*/
LUA_API int lua_getfield (lua_State *L, int idx, const char *k) {
  lua_lock(L);
  /*index2addr返回Table，即&G(L)->l_registry，在该table中查找k，查找结果压入栈中，返回入栈对象类型*/
  return auxgetstr(L, index2addr(L, idx), k);
}


LUA_API int lua_geti (lua_State *L, int idx, lua_Integer n) {
  StkId t;
  const TValue *slot;
  lua_lock(L);
  t = index2addr(L, idx);
  if (luaV_fastget(L, t, n, slot, luaH_getint)) {
    setobj2s(L, L->top, slot);
    api_incr_top(L);
  }
  else {
    setivalue(L->top, n);
    api_incr_top(L);
    luaV_finishget(L, t, L->top - 1, L->top - 1, slot);
  }
  lua_unlock(L);
  return ttnov(L->top - 1);
}


LUA_API int lua_rawget (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setobj2s(L, L->top - 1, luaH_get(hvalue(t), L->top - 1));
  lua_unlock(L);
  return ttnov(L->top - 1);
}


/*找到n对应的table并入栈，使用该函数可以把保存在全局表中的闭包对象入栈，然后执行*/
LUA_API int lua_rawgeti (lua_State *L, int idx, lua_Integer n) {
  StkId t;
  lua_lock(L);
  /*从栈上使用idx获取对应的节点，此处应该为table类型*/
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  /*从t中查找n对应的节点，然后把节点入栈*/
  setobj2s(L, L->top, luaH_getint(hvalue(t), n));
  api_incr_top(L);
  lua_unlock(L);
  return ttnov(L->top - 1);
}


LUA_API int lua_rawgetp (lua_State *L, int idx, const void *p) {
  StkId t;
  TValue k;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setpvalue(&k, cast(void *, p));
  setobj2s(L, L->top, luaH_get(hvalue(t), &k));
  api_incr_top(L);
  lua_unlock(L);
  return ttnov(L->top - 1);
}


/*创建新的table并入栈*/
LUA_API void lua_createtable (lua_State *L, int narray, int nrec) {
  Table *t;
  lua_lock(L);
  /*创建新的Table*/
  t = luaH_new(L);
  /*入栈*/
  sethvalue(L, L->top, t);
  /*更新栈顶*/
  api_incr_top(L);
  /*更新Table大小*/
  if (narray > 0 || nrec > 0)
    luaH_resize(L, t, narray, nrec);
  luaC_checkGC(L);
  lua_unlock(L);
}


LUA_API int lua_getmetatable (lua_State *L, int objindex) {
  const TValue *obj;
  Table *mt;
  int res = 0;
  lua_lock(L);
  obj = index2addr(L, objindex);
  switch (ttnov(obj)) {
    case LUA_TTABLE:
      mt = hvalue(obj)->metatable;
      break;
    case LUA_TUSERDATA:
      mt = uvalue(obj)->metatable;
      break;
    default:
      mt = G(L)->mt[ttnov(obj)];
      break;
  }
  if (mt != NULL) {
    sethvalue(L, L->top, mt);
    api_incr_top(L);
    res = 1;
  }
  lua_unlock(L);
  return res;
}


LUA_API int lua_getuservalue (lua_State *L, int idx) {
  StkId o;
  lua_lock(L);
  o = index2addr(L, idx);
  api_check(L, ttisfulluserdata(o), "full userdata expected");
  getuservalue(L, uvalue(o), L->top);
  api_incr_top(L);
  lua_unlock(L);
  return ttnov(L->top - 1);
}


/*
** set functions (stack -> Lua)
*/

/*
** t[k] = value at the top of the stack (where 'k' is a string)
*/
/*设置t[k]=value，value已经入栈*/
static void auxsetstr (lua_State *L, const TValue *t, const char *k) {
  const TValue *slot;
  /*获取字符串对象str*/
  TString *str = luaS_new(L, k);
  api_checknelems(L, 1);
  /*slot = luaH_getstr(t, str); *slot = L->top-1，L->top-1存储着value*/
  if (luaV_fastset(L, t, str, slot, luaH_getstr, L->top - 1))
	/*已经设置t[k] = value，出栈*/
    L->top--;  /* pop value */
  else { /*根据k在table t中未找到*/
  	/*str对象入栈，str相当于key，此时L->top-1存储着key而L->top-2存储着value*/
    setsvalue2s(L, L->top, str);  /* push 'str' (to make it a TValue) */
    api_incr_top(L);
  /*
(gdb) print L->top[0]
$65 = {value_ = {gc = 0x0, p = 0x0, b = 0, f = 0x0, i = 0, n = 0}, tt_ = 0}
(gdb) print L->top[-1]
$66 = {value_ = {gc = 0x645830, p = 0x645830, b = 6576176, f = 0x645830, i = 6576176, n = 3.2490626426057053e-317}, tt_ = 68}
(gdb) print L->top[-2]
$67 = {value_ = {gc = 0x426667 <luaB_dofile>, p = 0x426667 <luaB_dofile>, b = 4351591, f = 0x426667 <luaB_dofile>, i = 4351591, n = 2.1499716178519559e-317}, tt_ = 22}
(gdb) print (char*)0x645830+24
$68 = 0x645848 "dofile"
  */
	/*把key/value添加到table中*/
    luaV_finishset(L, t, L->top - 1, L->top - 2, slot);
    /*平衡堆栈，把value和key出栈*/
    L->top -= 2;  /* pop value and key */
  }
  lua_unlock(L);  /* lock done by caller */
}


/*添加key/value到全局表中*/
LUA_API void lua_setglobal (lua_State *L, const char *name) {
  /*获取registry表*/
  Table *reg = hvalue(&G(L)->l_registry);
  lua_lock(L);  /* unlock done in 'auxsetstr' */
  /*设置key/value到table LUA_RIDX_GLOBALS中，根据LUA_RIDX_GLOBALS可以从registry表中获取global表*/
  auxsetstr(L, luaH_getint(reg, LUA_RIDX_GLOBALS), name);
}


LUA_API void lua_settable (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  api_checknelems(L, 2);
  t = index2addr(L, idx);
  luaV_settable(L, t, L->top - 2, L->top - 1);
  L->top -= 2;  /* pop index and value */
  lua_unlock(L);
}


/*根据idx获取对应的table，然后设置t[k]=value，value在调用该函数前已经入栈，
 * 调用该函数前一般都会调用lua_pushXXX把value先入栈*/
LUA_API void lua_setfield (lua_State *L, int idx, const char *k) {
  lua_lock(L);  /* unlock done in 'auxsetstr' */
  /*如果idx为LUA_REGISTRYINDEX，则获取全局表，然后设置l_registry[k]=value，value已入栈，函数返回前会把key/value出栈*/
  auxsetstr(L, index2addr(L, idx), k);
}


LUA_API void lua_seti (lua_State *L, int idx, lua_Integer n) {
  StkId t;
  const TValue *slot;
  lua_lock(L);
  api_checknelems(L, 1);
  t = index2addr(L, idx);
  if (luaV_fastset(L, t, n, slot, luaH_getint, L->top - 1))
    L->top--;  /* pop value */
  else {
    setivalue(L->top, n);
    api_incr_top(L);

    luaV_finishset(L, t, L->top - 1, L->top - 2, slot);
    L->top -= 2;  /* pop value and key */
  }
  lua_unlock(L);
}


LUA_API void lua_rawset (lua_State *L, int idx) {
  StkId o;
  TValue *slot;
  lua_lock(L);
  api_checknelems(L, 2);
  o = index2addr(L, idx);
  api_check(L, ttistable(o), "table expected");
  slot = luaH_set(L, hvalue(o), L->top - 2);
  setobj2t(L, slot, L->top - 1);
  invalidateTMcache(hvalue(o));
  luaC_barrierback(L, hvalue(o), L->top-1);
  L->top -= 2;
  lua_unlock(L);
}


/*使用该函数可以把编译后的闭包对象保存到全局表中*/
LUA_API void lua_rawseti (lua_State *L, int idx, lua_Integer n) {
  StkId o;
  lua_lock(L);
  api_checknelems(L, 1);
  /*获取栈中节点*/
  o = index2addr(L, idx);
  api_check(L, ttistable(o), "table expected");
  /*使用n做为key，L->top-1为value，添加key/value到table o中*/
  luaH_setint(L, hvalue(o), n, L->top - 1);
  luaC_barrierback(L, hvalue(o), L->top-1);
  /*value出栈*/
  L->top--;
  lua_unlock(L);
}


LUA_API void lua_rawsetp (lua_State *L, int idx, const void *p) {
  StkId o;
  TValue k, *slot;
  lua_lock(L);
  api_checknelems(L, 1);
  o = index2addr(L, idx);
  api_check(L, ttistable(o), "table expected");
  setpvalue(&k, cast(void *, p));
  slot = luaH_set(L, hvalue(o), &k);
  setobj2t(L, slot, L->top - 1);
  luaC_barrierback(L, hvalue(o), L->top - 1);
  L->top--;
  lua_unlock(L);
}


LUA_API int lua_setmetatable (lua_State *L, int objindex) {
  TValue *obj;
  Table *mt;
  lua_lock(L);
  api_checknelems(L, 1);
  /*获取字符串对象*/
  obj = index2addr(L, objindex);
  /*检查top-1对象是否是table*/
  if (ttisnil(L->top - 1))
    mt = NULL;
  else {
    api_check(L, ttistable(L->top - 1), "table expected");
    mt = hvalue(L->top - 1); /*获取table*/
  }
  switch (ttnov(obj)) {
    case LUA_TTABLE: {
      hvalue(obj)->metatable = mt;
      if (mt) {
        luaC_objbarrier(L, gcvalue(obj), mt);
        luaC_checkfinalizer(L, gcvalue(obj), mt);
      }
      break;
    }
    case LUA_TUSERDATA: {
      uvalue(obj)->metatable = mt;
      if (mt) {
        luaC_objbarrier(L, uvalue(obj), mt);
        luaC_checkfinalizer(L, gcvalue(obj), mt);
      }
      break;
    }
    default: {
	  /*设置mt table作为基础类型的metatable*/
      G(L)->mt[ttnov(obj)] = mt;
      break;
    }
  }
  /*table出栈*/
  L->top--;
  lua_unlock(L);
  return 1;
}


LUA_API void lua_setuservalue (lua_State *L, int idx) {
  StkId o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = index2addr(L, idx);
  api_check(L, ttisfulluserdata(o), "full userdata expected");
  setuservalue(L, uvalue(o), L->top - 1);
  luaC_barrier(L, gcvalue(o), L->top - 1);
  L->top--;
  lua_unlock(L);
}


/*
** 'load' and 'call' functions (run Lua code)
*/


#define checkresults(L,na,nr) \
     api_check(L, (nr) == LUA_MULTRET || (L->ci->top - L->top >= (nr) - (na)), \
	"results from function overflow current stack size")


LUA_API void lua_callk (lua_State *L, int nargs, int nresults,
                        lua_KContext ctx, lua_KFunction k) {
  StkId func;
  lua_lock(L);
  api_check(L, k == NULL || !isLua(L->ci),
    "cannot use continuations inside hooks");
  api_checknelems(L, nargs+1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);
  /*栈顶减去参数获取要执行的函数*/
  func = L->top - (nargs+1);
  /*设置了恢复函数*/
  if (k != NULL && L->nny == 0) {  /* need to prepare continuation? */
    L->ci->u.c.k = k;  /* save continuation */
    L->ci->u.c.ctx = ctx;  /* save context */
    luaD_call(L, func, nresults);  /* do the call */
  }
  /*不能挂起*/
  else  /* no continuation or no yieldable */
    luaD_callnoyield(L, func, nresults);  /* just do the call */
  adjustresults(L, nresults);
  lua_unlock(L);
}



/*
** Execute a protected call.
*/
struct CallS {  /* data to 'f_call' */
  StkId func;
  int nresults;
};


/*
ud：传递的用户数据
(gdb) print c[0]
$39 = {func = 0x644640, nresults = 1}
*/
static void f_call (lua_State *L, void *ud) {
  struct CallS *c = cast(struct CallS *, ud);
  luaD_callnoyield(L, c->func, c->nresults);
}


/*nargs：参数个数
nresults: 函数返回的结果数*/
LUA_API int lua_pcallk (lua_State *L, int nargs, int nresults, int errfunc,
                        lua_KContext ctx, lua_KFunction k) {
  struct CallS c;
  int status;
  ptrdiff_t func;
  lua_lock(L);
  api_check(L, k == NULL || !isLua(L->ci),
    "cannot use continuations inside hooks");
  api_checknelems(L, nargs+1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);
  /*没有指定错误函数*/
  if (errfunc == 0)
    func = 0;
  /*指定了错误函数*/
  else {
  	/*错误函数在栈中的StkId*/
    StkId o = index2addr(L, errfunc);
    api_checkstackindex(L, errfunc, o);
  	/*错误函数相对于栈底的偏移*/
    func = savestack(L, o);
  }
  /*获取要调用的函数，函数入栈，参数入栈，top会递增，所以此处根据top减去参数个数即可获取要执行的函数*/
  c.func = L->top - (nargs+1);  /* function to be called */
  if (k == NULL || L->nny > 0) {  /* no continuation or no yieldable? */
    c.nresults = nresults;  /* do a 'conventional' protected call */
    status = luaD_pcall(L, f_call, &c, savestack(L, c.func), func);
  }
  else {  /* prepare continuation (call is already protected by 'resume') */
    CallInfo *ci = L->ci;
	/*保存恢复执行后调用的函数*/
    ci->u.c.k = k;  /* save continuation */
  	/*保存上下文*/
    ci->u.c.ctx = ctx;  /* save context */
    /* save information for error recovery */
    ci->extra = savestack(L, c.func);
    ci->u.c.old_errfunc = L->errfunc;
    L->errfunc = func;
    setoah(ci->callstatus, L->allowhook);  /* save value of 'allowhook' */
    ci->callstatus |= CIST_YPCALL;  /* function can do error recovery */
    luaD_call(L, c.func, nresults);  /* do the call */
    ci->callstatus &= ~CIST_YPCALL;
    L->errfunc = ci->u.c.old_errfunc;
    status = LUA_OK;  /* if it is here, there were no errors */
  }
  adjustresults(L, nresults);
  lua_unlock(L);
  return status;
}


/*
#0  lua_load (L=0x644018, reader=0x4203dc <getF>, data=0x7fffffffbf90, chunkname=0x64aba8 "@./test.lua", mode=0x0) at lapi.c:997
data：LoadF类型
*/
LUA_API int lua_load (lua_State *L, lua_Reader reader, void *data,
                      const char *chunkname, const char *mode) {
  ZIO z;
  int status;
  lua_lock(L);
  if (!chunkname) chunkname = "?";
  /*使用reader和data初始化z*/
  luaZ_init(L, &z, reader, data);
  status = luaD_protectedparser(L, &z, chunkname, mode);
  if (status == LUA_OK) {  /* no errors? */
    LClosure *f = clLvalue(L->top - 1);  /* get newly created function */
    if (f->nupvalues >= 1) {  /* does it have an upvalue? */
      /* get global table from registry */
      Table *reg = hvalue(&G(L)->l_registry);
      const TValue *gt = luaH_getint(reg, LUA_RIDX_GLOBALS);
      /* set global table as 1st upvalue of 'f' (may be LUA_ENV) */
      setobj(L, f->upvals[0]->v, gt);
      luaC_upvalbarrier(L, f->upvals[0]);
    }
  }
  lua_unlock(L);
  return status;
}


LUA_API int lua_dump (lua_State *L, lua_Writer writer, void *data, int strip) {
  int status;
  TValue *o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = L->top - 1;
  if (isLfunction(o))
    status = luaU_dump(L, getproto(o), writer, data, strip);
  else
    status = 1;
  lua_unlock(L);
  return status;
}


LUA_API int lua_status (lua_State *L) {
  return L->status;
}


/*
** Garbage-collection function
*/

LUA_API int lua_gc (lua_State *L, int what, int data) {
  int res = 0;
  global_State *g;
  lua_lock(L);
  g = G(L);
  switch (what) {
    case LUA_GCSTOP: {
      g->gcrunning = 0;
      break;
    }
    case LUA_GCRESTART: {
      luaE_setdebt(g, 0);
      g->gcrunning = 1;
      break;
    }
    case LUA_GCCOLLECT: {
      luaC_fullgc(L, 0);
      break;
    }
    case LUA_GCCOUNT: {
      /* GC values are expressed in Kbytes: #bytes/2^10 */
      res = cast_int(gettotalbytes(g) >> 10);
      break;
    }
    case LUA_GCCOUNTB: {
      res = cast_int(gettotalbytes(g) & 0x3ff);
      break;
    }
    case LUA_GCSTEP: {
      l_mem debt = 1;  /* =1 to signal that it did an actual step */
      lu_byte oldrunning = g->gcrunning;
      g->gcrunning = 1;  /* allow GC to run */
      if (data == 0) {
        luaE_setdebt(g, -GCSTEPSIZE);  /* to do a "small" step */
        luaC_step(L);
      }
      else {  /* add 'data' to total debt */
        debt = cast(l_mem, data) * 1024 + g->GCdebt;
        luaE_setdebt(g, debt);
        luaC_checkGC(L);
      }
      g->gcrunning = oldrunning;  /* restore previous state */
      if (debt > 0 && g->gcstate == GCSpause)  /* end of cycle? */
        res = 1;  /* signal it */
      break;
    }
    case LUA_GCSETPAUSE: {
      res = g->gcpause;
      g->gcpause = data;
      break;
    }
    case LUA_GCSETSTEPMUL: {
      res = g->gcstepmul;
      if (data < 40) data = 40;  /* avoid ridiculous low values (and 0) */
      g->gcstepmul = data;
      break;
    }
    case LUA_GCISRUNNING: {
      res = g->gcrunning;
      break;
    }
    default: res = -1;  /* invalid option */
  }
  lua_unlock(L);
  return res;
}



/*
** miscellaneous functions
*/


LUA_API int lua_error (lua_State *L) {
  lua_lock(L);
  api_checknelems(L, 1);
  luaG_errormsg(L);
  /* code unreachable; will unlock when control actually leaves the kernel */
  return 0;  /* to avoid warnings */
}


LUA_API int lua_next (lua_State *L, int idx) {
  StkId t;
  int more;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  more = luaH_next(L, hvalue(t), L->top - 1);
  if (more) {
    api_incr_top(L);
  }
  else  /* no more elements */
    L->top -= 1;  /* remove key */
  lua_unlock(L);
  return more;
}


LUA_API void lua_concat (lua_State *L, int n) {
  lua_lock(L);
  api_checknelems(L, n);
  if (n >= 2) {
    luaV_concat(L, n);
  }
  else if (n == 0) {  /* push empty string */
    setsvalue2s(L, L->top, luaS_newlstr(L, "", 0));
    api_incr_top(L);
  }
  /* else n == 1; nothing to do */
  luaC_checkGC(L);
  lua_unlock(L);
}


LUA_API void lua_len (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  luaV_objlen(L, L->top, t);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API lua_Alloc lua_getallocf (lua_State *L, void **ud) {
  lua_Alloc f;
  lua_lock(L);
  if (ud) *ud = G(L)->ud;
  f = G(L)->frealloc;
  lua_unlock(L);
  return f;
}


LUA_API void lua_setallocf (lua_State *L, lua_Alloc f, void *ud) {
  lua_lock(L);
  G(L)->ud = ud;
  G(L)->frealloc = f;
  lua_unlock(L);
}


LUA_API void *lua_newuserdata (lua_State *L, size_t size) {
  Udata *u;
  lua_lock(L);
  u = luaS_newudata(L, size);
  setuvalue(L, L->top, u);
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
  return getudatamem(u);
}



static const char *aux_upvalue (StkId fi, int n, TValue **val,
                                CClosure **owner, UpVal **uv) {
  switch (ttype(fi)) {
    case LUA_TCCL: {  /* C closure */
      CClosure *f = clCvalue(fi);
      if (!(1 <= n && n <= f->nupvalues)) return NULL;
      *val = &f->upvalue[n-1];
      if (owner) *owner = f;
      return "";
    }
    case LUA_TLCL: {  /* Lua closure */
      LClosure *f = clLvalue(fi);
      TString *name;
      Proto *p = f->p;
      if (!(1 <= n && n <= p->sizeupvalues)) return NULL;
      *val = f->upvals[n-1]->v;
      if (uv) *uv = f->upvals[n - 1];
      name = p->upvalues[n-1].name;
      return (name == NULL) ? "(*no name)" : getstr(name);
    }
    default: return NULL;  /* not a closure */
  }
}


LUA_API const char *lua_getupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TValue *val = NULL;  /* to avoid warnings */
  lua_lock(L);
  name = aux_upvalue(index2addr(L, funcindex), n, &val, NULL, NULL);
  if (name) {
    setobj2s(L, L->top, val);
    api_incr_top(L);
  }
  lua_unlock(L);
  return name;
}


LUA_API const char *lua_setupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TValue *val = NULL;  /* to avoid warnings */
  CClosure *owner = NULL;
  UpVal *uv = NULL;
  StkId fi;
  lua_lock(L);
  fi = index2addr(L, funcindex);
  api_checknelems(L, 1);
  name = aux_upvalue(fi, n, &val, &owner, &uv);
  if (name) {
    L->top--;
    setobj(L, val, L->top);
    if (owner) { luaC_barrier(L, owner, L->top); }
    else if (uv) { luaC_upvalbarrier(L, uv); }
  }
  lua_unlock(L);
  return name;
}


static UpVal **getupvalref (lua_State *L, int fidx, int n, LClosure **pf) {
  LClosure *f;
  StkId fi = index2addr(L, fidx);
  api_check(L, ttisLclosure(fi), "Lua function expected");
  f = clLvalue(fi);
  api_check(L, (1 <= n && n <= f->p->sizeupvalues), "invalid upvalue index");
  if (pf) *pf = f;
  return &f->upvals[n - 1];  /* get its upvalue pointer */
}


LUA_API void *lua_upvalueid (lua_State *L, int fidx, int n) {
  StkId fi = index2addr(L, fidx);
  switch (ttype(fi)) {
    case LUA_TLCL: {  /* lua closure */
      return *getupvalref(L, fidx, n, NULL);
    }
    case LUA_TCCL: {  /* C closure */
      CClosure *f = clCvalue(fi);
      api_check(L, 1 <= n && n <= f->nupvalues, "invalid upvalue index");
      return &f->upvalue[n - 1];
    }
    default: {
      api_check(L, 0, "closure expected");
      return NULL;
    }
  }
}


LUA_API void lua_upvaluejoin (lua_State *L, int fidx1, int n1,
                                            int fidx2, int n2) {
  LClosure *f1;
  UpVal **up1 = getupvalref(L, fidx1, n1, &f1);
  UpVal **up2 = getupvalref(L, fidx2, n2, NULL);
  luaC_upvdeccount(L, *up1);
  *up1 = *up2;
  (*up1)->refcount++;
  if (upisopen(*up1)) (*up1)->u.open.touched = 1;
  luaC_upvalbarrier(L, *up1);
}


