#include "DSP/TuneEngine.h"
#if __has_include("DSP/ShiftGlide.h")
 #include "DSP/ShiftGlide.h"
 #define HAS_GLIDE 1
#endif
#include <cstdio>
#include <vector>
// harness3 in.f32 out.f32 notes.txt variation humanize lagSamples
// notes.txt: startSample endSample detectedMidi targetMidi edited   (the piano roll's notes; hop = 512)
struct N { long s, e; float det, tgt; int ed; };
int main(int argc, char** argv){
  FILE* f=fopen(argv[1],"rb"); std::vector<float> x; float v; while(fread(&v,4,1,f)==1) x.push_back(v); fclose(f);
  std::vector<N> ns; f=fopen(argv[3],"r"); N n; while(fscanf(f,"%ld %ld %f %f %d",&n.s,&n.e,&n.det,&n.tgt,&n.ed)==5){ n.s/=512; n.e/=512; ns.push_back(n);} fclose(f);
  float var=atof(argv[4]), hum=atof(argv[5]); long lag=atol(argv[6]);
  const float maxGlide = 0.090f*44100.0f/512.0f;
  TuneEngine e; juce::dsp::ProcessSpec spec{44100.0, 512, 1}; e.prepare(spec);
  e.setScale(0xFFF); e.setCenterAmount(1.0f); e.setVariationAmount(var);
  e.setHumanizeAmount(hum); e.setHumanizeEnabled(hum>0.005f);
  juce::AudioBuffer<float> b(1,512); std::vector<float> y; x.resize(x.size()+4096, 0.0f);
  for(size_t s=0;s<x.size();s+=512){ int nn=(int)std::min<size_t>(512,x.size()-s); b.setSize(1,nn,false,false,true);
    for(int i=0;i<nn;++i) b.setSample(0,i,x[s+i]);
    long hop=((long)s-lag)/512; bool set=false;
    for(size_t k=0;k<ns.size();++k){ const N& q=ns[k]; if(!q.ed||hop<q.s||hop>=q.e) continue;
      float shift=0; bool has=false;
#ifdef HAS_GLIDE
      auto nb=[&](size_t j, bool touch){ ShiftGlide::Neighbour r; if(j<ns.size()&&touch){ r.touching=true; r.edited=ns[j].ed; r.shift=ns[j].ed? ns[j].tgt-ns[j].det:0.0f;} return r; };
      shift=ShiftGlide::shiftAtHop(q.tgt-q.det,(int)(hop-q.s),(int)(q.e-q.s),0.18f,0.18f, nb(k-1,k>0&&ns[k-1].e==q.s), nb(k+1,k+1<ns.size()&&ns[k+1].s==q.e), maxGlide); has=true;
      e.setLiveNoteOverride(true,q.tgt,1.0f,1.0f,0.0f,0.18f,0.0f,false,60.0f,1.0f,0.18f,has,shift);
#else
      e.setLiveNoteOverride(true,q.tgt,1.0f,1.0f,0.0f,0.18f);
#endif
      set=true; }
    if(!set) e.setLiveNoteOverride(false,60.0f,1.0f,1.0f,0.0f,0.18f);
    juce::dsp::AudioBlock<float> blk(b); e.process(juce::dsp::ProcessContextReplacing<float>(blk));
    for(int i=0;i<nn;++i) y.push_back(b.getSample(0,i)); }
  int L=e.getLatencySamples(); y.erase(y.begin(), y.begin()+L); y.resize(x.size()-4096);
  f=fopen(argv[2],"wb"); fwrite(y.data(),4,y.size(),f); fclose(f); fprintf(stderr,"latency %d\n",L);
}
