/* WinePS4's eboot.bin: starts the app proper, /app0/wineps4.bin.
 *
 * The PS4 maps every eboot like this one (ET_SCE_DYNEXEC, what create-fself writes) at 0x400000,
 * where 32-bit Windows programs without relocations must load. wineps4.bin is an ET_SCE_EXEC image
 * linked at 0x60000000 (app/CMakeLists.txt, app/package.sh), which the kernel maps where it was
 * linked; the base probe (baseprobe/) showed that it runs there, started this way, and that
 * 0x400000 is then free. */
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int sceSystemServiceLoadExec( const char *path, const char *args[] );

int main( void )
{
    const int r = sceSystemServiceLoadExec( "/app0/wineps4.bin", NULL );
    FILE *f;

    /* still here: it did not start */
    mkdir( "/data/WinePS4", 0777 );
    if ((f = fopen( "/data/WinePS4/launcher.log", "a" )))
    {
        fprintf( f, "launcher: /app0/wineps4.bin did not start (%08x)\n", (unsigned int)r );
        fclose( f );
    }
    return 1;
}
