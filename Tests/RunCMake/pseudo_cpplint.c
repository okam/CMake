#include <stdio.h>
#include <string.h>

int main(int argc, char* argv[])
{
  int error = 0;
  int i;
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--error") == 0) {
      error = 1;
    }
    if (argv[i][0] != '-') {
      if (error) {
        fprintf(stderr, "%s:0:  message  [category/category] [0]\n", argv[i]);
        fprintf(stdout, "Total errors found: 1\n");
        return 1;
      }
      fprintf(stdout, "Done processing %s\n", argv[i]);
      fprintf(stdout, "Total errors found: 0\n");
      return 0;
    }
  }
}
