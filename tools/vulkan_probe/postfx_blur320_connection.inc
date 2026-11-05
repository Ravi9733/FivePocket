// PostFX pass81 at 320x180 (18rl488a: A texture -> A RT3 base720, the ABI42
// program's third row) consumes THIS GPU's upsample320 sum (asumWords) through
// the existing 7e3->binary16 resolve conversion of the A 320x180 flags0 row
// (the same conversion expanded16_copy_connection.inc proves on the bright
// pass). previous = the captured slot5 bytes, so variant0 must reproduce the
// live A texture byte-exactly.
{
    auto x3Fixture=loadPostfxBlurFixture(p+"/postfx-blur320",[](const std::string& path,unsigned bytes){return read(path,bytes);});
    auto& x3d=x3Fixture.draw;auto& x3b=x3Fixture.bound;auto& x3prog=x3Fixture.program;
    x3prog.vertexCode=code(p+"/postfx-blur320/original.vert.spv");x3prog.pixelCode=code(p+"/postfx-blur320/original.frag.spv");
    assert(x3prog.vertexIdentity==bprog.vertexIdentity && x3prog.pixelIdentity==bprog.pixelIdentity &&
        x3prog.vertexCode==bprog.vertexCode && x3prog.pixelCode==bprog.pixelCode && validateNativePostfxBlurProgram(x3prog));
    const auto liveA=x3d.textures[0].source;
    const auto aDescription=XenosTextureDescription::decode(x3d.textures[0].fetch);
    const auto aStorage=aDescription?planXenosExpanded16FloatStorage(*aDescription):std::nullopt;
    assert(aStorage && aDescription->width==320 && aDescription->height==180 && aDescription->pitchTexels==320 &&
        aDescription->baseAddress==0x0641A000 && liveA.size()==aStorage->sourceBytes && liveA.size()==491520);
    const char* x3why=nullptr;
    std::vector<std::uint8_t> aConverted(320*180*8);
    for(std::size_t i=0;i<asumWords->size();++i) {
        const auto pixel=color7e3ToBinary16((*asumWords)[i]);
        for(unsigned c=0;c<4;++c){aConverted[i*8+c*2]=pixel[c]&255;aConverted[i*8+c*2+1]=pixel[c]>>8;}
    }
    const auto producedA=storeXenosTexture2D(aStorage->description,aConverted,liveA);assert(producedA);
    unsigned x3Differing=0;for(std::size_t i=0;i<liveA.size();++i)x3Differing+=(*producedA)[i]!=liveA[i];
    std::printf("POSTFX_BLUR320_SOURCE variant=%u differing_bytes=%u\n",variant,x3Differing);
    if(!variant)assert(!x3Differing);
    const auto x3SourceLinear=read(p+"/postfx-blur320/source-linear-rgba16le.bin",320*180*8);
    assert(linearizeXenosExpanded16FloatStorage(*aDescription,liveA)==x3SourceLinear);
    const auto x3Now=linearizeXenosExpanded16FloatStorage(*aDescription,*producedA);
    assert(x3Now && *x3Now==aConverted);
    x3d.textures[0].source=*producedA;
    const auto x3plan=planNativePostfxBlurDraw(x3d,x3b,x3prog,x3prog.vertexIdentity,x3prog.pixelIdentity,false,true,&x3why);
    if(!x3plan)std::printf("POSTFX_BLUR320_PLAN_REFUSED %s\n",x3why);assert(x3plan);
    assert(x3plan->target.width==320 && x3plan->target.height==180 && x3plan->target.pitch==320 && x3plan->target.baseTile==720 &&
        x3plan->target.tileCount==48 && x3plan->target.object==0x10271110 && x3plan->target.object==x3b.surfaces[0].object &&
        x3plan->submitted.pitchTexels==320);
    const auto x3blurred=renderNativePostfxBlur(device,x3prog,x3d,*x3plan,&x3why);
    if(!x3blurred)std::printf("POSTFX_BLUR320_RENDER_REFUSED %s\n",x3why);assert(x3blurred && x3blurred->size()==320*180*4);
    auto x3UvProgram=x3prog;x3UvProgram.pixelCode=code(p+"/postfx-blur320/coordinates.frag.spv");
    const auto x3Uv=renderNativePostfxBlur(device,x3UvProgram,x3d,*x3plan,&x3why);assert(x3Uv && x3Uv->size()==320*180*4);
    const auto x3Half=[](unsigned h){_Float16 value;std::memcpy(&value,&h,2);return double(float(value));};
    const auto x3Texel=[&](int x,int y,unsigned c){x=std::clamp(x,0,319);y=std::clamp(y,0,179);const auto at=(y*320+x)*8+c*2;
        return x3Half((*x3Now)[at]|(unsigned{(*x3Now)[at+1]}<<8));};
    const auto x3Constants=read(p+"/postfx-blur320/pixel-constants.bin",4096);
    const auto x3Constant=[&](unsigned r,unsigned i){
        const auto at=(r*4+i)*4;
        const unsigned bits=(unsigned{x3Constants[at]}<<24)|(unsigned{x3Constants[at+1]}<<16)|(unsigned{x3Constants[at+2]}<<8)|x3Constants[at+3];
        assert(bits==x3d.pixelConstants[r*4+i]);float value;std::memcpy(&value,&bits,4);assert(std::isfinite(value));return value;
    };
    const float x3Step=x3Constant(189,0);
    const std::array<float,5> x3Scale{x3Constant(254,2),x3Constant(255,0),0.0f,x3Constant(255,2),x3Constant(255,1)};
    const std::array<float,5> x3Weight{x3Constant(255,3),x3Constant(254,0),x3Constant(254,1),x3Constant(254,0),x3Constant(255,3)};
    const auto x3Mul=[](float a,float b){return (a==0 || b==0)?0.0f:float(double(a)*double(b));};
    unsigned x3Outside=0,x3Nonzero=0,x3ClampedTaps=0;double x3MaxRatio=0;
    for(unsigned y=0;y<180;++y)for(unsigned x=0;x<320;++x) {
        const auto at=(y*320+x)*4;const float u=(*x3Uv)[at],v=(*x3Uv)[at+1];
        assert(u>0 && u<1 && v>0 && v<1 && (*x3Uv)[at+2]==0 && (*x3Uv)[at+3]==0);
        for(unsigned c=0;c<4;++c) {
            std::array<double,5> ideal{},tapBound{};
            for(unsigned k=0;k<5;++k) {
                const float uk=k==2?u:float(double(x3Mul(x3Step,x3Scale[k]))+double(u));
                const double tx=double(uk)*320-.5,ty=double(v)*180-.5;const int ix=std::floor(tx),iy=std::floor(ty);
                x3ClampedTaps+=!c && (ix<0 || ix+1>319);
                const double wx=tx-ix,wy=ty-iy;
                const double t00=x3Texel(ix,iy,c),t10=x3Texel(ix+1,iy,c),t01=x3Texel(ix,iy+1,c),t11=x3Texel(ix+1,iy+1,c);
                ideal[k]=(1-wx)*(1-wy)*t00+wx*(1-wy)*t10+(1-wx)*wy*t01+wx*wy*t11;
                const double ulp=std::ldexp(1.0,std::max(-24,int(std::floor(std::log2(std::max(std::abs(ideal[k]),1e-30))))-10));
                tapBound[k]=ulp+(std::abs(t10-t00)+std::abs(t11-t01)+std::abs(t01-t00)+std::abs(t11-t10))/256;
            }
            double reference=ideal[4]*x3Weight[4];
            reference+=ideal[3]*x3Weight[3];reference+=ideal[2]*x3Weight[2];
            reference+=ideal[1]*x3Weight[1];reference+=ideal[0]*x3Weight[0];
            double tapTerm=0,magnitude=0;
            for(unsigned k=0;k<5;++k) {
                tapTerm+=std::abs(double(x3Weight[k]))*tapBound[k];
                magnitude+=std::abs(double(x3Weight[k]))*(std::abs(ideal[k])+tapBound[k]);
            }
            const double bound=tapTerm+9*std::ldexp(magnitude,-24)+9*std::ldexp(1.0,-126);
            const double deviation=std::abs(double((*x3blurred)[at+c])-reference);
            x3MaxRatio=std::max(x3MaxRatio,deviation/bound);x3Outside+=deviation>bound;
        }
        x3Nonzero+=(*x3blurred)[at]!=0 || (*x3blurred)[at+1]!=0 || (*x3blurred)[at+2]!=0;
    }
    std::printf("POSTFX_BLUR320_SAMPLE_BOUND variant=%u outside=%u max_ratio=%.6g clamped_taps=%u "
        "model=5tap_ideal_bilinear+1ulp16+weight_step+fp32_accumulation\n",variant,x3Outside,x3MaxRatio,x3ClampedTaps);
    assert(!x3Outside && (variant || x3Nonzero) && x3ClampedTaps);
    NativeEdram32Bands x3owner;const auto x3serial=x3owner.nextSerial();
    {NativeEdram32Mutation tx(x3owner);
     assert(executeNativePostfxBlurDraw(device,x3prog,x3d,x3b,x3prog.vertexIdentity,x3prog.pixelIdentity,true,tx,&x3why));}
    assert(x3owner.nextSerial()==x3serial+1 && std::string(x3why)=="fenced-postfx-blur-physical7e3");
    const auto x3words=x3owner.readKnownColorRows(720,320,320,180);assert(x3words && x3words->size()==320*180);
    for(unsigned i=0;i<320*180;++i) {
        std::array<std::uint32_t,4> bits{};for(unsigned c=0;c<4;++c)bits[c]=colorBits((*x3blurred)[i*4+c]);
        unsigned packed=0;assert(packColor7e3(bits,packed,Color7e3Rounding::XeniaShiftRound) && (*x3words)[i]==packed);
    }
    assert(!x3owner.readKnownColorRows(720,320,320,181) && !x3owner.readColorWords(768,80,80,16)); // 48-tile footprint only.
    NativeEdram32Bands x3pending;
    {NativeEdram32Mutation tx(x3pending);
     assert(!executeNativePostfxBlurDraw(device,x3prog,x3d,x3b,x3prog.vertexIdentity,x3prog.pixelIdentity,false,tx));}
    assert(!x3pending.knownTiles());
    std::printf("POSTFX_BLUR320_CONNECTION_PASS variant=%u upsample320_producer=1 a_resolve_conversion=1 live_source_reproduced=%u "
        "actual_shader=1 independent_source_linear=1 measured_uv=1 runtime_constants=1 taps=5 sample_bound=1 nonzero=%u physical7e3=1 "
        "base=720 tiles=48 pending_query_refused=1 gameplay=0\n",variant,unsigned(!variant),x3Nonzero);
}
