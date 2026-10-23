// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 clarification 6: prints the exported names for the libraries in unixlibs.txt using the
// loader's own rule (ios_unixlib_symbol, ios_unixlib_init_symbol in ios_pure.c), in the format of
// write-exports.sh, so run-host-tests.sh can check that the build's exports.txt matches what the
// loader looks up.
//
// usage: print_unixlib_names unixlibs.txt

#include <stdio.h>
#include <string.h>

#include "ios_pure.h"

int main( int argc, char **argv )
{
    char line[512], dll[256], flag[64], name[256];
    FILE *f;

    if (argc != 2 || !(f = fopen( argv[1], "r" ))) return 2;
    while (fgets( line, sizeof(line), f ))
    {
        char *hash = strchr( line, '#' );
        int n;

        if (hash) *hash = 0;
        flag[0] = 0;
        if ((n = sscanf( line, "%255s %63s", dll, flag )) < 1) continue;
        if (ios_unixlib_symbol( dll, 0, name, sizeof(name) )) return 1;
        printf( "_%s\n", name );
        if (ios_unixlib_symbol( dll, 1, name, sizeof(name) )) return 1;
        printf( "_%s\n", name );
        if (n == 2 && !strcmp( flag, "init" ))
        {
            if (ios_unixlib_init_symbol( dll, name, sizeof(name) )) return 1;
            printf( "_%s\n", name );
        }
    }
    fclose( f );
    return 0;
}
