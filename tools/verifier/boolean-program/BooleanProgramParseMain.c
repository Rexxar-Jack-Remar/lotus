#include <stdio.h>

extern FILE *yyin;
extern int yyparse(void);

int main(int argc, char *argv[]) {
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "usage: %s input.bp [output.txt]\n", argv[0]);
    return 2;
  }
  yyin = fopen(argv[1], "r");
  if (!yyin) {
    perror(argv[1]);
    return 2;
  }
  if (argc == 3 && !freopen(argv[2], "w", stdout)) {
    perror(argv[2]);
    fclose(yyin);
    return 2;
  }
  int result = yyparse();
  fclose(yyin);
  return result;
}
