#include <stdio.h>

const char *tk_where(void);

int main(void)
{
    /* __FILE__ in both: the paths the prefix maps rewrite. */
    printf("%s %s\n", __FILE__, tk_where());
    return 0;
}
