// Binary standard streams for a host program that reads or writes binary data through them, on Windows.
//
// Windows opens stdin and stdout as text: a 0x1A byte ends the input and every 0x0A written grows a 0x0D. The shader
// checker takes its containers on its standard input. This file is added to such a program in the Windows build (the
// program's own sources stay as they are); the object below is made before main() runs and sets both streams to
// binary. On other hosts the file is empty. Nothing here comes from the game.
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <stdio.h>

namespace
{
struct BinaryStandardStreams
{
    BinaryStandardStreams()
    {
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
    }
} binaryStandardStreams;
}
#endif
