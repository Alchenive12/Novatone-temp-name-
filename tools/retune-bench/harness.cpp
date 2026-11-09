#include "DSP/TuneEngine.h"
#include <cstdio>
#include <vector>
// usage: harness in.f32 out.f32 targetMidi(<=0: none) variation humanize
int main(int argc, char** argv){
  FILE* f=fopen(argv[1],"rb"); std::vector<float> x; float v; while(fread(&v,4,1,f)==1) x.push_back(v); fclose(f);
  float target=atof(argv[3]), var=atof(argv[4]), hum=atof(argv[5]);
  TuneEngine e; juce::dsp::ProcessSpec spec{44100.0, 512, 1}; e.prepare(spec);
  e.setScale(0xFFF); e.setCenterAmount(1.0f); e.setVariationAmount(var);
  e.setHumanizeAmount(hum); e.setHumanizeEnabled(hum>0.005f);
  if (target>0) e.setLiveNoteOverride(true, target, 1.0f, 1.0f, 0.0f, 0.18f);
  juce::AudioBuffer<float> b(1,512); std::vector<float> y; y.reserve(x.size());
  for(size_t s=0;s<x.size();s+=512){ int n=(int)std::min<size_t>(512,x.size()-s); b.setSize(1,n,false,false,true);
    for(int i=0;i<n;++i) b.setSample(0,i,x[s+i]);
    if (target>0) e.setLiveNoteOverride(true, target, 1.0f, 1.0f, 0.0f, 0.18f);
    juce::dsp::AudioBlock<float> blk(b); e.process(juce::dsp::ProcessContextReplacing<float>(blk));
    for(int i=0;i<n;++i) y.push_back(b.getSample(0,i)); }
  f=fopen(argv[2],"wb"); fwrite(y.data(),4,y.size(),f); fclose(f);
  printf("latency %d\n", e.getLatencySamples());
}
