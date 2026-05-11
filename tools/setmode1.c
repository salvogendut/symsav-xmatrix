// setmode1.c — Forces CPC screen to Mode 1 and exits.
#include <symbos.h>
#include <stdlib.h>

int main(void)
{
    Screen_Mode_Set(1, 1, 80);
    exit(0);
    return 0;
}
