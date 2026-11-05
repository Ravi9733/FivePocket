#pragma once
// script-mods (2026-10-25; shared library code for every port). Owner 2026-10-25: "the Script mod support in the main options menu".
//
// files/mods/ holds files that override the game's own archive entries, keyed by guest path:
//   mods/<device>/<archive path>/<entry path inside it>      e.g. mods/game/xbox360b.rpf/levels/gta5/script/script.rpf/abigail1.xsc
//   mods/<device>/<file path>                                 a whole file replaced (e.g. a complete modified .rpf)
// Nested archives are followed (script.rpf is an uncompressed entry of xbox360b.rpf). Only replacements: a mod for an entry that the
// archive does not have is skipped. Enabled only by files/settings/mods.txt "scripts=on"; read once when the library starts.
//
// How (host side only; the game's own archive code reads the result): the guest opens an archive through NtCreateFile; the library
// hands it a patched VIEW of the file: the original bytes, the archive's entry table re-encrypted with the overridden entries pointing
// past the original end, and the mods' data there (each 512-aligned). Nothing is written to disk; the view is built on the first open.
//
// Facts this relies on (verified 2026-10-25 on the disc's xbox360b.rpf with our tools/rpf7_inventory helpers, and on the recompiled
// loader sub_831E5850):
//  - RPF7 header (16 bytes, big-endian): "RPF7", entry count, names size | name shift << 28, encryption tag. The loader decrypts the
//    entry table with AES-256-ECB (whole 16-byte blocks; a partial tail stays as is) when the tag is not 0, and ALWAYS decrypts the
//    names table. The key is the 32 bytes at guest 0x83BD4C98 (sub_831A86B8 expands it); the library reads it from guest memory at
//    run time (no key in any source) and checks it on each archive (the root entry must decrypt to a directory at name offset 0).
//  - Entry (16 bytes): [0:3] resource bit 0x800000 | offset in 512-byte blocks (0x7FFFFF = directory), [3:6] stored size, [6:8] name
//    offset >> shift; directory: [8:12] first child, [12:16] child count; binary file: [8:12] size, [12:16] encrypted flag, stored size
//    0 = uncompressed; resource: [8:12] system flags, [12:16] graphics flags (version = sys>>28 << 4 | gfx>>28).
//  - A resource's payload = a 16-byte prefix + XMem LZX frames ("FF out16 in16" or "in16" = 0x8000 out, a 0 size ends) whose output
//    totals exactly the flags' virtual + physical size (51/51 resources of xbox360b.rpf). Scripts (.xsc, version 9) are additionally
//    AES-256-ECB encrypted with the same key (667/667 of script.rpf); the other resources are stored plain.
// A mod resource is an RSC7 file (16-byte header "RSC7", version, system flags, graphics flags; big- or little-endian header) followed
// by that payload, encrypted or not: the layer checks the frames against the flags, requires the original entry's version, and stores
// the payload exactly in the original entry's form (encrypting it when the original is encrypted). Anything else is skipped with a
// log line; nothing here can stop the game. A plain binary entry is replaced by the mod file's bytes, stored uncompressed.
//
// What cannot work: PowerPC code plugins (the XEX-based 360 menus) are machine code for the 360's CPU; this port runs a static
// recompilation of the game's own code, so no new PowerPC code can be loaded. PC .ysc scripts are another platform's format.
#include "guest_read_only_file.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace recomp::mods {

// ------------------------------------------------------------------------------------------------------------------- AES-256
// FIPS-197 AES with a 256-bit key (14 rounds), ECB on 16-byte blocks. Table-free S-box use: small and constant data only.
class Aes256 {
public:
    explicit Aes256(const std::uint8_t key[32]) noexcept {expand(key);}
    void encrypt(std::uint8_t* b) const noexcept {
        addRound(b,0);
        for(int r=1;r<14;++r){subBytes(b,sbox());shiftRows(b);mixColumns(b);addRound(b,r);}
        subBytes(b,sbox());shiftRows(b);addRound(b,14);
    }
    void decrypt(std::uint8_t* b) const noexcept {
        addRound(b,14);
        for(int r=13;r>0;--r){invShiftRows(b);subBytes(b,inv());addRound(b,r);invMixColumns(b);}
        invShiftRows(b);subBytes(b,inv());addRound(b,0);
    }
    // Whole blocks only; a partial tail is left as is (the game's rule for tables and payloads).
    void encryptBlocks(std::uint8_t* p,std::size_t n) const noexcept {for(std::size_t i=0;i+16<=n;i+=16)encrypt(p+i);}
    void decryptBlocks(std::uint8_t* p,std::size_t n) const noexcept {for(std::size_t i=0;i+16<=n;i+=16)decrypt(p+i);}
private:
    static const std::uint8_t* sbox() noexcept {
        static const std::array<std::uint8_t,256> s=[]{
            std::array<std::uint8_t,256> t{};std::uint8_t p=1,q=1;
            do { // generator 3 over GF(2^8); q = p's inverse
                p=std::uint8_t(p^(p<<1)^(p&0x80?0x1B:0));
                q^=std::uint8_t(q<<1);q^=std::uint8_t(q<<2);q^=std::uint8_t(q<<4);if(q&0x80)q^=0x09;
                const auto rot=[](std::uint8_t x,int k){return std::uint8_t((x<<k)|(x>>(8-k)));};
                t[p]=std::uint8_t(q^rot(q,1)^rot(q,2)^rot(q,3)^rot(q,4)^0x63);
            } while(p!=1);
            t[0]=0x63;return t;}();
        return s.data();
    }
    static const std::uint8_t* inv() noexcept {
        static const std::array<std::uint8_t,256> t=[]{std::array<std::uint8_t,256> r{};for(int i=0;i<256;++i)r[sbox()[i]]=std::uint8_t(i);return r;}();
        return t.data();
    }
    static std::uint8_t xt(std::uint8_t x) noexcept {return std::uint8_t((x<<1)^(x&0x80?0x1B:0));}
    static std::uint8_t mul(std::uint8_t a,std::uint8_t b) noexcept {std::uint8_t r=0;while(b){if(b&1)r^=a;a=xt(a);b>>=1;}return r;}
    void expand(const std::uint8_t key[32]) noexcept {
        std::memcpy(w_.data(),key,32);std::uint8_t rcon=1;
        for(int i=8;i<60;++i) {
            std::uint8_t t[4];std::memcpy(t,&w_[std::size_t(4*(i-1))],4);
            if(i%8==0){const std::uint8_t a=t[0];t[0]=std::uint8_t(sbox()[t[1]]^rcon);t[1]=sbox()[t[2]];t[2]=sbox()[t[3]];t[3]=sbox()[a];rcon=xt(rcon);}
            else if(i%8==4)for(auto& c:t)c=sbox()[c];
            for(int k=0;k<4;++k)w_[std::size_t(4*i+k)]=std::uint8_t(w_[std::size_t(4*(i-8)+k)]^t[k]);
        }
    }
    void addRound(std::uint8_t* b,int r) const noexcept {for(int i=0;i<16;++i)b[i]^=w_[std::size_t(16*r+i)];}
    static void subBytes(std::uint8_t* b,const std::uint8_t* s) noexcept {for(int i=0;i<16;++i)b[i]=s[b[i]];}
    static void shiftRows(std::uint8_t* b) noexcept {
        std::uint8_t t[16];for(int c=0;c<4;++c)for(int r=0;r<4;++r)t[4*c+r]=b[4*((c+r)%4)+r];std::memcpy(b,t,16);
    }
    static void invShiftRows(std::uint8_t* b) noexcept {
        std::uint8_t t[16];for(int c=0;c<4;++c)for(int r=0;r<4;++r)t[4*((c+r)%4)+r]=b[4*c+r];std::memcpy(b,t,16);
    }
    static void mixColumns(std::uint8_t* b) noexcept {
        for(int c=0;c<4;++c){std::uint8_t* s=b+4*c;const std::uint8_t a0=s[0],a1=s[1],a2=s[2],a3=s[3];
            s[0]=std::uint8_t(xt(a0)^xt(a1)^a1^a2^a3);s[1]=std::uint8_t(a0^xt(a1)^xt(a2)^a2^a3);
            s[2]=std::uint8_t(a0^a1^xt(a2)^xt(a3)^a3);s[3]=std::uint8_t(xt(a0)^a0^a1^a2^xt(a3));}
    }
    static void invMixColumns(std::uint8_t* b) noexcept {
        for(int c=0;c<4;++c){std::uint8_t* s=b+4*c;const std::uint8_t a0=s[0],a1=s[1],a2=s[2],a3=s[3];
            s[0]=std::uint8_t(mul(a0,14)^mul(a1,11)^mul(a2,13)^mul(a3,9));s[1]=std::uint8_t(mul(a0,9)^mul(a1,14)^mul(a2,11)^mul(a3,13));
            s[2]=std::uint8_t(mul(a0,13)^mul(a1,9)^mul(a2,14)^mul(a3,11));s[3]=std::uint8_t(mul(a0,11)^mul(a1,13)^mul(a2,9)^mul(a3,14));}
    }
    std::array<std::uint8_t,240> w_{};
};

// ------------------------------------------------------------------------------------------------------- formats (pure)
inline std::uint32_t be32(const std::uint8_t* p) noexcept {return std::uint32_t(p[0])<<24|std::uint32_t(p[1])<<16|std::uint32_t(p[2])<<8|p[3];}
inline std::uint32_t le32(const std::uint8_t* p) noexcept {return std::uint32_t(p[3])<<24|std::uint32_t(p[2])<<16|std::uint32_t(p[1])<<8|p[0];}
inline void put32(std::uint8_t* p,std::uint32_t v) noexcept {p[0]=std::uint8_t(v>>24);p[1]=std::uint8_t(v>>16);p[2]=std::uint8_t(v>>8);p[3]=std::uint8_t(v);}
inline void put24(std::uint8_t* p,std::uint32_t v) noexcept {p[0]=std::uint8_t(v>>16);p[1]=std::uint8_t(v>>8);p[2]=std::uint8_t(v);}

constexpr std::uint32_t Directory=0x7FFFFF,ResourceBit=0x800000;
constexpr std::uint64_t Block=512;
struct Entry {
    std::uint8_t raw[16]{};
    std::uint32_t head() const noexcept {return std::uint32_t(raw[0])<<16|std::uint32_t(raw[1])<<8|raw[2];}
    bool resource() const noexcept {return head()&ResourceBit;}
    bool directory() const noexcept {return (head()&0x7FFFFF)==Directory;}
    std::uint64_t offset() const noexcept {return std::uint64_t(head()&0x7FFFFF)*Block;}
    std::uint32_t stored() const noexcept {return std::uint32_t(raw[3])<<16|std::uint32_t(raw[4])<<8|raw[5];}
    std::uint32_t nameUnits() const noexcept {return std::uint32_t(raw[6])<<8|raw[7];}
    std::uint32_t f5() const noexcept {return be32(raw+8);}
    std::uint32_t f6() const noexcept {return be32(raw+12);}
    std::uint32_t version() const noexcept {return (f5()>>28)<<4|(f6()>>28);}
    std::uint64_t binarySize() const noexcept {return stored()?stored():f5();}   // bytes in the archive
};
// The virtual or physical size an RSC7 flag word declares (tools/rpf7_inventory/rsc7_inspect.py resource_size).
inline std::uint64_t resourceSize(std::uint32_t f) noexcept {
    const std::uint64_t base=0x200ull<<(f&0xF);
    const std::uint64_t m=(((f>>27)&1u)<<0)+(((f>>26)&1u)<<1)+(((f>>25)&1u)<<2)+(((f>>24)&1u)<<3)+(((f>>17)&0x7Fu)<<4)+
        (((f>>11)&0x3Fu)<<5)+(((f>>7)&0xFu)<<6)+(((f>>5)&0x3u)<<7)+(((f>>4)&1u)<<8);
    return base*m;
}
// The XMem frames after the 16-byte prefix: their output total, or nothing when they do not end inside the data (<= 32 bytes left).
inline std::optional<std::uint64_t> xmemOutput(const std::uint8_t* p,std::size_t n) noexcept {
    std::size_t pos=16;std::uint64_t out=0;
    if(n<18)return std::nullopt;
    while(pos+2<=n) {
        std::uint32_t in=0,o=0x8000;
        if(p[pos]==0xFF){if(pos+5>n)return std::nullopt;o=std::uint32_t(p[pos+1])<<8|p[pos+2];in=std::uint32_t(p[pos+3])<<8|p[pos+4];pos+=5;}
        else {in=std::uint32_t(p[pos])<<8|p[pos+1];pos+=2;}
        if(!in)break;
        if(in>n-pos || !o)return std::nullopt;
        pos+=in;out+=o;
    }
    if(n-pos>32)return std::nullopt;
    return out;
}
enum class Form:std::uint8_t {Unknown,Plain,Encrypted};
// Which form a resource payload is in, checked against the size its flags declare.
inline Form payloadForm(const std::uint8_t* p,std::size_t n,std::uint64_t declared,const Aes256* aes) noexcept {
    if(const auto o=xmemOutput(p,n);o && *o==declared)return Form::Plain;
    if(!aes)return Form::Unknown;
    std::vector<std::uint8_t> d(p,p+n);aes->decryptBlocks(d.data(),d.size());
    if(const auto o=xmemOutput(d.data(),d.size());o && *o==declared)return Form::Encrypted;
    return Form::Unknown;
}
struct Rsc7 {std::uint32_t version=0,sys=0,gfx=0;};
// A standalone RSC7 header (either byte order); its flags carry the version nibbles the archive entry needs.
inline std::optional<Rsc7> parseRsc7(const std::uint8_t* h,std::size_t n) noexcept {
    if(n<16 || std::memcmp(h,"RSC7",4))return std::nullopt;
    for(int order=0;order<2;++order) {
        const auto rd=[&](int at){return order==0?be32(h+at):le32(h+at);};
        Rsc7 r{rd(4),rd(8),rd(12)};
        if(r.version>0xFF)continue;
        r.sys=(r.sys&0x0FFFFFFF)|((r.version>>4)<<28);r.gfx=(r.gfx&0x0FFFFFFF)|((r.version&0xF)<<28);
        return r;
    }
    return std::nullopt;
}

// ------------------------------------------------------------------------------------------------------------ the view
using File=std::shared_ptr<const recomp::bootstrap::GuestReadOnlyFile>;
// A file assembled from pieces: original ranges, mod file ranges and bytes held in memory. Immutable once built.
class PatchedFile final : public recomp::bootstrap::GuestReadOnlyFile {
public:
    struct Piece {std::uint64_t at=0,size=0;File file;std::uint64_t from=0;std::shared_ptr<const std::vector<std::uint8_t>> bytes;};
    explicit PatchedFile(std::vector<Piece> pieces,std::uint64_t size) noexcept:pieces_(std::move(pieces)),size_(size) {}
    std::uint64_t size() const noexcept override {return size_;}
    bool read(std::uint64_t offset,std::uint8_t* out,std::size_t bytes) const noexcept override {
        if(offset>size_ || bytes>size_-offset || (bytes && !out))return false;
        auto it=std::upper_bound(pieces_.begin(),pieces_.end(),offset,[](std::uint64_t o,const Piece& p){return o<p.at;});
        if(it!=pieces_.begin())--it;
        std::uint64_t at=offset;std::size_t done=0;
        while(done<bytes) {
            if(it==pieces_.end())return false;
            if(at<it->at){ // a gap (alignment padding): zeros
                const auto n=std::size_t(std::min<std::uint64_t>(it->at-at,bytes-done));std::memset(out+done,0,n);done+=n;at+=n;continue;}
            if(at>=it->at+it->size){++it;continue;}
            const auto in=at-it->at;const auto n=std::size_t(std::min<std::uint64_t>(it->size-in,bytes-done));
            if(it->bytes)std::memcpy(out+done,it->bytes->data()+it->from+in,n);
            else {
                // pass through in pieces of at most 1 MiB (the native file's bound per read)
                for(std::size_t k=0;k<n;){const std::size_t c=std::min<std::size_t>(n-k,1u<<20);
                    if(!it->file->read(it->from+in+k,out+done+k,c))return false;k+=c;}
            }
            done+=n;at+=n;
        }
        return true;
    }
private:
    std::vector<Piece> pieces_;std::uint64_t size_;
};

using LogFn=void(*)(const char* line);
struct Override {std::string entry;std::string key;File data;};   // entry: path inside the archive ("a/b/c.xsc"), lower case
struct Result {std::string key;bool ok=false;std::string detail;};

class ArchiveBuilder {
public:
    ArchiveBuilder(const Aes256* aes,LogFn log,std::vector<Result>* results) noexcept:aes_(aes),log_(log),results_(results) {}
    // The patched archive at [base, base+length) of `source` as a list of pieces relative to the archive's own start, with the
    // overrides applied. Empty when nothing could be applied (the caller then keeps the original).
    struct Layout {std::vector<PatchedFile::Piece> pieces;std::uint64_t length=0;unsigned applied=0;};
    Layout build(const File& source,std::uint64_t base,std::uint64_t length,const std::vector<Override>& overrides,const std::string& where) {
        Layout out;
        std::uint8_t h[16];
        if(length<16 || !source->read(base,h,16) || std::memcmp(h,"RPF7",4)){skipAll(overrides,"not an RPF7 archive: "+where);return out;}
        const std::uint32_t count=be32(h+4),namesInfo=be32(h+8),tag=be32(h+12);
        const std::uint32_t namesSize=namesInfo&0x0FFFFFFF,shift=(namesInfo>>28)&7;
        if(!count || count>200000 || !namesSize || namesSize>(16u<<20) || 16+std::uint64_t(count)*16+namesSize>length)
            {skipAll(overrides,"bad archive header: "+where);return out;}
        std::vector<std::uint8_t> table(std::size_t(count)*16),names(namesSize);
        if(!source->read(base+16,table.data(),table.size()) || !source->read(base+16+table.size(),names.data(),names.size()))
            {skipAll(overrides,"archive table unreadable: "+where);return out;}
        if(!aes_){skipAll(overrides,"no archive key: "+where);return out;}
        if(tag)aes_->decryptBlocks(table.data(),table.size());
        aes_->decryptBlocks(names.data(),names.size());
        std::vector<Entry> e(count);for(std::uint32_t i=0;i<count;++i)std::memcpy(e[i].raw,table.data()+16*i,16);
        if(!e[0].directory() || e[0].nameUnits()!=0 || names[0]!=0){skipAll(overrides,"archive key check failed: "+where);return out;}
        const auto nameOf=[&](const Entry& x)->std::string{
            const std::size_t at=std::size_t(x.nameUnits())<<shift;if(at>=names.size())return {};
            const auto* s=reinterpret_cast<const char*>(names.data()+at);return std::string(s,strnlen(s,names.size()-at));};
        const auto child=[&](std::uint32_t dir,const std::string& want)->std::optional<std::uint32_t>{
            const std::uint64_t first=e[dir].f5(),n=e[dir].f6();
            if(first>count || n>count-first)return std::nullopt;
            for(std::uint64_t i=first;i<first+n;++i){std::string nm=nameOf(e[std::size_t(i)]);for(auto& c:nm)c=char(std::tolower(static_cast<unsigned char>(c)));
                if(nm==want)return std::uint32_t(i);}
            return std::nullopt;};

        // The appended region: after the original archive, 512-aligned.
        out.pieces.push_back({0,length,source,base,nullptr});
        std::uint64_t end=(length+Block-1)/Block*Block;
        bool changed=false;
        // Group by the first component that is a nested archive; resolve plain entries directly.
        std::map<std::uint32_t,std::vector<Override>> nested;std::map<std::uint32_t,std::string> nestedPath;
        for(const auto& o:overrides) {
            std::uint32_t at=0;std::size_t pos=0;bool fail=false;std::optional<std::uint32_t> target;
            while(pos<=o.entry.size()) {
                const std::size_t slash=o.entry.find('/',pos);
                const std::string part=o.entry.substr(pos,slash==std::string::npos?std::string::npos:slash-pos);
                const auto c=child(at,part);
                if(!c){fail=true;break;}
                if(slash==std::string::npos){target=*c;break;}
                if(e[*c].directory()){at=*c;pos=slash+1;continue;}
                if(part.size()>4 && part.compare(part.size()-4,4,".rpf")==0 && !e[*c].resource()) {
                    nested[*c].push_back({o.entry.substr(slash+1),o.key,o.data});nestedPath[*c]=where+"/"+o.entry.substr(0,slash);break;}
                fail=true;break;
            }
            if(fail){result(o.key,false,"the archive has no such entry");continue;}
            if(!target)continue; // (handed to a nested archive)
            Entry& x=e[*target];
            if(x.directory()){result(o.key,false,"names a directory");continue;}
            std::vector<PatchedFile::Piece> add;std::uint64_t size=0;std::string why;
            if(!(x.resource()?prepareResource(source,base,x,o,add,size,why):prepareBinary(x,o,add,size,why))){result(o.key,false,why);continue;}
            for(auto& p:add){p.at+=end;out.pieces.push_back(p);}
            setOffset(x,end);end=(end+size+Block-1)/Block*Block;changed=true;++out.applied;
            result(o.key,true,why);
        }
        for(auto& [index,list]:nested) {
            Entry& x=e[index];
            if(x.stored()!=0 || x.f6()!=0){skipAll(list,"nested archive is compressed or encrypted: "+nestedPath[index]);continue;}
            const auto inner=build(source,base+x.offset(),x.f5(),list,nestedPath[index]);
            if(!inner.applied)continue;
            if(inner.length>0xFFFFFFFFull){skipAll(list,"nested archive too large: "+nestedPath[index]);continue;}
            for(auto p:inner.pieces){p.at+=end;out.pieces.push_back(p);}
            setOffset(x,end);put32(x.raw+8,std::uint32_t(inner.length));
            end=(end+inner.length+Block-1)/Block*Block;changed=true;out.applied+=inner.applied;
        }
        if(!changed){out.pieces.clear();return out;}
        if(end/Block>=Directory){skipAll(overrides,"archive would exceed the 4 GB offset range: "+where);out=Layout{};return out;}
        // The entry table as the loader reads it: re-encrypted when the header says so.
        auto patched=std::make_shared<std::vector<std::uint8_t>>(table.size());
        for(std::uint32_t i=0;i<count;++i)std::memcpy(patched->data()+16*i,e[i].raw,16);
        if(tag)aes_->encryptBlocks(patched->data(),patched->size());
        // split the original piece around the table
        out.pieces[0].size=16;
        out.pieces.insert(out.pieces.begin()+1,PatchedFile::Piece{16,table.size(),nullptr,0,patched});
        out.pieces.insert(out.pieces.begin()+2,PatchedFile::Piece{16+table.size(),length-16-table.size(),source,base+16+table.size(),nullptr});
        out.length=end;
        std::sort(out.pieces.begin(),out.pieces.end(),[](const auto& a,const auto& b){return a.at<b.at;});
        return out;
    }
private:
    static void setOffset(Entry& x,std::uint64_t at) noexcept {put24(x.raw,(x.resource()?ResourceBit:0u)|std::uint32_t(at/Block));}
    bool prepareResource(const File& source,std::uint64_t base,Entry& x,const Override& o,std::vector<PatchedFile::Piece>& add,std::uint64_t& size,std::string& why) {
        const std::uint64_t n=o.data->size();
        if(n<16+18 || n>(64u<<20)){why="not a resource (size)";return false;}
        std::uint8_t h[16];if(!o.data->read(0,h,16)){why="unreadable";return false;}
        const auto r=parseRsc7(h,16);
        if(!r){why="not an RSC7 resource";return false;}
        if(r->version!=x.version()){char b[80];std::snprintf(b,sizeof b,"resource version %u, the game's entry has %u",r->version,x.version());why=b;return false;}
        const std::uint64_t payload=n-16;
        if(payload>=0xFFFFFF){why="resource too large for an RPF7 entry";return false;}
        auto data=std::make_shared<std::vector<std::uint8_t>>(std::size_t(payload));
        if(!o.data->read(16,data->data(),data->size())){why="unreadable";return false;}
        const std::uint64_t declared=resourceSize(r->sys)+resourceSize(r->gfx);
        const Form mine=payloadForm(data->data(),data->size(),declared,aes_);
        if(mine==Form::Unknown){why="payload does not match its RSC7 flags (not an Xbox 360 resource?)";return false;}
        // the original entry's form decides how the game reads it
        std::vector<std::uint8_t> orig(std::size_t(std::min<std::uint64_t>(x.stored(),64u<<20)));
        if(orig.empty() || !source->read(base+x.offset(),orig.data(),orig.size())){why="original entry unreadable";return false;}
        const Form theirs=payloadForm(orig.data(),orig.size(),resourceSize(x.f5()&0x0FFFFFFF)+resourceSize(x.f6()&0x0FFFFFFF),aes_);
        if(theirs==Form::Unknown){why="original entry not understood";return false;}
        if(mine!=theirs){if(theirs==Form::Encrypted)aes_->encryptBlocks(data->data(),data->size());else aes_->decryptBlocks(data->data(),data->size());}
        put24(x.raw+3,std::uint32_t(payload));put32(x.raw+8,r->sys);put32(x.raw+12,r->gfx);
        add.push_back({0,payload,nullptr,0,data});size=payload;
        char b[96];std::snprintf(b,sizeof b,"resource v%u %llu bytes%s",r->version,static_cast<unsigned long long>(payload),
            theirs==Form::Encrypted?(mine==Form::Plain?" (encrypted for the archive)":" (already encrypted)"):"");
        why=b;return true;
    }
    static bool prepareBinary(Entry& x,const Override& o,std::vector<PatchedFile::Piece>& add,std::uint64_t& size,std::string& why) {
        const std::uint64_t n=o.data->size();
        if(!n || n>0xFFFFFFFFull){why="empty or too large";return false;}
        put24(x.raw+3,0);put32(x.raw+8,std::uint32_t(n));put32(x.raw+12,0); // uncompressed, not encrypted
        add.push_back({0,n,o.data,0,nullptr});size=n;
        why="file "+std::to_string(n)+" bytes";return true;
    }
    void result(const std::string& key,bool ok,const std::string& detail) {
        if(results_)results_->push_back({key,ok,detail});
        if(log_){char line[400];std::snprintf(line,sizeof line,"RENDER_MODS %s %s: %s",ok?"applied":"skipped",key.c_str(),detail.c_str());log_(line);}
    }
    void skipAll(const std::vector<Override>& list,const std::string& why) {for(const auto& o:list)result(o.key,false,why);}
    const Aes256* aes_;LogFn log_;std::vector<Result>* results_;
};

// ----------------------------------------------------------------------------------------------------------- the layer
// files/settings/mods.txt: "scripts=on" (1 / true / yes) enables; anything else = off.
inline bool parseEnabled(const char* text,std::size_t n) noexcept {
    bool on=false;std::size_t i=0;
    while(i<n) {
        while(i<n && (text[i]==' ' || text[i]=='\n' || text[i]=='\r' || text[i]=='\t'))++i;
        std::size_t j=i;while(j<n && text[j]!=' ' && text[j]!='\n' && text[j]!='\r' && text[j]!='\t')++j;
        std::string w(text+i,j-i);for(auto& c:w)c=char(std::tolower(static_cast<unsigned char>(c)));
        if(w.rfind("scripts=",0)==0){const auto v=w.substr(8);on=v=="on" || v=="1" || v=="true" || v=="yes";}
        i=j;
    }
    return on;
}
// "game:\xbox360b.rpf" -> "game/xbox360b.rpf" (a normalized guest path: lower case, backslashes; see normalizeGuestFilePath).
inline std::string keyOf(std::string_view guest) {
    std::string k;k.reserve(guest.size());
    for(std::size_t i=0;i<guest.size();++i){char c=guest[i];
        if(c==':'){if(i+1<guest.size() && (guest[i+1]=='\\' || guest[i+1]=='/'))++i;k+='/';continue;}
        if(c=='\\')c='/';k+=char(std::tolower(static_cast<unsigned char>(c)));}
    while(!k.empty() && k.back()=='/')k.pop_back();
    return k;
}

class Layer {
public:
    using Opener=File(*)(const std::string& hostPath);
    Layer(LogFn log=nullptr) noexcept:log_(log) {}
    // The mods found (key = path under files/mods, lower case, '/'): set once at start, before any guest file is opened.
    void setMods(std::map<std::string,File> mods) {mods_=std::move(mods);}
    void setKey(const std::uint8_t key[32]) {aes_.emplace(key);}
    bool active() const noexcept {return !mods_.empty();}
    // The file the guest gets for `guest` (normalized guest path), given the original (null = absent): the original when no mod
    // applies. Built once per path; later calls return the same object (its size answers the metadata query consistently).
    File view(std::string_view guest,const File& original) {
        if(mods_.empty())return original;
        const std::string key=keyOf(guest);
        if(!covers(key))return original;
        std::lock_guard l(m_);   // (guest threads open files concurrently)
        if(auto it=built_.find(key);it!=built_.end())return it->second?it->second:original;
        File result;
        if(auto whole=mods_.find(key);whole!=mods_.end()) {
            if(original){result=whole->second;note(key,true,"whole file "+std::to_string(whole->second->size())+" bytes");}
            else note(key,false,"the game has no such file");
        } else if(original) {
            std::vector<Override> list;const std::string prefix=key+"/";
            for(auto it=mods_.lower_bound(prefix);it!=mods_.end() && it->first.compare(0,prefix.size(),prefix)==0;++it)
                list.push_back({it->first.substr(prefix.size()),it->first,it->second});
            if(!list.empty()) {
                ArchiveBuilder b(aes_?&*aes_:nullptr,log_,&results_);
                auto layout=b.build(original,0,original->size(),list,key);
                if(layout.applied)result=std::make_shared<PatchedFile>(std::move(layout.pieces),layout.length);
            }
        }
        built_[key]=result;
        return result?result:original;
    }
    // A mod for this file or for an entry inside it?
    bool covers(std::string_view guest) const {return !mods_.empty() && coversKey(keyOf(guest));}
    // Every mod's outcome so far (applied / skipped, why), for the log and the app's status line.
    std::vector<Result> results() {std::lock_guard l(m_);return results_;}
    const std::map<std::string,File>& mods() const noexcept {return mods_;}
private:
    bool covers(const std::string& key) const {return coversKey(key);}
    bool coversKey(const std::string& key) const {
        if(mods_.empty() || key.empty())return false;
        if(mods_.count(key))return true;
        const std::string prefix=key+"/";const auto it=mods_.lower_bound(prefix);
        return it!=mods_.end() && it->first.compare(0,prefix.size(),prefix)==0;
    }
    void note(const std::string& key,bool ok,const std::string& d) {
        results_.push_back({key,ok,d});
        if(log_){char line[400];std::snprintf(line,sizeof line,"RENDER_MODS %s %s: %s",ok?"applied":"skipped",key.c_str(),d.c_str());log_(line);}
    }
    LogFn log_;std::map<std::string,File> mods_;std::optional<Aes256> aes_;
    std::mutex m_;std::map<std::string,File> built_;std::vector<Result> results_;
};

} // namespace recomp::mods
