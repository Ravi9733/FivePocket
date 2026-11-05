#include "guest_symbolic_links.h"
#include <cassert>
using namespace recomp::bootstrap;
int main() {
    GuestSymbolicLinks links;
    assert(!links.erase("\\??\\xbmovie:") && links.size()==0);
    assert(links.resolve("GAME:\\folder\\file.bin")=="game:\\folder\\file.bin");
    assert(links.create("\\??\\MOVIE:","game:\\movies"));
    assert(links.contains("movie:") && links.resolve("movie:\\intro.bin")=="game:\\movies\\intro.bin");
    assert(links.create("second:","movie:\\cutscenes"));
    assert(links.resolve("second:\\intro.bin")=="game:\\movies\\cutscenes\\intro.bin");
    for (auto invalid : {"",":","relative","../bad:","game:\\file", "a:b:","a?:", "\\Device\\CdRom0"})
        assert(!links.create(invalid,"game:\\") && !links.erase(invalid));
    assert(!links.create("movie:","other:\\") && !links.create("self:","self:\\"));
    assert(!links.create("bad:","game:\\..\\bad") && !links.create("bad:","/tmp/host"));
    assert(links.create("future:","unknown:\\"));
    assert(!links.create("unknown:","future:\\") && !links.contains("unknown:"));
    const auto before=links.resolve("second:\\intro.bin");
    assert(links.erase("\\??\\MoViE:") && !links.erase("movie:"));
    assert(links.resolve("second:\\intro.bin")=="movie:\\cutscenes\\intro.bin");
    assert(before=="game:\\movies\\cutscenes\\intro.bin");
    links.clear();
    for (unsigned i=0;i<GuestSymbolicLinks::Limit;++i)
        assert(links.create("link"+std::to_string(i)+":", "game:\\"));
    assert(!links.create("overflow:","game:\\") && links.size()==GuestSymbolicLinks::Limit);
    assert(links.erase("link0:") && links.create("replacement:","game:\\"));
    links.clear();
    assert(links.create("a:","b:\\"+std::string(900,'a')));
    assert(!links.create("b:","game:\\"+std::string(200,'b')) && links.size()==1);
}
