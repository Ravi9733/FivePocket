    // ---- fixer2 (2026-10-22, DIAGNOSTIC): frame facts (frame_facts.h: the switches and what a Fact holds). Included INSIDE
    // class Renderer as its last members (after fg_gen_renderer.inc), so the members above keep their offsets. Hooks:
    //   ffShadowDraw()  draw(), where the v76 shadow counters see a draw that samples a shadow map (one branch while off)
    //   ffPresent()     present(), before the generator's hook (one branch while off)
    //   ffWindow()      the 60-present switch poll
    bool ffOn=false;
    std::unique_ptr<ff::FactRing> ffRing;   // made at the first arming (~50 KB), kept for the run
    ff::Fact ffCur;                          // the frame being drawn
    std::uint64_t ffPrevDraws=0,ffPrevResolves=0,ffPrevShadowResolves=0,ffPrevCasters=0,ffPrevShadowDraws=0,ffPrevStale=0,ffPrevPasses=0;
    std::int64_t ffDumpMtimeNs=-1;          // the dump file's modification time last seen (-1: not looked yet)
    unsigned ffPoll=0,ffDumps=0;
    std::uint16_t ffTonemapNow=0;           // set by fgGenTonemap (v577's family member of the frame's tonemap draw)

    static std::int64_t ffMtimeNs(const char* path) noexcept {
        struct stat st{};if(stat(path,&st)!=0)return 0;
#if defined(__APPLE__)
        return std::int64_t(st.st_mtimespec.tv_sec)*1000000000ll+st.st_mtimespec.tv_nsec;
#else
        return std::int64_t(st.st_mtim.tv_sec)*1000000000ll+st.st_mtim.tv_nsec;
#endif
    }
    void ffBaseline() noexcept {
        ffPrevDraws=perf.draws;ffPrevResolves=counters.resolves;ffPrevShadowResolves=perf.shadowResolves;ffPrevCasters=perf.casterDraws;
        ffPrevShadowDraws=perf.shadowSampleDraws;ffPrevStale=perf.staleShadowSamples;ffPrevPasses=perf.passes;ffCur=ff::Fact{};
    }
    [[gnu::noinline]] void ffWindow() noexcept {
        if(ffSkoWinFrames) { // v584: the per-frame skinned-draw scan's render-thread cost over the window (only while armed)
            GLOG("RENDER_FRAME_SKO_COST frames=%u us_mean=%.1f us_max=%.1f",ffSkoWinFrames,double(ffSkoWinNs)/1000.0/ffSkoWinFrames,double(ffSkoWinMaxNs)/1000.0);
            ffSkoWinNs=ffSkoWinMaxNs=0;ffSkoWinFrames=0;
        }
        if(ffSkoWinBad) { // v595: the window's flagged skinned draws (lines are rate-limited to one pair per shader)
            GLOG("RENDER_FRAME_SKO_SUM draws=%u shaders_logged=%u shaders_muted_draws=%u",ffSkoWinBad,ffSkoWinVsN,ffSkoWinMuted);
            ffSkoWinBad=ffSkoWinMuted=0;
        }
        ffSkoWinVsN=0;
        const bool on=access(RECOMP_APP_PATH("/private-trace/diag-frame-facts"),F_OK)==0;
        if(on && !ffOn) {
            if(!ffRing)ffRing=std::make_unique<ff::FactRing>();
            if(!ffKeys)ffKeys=std::make_unique<FfKeys>(); // v582 (~25 MB, made once)
            ffKeys->clear();ffKeys->begin(ffSeq);
            ffBaseline();ffDumpMtimeNs=ffMtimeNs(RECOMP_APP_PATH("/private-trace/diag-frame-facts-dump")); // (a dump file already there is not a request)
            GLOG("RENDER_FRAME_FACTS_ARMED on=1 ring=%u dump=%u",ff::Ring,ff::Dump);
        } else if(!on && ffOn)GLOG("RENDER_FRAME_FACTS_ARMED on=0 recorded=%llu dumps=%u",(unsigned long long)(ffRing?ffRing->count():0),ffDumps);
        ffOn=on;
    }
    template<class Images> [[gnu::noinline]] void ffShadowDraw(std::uint64_t psHash,const DrawInput& in,const Images& images) noexcept {
        if(ffCur.lightPs || !in.pixelConstants)return; // (the frame's first shadow-sampling draw only)
        ffCur.lightPs=std::uint32_t(psHash)|1u; // (|1: never 0 for a recorded draw)
        std::uint64_t age=~0ull;
        for(const auto* im:images)if(im && im->shadowMap){const std::uint64_t a=counters.presents-im->resolvedAt;if(a<age)age=a;}
        ffCur.shadowAge=age>0xFFFFFFF0ull?~0u:std::uint32_t(age);
        const auto& pc=*in.pixelConstants;
        for(unsigned k=0;k<3;++k)ffCur.sunDir[k]=regFloat(pc[21*4+k]);
        for(unsigned k=0;k<4;++k)ffCur.sunColour[k]=regFloat(pc[22*4+k]);
    }
    [[gnu::noinline]] void ffPresent(const Image& shown,unsigned pw,unsigned ph) noexcept {
        if(!ffOn || !ffRing)return;
        ffGrid(shown,pw,ph); // v582: the luma grid of the picture just blitted (read back after the slot's fence)
        ffCur.present=counters.presents;ffCur.frame=frame;
        ffCur.wallMs=std::int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        ffCur.draws=ff::delta(perf.draws,ffPrevDraws);ffCur.resolves=ff::delta(counters.resolves,ffPrevResolves);
        ffCur.shadowResolves=ff::delta(perf.shadowResolves,ffPrevShadowResolves);ffCur.casterDraws=ff::delta(perf.casterDraws,ffPrevCasters);
        ffCur.shadowDraws=ff::delta(perf.shadowSampleDraws,ffPrevShadowDraws);ffCur.staleShadowDraws=ff::delta(perf.staleShadowSamples,ffPrevStale);
        ffCur.passes=ff::delta(perf.passes,ffPrevPasses);
        ffCur.tonemap=ffTonemapNow;ffCur.camera=fgGenCameraDraws && fgGenCameraInterval+1==presentCount;
        ffCur.skoUs=std::uint32_t(std::min<std::uint64_t>(ffSkoNs/1000,0xFFFFFFFFull)); // v584: the scan's cost in this frame
        ffSkoWinNs+=ffSkoNs;ffSkoWinMaxNs=std::max(ffSkoWinMaxNs,ffSkoNs);++ffSkoWinFrames;ffSkoNs=0;
        ffCur.seq=ffSeq++;ffRing->push(ffCur);ffCur=ff::Fact{};ffTonemapNow=0;ffSkoLines=0; // (v579: seq; v584: the SKO line budget)
        if(ffKeys)ffKeys->begin(ffSeq); // v582: the next frame's draws
        if(++ffPoll%15)return;
        const std::int64_t m=ffMtimeNs(RECOMP_APP_PATH("/private-trace/diag-frame-facts-dump"));
        if(m && m!=ffDumpMtimeNs) {
            ffDumpMtimeNs=m;++ffDumps;
            GLOG("RENDER_FRAME_FACTS_DUMP n=%u request=%u recorded=%llu",std::min<unsigned>(ff::Dump,unsigned(std::min<std::uint64_t>(ffRing->count(),ff::Dump))),ffDumps,
                 (unsigned long long)ffRing->count());
            // v580: three facts per line, separated by " |" (v579's one line per fact lost a third of a dump to logcat's burst limit).
            // v584: two per line: with v582's lg= a line of three passed logcat's ~4 KB entry limit and lost its last fact's tail.
            std::string line;unsigned n=0;
            ffRing->last(ff::Dump,[&](const ff::Fact& f){line+=ff::text(f);if(++n%2){line+=" |";}else{GLOG("RENDER_FRAME_FACTS%s",line.c_str());line.clear();}});
            if(!line.empty())GLOG("RENDER_FRAME_FACTS%s",line.c_str());
            ffOddFrames(); // v582
        }
    }
    // ---- v579 (G7, the sun dips): the sun pass probe (frame_facts.h Fact: sunDraws .. maskRgba). ffDraw() runs in draw() after
    // the textures are bound (one branch while off): it counts the shadow-mask and sun draws and, for the frame's first sun draw,
    // copies four texels of the image bound at s9 (the shadow mask) into this slot's small read-back buffer, in command order
    // right before that draw; ffSlotDone() reads them once the slot's fence has signalled and writes them into the frame's fact.
    std::uint32_t ffSeq=0; // the frame being drawn (ffCur.seq)
    struct FfProbe {Buffer buf;bool coherent=true,pending=false;std::uint32_t seq=0;VkFormat format=VK_FORMAT_UNDEFINED;
        Buffer gridBuf;bool gridCoherent=true,gridPending=false;std::uint32_t gridSeq=0;VkFormat gridFormat=VK_FORMAT_UNDEFINED;}; // (v582: the luma grid)
    std::array<FfProbe,FrameSlots> ffProbes{};
    static bool ffSunPs(std::uint64_t h) noexcept { // directional#6p .. #11p (the pack's directional-light pixel shaders)
        return h==0xec754a046c5a164cull || h==0x17afe88a81e04a44ull || h==0x55cfd958507a9dbcull || h==0x9e638c4260b076e3ull ||
               h==0x728a5e268f6444a3ull || h==0xa70eee0c4192d36full;
    }
    static constexpr std::uint64_t FfMaskPs=0x981c226950363e15ull; // cascadeshadows_rendering#18p (writes the screen-space shadow mask)
    static unsigned ffTexelBytes(VkFormat f) noexcept {
        switch(f){case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_R32_SFLOAT:return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT:return 8;case VK_FORMAT_R32G32B32A32_SFLOAT:return 16;default:return 0;}
    }
    template<class Images> [[gnu::noinline]] void ffDraw(std::uint64_t psHash,const Images& images) noexcept {
        if(psHash==FfMaskPs){++ffCur.maskDraws;return;}
        if(!ffSunPs(psHash) || ffCur.sunDraws++)return;
        const Image* m=images[9];
        if(!m || !m->image || m->aspect!=VK_IMAGE_ASPECT_COLOR_BIT){ffCur.probe=2;return;}
        ffCur.maskFormat=int(m->format);
        ffCur.maskAge=m->resolvedImage?std::uint32_t(std::min<std::uint64_t>(counters.presents-m->resolvedAt,0xFFFFFFF0ull)):~0u;
        ffCur.maskImage=std::uint32_t(reinterpret_cast<std::uintptr_t>(m->image)>>4);
        auto& pr=ffProbes[slotIndex%ffProbes.size()];
        if(pr.pending || !ffTexelBytes(m->format) || m->layers!=1 || m->volume){ffCur.probe=2;return;}
        if(!pr.buf.buffer && !makeReadbackBuffer(256,pr.buf,pr.coherent)){pr.buf={};ffCur.probe=2;return;}
        endPass(__LINE__);
        hazards({m->image},{});
        const unsigned w=rscale::extent(m->width,m->k),h=rscale::extent(m->height,m->k);
        const std::int32_t pts[4][2]{{std::int32_t(w/2),std::int32_t(h*3/4)},{std::int32_t(w/4),std::int32_t(h*7/8)},
                                     {std::int32_t(w*3/4),std::int32_t(h*7/8)},{std::int32_t(w/2),std::int32_t(h/2)}};
        VkBufferImageCopy c[4]{};
        for(unsigned q=0;q<4;++q){c[q].bufferOffset=q*16;c[q].imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            c[q].imageOffset={std::min<std::int32_t>(pts[q][0],std::int32_t(w)-1),std::min<std::int32_t>(pts[q][1],std::int32_t(h)-1),0};c[q].imageExtent={1,1,1};}
        vkCmdCopyImageToBuffer(cmd,m->image,VK_IMAGE_LAYOUT_GENERAL,pr.buf.buffer,4,c);
        VkMemoryBarrier mb{};mb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
        pr.pending=true;pr.seq=ffSeq;pr.format=m->format;ffCur.probe=0;
    }
    // After slot k's fence: its probe's texels into the frame's fact (still in the ring).
    void ffSlotDone(unsigned k) noexcept {
        if(k>=ffProbes.size())return;
        ffGridDone(ffProbes[k]); // v582
        auto& pr=ffProbes[k];if(!pr.pending)return;pr.pending=false;
        if(!pr.coherent){VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=pr.buf.memory;r.size=VK_WHOLE_SIZE;vkInvalidateMappedMemoryRanges(d.device,1,&r);}
        ff::Fact* f=ffRing?ffRing->find(pr.seq):nullptr;
        if(!f || !pr.buf.mapped)return;
        const auto* b=static_cast<const std::uint8_t*>(pr.buf.mapped);
        for(unsigned q=0;q<4;++q) {
            const std::uint8_t* t=b+q*16;
            for(unsigned c=0;c<4;++c) {
                float v=0;
                switch(pr.format) {
                case VK_FORMAT_R8G8B8A8_UNORM:v=float(t[c])/255.0f;break;
                case VK_FORMAT_B8G8R8A8_UNORM:v=float(t[c==3?3:2-c])/255.0f;break;
                case VK_FORMAT_R16G16B16A16_SFLOAT:{std::uint16_t hb;std::memcpy(&hb,t+2*c,2);v=tex::fromHalf(hb);break;}
                case VK_FORMAT_R32_SFLOAT:if(!c)std::memcpy(&v,t,4);break;
                case VK_FORMAT_R32G32B32A32_SFLOAT:std::memcpy(&v,t+4*c,4);break;
                default:break;
                }
                f->maskRgba[q][c]=v;
            }
        }
        f->probe=1;
    }
    // v579: a draw or resolve the renderer refused in this frame (counted; the first reason kept)
    [[gnu::noinline]] void ffRefused(const char* w,bool resolve) noexcept {
        ++(resolve?ffCur.resolveRefused:ffCur.drawRefused);
        if(!ffCur.firstRefuse[0] && w){std::snprintf(ffCur.firstRefuse,sizeof ffCur.firstRefuse,"%s%s",resolve?"R:":"D:",w);}
    }

    // ---- v581 (G7, DIAGNOSTIC): the v310 residue events of the frame (frame_facts.h Fact::res). Called only while ffOn.
    [[gnu::noinline,gnu::format(printf,2,3)]] void ffRes(const char* f,...) noexcept {
        char b[72];va_list a;va_start(a,f);std::vsnprintf(b,sizeof b,f,a);va_end(a);ff::note(ffCur,b);
    }
    // A pass just begun: each attachment it binds past its tiled rows (not a tiled pass) whose EDRAM lies over tiles 1536-1871
    // (G-buffer 2's bottom tile at 1280x720, the band v310 copies into 1152's rows 384-719).
    [[gnu::noinline]] void ffPassNote(const Bound& b) noexcept {
        for(unsigned i=0;i<5;++i) {
            const Target* t=b.t[i];if(!t || (t->edramRows && t->edramRows<t->image.height))continue;
            v310::Surface x;x.base=t->base;x.pitch=t->pitch;x.height=t->image.height;x.edramRows=t->edramRows;
            if(!v310::overlaps(1536,336,t->base%edram::Tiles,v310::footprintTiles(x)))continue;
            ffRes("P%u:%u%s:%u/%u",t->base,t->xenosFormat,t->depth?"d":"",t->pitch,t->edramRows);
        }
    }

    // ---- v582 (fixer3, G10 / G1, DIAGNOSTIC; frame_facts.h: oddCells, KeyRing, oddDraws). While diag-frame-facts is armed:
    //   ffGrid()   present(): GridW x GridH single texels of the presented real picture (cell centres) into this slot's read-back buffer
    //              (one vkCmdCopyImageToBuffer, 576 bytes); ffGridDone() turns them into the frame's luma grid after the slot's fence.
    //   ffKey()    draw(), once the draw's targets are bound: its keys into the key ring (a bounds check and a 24-byte copy).
    //   ffOddFrames() at a dump: the dumped frames whose grid shows cells black that both neighbours had lit (a one-frame shape,
    //              G10) are logged with the draws that occur more often in them than in either neighbour (RENDER_FRAME_ODD lines).
    // The picture is not changed; off (no diag-frame-facts): one branch per draw, as before.
    unsigned ffSkoLines=0; // v584: RENDER_FRAME_SKO lines of the frame being drawn
    std::array<std::uint64_t,4> ffSkoWinVs{};unsigned ffSkoWinVsN=0,ffSkoWinBad=0,ffSkoWinMuted=0; // v595: shaders logged this window
    std::uint64_t ffSkoNs=0,ffSkoWinNs=0,ffSkoWinMaxNs=0;unsigned ffSkoWinFrames=0; // v584: the scan's time in this frame / this 60-present window
    using FfKeys=ff::KeyRing<256,4096>;
    std::unique_ptr<FfKeys> ffKeys;
    [[gnu::noinline]] void ffKey(std::uint64_t vs,std::uint64_t ps,const Bound& b,std::uint32_t n) noexcept {
        if(!ffKeys)return;
        ff::DrawKey k;k.vs=vs;k.ps=ps;k.n=n;
        const Target* t=nullptr;for(unsigned i=0;i<4 && !t;++i)t=b.t[i];
        if(t)k.target=(t->base&0xFFFFu)|((t->xenosFormat&0x7FFFu)<<16);
        else if(b.t[4])k.target=(b.t[4]->base&0xFFFFu)|((b.t[4]->xenosFormat&0x7FFFu)<<16)|0x80000000u;
        ffKeys->add(k);
    }
    void ffGrid(const Image& im,unsigned pw,unsigned ph) noexcept {
        auto& pr=ffProbes[slotIndex%ffProbes.size()];
        if(pr.gridPending || !im.image || !pw || !ph || (im.format!=VK_FORMAT_R8G8B8A8_UNORM && im.format!=VK_FORMAT_B8G8R8A8_UNORM))return;
        if(!pr.gridBuf.buffer && !makeReadbackBuffer(ff::GridN*4,pr.gridBuf,pr.gridCoherent)){pr.gridBuf={};return;}
        hazards({im.image},{});
        std::array<VkBufferImageCopy,ff::GridN> c{};
        for(unsigned j=0;j<ff::GridH;++j)for(unsigned i=0;i<ff::GridW;++i) {
            auto& r=c[j*ff::GridW+i];r.bufferOffset=(j*ff::GridW+i)*4;r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            r.imageOffset={std::int32_t(std::min(pw-1,(2*i+1)*pw/(2*ff::GridW))),std::int32_t(std::min(ph-1,(2*j+1)*ph/(2*ff::GridH))),0};r.imageExtent={1,1,1};
        }
        vkCmdCopyImageToBuffer(cmd,im.image,VK_IMAGE_LAYOUT_GENERAL,pr.gridBuf.buffer,ff::GridN,c.data());
        VkMemoryBarrier mb{};mb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
        pr.gridPending=true;pr.gridSeq=ffSeq;pr.gridFormat=im.format;
    }
    void ffGridDone(FfProbe& pr) noexcept {
        if(!pr.gridPending)return;pr.gridPending=false;
        if(!pr.gridCoherent){VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=pr.gridBuf.memory;r.size=VK_WHOLE_SIZE;vkInvalidateMappedMemoryRanges(d.device,1,&r);}
        ff::Fact* f=ffRing?ffRing->find(pr.gridSeq):nullptr;
        if(!f || !pr.gridBuf.mapped)return;
        ff::fillGrid(*f,static_cast<const std::uint8_t*>(pr.gridBuf.mapped),pr.gridFormat==VK_FORMAT_B8G8R8A8_UNORM);
    }
    void ffOddFrames() noexcept {
        try {
            std::vector<const ff::Fact*> v;v.reserve(ff::Dump);
            ffRing->last(ff::Dump,[&](const ff::Fact& f){v.push_back(&f);});
            std::vector<std::pair<unsigned,std::size_t>> odd; // (cells, index)
            for(std::size_t i=1;i+1<v.size();++i)
                if(v[i-1]->seq+1==v[i]->seq && v[i]->seq+1==v[i+1]->seq)
                    if(const unsigned n=ff::oddCells(*v[i-1],*v[i],*v[i+1]);n>=ff::OddMin)odd.push_back({n,i});
            std::stable_sort(odd.begin(),odd.end(),[](const auto& a,const auto& b){return a.first>b.first;});
            GLOG("RENDER_FRAME_ODD_SCAN frames=%zu odd=%zu",v.size(),odd.size());
            for(std::size_t o=0;o<odd.size() && o<4;++o) {
                const ff::Fact& f=*v[odd[o].second];
                const auto* a=ffKeys?ffKeys->find(f.seq-1):nullptr;const auto* c=ffKeys?ffKeys->find(f.seq):nullptr;const auto* b=ffKeys?ffKeys->find(f.seq+1):nullptr;
                if(!a || !b || !c){GLOG("RENDER_FRAME_ODD p=%llu seq=%u cells=%u keys=lost",(unsigned long long)f.present,f.seq,odd[o].first);continue;}
                const auto ds=ff::oddDraws(*a,*c,*b);
                GLOG("RENDER_FRAME_ODD p=%llu seq=%u cells=%u draws=%u/%u/%u overflow=%u/%u/%u odd_draws=%zu",(unsigned long long)f.present,f.seq,odd[o].first,
                     a->count,c->count,b->count,a->overflow,c->overflow,b->overflow,ds.size());
                std::string line;unsigned k=0;
                for(const auto& x:ds) {
                    if(k>=48)break;
                    char e[96];std::snprintf(e,sizeof e," %016llx:%016llx:%08x:%u:%u+%u",(unsigned long long)x.key.vs,(unsigned long long)x.key.ps,x.key.target,x.key.n,x.count,x.extra);
                    line+=e;
                    if(++k%6==0){GLOG("RENDER_FRAME_ODD_DRAWS p=%llu%s",(unsigned long long)f.present,line.c_str());line.clear();}
                }
                if(!line.empty())GLOG("RENDER_FRAME_ODD_DRAWS p=%llu%s",(unsigned long long)f.present,line.c_str());
            }
        } catch(...) {GLOG("RENDER_FRAME_ODD_SCAN failed=alloc");}
    }
