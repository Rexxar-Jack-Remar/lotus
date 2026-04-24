%{

// compile with: yacc -vd -p bp

#include <stdlib.h>
#include <stdio.h>

	#define YYERROR_VERBOSE

	extern int bplex(void);
	extern int bperror(const char *error);
	extern int bplineno;
	extern char *bptext;
	extern int bpleng;
	extern FILE *bpin;

unsigned int parallel_assign__counter, assign_constrain_opt__counter, CONSTRAIN_expression__counter, function_definition__counter, labelled_statement__counter, start_thread_statement__counter, end_thread_statement__counter, sync_statement__counter, atomic_begin_statement__counter, atomic_end_statement__counter;
%}

%union {
	int number;
	char *str;
  	// ...
}

%token <str> IDENTIFIER "identifier"
%token <str> TICKED_IDENTIFIER "escaped identifier"
%token <number> INTEGER "integer"
%token NONDET "*"
%token ASSIGN "assignment"
%token EQ_OP "="
%token NE_OP "<>"
%token IMPLIES "implication"
%token DECL "decl"
%token ENFORCE "enforce"
%token AND "&"
%token OR "|"
%token TERNARY "?"
%token IF "if"
%token THEN "then"
%token ELSE "else"
%token ELSIF "elsif"
%token FI "fi"
%token WHILE "while"
%token DO "do"
%token OD "od"
%token RETURN "return"
%token SKIP "skip"
%token GOTO "goto"
%token BEG "begin"
%token END "end"
%token BOOL "bool"
%token VOID "void"
%token ASSERT "assert"
%token ASSUME "assume"
%token PRINT "print"
%token CHOOSE "schoose"
%token CONSTRAIN "constrain"
%token DEAD "dead"
%token ABORTIF "abortif"
%token DFS "dfs"
%token START_THREAD "start_thread"
%token END_THREAD "end_thread"
%token SYNC "sync"
%token ATOMIC_BEGIN "atomic_begin"
%token ATOMIC_END "atomic_end"

%type <number> fun_type
%type <idref> identifier_list g_identifier_list parameter_list assign_id_list
%type <symbol> decl_id g_decl_id
%type <symbol> asgn_id
%type <exprlist> expression_list assign_expression_list
%type <stmtlist> statement_list elsif_list
%type <func> function_heading
%type <stmt> labelled_statement statement concurrent_statement
%type <stmt> parallel_assign call assert assume print selection_statement iteration_statement jump_statement dead_statement
%type <stmt> start_thread_statement end_thread_statement sync_statement atomic_begin_statement atomic_end_statement
%type <expr> primary_expression unary_expression equality_expression
%type <expr> and_expression exclusive_or_expression expression
%type <expr> or_expression enforce abortif choose_expression assign_constrain_opt
%type <labels> label_list target_label_list

%start program

%%

program:
	  pstart g_decl_list fun_list pend
	   ;

pstart:
	  {
	  } ;

pend:
	  {
	  }
	;

/***** decls *****/

g_decl_list
	: DECL g_identifier_list ';' g_decl_list
	  {
	  	// ...
	  }
	| ;
	
g_identifier_list
	: g_decl_id
	  {
	  	// ...
	  }
	| g_identifier_list ',' g_decl_id
	  {
	  	// ...
	  } ;

g_decl_id
	: IDENTIFIER			
	  {
	  	// ...
	  } ;
	
decl_list
	: DECL identifier_list ';' decl_list
	  {
	  	// ...
	  }
	| ;

identifier_list
	: decl_id
	  {
	  	// ...
	  }
	| identifier_list ',' decl_id
	  {
	  	// ...
	  } ;

decl_id	: IDENTIFIER			
	  {
	  	// ...
	  } ;
	  
/***** functions *****/

fun_list
	: function_definition
	| function_definition fun_list
	; 

function_definition
	: function_heading parms BEG decl_list enforce abortif statement_list END
	  {
	    function_definition__counter++;
	  	// ...
	  }
	 ;

function_heading
	: fun_type IDENTIFIER
	  {
	  	// ...
	  }
	;

fun_type
    : fun_search_order BOOL	{ $$ = 1; }
	| fun_search_order BOOL '<' INTEGER '>' { $$ = $4; }
    | fun_search_order VOID	{ $$ = 0; }
    ;

fun_search_order
	: DFS
	|
	;

enforce
	: ENFORCE expression ';'
	  {
	  	// ...
	  }
	|
	  {
	  	// ...
	  } ;

abortif
	: ABORTIF expression ';'
	  {
	  	// ...
	  }
	|
	  {
	  	// ...
	  } ;

parms	: '(' parameter_list ')'
	  {
	  	// ...
	  }
	  ;

parameter_list
	: identifier_list
	| 
	  {
	  	// ...
	  } ;

/***** statements *****/

statement_list
    : labelled_statement
      {
        labelled_statement__counter++;
	  	// ...
	  }
	| statement_list labelled_statement
	  {
	  	// ...
	  }
    | statement_list ';'
      {
	  	// ...
      }
    ;

labelled_statement
    : label_list concurrent_statement 
	  {
	  	// ...
	  }
    | concurrent_statement
	  {
	  	// ...
	  }
    ;

label_list
		: label_list IDENTIFIER ':'
		  {
	  		// ...
		  }
		| IDENTIFIER ':'
		  {
	  		// ...
		  }
		;

target_label_list
		: target_label_list ',' IDENTIFIER
		  {
		  	// ...
		  }
		| IDENTIFIER
		  {
		  	// ...
		  }
		;

concurrent_statement
    : statement
	| start_thread_statement ';'
	| end_thread_statement ';'
	| sync_statement ';'
	| atomic_begin_statement ';'
	| atomic_end_statement ';'
    ;
        
statement
    : dead_statement ';'
    | parallel_assign ';'
    | assert ';'
    | assume ';'
    | print ';'
    | call ';'
    | selection_statement
    | iteration_statement
    | jump_statement ';'
    ;

dead_statement
	: DEAD assign_id_list
      {
	  	// ...
      }
	;
	   
parallel_assign 
       	: assign_id_list ASSIGN assign_expression_list assign_constrain_opt
	  {
	    parallel_assign__counter++;
	  	// ...
	  }
       	| assign_id_list ASSIGN call
	  { 
	  	// ...
	  } ;

assign_constrain_opt
	: {
	  assign_constrain_opt__counter++;
#ifdef HUUUU
	$$ = NULL; 
#endif
	}
	| CONSTRAIN expression
	  {
	    CONSTRAIN_expression__counter++;
#ifdef HUUUU
	  $$ = $2; 
#endif
	  }
	;

assign_expression_list
	: choose_expression
	  {
	  	// ...
	  }
	| assign_expression_list ',' choose_expression
	  {
	  	// ...
	  } ;
  
assign_id_list
	: asgn_id
	  {
	  	// ...
	  }
	| assign_id_list ',' asgn_id
	  {
	  	// ...
	  } ;

asgn_id	
	: IDENTIFIER			
	  {
	  	// ...
	  } ;

assert	
	: ASSERT expression
	  {
	  	// ...
	  };
	  
assume	
	: ASSUME expression
	  {
	  	// ...
	  } ;

print	
	: PRINT '(' expression_list ')'
	  {
	  	// ...
	  } ;

call
	: IDENTIFIER '(' expression_list ')'
	  {
	  	// ...
	  }
	| IDENTIFIER '(' ')'
	  {
	  	// ...
	  } ;

selection_statement
	: IF choose_expression THEN statement_list elsif_list
	  {
	  	// ...
	  } ;

elsif_list
	: FI
	  {	
	  	// ...
	  }
	| ELSE statement_list FI
	  {	
	  	// ...
	  }
	| ELSIF choose_expression THEN statement_list elsif_list
	  {	
	  	// ...
	  } ;

iteration_statement
	: WHILE expression DO statement_list OD
	  {
	  	// ...
	  } ;

jump_statement
	: RETURN
	  {
	  	// ...
	  }
	| RETURN expression_list
	  {
	  	// ...
	  }
	| SKIP
	  {
	  	// ...
	  }
	| GOTO target_label_list
	  {
	  	// ...
	  } ;

start_thread_statement
    : START_THREAD GOTO IDENTIFIER
      {
        start_thread_statement__counter++;
	  	// ...
      }
    | START_THREAD IDENTIFIER '(' ')'
      {
	  	// ...
      }
    ;

end_thread_statement
    : END_THREAD
      {
        end_thread_statement__counter++;
	  	// ...
      }
    ;

sync_statement
    : SYNC IDENTIFIER
      {
        sync_statement__counter++;
	  	// not generated by SATABS - just ignore
      }
    ;

atomic_begin_statement
    : ATOMIC_BEGIN
      {
        atomic_begin_statement__counter++;
	  	// ...
      }
    ;

atomic_end_statement
    : ATOMIC_END
      {
        atomic_end_statement__counter++;
	  	// ...
      }
    ;


/***** expressions *****/

primary_expression
	: '(' expression ')'
	  {
	  	// ...
  	  }
    | NONDET
      {
	  	// ...
      }
    | CHOOSE '[' expression ',' expression ']'
	  {
	  	// ...
	  }
	| IDENTIFIER 	
	  { 
	  	// ...
	  }
	| '\'' IDENTIFIER
	  {
	  	// ...
	  }
	| INTEGER 
	  { 
	  	// ...
	  }
	;
	
unary_expression
	: primary_expression	
	| unary_operator primary_expression 
	  { 
	  	// ...
	  }
	;

unary_operator
	: '~'
	| '!' ;

equality_expression
	: unary_expression 
	| equality_expression EQ_OP unary_expression
	  { 
	  	// ...
	  }
	| equality_expression NE_OP unary_expression
	  { 
	  	// ...
	  }
	;

and_expression
	: equality_expression
	| and_expression AND equality_expression
	  { 
	  	// ...
	  }
	;

exclusive_or_expression
	: and_expression
	| exclusive_or_expression '^' and_expression
	  { 
	  	// ...
	  }
	;

or_expression
	: exclusive_or_expression
	| or_expression OR exclusive_or_expression
	  { 
	  	// ...
	  }
	| or_expression IMPLIES exclusive_or_expression
	  { 
	  	// ...
	  }
	;
	
expression
	: or_expression
	| expression TERNARY expression ':' or_expression 
	  { 
	  	// ...
	  }
	;

choose_expression
	: expression
	;

expression_list
	: choose_expression
	  { 
	  	// ...
	  }
	| expression_list ',' choose_expression
	  { 
	  	// ...
	  }
	;

%%
