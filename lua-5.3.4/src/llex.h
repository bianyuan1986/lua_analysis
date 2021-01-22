/*
** $Id: llex.h,v 1.79 2016/05/02 14:02:12 roberto Exp $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#ifndef llex_h
#define llex_h

#include "lobject.h"
#include "lzio.h"


#define FIRST_RESERVED	257


#if !defined(LUA_ENV)
#define LUA_ENV		"_ENV"
#endif


/*
* WARNING: if you change the order of this enumeration,
* grep "ORDER RESERVED"
*/
/*
TK_STRING 293
*/
enum RESERVED {
  /* terminal symbols denoted by reserved words */
  TK_AND = FIRST_RESERVED, TK_BREAK,
  TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION,
  TK_GOTO, TK_IF, TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT,
  TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
  /* other terminal symbols */
  TK_IDIV, TK_CONCAT, TK_DOTS, TK_EQ, TK_GE, TK_LE, TK_NE,
  TK_SHL, TK_SHR,
  TK_DBCOLON, TK_EOS,
  TK_FLT, TK_INT, TK_NAME, TK_STRING
};

/* number of reserved words */
#define NUM_RESERVED	(cast(int, TK_WHILE-FIRST_RESERVED+1))


typedef union {
  /*数字*/
  lua_Number r;
  /*整数*/
  lua_Integer i;
  /*token字符串*/
  TString *ts;
} SemInfo;  /* semantics information */


typedef struct Token {
  /*如果是关键字则设置该字段，表示token类型索引，即enum RESERVED，如果为字符串则为TK_NAME*/
  int token;
  /*非关键字则保存字符串字面值到该字段，TString类型对象*/
  SemInfo seminfo;
} Token;


/* state of the lexer plus state of the parser when shared by all
   functions */
typedef struct LexState {
  int current;  /* current character (charint) */
  int linenumber;  /* input line counter */
  /*上一个token所在的行数*/
  int lastline;  /* line of last token 'consumed' */
  /*当前token*/
  Token t;  /* current token */
  /*预读token*/
  Token lookahead;  /* look ahead token */
  struct FuncState *fs;  /* current function (parser) */
  struct lua_State *L;
  ZIO *z;  /* input stream */
  /*数据缓冲区，每次用于读取一个token*/
  Mbuffer *buff;  /* buffer for tokens */
  /*存放可以被复用的常量索引，根据key找到对应节点，获取节点中保存的索引然后
  通过Proto->k[idx]获取到常量*/
  Table *h;  /* to avoid collection/reuse strings */
  /*存储局部变量，指向存储局部变量的地址，在解析时保存通过参数传递的地址*/
  struct Dyndata *dyd;  /* dynamic structures used by the parser */
  /*构造的代码块名称
  (gdb) print ls->source 
$26 = (TString *) 0x64ab90
(gdb) print (char*)$26+24
$27 = 0x64aba8 "@./test.lua"*/
  TString *source;  /* current source name */
  TString *envn;  /* environment variable name */
} LexState;


LUAI_FUNC void luaX_init (lua_State *L);
LUAI_FUNC void luaX_setinput (lua_State *L, LexState *ls, ZIO *z,
                              TString *source, int firstchar);
LUAI_FUNC TString *luaX_newstring (LexState *ls, const char *str, size_t l);
LUAI_FUNC void luaX_next (LexState *ls);
LUAI_FUNC int luaX_lookahead (LexState *ls);
LUAI_FUNC l_noret luaX_syntaxerror (LexState *ls, const char *s);
LUAI_FUNC const char *luaX_token2str (LexState *ls, int token);


#endif
