// gpu-cost glue: included by generic_renderer.cpp inside its anonymous namespace (after GLOG and the other packages' window
// hooks). Every renderer function that calls into it does so unconditionally (no new branch in those functions: their control
// flow, and so their PGO records, stay v442's); all state lives here, outside Renderer (its layout stays v442's).
//
// Switches (files/private-trace, polled at the 60-present window boundary, i.e. while the worker is drained):
//   diag-gc-index-fold  pipelines made while it is on use modules whose draw-invariant shared-block loads are constants
//                       (spirv_gc_fold.h: descriptor indices, swap flags, clip plane); same pixels by construction. The
//                       pipeline key gets a salt while on, so pipelines of both settings stay cached side by side (the first
//                       window after the first switch-on compiles the folded pipelines). draw() audits the shared block it
//                       writes against the fold's table (one draw in 256): a mismatch turns the fold off for the rest of the
//                       run (RENDER_DIAG_GC audit_failures).
//   diag-gc-all         = diag-gc-index-fold (the exact saving; the profile below is diagnostic only)
//   diag-gc-profile     (diagnostic; with diag-v287-gpu-passes) in v287's profiled frames each pass begins with a
//                       BOTTOM_OF_PIPE timestamp (everything recorded before it done) instead of TOP_OF_PIPE, so pass times
//                       exclude the earlier work's tail and the gaps between passes hold the out-of-pass work; each gap's
//                       recorded operations are counted (resolves, depth resolves, gather dispatches, texture uploads,
//                       barriers, chunk submissions): RENDER_DIAG_GC_GAPS per profiled frame, RENDER_DIAG_GC_GAP per gap
//                       with diag-v338-pass-dump.
// Log: RENDER_GENERIC_V210Z gc_index_fold / gc_all / gc_profile (1 = on for the whole window that just ended; the names
// fast_exp.sh derives from the switch files; ab_windows.py / ab_joint.py merge V210Z lines) and RENDER_DIAG_GC (counters since boot).
namespace gpucost {
constexpr std::uint64_t KeySalt=0x6C1F0D5A9E3779B9ull; // XORed into pipeline keys while the fold is on (0 when off: keys unchanged)
constexpr std::size_t RecordCapBytes=std::size_t{48}<<20; // module words kept for folding (beyond: modules stay unfolded)
struct Gap {std::uint32_t barriers=0,chunks=0,resolves=0,depthResolves=0,gathers=0,uploads=0,clears=0;};
struct Counters {std::uint64_t barriers=0,chunks=0,resolves=0,depthResolves=0,gathers=0,uploads=0,clears=0;};
struct State {
#if defined(RECOMP_GC_TEST_FOLD) // host harness: the fold on from the first draw (before the first poll)
    bool fold=RECOMP_GC_TEST_FOLD!=0;std::atomic<std::uint64_t> keySalt{RECOMP_GC_TEST_FOLD!=0?KeySalt:0};
#else
    bool fold=false;std::atomic<std::uint64_t> keySalt{0};
#endif
#if defined(RECOMP_GC_TEST_PROFILE)
    bool profile=RECOMP_GC_TEST_PROFILE!=0;
#else
    bool profile=false;
#endif
    bool auditDisabled=false,windowAuditFailed=false;
    std::atomic<bool> prewarmFold{false};std::atomic<std::uint64_t> prewarmCompiled{0},prewarmFailed{0},prewarmUnfoldable{0}; // (prewarm thread)
    std::mutex m; // words / folded: graphicsModule() also runs on the prewarm thread
    std::unordered_map<VkShaderModule,std::vector<std::uint32_t>> words; // module -> the words it was created from
    std::unordered_map<VkShaderModule,VkShaderModule> folded;            // module -> its folded module (itself: nothing to fold)
    std::size_t recordedBytes=0;
    std::uint64_t foldedModules=0,foldedLoads=0,sameModules=0,unrecorded=0,createFailed=0,lookups=0,auditCalls=0,auditChecks=0,
        auditFailures=0,recordSkipped=0;
    // profile: per frame slot, the counters at the last pass end (or the frame's begin) and each pass's preceding gap
    std::array<Counters,2> last{};std::array<std::vector<Gap>,2> gaps;
};
State state; // (the worker / present path; words and folded under m)

inline Counters now(std::uint64_t barriers,std::uint64_t chunks,std::uint64_t resolves,std::uint64_t depthResolves,std::uint64_t gathers,
                    std::uint64_t uploads,std::uint64_t clears){return {barriers,chunks,resolves,depthResolves,gathers,uploads,clears};}
inline std::uint32_t delta(std::uint64_t a,std::uint64_t b){return std::uint32_t(a>=b?a-b:a);} // (a window reset in between: since it)
#if defined(RECOMP_GC_TEST_STAGECHECK) // host test: the code of each folded module made, and the stage check's counts
inline std::unordered_map<VkShaderModule,std::vector<std::uint32_t>> testCode;inline std::uint64_t stageChecks=0,stageMismatches=0;
#endif
}

// graphicsModule(): remember the words module m was created from (and forget the folded copy of an earlier module that had
// the same handle).
[[gnu::noinline]] void gcRecordModule(VkDevice dev,VkShaderModule m,const std::uint32_t* code,std::size_t words) noexcept {
    if(!m || !code)return;
    auto& s=gpucost::state;
    try {
        std::lock_guard lock(s.m);
        if(auto it=s.folded.find(m);it!=s.folded.end()){if(it->second!=m)vkDestroyShaderModule(dev,it->second,nullptr);s.folded.erase(it);}
        auto& w=s.words[m];s.recordedBytes-=std::min(s.recordedBytes,w.size()*4);
        if(s.recordedBytes+words*4>gpucost::RecordCapBytes){w.clear();w.shrink_to_fit();++s.recordSkipped;return;}
        w.assign(code,code+words);s.recordedBytes+=words*4;
    } catch(...) {}
}
// The prewarm thread, before it destroys one of its own modules: drop its recorded words (and a folded copy: none is made for
// prewarm modules, gcPrewarmFolded destroys its own).
[[gnu::noinline]] void gcForgetModule(VkShaderModule m) noexcept {
    if(!m)return;
    auto& s=gpucost::state;
    try {
        std::lock_guard lock(s.m);
        if(const auto it=s.words.find(m);it!=s.words.end()){s.recordedBytes-=std::min(s.recordedBytes,it->second.size()*4);s.words.erase(it);}
    } catch(...) {}
}
// pipeline(): the module a new pipeline uses for stage module m (m itself while the fold is off, m has nothing to fold, or
// its words were not kept).
[[gnu::noinline]] VkShaderModule gcModuleFor(VkDevice dev,VkShaderModule m) noexcept {
    auto& s=gpucost::state;
    if(!s.fold || !m)return m;
    try {
        std::lock_guard lock(s.m);
        ++s.lookups;
        if(const auto it=s.folded.find(m);it!=s.folded.end())return it->second;
        const auto w=s.words.find(m);
        if(w==s.words.end() || w->second.empty()){++s.unrecorded;s.folded[m]=m;return m;}
        auto r=spirv_gc::foldSharedWords(w->second.data(),w->second.size());
        if(r.code.empty()){++s.sameModules;s.folded[m]=m;return m;}
        VkShaderModuleCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;c.codeSize=r.code.size()*4;c.pCode=r.code.data();
        VkShaderModule f=VK_NULL_HANDLE;
        if(vkCreateShaderModule(dev,&c,nullptr,&f)!=VK_SUCCESS || !f){++s.createFailed;s.folded[m]=m;return m;}
        ++s.foldedModules;s.foldedLoads+=r.folded;s.folded[m]=f;
#if defined(RECOMP_GC_TEST_STAGECHECK)
        gpucost::testCode[f]=r.code;
#endif
        return f;
    } catch(...) {return m;}
}
#if defined(RECOMP_GC_TEST_STAGECHECK) // host test: with the fold on, each stage uses the folded copy of its module (or the
// module itself when nothing folds); off, the module itself
inline void gcCheckStage(VkShaderModule original,VkShaderModule used) {
    auto& s=gpucost::state;if(!original)return;
    ++gpucost::stageChecks;
    std::vector<std::uint32_t> w;{std::lock_guard lock(s.m);if(const auto it=s.words.find(original);it!=s.words.end())w=it->second;}
    const auto r=w.empty()?spirv_gc::FoldResult{}:spirv_gc::foldSharedWords(w.data(),w.size());
    bool ok;
    if(!s.fold || r.code.empty())ok=used==original;
    else {const auto it=gpucost::testCode.find(used);ok=used!=original && it!=gpucost::testCode.end() && it->second==r.code;}
    if(!ok)++gpucost::stageMismatches;
}
#endif
// pipeline(): the key salt (0 while the fold is off: the key is unchanged).
inline std::uint64_t gcKeySalt() noexcept {return gpucost::state.keySalt.load(std::memory_order_relaxed);}
// draw(), right after the shared block is copied to the ring: one draw in 256 (every draw in host tests) the block is
// compared with the fold's table while the fold is on; a mismatch turns the fold off for the rest of the run.
[[gnu::noinline]] void gcAuditShared(const std::uint32_t* shared) noexcept {
    auto& s=gpucost::state;
    if(!s.fold)return;
#if !defined(RECOMP_GC_TEST_AUDIT)
    if((s.auditCalls++)&255u)return;
#endif
    ++s.auditChecks;
    for(std::uint32_t k=0;k<spirv_gc::SharedWords;++k) {
        std::uint32_t v=0;
        if(spirv_gc::sharedWordConstant(k,v) && shared[k]!=v) {
            ++s.auditFailures;s.windowAuditFailed=true;s.auditDisabled=true;s.fold=false;s.keySalt.store(0,std::memory_order_relaxed);
            GLOG("RENDER_DIAG_GC audit_failed word=%u value=%08x expected=%08x: fold off for the rest of the run",k,shared[k],v);
            return;
        }
    }
}
// v287 profile hooks (called inside the profiled frame's blocks only): the begin timestamp's stage, the counters at a
// frame's begin / a pass end, and a pass begin (its preceding gap's operations).
// (out of line: inlined, its select becomes a branch in ensurePass() before IR PGO's instrumentation and changes that
// function's profile hash; the NDK check found it)
[[gnu::noinline]] VkPipelineStageFlagBits gcBeginStage() noexcept {
    return gpucost::state.profile?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
}
[[gnu::noinline]] void gcMark(unsigned slot,const gpucost::Counters& c,bool frameBegin) noexcept {
    auto& s=gpucost::state;if(slot>=s.last.size())return;
    s.last[slot]=c;
    if(frameBegin)try{s.gaps[slot].clear();}catch(...){}
}
[[gnu::noinline]] void gcGap(unsigned slot,unsigned pass,const gpucost::Counters& c) noexcept {
    auto& s=gpucost::state;if(slot>=s.last.size())return;
    const auto& l=s.last[slot];
    try {
        auto& v=s.gaps[slot];if(v.size()<=pass)v.resize(pass+1);
        v[pass]={gpucost::delta(c.barriers,l.barriers),gpucost::delta(c.chunks,l.chunks),gpucost::delta(c.resolves,l.resolves),
                 gpucost::delta(c.depthResolves,l.depthResolves),gpucost::delta(c.gathers,l.gathers),gpucost::delta(c.uploads,l.uploads),
                 gpucost::delta(c.clears,l.clears)};
    } catch(...) {}
}
// waitSlot(), with the profiled frame's timestamps: gap time by what the gap recorded (a gap with a chunk submission also
// holds the GPU's idle wait for that submission: counted apart).
[[gnu::noinline]] void gcProfileReport(unsigned slot,unsigned long long frame,unsigned passes,const std::uint64_t* t,std::uint64_t mask,
                                       double period,bool dump) noexcept {
    auto& s=gpucost::state;
    if(!s.profile || slot>=s.gaps.size() || passes<2)return;
    const auto us=[&](std::uint64_t a,std::uint64_t b){return double((b-a)&mask)*period/1000.0;};
    // classes: 0 chunk submission (idle inside), 1 depth resolve, 2 gather, 3 upload, 4 colour resolve, 5 barrier only, 6 nothing
    std::array<double,7> u{};std::array<unsigned,7> n{};gpucost::Counters tot{};
    const auto& g=s.gaps[slot];
    for(unsigned i=1;i<passes && i<g.size();++i) {
        const auto& x=g[i];const double d=us(t[2*i-1],t[2*i]);
        const unsigned c=x.chunks?0:x.depthResolves?1:x.gathers?2:x.uploads?3:x.resolves?4:x.barriers?5:6;
        u[c]+=d;++n[c];
        tot.barriers+=x.barriers;tot.chunks+=x.chunks;tot.resolves+=x.resolves;tot.depthResolves+=x.depthResolves;tot.gathers+=x.gathers;
        tot.uploads+=x.uploads;tot.clears+=x.clears;
        if(dump)GLOG("RENDER_DIAG_GC_GAP f=%llu i=%u us=%.0f chunk=%u res=%u dres=%u gat=%u upl=%u bar=%u clr=%u",frame,i,d,x.chunks,x.resolves,
                     x.depthResolves,x.gathers,x.uploads,x.barriers,x.clears);
    }
    GLOG("RENDER_DIAG_GC_GAPS frame=%llu stage=bottom chunk=%u/%.0f depth_resolve=%u/%.0f gather=%u/%.0f upload=%u/%.0f resolve=%u/%.0f "
         "barrier_only=%u/%.0f none=%u/%.0f ops res=%llu dres=%llu gat=%llu upl=%llu bar=%llu chunks=%llu clr=%llu",frame,n[0],u[0],n[1],u[1],n[2],u[2],
         n[3],u[3],n[4],u[4],n[5],u[5],n[6],u[6],(unsigned long long)tot.resolves,(unsigned long long)tot.depthResolves,(unsigned long long)tot.gathers,
         (unsigned long long)tot.uploads,(unsigned long long)tot.barriers,(unsigned long long)tot.chunks,(unsigned long long)tot.clears);
}
// startRecipes() (before the prewarm thread starts): diag-gc-prewarm, diag-gc-index-fold or diag-gc-all present at startup
// makes the prewarm also compile every recipe with the folded modules, so the first window with the fold on does not compile
// its pipelines on the worker (the driver's pipeline cache keys on the module code).
[[gnu::noinline]] bool gcFile(const char* name) noexcept;
[[gnu::noinline]] void gcStartup() noexcept {
    const bool on=gcFile("diag-gc-prewarm") || gcFile("diag-gc-index-fold") || gcFile("diag-gc-all");
    gpucost::state.prewarmFold.store(on,std::memory_order_release);
    GLOG("RENDER_DIAG_GC_STARTUP prewarm_fold=%u",unsigned(on));
}
// The prewarm thread, after compiling a recipe with modules vm / pm: the same recipe with their folded forms (made from the
// words graphicsModule() recorded for them, exactly as gcModuleFor() will fold them), destroyed again (the pipeline cache keeps
// the result). A stage whose module has nothing to fold (or was not recorded: the prewarm's own vertex variants) keeps it.
template<class Compile> void gcPrewarmFolded(VkDevice dev,VkShaderModule vm,VkShaderModule pm,Compile&& compile) noexcept {
    auto& s=gpucost::state;
    if(!s.prewarmFold.load(std::memory_order_acquire))return;
    try {
        std::array<VkShaderModule,2> in{vm,pm},out{vm,pm};bool any=false;
        for(unsigned k=0;k<2;++k) {
            if(!in[k])continue;
            std::vector<std::uint32_t> w;
            {std::lock_guard lock(s.m);const auto it=s.words.find(in[k]);if(it!=s.words.end())w=it->second;}
            if(w.empty()){++s.prewarmUnfoldable;continue;}
            auto r=spirv_gc::foldSharedWords(w.data(),w.size());if(r.code.empty())continue;
            VkShaderModuleCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;c.codeSize=r.code.size()*4;c.pCode=r.code.data();
            VkShaderModule f=VK_NULL_HANDLE;if(vkCreateShaderModule(dev,&c,nullptr,&f)==VK_SUCCESS && f){out[k]=f;any=true;}
        }
        if(any){if(compile(out[0],out[1]))++s.prewarmCompiled;else ++s.prewarmFailed;}
        for(unsigned k=0;k<2;++k)if(out[k]!=in[k])vkDestroyShaderModule(dev,out[k],nullptr);
    } catch(...) {}
}
#if defined(RECOMP_GC_TEST_CANON) // host test: the folded form of a module (empty: nothing to fold)
std::vector<std::uint32_t> gcCanon(const std::uint32_t* code,std::size_t words) {return spirv_gc::foldSharedWords(code,words).code;}
#endif
// The v287 poll: host tests can force the per-pass profile (and its dump: one frame in 120) on; nothing otherwise.
inline void gcForceV287([[maybe_unused]] bool& profile,[[maybe_unused]] bool& dump) noexcept {
#if defined(RECOMP_GC_TEST_FORCE_V287)
    profile=dump=true;
#endif
}
// The 60-present window boundary (the worker drained): poll the switches, log the window's settings and the counters.
#if defined(RECOMP_GC_TEST_FOLD) || defined(RECOMP_GC_TEST_PROFILE)
[[gnu::noinline]] bool gcFile(const char*) noexcept {return false;}
#else
[[gnu::noinline]] bool gcFile(const char* name) noexcept {
    char p[128];std::snprintf(p,sizeof p,RECOMP_APP_PATH("/private-trace/%s"),name);return access(p,F_OK)==0;
}
#endif
// The KGSL clock (Hz) and busy share (%) right now (-1: unreadable, e.g. host builds or a denied read).
[[gnu::noinline]] long long gcSysfsNumber(const char* path) noexcept {
    std::FILE* f=std::fopen(path,"r");if(!f)return -1;
    long long v=-1;if(std::fscanf(f,"%lld",&v)!=1)v=-1;std::fclose(f);return v;
}
[[gnu::noinline]] void gcWindow() noexcept {
    auto& s=gpucost::state;
    const long long hz=gcSysfsNumber("/sys/class/kgsl/kgsl-3d0/gpuclk"),busy=gcSysfsNumber("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");
#if defined(RECOMP_GC_TEST_FOLD) || defined(RECOMP_GC_TEST_PROFILE) // host harness: the compiled-in state, or the toggle file
    static bool foldFile=s.fold,profFile=s.profile,allFile=false;
#if defined(RECOMP_GC_TEST_TOGGLE) // host harness: the fold toggled at every poll (both settings' pipelines cached side by side)
    const bool foldWindow=foldFile,profWindow=profFile,allWindow=allFile;foldFile=!foldFile;
#else
    const bool foldWindow=foldFile,profWindow=profFile,allWindow=allFile;
#endif
#else
    static bool foldFile=false,profFile=false,allFile=false;
    const bool foldWindow=foldFile,profWindow=profFile,allWindow=allFile;
    allFile=gcFile("diag-gc-all");foldFile=gcFile("diag-gc-index-fold") || allFile;profFile=gcFile("diag-gc-profile");
#endif
    const bool audited=!s.windowAuditFailed;s.windowAuditFailed=false;
    s.fold=foldFile && !s.auditDisabled;s.keySalt.store(s.fold?gpucost::KeySalt:0,std::memory_order_relaxed);
    s.profile=profFile;
    const bool foldWhole=foldWindow && foldFile && audited && !s.auditDisabled; // (on for the whole window that just ended)
    GLOG("RENDER_GENERIC_V210Z gc_index_fold=%u gc_all=%u gc_profile=%u",unsigned(foldWhole),unsigned(foldWhole && allWindow && allFile),
         unsigned(profWindow && profFile));
    std::size_t recorded=0,bytes=0;{std::lock_guard lock(s.m);recorded=s.words.size();bytes=s.recordedBytes;}
    GLOG("RENDER_DIAG_GC fold=%u prof=%u folded_modules=%llu folded_loads=%llu same_modules=%llu unrecorded=%llu create_failed=%llu lookups=%llu "
         "audit_checks=%llu audit_failures=%llu recorded=%zu recorded_kb=%zu record_skipped=%llu prewarm_fold=%u prewarm_compiled=%llu "
         "prewarm_failed=%llu prewarm_unfoldable=%llu gpu_mhz=%lld gpu_busy=%lld",unsigned(s.fold),unsigned(s.profile),
         (unsigned long long)s.foldedModules,(unsigned long long)s.foldedLoads,(unsigned long long)s.sameModules,(unsigned long long)s.unrecorded,
         (unsigned long long)s.createFailed,(unsigned long long)s.lookups,(unsigned long long)s.auditChecks,(unsigned long long)s.auditFailures,
         recorded,bytes>>10,(unsigned long long)s.recordSkipped,unsigned(s.prewarmFold.load()),(unsigned long long)s.prewarmCompiled.load(),
         (unsigned long long)s.prewarmFailed.load(),(unsigned long long)s.prewarmUnfoldable.load(),hz<0?-1LL:hz/1000000,busy);
}
#if defined(RECOMP_GC_TEST_REPORT) // host harness: the run's totals on stderr
struct GcHarnessReport {~GcHarnessReport(){auto& s=gpucost::state;std::fprintf(stderr,"GC_HARNESS fold=%u folded_modules=%llu folded_loads=%llu "
    "same_modules=%llu unrecorded=%llu lookups=%llu audit_checks=%llu audit_failures=%llu stage_checks=%llu stage_mismatches=%llu\n",unsigned(s.fold),
    (unsigned long long)s.foldedModules,(unsigned long long)s.foldedLoads,(unsigned long long)s.sameModules,(unsigned long long)s.unrecorded,
    (unsigned long long)s.lookups,(unsigned long long)s.auditChecks,(unsigned long long)s.auditFailures,
#if defined(RECOMP_GC_TEST_STAGECHECK)
    (unsigned long long)gpucost::stageChecks,(unsigned long long)gpucost::stageMismatches
#else
    0ull,0ull
#endif
    );}} gcHarnessReport;
#endif
