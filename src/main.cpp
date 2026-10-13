#include <M5Unified.h>
#include <atomic>
#include <cstring>
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/stream_buffer.h"
#include "usb_device_uac.h"
#include "tusb.h"
#include "spectrum.hpp"

static Spectrum spectrum;
static M5Canvas screen(&M5.Display);
static int16_t history[Spectrum::N*2]={},snapshot[Spectrum::N*2]={};
static portMUX_TYPE audioLock=portMUX_INITIALIZER_UNLOCKED;
static unsigned writeFrame=0;
static uint32_t receivedFrames=0,packets=0;
static int64_t lastAudioUs=0;
static StreamBufferHandle_t speakerStream;
static std::atomic<uint32_t> droppedBytes{0},hostVolume{100};
static std::atomic<bool> hostMute{false},localMute{false};
static float shown[32]={},peaks[32]={};
static float peakAge[32]={};
static bool hold=false,waveformMode=false,active=false;
static uint32_t frameNumber=0;
static uint32_t analyzedFrames=0,analysisCount=0;
static int64_t lastAnalysisUs=0;
static bool newAnalysis=false;
// Internal DMA staging avoids M5Canvas's non-DMA path for a PSRAM sprite.
// Alternate buffers so the CPU can fill one while SPI consumes the other.
alignas(16) static lgfx::swap565_t dmaPixels[2][320*8];
static unsigned dmaIndex=0;
static uint32_t frameBytes=0,transferUs=0;
struct Performance {
    int64_t start=0;
    uint64_t fftTotal=0,renderTotal=0,lcdTotal=0,byteTotal=0;
    uint32_t analyses=0,frames=0,maxWork=0;
    float fps=0;
    uint32_t fftUs=0,renderUs=0,lcdUs=0,bytesPerFrame=0,maxFrameUs=0;
    void record(int64_t now,uint32_t fft,uint32_t render,uint32_t lcd,uint32_t work,bool analyzed) {
        if(!start) start=now;
        if(analyzed) { fftTotal+=fft; ++analyses; }
        if(frameBytes) {
            ++frames; renderTotal+=render; lcdTotal+=lcd; byteTotal+=frameBytes;
            maxWork=std::max(maxWork,work);
        }
        if(now-start>=1000000) {
            fps=frames*1000000.f/(now-start);
            fftUs=analyses?fftTotal/analyses:0;
            renderUs=frames?renderTotal/frames:0; lcdUs=frames?lcdTotal/frames:0;
            bytesPerFrame=frames?byteTotal/frames:0; maxFrameUs=maxWork;
            start=now; fftTotal=renderTotal=lcdTotal=byteTotal=0;
            analyses=frames=maxWork=0;
        }
    }
} perf;
static constexpr uint16_t BG=0x0862,GRID=0x1926,TEXT=0xE73C,DIM=0x7C72;

static esp_err_t receiveAudio(uint8_t *buf,size_t len,void *) {
    // Nonblocking USB callback. FFT, LCD and audio playback run elsewhere.
    const unsigned frames=len/4;
    if(!frames || frames>Spectrum::N) return ESP_OK;
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&audioLock);
    if(now-lastAudioUs>200000) { memset(history,0,sizeof(history)); writeFrame=0; }
    unsigned first=std::min(frames,unsigned(Spectrum::N)-writeFrame);
    memcpy(history+writeFrame*2,buf,first*4);
    if(first<frames) memcpy(history,buf+first*4,(frames-first)*4);
    writeFrame=(writeFrame+frames)%Spectrum::N;
    receivedFrames+=frames; ++packets; lastAudioUs=now;
    portEXIT_CRITICAL(&audioLock);
    size_t used=xStreamBufferSend(speakerStream,buf,frames*4,0);
    droppedBytes.fetch_add(frames*4-used);
    return ESP_OK;
}
static void setMute(uint32_t mute,void *) { hostMute.store(mute!=0); }
static void setVolume(uint32_t volume,void *) { hostVolume.store(std::min<uint32_t>(volume,100)); }

static void speakerTask(void *) {
    // M5Unified holds pointers to two queued blocks. Rotate three buffers.
    static int16_t buffers[3][960]; unsigned index=0;
    uint32_t previousVolume=UINT32_MAX; bool previousMute=false;
    for(;;) {
        int16_t *block=buffers[index];
        size_t len=xStreamBufferReceive(speakerStream,block,sizeof(buffers[0]),pdMS_TO_TICKS(50));
        if(!len) continue;
        bool muted=localMute.load()||hostMute.load();
        // Espressif maps -50..0 dB to 0..100. M5 volume uses squared scaling.
        uint32_t volume=hostVolume.load();
        if(volume!=previousVolume || muted!=previousMute) {
            float gain=std::pow(10.f,(float(volume)*0.5f-50.f)/20.f);
            M5.Speaker.setVolume(muted?0:uint8_t(90.f*std::sqrt(gain)));
            previousVolume=volume; previousMute=muted;
        }
        M5.Speaker.playRaw(block,len/2,48000,true,1,0,false);
        index=(index+1)%3;
    }
}
static void captureAudio() {
    const int64_t now=esp_timer_get_time();
    newAnalysis=false;
    portENTER_CRITICAL(&audioLock);
    active=lastAudioUs && now-lastAudioUs<250000;
    if(active && !hold && receivedFrames!=analyzedFrames) {
        unsigned first=Spectrum::N-writeFrame;
        memcpy(snapshot,history+writeFrame*2,first*4);
        if(writeFrame) memcpy(snapshot+first*2,history,writeFrame*4);
        analyzedFrames=receivedFrames; newAnalysis=true;
    }
    portEXIT_CRITICAL(&audioLock);
    if(!hold && !active && lastAnalysisUs) {
        spectrum.clear(); lastAnalysisUs=0;
        for(int i=0;i<32;++i) shown[i]=peaks[i]=peakAge[i]=0;
    }
    if(newAnalysis) {
        spectrum.analyze(snapshot);
        ++analysisCount;
        const float dt=lastAnalysisUs?std::min(.25f,(now-lastAnalysisUs)*1e-6f):.016f;
        lastAnalysisUs=now;
        const float attack=1.f-std::exp(-dt/.04f);
        for(int i=0;i<32;++i) {
            float target=std::max(0.f,std::min(1.f,(spectrum.db[i]+72.f)/72.f));
            shown[i]=target>shown[i]?shown[i]+attack*(target-shown[i]):std::max(target,shown[i]-.4f*dt);
            if(shown[i]>=peaks[i]) { peaks[i]=shown[i]; peakAge[i]=0; }
            else if((peakAge[i]+=dt)>1.2f) peaks[i]=std::max(shown[i],peaks[i]-.2f*dt);
        }
    }
}
static uint16_t barColor(int y) {
    if(y<95) return screen.color565(255,108,113);
    if(y<130) return screen.color565(252,209,104);
    return screen.color565(71,221,201);
}
static void label(const char *s,int x,int y,uint16_t color=DIM,int size=1) {
    screen.setTextColor(color,BG); screen.setTextSize(size); screen.setCursor(x,y); screen.print(s);
}
static void renderFullScene() {
    screen.fillScreen(BG);
    label("SPECTRUM",12,10,TEXT,2);
    screen.fillCircle(218,15,3,active?0x5EF5:0x8410);
    label(active?"USB 48K":"USB WAIT",229,12,active?0x5EF5:DIM);
    screen.drawFastHLine(12,33,296,GRID);
    char line[48];
    snprintf(line,sizeof(line),"PEAK %7.1f Hz",spectrum.peakHz); label(line,12,42,TEXT);
    label(hold?"HOLD":spectrum.clipping?"CLIP":"LIVE",270,42,hold?0xFEA8:spectrum.clipping?0xFAAA:0x5EF5);
    for(int ch=0;ch<2;++ch) {
        int x=12+ch*151; label(ch?"R":"L",x,58);
        screen.fillRoundRect(x+12,57,130,5,2,GRID);
        int w=int(std::max(0.f,std::min(1.f,(spectrum.rms[ch]+60.f)/60.f))*130);
        if(w) screen.fillRoundRect(x+12,57,w,5,2,spectrum.rms[ch]>-3?0xFAAA:0x5EF5);
    }
    for(int j=0;j<=3;++j) {
        int y=76+j*40; screen.drawFastHLine(28,y,280,GRID);
        snprintf(line,sizeof(line),"%d",-24*j); label(line,2,y-3);
    }
    if(!waveformMode) {
        for(int i=0;i<32;++i) {
            int x=29+i*280/32,h=int(shown[i]*120);
            for(int dy=0;dy<h;dy+=4) screen.fillRect(x,196-dy-3,6,std::min(3,h-dy),barColor(196-dy));
            if(peaks[i]>0.01f) screen.drawFastHLine(x,196-int(peaks[i]*120),6,TEXT);
        }
        label("25",28,203); label("100",78,203); label("1k",176,203); label("20k Hz",270,203);
    } else {
        screen.drawFastHLine(28,136,280,0x43AB);
        for(int x=1;x<256;++x) screen.drawLine(28+x-1,136-int(spectrum.waveform[x-1]*56),28+x,136-int(spectrum.waveform[x]*56),0x5EF5);
        label("L WAVEFORM / 10.7 ms",70,203);
    }
    if(!active && !hold) {
        screen.fillRoundRect(53,109,233,47,5,BG);
        label(tud_mounted()?"Select PC sound output:":"Connect USB to your PC",65,119,TEXT);
        label("CoreS3 USB Spectrum",67,137,0x5EF5);
    }
    const char *buttons[]={waveformMode?"BARS":"WAVE",hold?"RESUME":"HOLD",localMute.load()?"SPK OFF":"SPK ON"};
    for(int i=0;i<3;++i) {
        screen.fillRoundRect(9+i*104,219,96,18,4,GRID);
        screen.setTextSize(1); screen.setTextColor(i==2&&localMute.load()?DIM:TEXT,GRID);
        screen.drawCenterString(buttons[i],57+i*104,224);
    }
}

static void presentRect(int x,int y,int w,int h) {
    const int64_t begin=esp_timer_get_time();
    const auto *pixels=static_cast<const lgfx::swap565_t*>(screen.getBuffer());
    const int rowsPerChunk=int(sizeof(dmaPixels[0])/sizeof(dmaPixels[0][0]))/w;
    for(int row=0;row<h;row+=rowsPerChunk) {
        const int rows=std::min(rowsPerChunk,h-row);
        auto *buffer=dmaPixels[dmaIndex];
        for(int dy=0;dy<rows;++dy)
            memcpy(buffer+dy*w,pixels+(y+row+dy)*320+x,w*sizeof(*pixels));
        M5.Display.waitDMA();
        M5.Display.pushImageDMA(x,y+row,w,rows,buffer);
        dmaIndex^=1;
    }
    frameBytes+=w*h*2;
    transferUs+=esp_timer_get_time()-begin;
}

static int meterWidth(int ch) {
    return int(std::max(0.f,std::min(1.f,(spectrum.rms[ch]+60.f)/60.f))*130);
}
static int peakHeight(int band) { return peaks[band]>.01f?int(peaks[band]*120):-1; }

static void drawScreen() {
    static int oldScene=-1,oldHeight[32],oldPeak[32],oldMeter[2],oldFps=-1;
    static int oldWaveTop=76,oldWaveBottom=196;
    static bool oldRed[2];
    static char oldPeakText[48]={};
    static bool oldClip=false;
    const int scene=int(active)|(int(hold)<<1)|(int(waveformMode)<<2)
        |(int(localMute.load())<<3)|(int(tud_mounted())<<4);
    const bool full=scene!=oldScene;
    frameBytes=transferUs=0;
    char line[48]; snprintf(line,sizeof(line),"PEAK %7.1f Hz",spectrum.peakHz);
    M5.Display.startWrite();
    if(full) {
        renderFullScene();
        for(int i=0;i<32;++i) { oldHeight[i]=int(shown[i]*120); oldPeak[i]=peakHeight(i); }
        for(int ch=0;ch<2;++ch) { oldMeter[ch]=meterWidth(ch); oldRed[ch]=spectrum.rms[ch]>-3; }
        strcpy(oldPeakText,line); oldClip=spectrum.clipping;
        oldScene=scene; oldFps=-1; oldWaveTop=76; oldWaveBottom=196;
    } else if(newAnalysis) {
        if(strcmp(line,oldPeakText)) {
            screen.fillRect(12,42,156,8,BG); label(line,12,42,TEXT);
            presentRect(12,42,156,8); strcpy(oldPeakText,line);
        }
        if(spectrum.clipping!=oldClip) {
            screen.fillRect(270,42,30,8,BG);
            label(spectrum.clipping?"CLIP":"LIVE",270,42,spectrum.clipping?0xFAAA:0x5EF5);
            presentRect(270,42,30,8); oldClip=spectrum.clipping;
        }
        for(int ch=0;ch<2;++ch) {
            int w=meterWidth(ch); bool red=spectrum.rms[ch]>-3;
            if(w==oldMeter[ch] && red==oldRed[ch]) continue;
            int x=24+151*ch;
            screen.fillRoundRect(x,57,130,5,2,GRID);
            if(w) screen.fillRoundRect(x,57,w,5,2,red?0xFAAA:0x5EF5);
            presentRect(x,57,130,5); oldMeter[ch]=w; oldRed[ch]=red;
        }
        if(waveformMode) {
            // Include both old and new extents so a shrinking trace leaves no
            // stale pixels. The rest of the grid and centerline stay on the LCD.
            int top=136,bottom=136;
            for(float sample:spectrum.waveform) {
                int y=136-int(sample*56);
                top=std::min(top,y); bottom=std::max(bottom,y);
            }
            int dirtyTop=std::min(top,oldWaveTop),dirtyBottom=std::max(bottom,oldWaveBottom);
            screen.fillRect(28,dirtyTop,256,dirtyBottom-dirtyTop+1,BG);
            for(int j=0;j<=3;++j) {
                int y=76+j*40;
                if(y>=dirtyTop && y<=dirtyBottom) screen.drawFastHLine(28,y,256,GRID);
            }
            screen.drawFastHLine(28,136,256,0x43AB);
            for(int x=1;x<256;++x)
                screen.drawLine(28+x-1,136-int(spectrum.waveform[x-1]*56),28+x,136-int(spectrum.waveform[x]*56),0x5EF5);
            presentRect(28,dirtyTop,256,dirtyBottom-dirtyTop+1);
            oldWaveTop=top; oldWaveBottom=bottom;
        } else for(int i=0;i<32;++i) {
            int h=int(shown[i]*120),p=peakHeight(i);
            if(h==oldHeight[i] && p==oldPeak[i]) continue;
            int x=29+i*280/32;
            int top=std::max(76,196-std::max({h+2,oldHeight[i]+2,p,oldPeak[i]}));
            screen.fillRect(x,76,6,121,BG);
            for(int j=0;j<=3;++j) screen.drawFastHLine(x,76+j*40,6,GRID);
            for(int dy=0;dy<h;dy+=4)
                screen.fillRect(x,196-dy-3,6,std::min(3,h-dy),barColor(196-dy));
            if(p>=0) screen.drawFastHLine(x,196-p,6,TEXT);
            presentRect(x,top,6,197-top);
            oldHeight[i]=h; oldPeak[i]=p;
        }
    }
    int fps=int(perf.fps+.5f);
    if(fps!=oldFps) {
        screen.fillRect(158,10,54,10,BG);
        char rate[16]; snprintf(rate,sizeof(rate),"%3dFPS",fps); label(rate,158,12,DIM);
        if(!full) presentRect(158,10,54,10);
        oldFps=fps;
    }
    if(full) presentRect(0,0,320,240);
    const int64_t waitStart=esp_timer_get_time();
    M5.Display.waitDMA(); M5.Display.endWrite();
    transferUs+=esp_timer_get_time()-waitStart;
    // Count completed LCD updates, not iterations that drew nothing.
    if(frameBytes) ++frameNumber;
}
static void handleTouch() {
    M5.update(); auto touch=M5.Touch.getDetail();
    if(touch.wasClicked() && touch.y>=214) {
        if(touch.x<109) waveformMode=!waveformMode;
        else if(touch.x<213) hold=!hold;
        else { localMute.store(!localMute.load()); if(localMute.load()) M5.Speaker.setVolume(0); }
    }
}
static bool serialWrite(const void *ptr,size_t length) {
    const uint8_t *data=static_cast<const uint8_t*>(ptr);
    int64_t deadline=esp_timer_get_time()+3000000;
    while(length && tud_cdc_connected()) {
        unsigned n=tud_cdc_write(data,length); tud_cdc_write_flush(); data+=n; length-=n;
        if(esp_timer_get_time()>deadline) return false;
        if(!n) vTaskDelay(pdMS_TO_TICKS(1));
    }
    return length==0;
}
static void status() {
    uint32_t total,count; int64_t age;
    portENTER_CRITICAL(&audioLock); total=receivedFrames; count=packets; age=esp_timer_get_time()-lastAudioUs; portEXIT_CRITICAL(&audioLock);
    char s[1100];
    int n=snprintf(s,sizeof(s),"{\"firmware\":\"1.1.0-dev\",\"board\":%d,\"usb\":%s,\"audio\":%s,\"rate\":48000,\"frames\":%lu,\"packets\":%lu,\"age_ms\":%lld,\"peak_hz\":%.2f,\"peak_dbfs\":%.2f,\"left_dbfs\":%.2f,\"right_dbfs\":%.2f,\"dropped_bytes\":%lu,\"draws\":%lu,\"hold\":%s,\"local_mute\":%s,\"host_mute\":%s,\"host_volume\":%lu,\"heap\":%lu,\"internal_heap\":%lu,\"waveform\":%s,\"analyses\":%lu,\"fps\":%.2f,\"analysis_us\":%lu,\"render_us\":%lu,\"lcd_us\":%lu,\"bytes_per_frame\":%lu,\"max_frame_us\":%lu}\n",
        int(M5.getBoard()),tud_mounted()?"true":"false",active?"true":"false",(unsigned long)total,(unsigned long)count,(long long)(age/1000),spectrum.peakHz,spectrum.peakDb,spectrum.rms[0],spectrum.rms[1],(unsigned long)droppedBytes.load(),(unsigned long)frameNumber,hold?"true":"false",localMute.load()?"true":"false",hostMute.load()?"true":"false",(unsigned long)hostVolume.load(),(unsigned long)esp_get_free_heap_size(),(unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),waveformMode?"true":"false",(unsigned long)analysisCount,perf.fps,(unsigned long)perf.fftUs,(unsigned long)perf.renderUs,(unsigned long)perf.lcdUs,(unsigned long)perf.bytesPerFrame,(unsigned long)perf.maxFrameUs);
    if(n>0) serialWrite(s,std::min(size_t(n),sizeof(s)-1));
}

static void verifyDisplay() {
    uint8_t expected[320*3],actual[320*3];
    unsigned mismatches=0,maxDelta=0;
    M5.Display.waitDMA();
    for(int y=0;y<240;++y) {
        screen.readRectRGB(0,y,320,1,expected);
        M5.Display.readRectRGB(0,y,320,1,actual);
        for(int x=0;x<320;++x) {
            unsigned delta=0;
            for(int c=0;c<3;++c) delta=std::max(delta,unsigned(std::abs(int(expected[x*3+c])-actual[x*3+c])));
            maxDelta=std::max(maxDelta,delta);
            if(delta>8) ++mismatches; // RGB565/666 readback quantization tolerance.
        }
        if(!(y%16)) vTaskDelay(1);
    }
    char result[128];
    int n=snprintf(result,sizeof(result),"{\"lcd_mismatched_pixels\":%u,\"max_channel_delta\":%u,\"pixels\":76800}\n",mismatches,maxDelta);
    serialWrite(result,n);
}
static void handleSerial() {
    while(tud_cdc_available()) {
        char c=0; tud_cdc_read(&c,1);
        if(c=='s') status();
        else if(c=='v') verifyDisplay();
        else if(c=='m') { localMute.store(!localMute.load()); if(localMute.load()) M5.Speaker.setVolume(0); status(); }
        else if(c=='h') { hold=!hold; status(); }
        else if(c=='w') { waveformMode=!waveformMode; status(); }
        else if(c=='p') {
            const char header[]="P6\n320 240\n255\n"; serialWrite(header,sizeof(header)-1);
            uint8_t row[320*3];
            for(int y=0;y<240;++y) { screen.readRectRGB(0,y,320,1,row); if(!serialWrite(row,sizeof(row))) break; vTaskDelay(1); }
        }
    }
}
extern "C" void app_main() {
    auto cfg=M5.config(); cfg.internal_mic=false; cfg.internal_imu=false; cfg.internal_rtc=false; cfg.output_power=false;
    M5.begin(cfg);
    M5.Display.setRotation(1); M5.Display.setBrightness(140);
    M5.Display.fillScreen(BG); M5.Display.setTextColor(TEXT,BG); M5.Display.setTextSize(2);
    M5.Display.println("Starting USB audio...");
    screen.setColorDepth(16); screen.setPsram(true);
    if(!screen.createSprite(320,240)) { M5.Display.println("Display allocation failed"); return; }
    M5.Display.initDMA();
    if(!spectrum.begin()) { M5.Display.println("FFT allocation failed"); return; }
    auto spk=M5.Speaker.config(); spk.sample_rate=48000; spk.task_priority=5; spk.task_pinned_core=0;
    M5.Speaker.config(spk); M5.Speaker.setVolume(60); M5.Speaker.begin();
    speakerStream=xStreamBufferCreate(32768,1920);
    if(!speakerStream || xTaskCreatePinnedToCore(speakerTask,"pcm_playback",4096,nullptr,5,nullptr,0)!=pdPASS) {
        M5.Display.println("Audio allocation failed"); return;
    }
    uac_device_config_t usb={}; usb.output_cb=receiveAudio; usb.set_mute_cb=setMute; usb.set_volume_cb=setVolume;
    usb.spk_itf_num=1; usb.mic_itf_num=-1;
    ESP_ERROR_CHECK(uac_device_init(&usb));
    int64_t lastTouchUs=0;
    for(;;) {
        const int64_t begin=esp_timer_get_time();
        if(begin-lastTouchUs>=10000) { handleTouch(); lastTouchUs=begin; }
        const int64_t captureStart=esp_timer_get_time();
        captureAudio();
        const int64_t drawStart=esp_timer_get_time();
        drawScreen();
        const int64_t end=esp_timer_get_time();
        perf.record(end,drawStart-captureStart,end-drawStart-transferUs,transferUs,end-begin,newAnalysis);
        handleSerial();
        // No fixed frame cap. One tick keeps the idle task/watchdog serviced.
        vTaskDelay(1);
    }
}
