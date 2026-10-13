#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

// Pack L/R into one complex FFT. Mirrored powers preserve stereo energy even
// with opposite-phase channels (mono summing would incorrectly cancel them).
class Spectrum {
public:
    static constexpr int N=4096, Bands=32, Rate=48000;
    float db[Bands]={}, rms[2]={-120,-120}, peakHz=0, peakDb=-120;
    float waveform[256]={};
    bool clipping=false;
    Spectrum() {
        for(int i=0;i<N;++i) window[i]=0.5f-0.5f*std::cos(2*Pi*i/N);
        for(auto &v:db) v=-120;
    }
    static float decibels(float a) { return 20*std::log10(std::max(a,1e-6f)); }
    static float edge(int b) { return 25.f*std::pow(800.f,float(b)/Bands); }
    void analyze(const int16_t *stereo) {
        float sums[2]={},means[2]={}; clipping=false;
        for(int i=0;i<N;++i) {
            float l=stereo[2*i]/32768.f,r=stereo[2*i+1]/32768.f;
            means[0]+=l; means[1]+=r; sums[0]+=l*l; sums[1]+=r*r;
            clipping|=std::abs(stereo[2*i])>=32760 || std::abs(stereo[2*i+1])>=32760;
        }
        for(int ch=0;ch<2;++ch) { means[ch]/=N; rms[ch]=decibels(std::sqrt(sums[ch]/N)); }
        for(int i=0;i<N;++i) {
            re[i]=(stereo[2*i]/32768.f-means[0])*window[i];
            im[i]=(stereo[2*i+1]/32768.f-means[1])*window[i];
        }
        for(int i=0;i<256;++i) waveform[i]=stereo[2*(N-512+i*2)]/32768.f;
        for(int i=1,j=0;i<N;++i) {
            int bit=N>>1; for(;j&bit;bit>>=1) j^=bit; j^=bit;
            if(i<j) { std::swap(re[i],re[j]); std::swap(im[i],im[j]); }
        }
        for(int len=2;len<=N;len<<=1) {
            float angle=-2*Pi/len,wr0=std::cos(angle),wi0=std::sin(angle);
            for(int start=0;start<N;start+=len) {
                float wr=1,wi=0;
                for(int j=0;j<len/2;++j) {
                    int a=start+j,b=a+len/2;
                    float tr=wr*re[b]-wi*im[b],ti=wr*im[b]+wi*re[b];
                    re[b]=re[a]-tr; im[b]=im[a]-ti; re[a]+=tr; im[a]+=ti;
                    float w=wr*wr0-wi*wi0; wi=wr*wi0+wi*wr0; wr=w;
                }
            }
        }
        float best=0; int bestBin=0;
        for(int k=1;k<N/2;++k) {
            power[k]=(re[k]*re[k]+im[k]*im[k]+re[N-k]*re[N-k]+im[N-k]*im[N-k])*(4.f/(N*N));
            if(power[k]>best) { best=power[k]; bestBin=k; }
        }
        peakDb=decibels(std::sqrt(best)); peakHz=0;
        if(peakDb>-90 && bestBin>1 && bestBin<N/2-1) {
            float a=std::log(std::max(power[bestBin-1],1e-20f));
            float b=std::log(std::max(power[bestBin],1e-20f));
            float c=std::log(std::max(power[bestBin+1],1e-20f));
            float denom=a-2*b+c,offset=std::abs(denom)>1e-9f?0.5f*(a-c)/denom:0;
            peakHz=(bestBin+std::max(-0.5f,std::min(0.5f,offset)))*Rate/N;
        } else if(peakDb>-90) peakHz=float(bestBin)*Rate/N;
        // Strongest FFT bin per logarithmic band; Hann coherent-gain corrected.
        for(int band=0;band<Bands;++band) {
            int lo=std::max(1,int(std::ceil(edge(band)*N/Rate)));
            int hi=std::min(N/2,std::max(lo+1,int(std::ceil(edge(band+1)*N/Rate))));
            float p=0; for(int k=lo;k<hi;++k) p=std::max(p,power[k]);
            db[band]=decibels(std::sqrt(p));
        }
    }
private:
    static constexpr float Pi=3.14159265358979323846f;
    float re[N],im[N],window[N],power[N/2]={};
};
