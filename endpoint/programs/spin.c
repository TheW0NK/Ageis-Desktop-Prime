#include "aegis.h"

int main(void)
{
    for (unsigned long i = 0;; i++) {
        printf("spin %lu\n", i);
        msleep(200);
    }
}
