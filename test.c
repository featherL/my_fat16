//
// Created by xi4oyu on 6/3/21.
//

#include "my_fat.h"

const char *g_filename = "fat16.raw";

int main()
{
//    my_init(NULL);
//    my_destroy(NULL);
    char path[] = "/";
    char *name = strtok(path, "/");

    while (name != NULL) {
        printf("%s\n", name);
        name = strtok(NULL, "/");
    }

    return 0;
}