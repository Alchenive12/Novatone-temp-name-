#include "DSP/TuneEngine.h"
#include <cstdio>
#include <vector>
// harness2 in.f32 out.f32 targets.f32 (per 512-block, <=0 none) variation humanize
int main(int argc, char** argv){
  auto rd=[](const char* p){ FILE* f=fopen(p,"rb"); std::vector<float> x; float v; while(fread(&v,4,1,f)==1) x.push_back(v); fclose(f); return x; };
  auto x=rd(argv[1]); auto tg=rd(argv[3]); float var=atof(argv[4]), hum=atof(argv[5]);
  TuneEngine e; juce::dsp::ProcessSpec spec{44100.0, 512, 1}; e.prepare(spec);
  e.setScale(0xFFF); e.setCenterAmount(1.0f); e.setVariationAmount(var);
  e.setHumanizeAmount(hum); e.setHumanizeEnabled(hum>0.005f);
  juce::AudioBuffer<float> b(1,512); std::vector<float> y; y.reserve(x.size()+4096);
  x.resize(x.size()+4096, 0.0f);
  for(size_t s=0, blk=0;s<x.size();s+=512, ++blk){ int n=(int)std::min<size_t>(512,x.size()-s); b.setSize(1,n,false,false,true);
    for(int i=0;i<n;++i) b.setSample(0,i,x[s+i]);
    float t = blk<tg.size()? tg[blk] : 0.0f;
    if (t>0) e.setLiveNoteOverride(true, t, 1.0f, 1.0f, 0.0f, 0.18f); else e.setLiveNoteOverride(false, 60.0f, 1.0f, 1.0f, 0.0f, 0.18f);
    juce::dsp::AudioBlock<float> blkk(b); e.process(juce::dsp::ProcessContextReplacing<float>(blkk));
    for(int i=0;i<n;++i) y.push_back(b.getSample(0,i)); }
  int L=e.getLatencySamples(); y.erase(y.begin(), y.begin()+L); y.resize(x.size()-4096);
  FILE* f=fopen(argv[2],"wb"); fwrite(y.data(),4,y.size(),f); fclose(f);
}
