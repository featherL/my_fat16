//
// Created by xi4oyu on 6/3/21.
//

#include <fcntl.h>
#include <stdio.h>

int main(int argc, char *argv[])
{
//    if (argc == 3) {
//        if (rename(argv[1], argv[2]) != 0)
//            perror("");
//    }

    int flags = 0x8201;

    if (flags & O_WRONLY) {
        puts("O_WRONLY");
    }

    if (flags & O_APPEND) {
        puts("O_APPEND");
    }

    if (flags & O_TRUNC) {
        puts("O_TRUNC");
    }

    if (flags & O_CREAT) {
        puts("O_CREAT");
    }

    if (flags & O_RDONLY) {
        puts("O_RDONLY");
    }

    return 0;
}