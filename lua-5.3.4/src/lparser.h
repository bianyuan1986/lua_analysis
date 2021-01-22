/*
** $Id: lparser.h,v 1.76 2015/12/30 18:16:13 roberto Exp $
** Lua Parser
** See Copyright Notice in lua.h
*/

#ifndef lparser_h
#define lparser_h

#include "llimits.h"
#include "lobject.h"
#include "lzio.h"


/*
** Expression and variable descriptor.
** Code generation for variables and expressions can be delayed to allow
** optimizations; An 'expdesc' structure describes a potentially-delayed
** variable/expression. It has a description of its "main" value plus a
** list of conditional jumps that can also produce its value (generated
** by short-circuit operators 'and'/'or').
*/

/* kinds of variables/expressions */
typedef enum {
  VVOID,  /* when 'expdesc' describes the last expression a list,
             this kind means an empty list (so, no expression) */
  VNIL,  /* constant nil */
  VTRUE,  /* constant true */
  VFALSE,  /* constant false */
  VK,  /* constant in 'k'; info = index of constant in 'k' */
  VKFLT,  /* floating constant; nval = numerical float value */
  VKINT,  /* integer constant; nval = numerical integer value */
  VNONRELOC,  /* expression has its value in a fixed register;
                 info = result register */
  VLOCAL,  /* local variable; info = local register */
  VUPVAL,  /* upvalue variable; info = index of upvalue in 'upvalues' */
  VINDEXED,  /* indexed variable;
                ind.vt = whether 't' is register or upvalue;
                ind.t = table register or upvalue;
                ind.idx = key's R/K index */
  VJMP,  /* expression is a test/comparison;
            info = pc of corresponding jump instruction */
  VRELOCABLE,  /* expression can put result in any register;
                  info = instruction pc */
  VCALL,  /* expression is a function call; info = instruction pc */
  VVARARG  /* vararg expression; info = instruction pc */
} expkind;


#define vkisvar(k)	(VLOCAL <= (k) && (k) <= VINDEXED)
#define vkisinreg(k)	((k) == VNONRELOC || (k) == VLOCAL)

/*用来描述表达式*/
typedef struct expdesc {
  /*表达式类型索引*/
  expkind k;
  union {
    lua_Integer ival;    /* for VKINT */
    lua_Number nval;  /* for VKFLT */
    int info;  /* for generic use */
    struct {  /* for indexed variables (VINDEXED) */
	  /*如果是常量则为常量哈希表fs->ls->h表中的索引，如果是变量则为寄存器索引*/
      short idx;  /* index (R/K) */
	  /*变量在寄存器数组或upvalue数组中的索引*/
      lu_byte t;  /* table (register or upvalue) */
	  /*变量类型为VUPVAL或者VLOCAL*/
      lu_byte vt;  /* whether 't' is register (VLOCAL) or upvalue (VUPVAL) */
    } ind;
  } u;
  /*条件为真时的跳转链表，每条指令中包含了跳转的目标地址*/
  int t;  /* patch list of 'exit when true' */
  /*条件为假时的跳转链表，如果t==f则说明不包含跳转*/
  int f;  /* patch list of 'exit when false' */
} expdesc;


/* description of active local variable */
typedef struct Vardesc {
  short idx;  /* variable index in stack */
} Vardesc;


/* description of pending goto statements and label statements */
typedef struct Labeldesc {
  TString *name;  /* label identifier */
  int pc;  /* position in code */
  int line;  /* line where it appeared */
  lu_byte nactvar;  /* local level where it appears in current block */
} Labeldesc;


/* list of labels or gotos */
typedef struct Labellist {
  Labeldesc *arr;  /* array */
  int n;  /* number of entries in use */
  int size;  /* array size */
} Labellist;


/* dynamic structures used by the parser */
typedef struct Dyndata {
  /*保存局部变量在Proto->locvars数组中的索引*/
  struct {  /* list of active local variables */
  	/*数组，每个节点中保存了对应局部变量的索引*/
    Vardesc *arr;
    int n;
    int size;
  } actvar;
  Labellist gt;  /* list of pending gotos */
  Labellist label;   /* list of active labels */
} Dyndata;


/* control of blocks */
struct BlockCnt;  /* defined in lparser.c */


/* state needed to generate code for a given function */
typedef struct FuncState {
  /*函数原型*/
  Proto *f;  /* current function header */
  /*当前函数的外层函数，即嵌入了当前函数的函数*/
  struct FuncState *prev;  /* enclosing function */
  struct LexState *ls;  /* lexical state */
  struct BlockCnt *bl;  /* chain of current blocks */
  /*当前代码对应的指令的保存位置，f->code中的索引*/
  int pc;  /* next position to code (equivalent to 'ncode') */
  int lasttarget;   /* 'label' of last 'jump label' */
  int jpc;  /* list of pending jumps to 'pc' */
  /*函数中用到的常量数目，常量保存在f->k的数组中，LexState的h哈希表中保存了常量对象在Proto->k数组中的索引*/
  int nk;  /* number of elements in 'k' */
  /*函数中嵌套定义的函数原型，保存在f->p中*/
  int np;  /* number of elements in 'p' */
  /*idx = fs->ls->dyd->actvar.arr[fs->firstlocal + i].idx，然后根据idx再从f->locvars[idx]中获取局部变量，记录第一个局部变量索引的数组节点索引*/
  int firstlocal;  /* index of first local var (in Dyndata array) */
  /*f->locvars数组容量，局部变量数组容量*/
  short nlocvars;  /* number of elements in 'f->locvars' */
  /*局部变量个数*/
  lu_byte nactvar;  /* number of active local variables */
  /*引用的upvalue的个数，upvalue信息保存在Proto->upvalues数组中*/
  lu_byte nups;  /* number of upvalues */
  /*指向第一个空闲的寄存器*/
  lu_byte freereg;  /* first free register */
} FuncState;


LUAI_FUNC LClosure *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                                 Dyndata *dyd, const char *name, int firstchar);


#endif
