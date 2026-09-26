%option noyywrap

%{
#include <stdio.h>

#define YY_DECL int yylex()

#include "bool.tab.h"

%}

D		[0-9]
L		[a-zA-Z_]
H		[a-fA-F0-9]
E		[Ee][+-]?{D}+
FS		(f|F|l|L)
IS		(u|U|l|L)*
RB       }

%%

"//"[^\n]+\n	{ }
"T"		{yylval.num = 1;return(TRUE);}
"F"		{yylval.num = 0;return(FALSE);}
"init"		{return(INIT); }
"return"	{return(RETURN); }
"goto"		{return(GOTO); }
"skip"		{return(SKIP); }
"do"		{return(DO); }
"od"		{return(OD); }
"else"		{return(ELSE); }
"elsif"		{return(ELSIF); }
"if"		{return(IF); }
"then"		{return(THEN); }
"fi"		{return(FI); }
"decl"		{return(DECL); }
"while"		{return(WHILE);}
"begin"		{return(BEG); }
"end"		{return(END); }
"bool"		{return(BOOL); }
"dfs"		{return(DFS); }
"void"		{return(VOID); }
"assert"	{return(ASSERT); }
"assume"	{return(ASSUME); }
"dead"		{return(DEAD); }
"enforce"   {return(ENFORCE); }
"constrain" {return(CONSTRAIN); }
"schoose"	{return(CHOOSE); }
"*"		{return(NONDET); }
"?"	 	{return(NONDET); }
({D})+      {yylval.num = atoi(yytext);return(INTEGER); }
"{"[^ }\n]+"}"	{yylval.str = strdup(yytext);return(IDENTIFIER);}
"_"		{yylval.str = strdup(yytext);return(IDENTIFIER); }
{L}({L}|{D})*	{yylval.str = strdup(yytext);return(IDENTIFIER); }
":="	{return(ASSIGN); }
"="		{return(EQ_OP); }
"!="	{return(NE_OP); }
"=>"	{return(IMPLIES); }
";"		{return(';'); }
"\'"    {return('\''); }
","		{return(','); }
":"		{return(':'); }
"("		{return('('); }
")"		{return(')'); }
"["		{return('['); }
"]"		{return(']'); }
"&"		{return(AND); }
"&&"		{return(AND); }
"!"		{return('!'); }
"~"		{return('~'); }
"^"		{return('^'); }
"|"		{return(OR); }
"||"		{return(OR); }
"<"		{return('<'); }
">"		{return('>'); }
[ \t\v\n\f]	{  }
.

%%
