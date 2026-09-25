%{

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define YYDEBUG 1

extern int yylex();
extern int yyparse();
extern FILE* yyin;
extern FILE* yyout;

int yystat=1;
int yyproc=0;
int yyvar=0;
int yyexpr=0;
int yylbl=0;
int curr_exit=0;

void yyerror(const char* s);
%}
%define parse.error verbose

%union {
	int num;
    char *str;
    struct {
		int fv;
		int lv;
	} gnode;
}

%type <str> decls declaration
%type <str> identifier_list decl_list label_list parameter_list
%type <str> expression_list

%type <gnode> statement_list
%type <num> labelled_statement statement optional_constrain
%type <num> dead_statement jump_statement iteration_statement
%type <num> selection_statement
%type <num> assert parallel_assign
%type <num> decider choose_expression

%type <num> return_type

%type <num> enforce expression or_expression exclusive_or_expression
%type <num> and_expression equality_expression unary_expression primary_expression
%type <str> elsif_list elsif_list2 elsif_clause call

%token <str> IDENTIFIER
%token <num> INTEGER
%token NONDET
%token ASSIGN EQ_OP NE_OP IMPLIES
%token DECL
%token ENFORCE
%token AND OR
%token TRUE FALSE
%token IF THEN ELSE ELSIF FI
%token WHILE DO OD
%token RETURN SKIP GOTO
%token BEG END
%token BOOL VOID DFS
%token ASSERT ASSUME CHOOSE CONSTRAIN DEAD
%token INIT



%start program

%%
// program definition
program
	: decls fun_list {
		if(strlen($1)>0) printf("var %d %d ", -1, yyvar++);
		for(int i=0;i<strlen($1);i++){
			if($1[i]==' '){
				printf("\nvar %d %d ", -1, yyvar++);
			}
			else printf("%c", $1[i]);
		}
		printf("\n");
		printf("var %d %d %s \n", -1, yyvar++, "_");
		printf("node %d %d error\n", -1, yystat-1);
	}
	;
decl_list
    : declaration {$$ = $1;}
    | decl_list declaration {
	int length = snprintf( NULL, 0, "%s %s", $1, $2);
	$$ = malloc( length + 1 );
	snprintf( $$, length + 1, "%s %s", $1, $2);
	free($1);free($2);

    }
    ;
declaration
	: DECL identifier_list ';' {$$ = $2;}
	;
identifier_list
	: IDENTIFIER {$$ = $1;}
	| identifier_list ',' IDENTIFIER {
		int length = snprintf( NULL, 0, "%s %s", $1, $3);
		$$ = malloc( length + 1 );
		snprintf( $$, length + 1, "%s %s", $1, $3);
		free($1);
	}
	;
fun_list
	: function_definition
	|fun_list function_definition
    ;

// function definition: optional DFS token should be supported but can be ignored after parsing

function_definition
	:  return_type IDENTIFIER parameter_list BEG decls enforce statement_list END
	{

		printf("return %d %d\n", yyproc, $1);

		printf("next %d %d %d\n", yyproc, $7.lv, -1);

		if(strlen($3)>0) printf("parameter %d %d ", yyproc, yyvar++);
		for(int i=0;i<strlen($3);i++){
			if($3[i]==' '){
				printf("\nparameter %d %d ", yyproc, yyvar++);
			}
			else printf("%c", $3[i]);
		}
		if(strlen($3)>0) printf("\n");

		if(strlen($5)>0) printf("var %d %d ", yyproc, yyvar++);
		for(int i=0;i<strlen($5);i++){
			if($5[i]==' '){
				printf("\nvar %d %d ", yyproc, yyvar++);
			}
			else printf("%c", $5[i]);
		}
		if(strlen($5)>0) printf("\n");

		//procedure, name, entry node, exit node
		printf("procedure %d %s %d %d %d\n",yyproc, $2, $7.fv, curr_exit, $6);

		//for the next procedure
		yyproc++;
		curr_exit=yystat++;
	}
	;
parameter_list
	: '(' ')' {$$ = "";}
	| '(' identifier_list ')' {$$ = $2;}
	;
return_type
    : BOOL {$$ = 1;}
    | BOOL '<' INTEGER '>' {$$ = $3;}
    | VOID { $$ =0;}
    ;
decls
	: {$$ = "";}
	| decl_list {$$ = $1;}
	;
enforce
	: {$$ = -1;}
	| ENFORCE expression ';' {$$ = $2;}
	;

// expressions

primary_expression
	: IDENTIFIER {
		$$ =yyexpr++;
		if(strcmp($1,"T")==0)printf("xp %d true\n", $$);
		else if(strcmp($1,"F")==0)printf("xp %d false\n", $$);
		else printf("xp %d var %d %s \n", $$, yyproc,  $1);
	}
	| INTEGER {
		$$ =yyexpr++;
		printf("xp %d int %d \n", $$, $1);
	}
	//put it back later
	| '\'' IDENTIFIER {
		char c=';';
		printf("xp %d primed %d %s \n", $$, yyproc,  $2);
	}     	// primed variables are only allowed inside constrain expression
	| TRUE {
		$$ =yyexpr++;
		printf("xp %d true\n", $$);
	}
	| FALSE {
		$$ =yyexpr++;
		printf("xp %d false\n", $$);
	}
	| '(' expression ')' {
		$$ = $2;
	}
	;
unary_expression
	: primary_expression {$$ = $1;}
	| unary_operator unary_expression {
		$$ =yyexpr++;
		printf("xp %d not %d \n", $$, $2);
	}
	;
unary_operator
	: '~'
	| '!'
	;
equality_expression
	: unary_expression {$$ = $1;}
	| equality_expression EQ_OP unary_expression {
		$$ =yyexpr++;
		printf("xp %d eq %d %d\n", $$, $1, $3);
	}
	| equality_expression NE_OP unary_expression {
		$$ =yyexpr++;
		printf("xp %d neq %d %d\n", $$, $1, $3);
	}
	;
and_expression
	: equality_expression {$$ = $1;}
	| and_expression AND equality_expression {
		$$ =yyexpr++;
		printf("xp %d and %d %d\n", $$, $1, $3);
	}
	;
exclusive_or_expression
	: and_expression {$$ = $1;}
	| exclusive_or_expression '^' and_expression {
		$$ =yyexpr++;
		printf("xp %d xor %d %d\n", $$, $1, $3);
	}
	;
or_expression
	: exclusive_or_expression {$$ = $1;}
	| or_expression OR exclusive_or_expression {
		$$ =yyexpr++;
		printf("xp %d or %d %d\n", $$, $1, $3);
	}
	;
expression
	: or_expression {$$ = $1;}
	| or_expression IMPLIES expression  {
		$$ =yyexpr++;
		printf("xp %d implies %d %d\n", $$, $1, $3);
	}
	;
decider
	: NONDET {
		$$ = yyexpr++;
		printf("xp %d nondet\n", $$);
	}
    | expression {$$ = $1;}
	;
choose_expression
	: expression {
		$$ = $1;
	}
	| CHOOSE '[' expression ',' expression ']' {
		$$ = yyexpr++;
		printf("xp %d choose %d %d\n", $$, $3, $5);
	}
    ;
expression_list
	: choose_expression {
		int length = snprintf( NULL, 0, "%d", $1 );
		$$ = malloc( length + 1 );
		snprintf( $$, length + 1, "%d", $1 );
	}
	| expression_list ',' choose_expression {
		int length = snprintf( NULL, 0, "%s %d", $1, $3);
		$$ = malloc( length + 1 );
		snprintf( $$, length + 1, "%s %d", $1, $3);
		free($1);
	}
	;

// statements

statement_list
    : labelled_statement {
	$$.fv = $$.lv = $1;
    }
	| statement_list labelled_statement {
		$$.fv = $1.fv;
		$$.lv = $2;
		printf("next %d %d %d\n", yyproc, $1.lv, $2);
	}
	;
labelled_statement
	: IDENTIFIER ':' labelled_statement {
		$$ = $3;
		// cur proc, num, name, nxt node
		printf("label %d %d %s %d\n", yyproc, yylbl++, $1, $3);
	}
	| statement {
		$$ = $1;
	}
	;
statement
	: parallel_assign ';' {
		$$ = $1;
	}
	| assert ';'{
		$$ = $1;
	}
	| call ';' {
		$$ = yystat++;
		printf("node %d %d call %s\n", yyproc, $$, $1);
		yystat++;
	}
	| selection_statement {
		$$ = $1;
	}
	| iteration_statement {
		$$ = $1;
	}
	| jump_statement ';' {
		$$ = $1;
	}
	| dead_statement ';' {
		$$ = $1;
	}
	;
parallel_assign
    : identifier_list ASSIGN expression_list optional_constrain {
	$$ = yystat++;
	int id_num=1;
		for(int i=0;i<strlen($1);i++){
			if($1[i]==' ')id_num++;
		}
		int xp_num=1;
		for(int i=0;i<strlen($3);i++){
			if($3[i]==' ')xp_num++;
		}
	printf("node %d %d parallel_assign %d %s %d %s %d\n", yyproc, $$, id_num, $1, xp_num, $3, $4);
    }
    | identifier_list ASSIGN call {
	$$ = yystat++;
	int id_num=1;
		for(int i=0;i<strlen($1);i++){
			if($1[i]==' ')id_num++;
		}
	printf("node %d %d assign_call %d %s %s\n", yyproc, $$, id_num, $1, $3);
	yystat++;
    }
    ;
optional_constrain
	: {$$ = -1;}
	| CONSTRAIN '(' decider ')' {$$ = $3;}
	;
assert
	: ASSERT '(' decider ')' {
		$$ = yystat++;
		printf("node %d %d assert %d\n", yyproc, $$, $3);
	}
	| ASSUME '(' decider ')' {
		$$ = yystat++;
		printf("node %d %d assume %d\n", yyproc, $$, $3);
	}
	| CONSTRAIN '(' decider ')' {
		$$ = yystat++;
		printf("node %d %d constrain %d\n", yyproc, $$, $3);
	}
	;
call
	// arity of identifier_list and expression_list must be the same
	: IDENTIFIER '(' expression_list ')' {
		int varnum=1;
		for(int i=0;i<strlen($3);i++){
			if($3[i]==' ')varnum++;
		}
		//procedure, identifier, number of vars, vars
		int length = snprintf( NULL, 0, "%s %d %s", $1 ,varnum, $3);
		$$ = malloc( length + 1 );
		snprintf( $$, length + 1, "%s %d %s", $1 ,varnum, $3);
		free($3);
	}
	| IDENTIFIER '(' ')' {
		int length = snprintf( NULL, 0, "%s %d", $1 ,0);
		$$ = malloc( length + 1 );
		snprintf( $$, length + 1, "%s %d", $1 ,0);
	}
	;
selection_statement
	: IF '(' decider ')' THEN statement_list FI {
		$$ = yystat++;
		printf("node %d %d if %d %d %d %d\n", yyproc, $$, 1, $3, $6.fv, $6.lv);
	}
	| IF '(' decider ')' THEN statement_list ELSE statement_list FI{
		$$ = yystat++;
		printf("node %d %d if %d %d %d %d %d %d %d\n", yyproc, $$, 2, $3, $6.fv, $6.lv, -1, $8.fv, $8.lv);
	}
	| IF '(' decider ')' THEN statement_list elsif_list  {
		$$ = yystat++;
		int varnum=4;
	for(int i=0;i<strlen($7);i++){
			if($7[i]==' ')varnum++;
		}
		varnum/=3;
		printf("node %d %d if %d %d %d %d %s\n", yyproc, $$, varnum, $3, $6.fv, $6.lv, $7);
	}
    ;
elsif_list
	: elsif_list2 FI {
		$$ = $1;
	}
	| elsif_list2 ELSE statement_list FI {
		int length = snprintf( NULL, 0, "%s %d %d %d", $1, -1, $3.fv, $3.lv);
		$$ = malloc( length + 1 );
		snprintf( $$, length + 1, "%s %d %d %d", $1, -1, $3.fv, $3.lv);
		free($1);
	}
	;
elsif_list2
	: elsif_clause {
		$$ = $1;
	}
	| elsif_list2 elsif_clause {
		int length = snprintf( NULL, 0, "%s %s", $1, $2);
		$$ = malloc( length + 1 );
		snprintf( $$, length + 1, "%s %s", $1, $2);
		free($1);free($2);
	}
	;
elsif_clause
    : ELSIF '(' decider ')' THEN statement_list {
	int length = snprintf( NULL, 0, "%d %d %d", $3, $6.fv, $6.lv);
	$$ = malloc( length + 1 );
	snprintf( $$, length + 1, "%d %d %d", $3, $6.fv, $6.lv);
    }
    ;

iteration_statement
	: WHILE '(' decider ')' DO statement_list OD {
		//while
		$$ = yystat++;
		printf("node %d %d while %d %d \n", yyproc, $$, $3, $6.fv);
		printf("next %d %d %d \n", yyproc, $6.fv, $$);
	}
	;
jump_statement
	: RETURN {
	$$ = yystat++;
	printf("node %d %d return %d\n", yyproc, $$, 0);
    }
	| RETURN expression_list {
		$$ = yystat++;
	int xpnum=1;
	for(int i=0;i<strlen($2);i++){
			if($2[i]==' ')xpnum++;
		}
		printf("node %d %d return %d %s\n", yyproc, $$, xpnum ,$2);
	}
	| SKIP {
	$$ = yystat++;
	printf("node %d %d skip\n", yyproc, $$);
    }
    | GOTO label_list {
	$$ = yystat++;
	int lblnum=1;
	for(int i=0;i<strlen($2);i++){
			if($2[i]==' ')lblnum++;
		}
		printf("node %d %d goto %d %s\n", yyproc, $$, lblnum ,$2);
    }
	;

label_list
	: IDENTIFIER {
		$$ = $1;
	}
	| label_list ',' IDENTIFIER {
		int length = snprintf( NULL, 0, "%s %s", $1, $3);
		$$ = malloc( length + 1 );
		snprintf( $$, length + 1, "%s %s", $1, $3);
		free($1);
	}
    ;

dead_statement
    : DEAD identifier_list {
	$$ = yystat++;
	int varnum=1;
	for(int i=0;i<strlen($2);i++){
			if($2[i]==' ')varnum++;
		}
		//procedure, identifier, number of vars, vars
		printf("node %d %d dead %d %s\n", yyproc, $$, varnum, $2);
    }

%%


void yyerror(const char* s) {
	fprintf(stderr, "Parse error: %s\n", s);
	exit(1);
}
