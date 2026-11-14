// Authored PS3 programs through the unchanged pinned DXSO compiler. No game data.
#include "dxso/dxso_module.h"
#include "dxso/dxso_modinfo.h"
#include "dxvk/dxvk_shader.h"
#include <spirv/GLSL.std.450.h>
#include <fstream>
#include <iostream>
#include <cstring>
#include <stdexcept>
namespace dxvk { Logger Logger::s_instance("dxso-authored.log"); }
// Only the log-file name is supplied by the fixture, not any shader operation.
namespace dxvk::env { std::string getExePath() { return "authored-shader-fixture"; } }
static uint32_t bits(float value) { uint32_t b; std::memcpy(&b,&value,4);return b; }
static void require(bool result) { if(!result) throw std::runtime_error("DXSO float fixture mismatch"); }
static unsigned extCount(const std::vector<uint32_t>& words,uint32_t opcode) {
    unsigned count=0;
    for(size_t at=5;at<words.size();) {
        size_t size=words[at]>>16;require(size && at+size<=words.size());
        if((words[at]&0xffff)==spv::OpExtInst && size>=5 && words[at+4]==opcode)++count;
        at+=size;
    }
    return count;
}
int main(int argc,char **argv) {
  try {
    require(argc==2);
    const unsigned ops[]={D3DSIO_RCP,D3DSIO_RSQ,D3DSIO_NRM,D3DSIO_LOG,D3DSIO_POW};
    const char *names[]={"rcp","rsq","nrm","log","pow"};
    unsigned cases=0;
    for(int mode=0;mode<2;++mode) for(unsigned k=0;k<5;++k) for(int input=0;input<3;++input) {
        unsigned op=ops[k];
        const bool normal=input==1, runtime=input==2;
        float x=normal ? (op==D3DSIO_NRM ? 3.f : op==D3DSIO_RCP || op==D3DSIO_POW ? 2.f:4.f):0.f;
        float y=normal ? (op==D3DSIO_NRM ? 4.f:op==D3DSIO_POW ? 3.f:0.f):0.f;
        std::vector<uint32_t> code={0xffff0300};
        // Runtime cases deliberately omit DEF. The shader reads c0 from the
        // application's constant buffer, so Metal cannot fold the boundary
        // arithmetic from a literal while compiling the function.
        if(!runtime) code.insert(code.end(),{0x05000051,0xa00f0000,bits(x),bits(y),0,0});
        code.insert(code.end(),{0x02000001,0x800f0000,0xa0e40000});
        code.push_back((op==D3DSIO_POW ? 0x03000000:0x02000000)|op);
        code.push_back(op==D3DSIO_NRM ? 0x80070000:0x800f0000);
        code.push_back(op==D3DSIO_NRM ? 0xa0e40000:0xa0000000);
        if(op==D3DSIO_POW)code.push_back(0xa0550000); // c0.y replicated
        code.insert(code.end(),{0x02000001,0x800f0800,0x80e40000,0x0000ffff});
        dxvk::DxsoReader reader(reinterpret_cast<const char*>(code.data()));
        dxvk::DxsoModule module(reader);dxvk::DxsoModuleInfo info;
        info.options.useDemoteToHelperInvocation=false;info.options.useSubgroupOpsForEarlyDiscard=false;
        info.options.strictConstantCopies=false;info.options.d3d9FloatEmulation=mode;
        info.options.strictPow=true;info.options.shaderModel=3;info.options.invariantPosition=false;
        info.options.forceSamplerTypeSpecConstants=false;info.options.vertexConstantBufferAsSSBO=false;
        info.options.longMad=false;info.options.alphaTestWiggleRoom=false;
        dxvk::D3D9ConstantLayout layout{224,16,16,1};
        auto analysis=module.analyze();auto shaders=module.compile(info,"authored-arithmetic",analysis,layout);
        std::string path=std::string(argv[1])+"/"+names[k]+"-"+(runtime ? "runtime":normal ? "normal":"zero")+"-"+std::to_string(mode)+".spv";
        {std::ofstream output(path,std::ios::binary);require(bool(output));shaders[dxvk::D3D9ShaderPermutations::None]->dump(output);require(bool(output));}
        std::ifstream in(path,std::ios::binary|std::ios::ate);size_t n=in.tellg();require(n>=20&&n%4==0);
        std::vector<uint32_t> words(n/4);in.seekg(0);in.read((char*)words.data(),n);require(bool(in));
        require(extCount(words,GLSLstd450NMin)==unsigned(mode&&(k<3)));
        require(extCount(words,GLSLstd450NMax)==unsigned(mode&&(k==3)));
        ++cases;
    }
    std::cout<<"DXSO_FLOAT actual_compiler_cases="<<cases<<" finite_boundary_emission=PASS game_rendering_unverified=1\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
