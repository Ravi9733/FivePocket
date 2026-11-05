#pragma once
// Lane fsr-sharpness (written on v458; the same file serves the merge into v461): the two user-facing scaler setting files and the one decision the presenter's poll makes from
// them and from the existing diagnostic switch files. Plain C++ with no Vulkan and no Android, so the host tests run this text
// (local-analysis/agents/fsr-sharpness/tests). direct_present.h's pollFilter() calls poll() and copies the result.
//
//   files/settings/scaler.txt         one word: fsr1 | cubic | nearest | gsr1 (letters in any case; blanks, tabs and line ends
//                                     around it ignored). Present and valid: it decides the output scaler. Absent, empty, a file
//                                     longer than 63 bytes or any other word: the diagnostic switch files decide, exactly as v458.
//                                       fsr1    = as if diag-v409-scale-cubic and diag-v450-scale-fsr1 both existed (FSR 1 EASU +
//                                                 RCAS; the cubic scaler is what runs if FSR fails its audit, as today's standing set)
//                                       cubic   = as if diag-v409-scale-cubic existed and diag-v450-scale-fsr1 did not
//                                       nearest = as if none of diag-v409-scale-cubic / -linear / diag-v450-scale-fsr1 existed
//                                       gsr1    = Snapdragon GSR 1 in a build that has its pass (v461 + this lane: pollFilter
//                                                 takes Poll::gsr1, search GSR1-HOOK; FSR 1 runs if GSR failed its audit);
//                                                 in a build without it (v458 + this lane) exactly the word fsr1
//                                     The other diagnostic files keep their meaning under the word (diag-v411-scale-compute,
//                                     diag-v42x-scale-packed / -gather with the cubic, diag-v450-fsr1-no-rcas with FSR).
//   files/settings/fsr-sharpness.txt  one decimal number, AMD's RCAS scale in stops: 0 = sharpest, each +1 halves the sharpening
//                                     (FsrRcasCon: sharpness = 2^-stops). Valid: 0 .. 4 inclusive (MinStops .. MaxStops), written as
//                                     digits with an optional '.' and more digits ("0", "0.2", "1.25"; at most 15 digits, the file
//                                     at most 63 bytes with its blanks and line ends; no sign, no exponent, '.' is the only decimal
//                                     mark). Present and valid: that value. Anything else (absent, empty, out of range - never
//                                     clamped -, other characters, a byte-order mark): exactly as v458, i.e. the diagnostic file
//                                     diag-v450-fsr1-stops if it holds a number in 0 .. 4, else fsr1::DefaultStops (0.2).
//                                     Only read while FSR 1 with RCAS is selected (as v458 reads its diagnostic file): the
//                                     words cubic / nearest and a standing diag-v450-fsr1-no-rcas make it a no-op. It does
//                                     not change GSR 1's picture (GSR has its own tuning files); in the merged build it is
//                                     still read while GSR presents, because FSR 1 stays selected underneath as the fallback.
// Both are read by the presenter's existing poll (v458's schedule, untouched: every 120th presented frame), so a change shows
// within 120 frames. A writer in another process writes a temporary file and renames it over the setting file.
// A setting file that is present but not valid is reported once in the log (RENDER_DIRECT_SETTINGS ... scaler_ignored=1 /
// sharpness_ignored=1) and otherwise treated as absent. A file the game cannot OPEN (ownership, SELinux) is indistinguishable
// from an absent one and leaves no log line: a writer gets no acknowledgement other than RENDER_DIRECT_SETTINGS appearing.
//
// EXACTNESS (tests/settings_test.cpp): with both setting files absent, poll() asks the diagnostic switch files the same
// questions in the same order as v458's pollFilter and returns the same want / compute / fast / fsr / stops for every one of
// the 2^7 switch states x 4 failure states x the stops file's contents. The only extra work is, per poll (not per frame), one
// failed open() of scaler.txt and, while FSR 1 with RCAS is the scaler, one failed open() of fsr-sharpness.txt.
#include "direct_present_reference.h" // Filter
#include "fsr1_cpu.h" // fsr1::DefaultStops
#include <cstddef>
#include <cstdint>
#include "recomp_app_dir.h" // data-paths: the app's files directory.
namespace recomp::gpu::direct::settings {
enum class Scaler : unsigned {Unset=0,Nearest=1,Cubic=2,Fsr1=3,Gsr1=4};
constexpr float MinStops=0.0f,MaxStops=4.0f;
constexpr const char* scalerName(Scaler s) noexcept {
    return s==Scaler::Nearest?"nearest":s==Scaler::Cubic?"cubic":s==Scaler::Fsr1?"fsr1":s==Scaler::Gsr1?"gsr1":"unset";
}
// The two texts poll() may ask its caller for.
enum class Text : unsigned {SettingsScaler=0,SettingsSharpness=1};
constexpr const char* SettingsDir=RECOMP_APP_PATH("/settings/");
constexpr const char* ScalerFile="scaler.txt";
constexpr const char* SharpnessFile="fsr-sharpness.txt";
constexpr std::size_t TextCap=64; // a read buffer; a file that fills it is too long to be valid

constexpr bool blank(char c) noexcept {return c==' ' || c=='\t' || c=='\n' || c=='\r';}
// scaler.txt's word. Unset: not one of the four words, or the file filled the read buffer (64 bytes or more: too long).
inline Scaler parseScaler(const char* s,std::size_t n) noexcept {
    if(!s || n>=TextCap)return Scaler::Unset;
    while(n && blank(s[n-1]))--n;
    while(n && blank(*s)){++s;--n;}
    const auto is=[&](const char* w) {
        std::size_t k=0;
        for(;k<n && w[k];++k){const char c=s[k]>='A' && s[k]<='Z'?char(s[k]-'A'+'a'):s[k];if(c!=w[k])return false;}
        return k==n && !w[k];
    };
    return is("fsr1")?Scaler::Fsr1:is("cubic")?Scaler::Cubic:is("nearest")?Scaler::Nearest:is("gsr1")?Scaler::Gsr1:Scaler::Unset;
}
// fsr-sharpness.txt's number: digits [ '.' digits ] with at least one digit, blanks around it ignored, value in
// MinStops .. MaxStops (tested on the written value before it is rounded to float, so "4.00000000000001" is out of range).
// The value is the integer written without its '.' divided by its power of ten (both exact in double), rounded to float:
// "0.2" gives the bits of 0.2f (fsr1::DefaultStops). Locale-independent on purpose (no strtof). A file that filled the read
// buffer (64 bytes or more) is too long.
inline bool parseStops(const char* s,std::size_t n,float& out) noexcept {
    if(!s || n>=TextCap)return false;
    while(n && blank(s[n-1]))--n;
    while(n && blank(*s)){++s;--n;}
    if(!n)return false;
    double mantissa=0.0,scale=1.0;bool dot=false;unsigned digits=0;
    for(std::size_t k=0;k<n;++k) {
        const char c=s[k];
        if(c=='.'){if(dot)return false;dot=true;continue;}
        if(c<'0' || c>'9')return false;
        if(++digits>15)return false; // (the mantissa stays exact in double)
        mantissa=mantissa*10.0+double(c-'0');
        if(dot)scale*=10.0;
    }
    if(!digits)return false;
    const double d=mantissa/scale;
    if(!(d>=double(MinStops) && d<=double(MaxStops)))return false;
    out=float(d);return true;
}
// v458's reading of diag-v450-fsr1-stops (std::fscanf("%f") accepted when 0 <= v <= 4) is kept as it is, in pollFilter()
// (direct_present.h): poll() receives it as its `diagStops` callback.

// What the presenter knows before the poll (v458's members).
struct State {bool filterFailed=false,fsrFailed=false;};
// What pollFilter() needs: v458's locals (want, wantCompute, fast, fsr, stops) plus where they came from.
struct Poll {
    Filter want=Filter::Nearest;bool wantCompute=false;unsigned fast=0,fsr=0;float stops=fsr1::DefaultStops;
    Scaler scaler=Scaler::Unset; // scaler.txt's word (Unset: the diagnostic files decided)
    bool stopsFromSettings=false; // stops came from fsr-sharpness.txt (false: v458's rule)
    bool gsr1=false; // GSR1-HOOK: scaler.txt asks for GSR 1 (fsr / want above are then the FSR 1 fallback)
    bool scalerIgnored=false,stopsIgnored=false; // the setting file was opened but is not valid (only for the log)
};
// The poll. exists(name): whether files/private-trace/<name> exists (v458's access()). text(which, buffer, cap): the bytes of
// that file read into buffer (at most cap), returning their count, or -1 when it cannot be opened. diagStops(out): v458's
// reading of diag-v450-fsr1-stops (true when it yields a number in 0 .. 4).
// With no valid scaler.txt and no valid fsr-sharpness.txt this is v458's pollFilter, question for question.
template<class Exists,class TextOf,class DiagStops>
inline Poll poll(const State& st,Exists&& exists,TextOf&& text,DiagStops&& diagStops) {
    Poll p;
    char buf[TextCap];
    {const int n=text(Text::SettingsScaler,buf,TextCap);if(n>0)p.scaler=parseScaler(buf,std::size_t(n));p.scalerIgnored=n>=0 && p.scaler==Scaler::Unset;}
    const bool set=p.scaler!=Scaler::Unset;
    // scaler.txt answers the three scaler-choosing switch questions itself (the files are then not consulted)
    const bool wordCubic=p.scaler==Scaler::Cubic || p.scaler==Scaler::Fsr1 || p.scaler==Scaler::Gsr1;
    const bool wordFsr=p.scaler==Scaler::Fsr1 || p.scaler==Scaler::Gsr1;
    const auto cubic=[&]{return set?wordCubic:bool(exists("diag-v409-scale-cubic"));};
    const auto linear=[&]{return set?false:bool(exists("diag-v409-scale-linear"));};
    const auto fsr1=[&]{return set?wordFsr:bool(exists("diag-v450-scale-fsr1"));};
    // --- from here v458's pollFilter, expression for expression ---
    p.want=st.filterFailed?Filter::Nearest:cubic()?Filter::Cubic:linear()?Filter::Linear:Filter::Nearest;
    p.wantCompute=exists("diag-v411-scale-compute");
    const bool wantPacked=p.want==Filter::Cubic && exists("diag-v42x-scale-packed");
    const bool wantGather=p.want==Filter::Cubic && !wantPacked && exists("diag-v42x-scale-gather");
    p.fast=wantPacked?2u:wantGather?1u:0u;
    p.fsr=!st.fsrFailed && fsr1()?(exists("diag-v450-fsr1-no-rcas")?2u:1u):0u;
    p.stops=fsr1::DefaultStops;
    if(p.fsr==1) {
        // fsr-sharpness.txt first; without a valid one, v458's diagnostic file, then the default
        float v=0.0f;const int n=text(Text::SettingsSharpness,buf,TextCap);
        if(n>0 && parseStops(buf,std::size_t(n),v)){p.stops=v;p.stopsFromSettings=true;}
        else if(diagStops(v))p.stops=v;
        p.stopsIgnored=n>=0 && !p.stopsFromSettings;
    }
    p.gsr1=p.scaler==Scaler::Gsr1; // GSR1-HOOK (the GSR lane: when its pass is in the build and has not failed, take the frame)
    return p;
}
}
