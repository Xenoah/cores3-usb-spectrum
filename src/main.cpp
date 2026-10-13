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
static unsigned peakAge[32]={};
static bool hold=false,waveformMode=false,active=false;
static uint32_t frameNumber=0;
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
    for(;;) {
        int16_t *block=buffers[index];
        size_t len=xStreamBufferReceive(speakerStream,block,sizeof(buffers[0]),pdMS_TO_TICKS(50));
        if(!len) continue;
        bool muted=localMute.load()||hostMute.load();
        // Espressif maps -50..0 dB to 0..100. M5 volume uses squared scaling.
        float gain=std::pow(10.f,(float(hostVolume.load())*0.5f-50.f)/20.f);
        M5.Speaker.setVolume(muted?0:uint8_t(90.f*std::sqrt(gain)));
        M5.Speaker.playRaw(block,len/2,48000,true,1,0,false);
        index=(index+1)%3;
    }
}
static void captureAudio() {
    portENTER_CRITICAL(&audioLock);
    active=lastAudioUs && esp_timer_get_time()-lastAudioUs<250000;
    if(active) {
        unsigned first=Spectrum::N-writeFrame;
        memcpy(snapshot,history+writeFrame*2,first*4);
        if(writeFrame) memcpy(snapshot+first*2,history,writeFrame*4);
    } else memset(snapshot,0,sizeof(snapshot));
    portEXIT_CRITICAL(&audioLock);
    if(!hold) {
        spectrum.analyze(snapshot);
        for(int i=0;i<32;++i) {
            if(!active) { shown[i]=peaks[i]=0; peakAge[i]=0; continue; }
            float target=std::max(0.f,std::min(1.f,(spectrum.db[i]+72.f)/72.f));
            shown[i]=target>shown[i]?shown[i]+0.8f*(target-shown[i]):std::max(target,shown[i]-0.025f);
            if(shown[i]>=peaks[i]) { peaks[i]=shown[i]; peakAge[i]=0; }
            else if(++peakAge[i]>18) peaks[i]=std::max(shown[i],peaks[i]-0.012f);
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
static void drawScreen() {
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
    screen.pushSprite(0,0); ++frameNumber;
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
    char s[600];
    int n=snprintf(s,sizeof(s),"{\"firmware\":\"1.0.0\",\"board\":%d,\"usb\":%s,\"audio\":%s,\"rate\":48000,\"frames\":%lu,\"packets\":%lu,\"age_ms\":%lld,\"peak_hz\":%.2f,\"peak_dbfs\":%.2f,\"left_dbfs\":%.2f,\"right_dbfs\":%.2f,\"dropped_bytes\":%lu,\"draws\":%lu,\"hold\":%s,\"local_mute\":%s,\"host_mute\":%s,\"host_volume\":%lu,\"heap\":%lu}\n",
        int(M5.getBoard()),tud_mounted()?"true":"false",active?"true":"false",(unsigned long)total,(unsigned long)count,(long long)(age/1000),spectrum.peakHz,spectrum.peakDb,spectrum.rms[0],spectrum.rms[1],(unsigned long)droppedBytes.load(),(unsigned long)frameNumber,hold?"true":"false",localMute.load()?"true":"false",hostMute.load()?"true":"false",(unsigned long)hostVolume.load(),(unsigned long)esp_get_free_heap_size());
    serialWrite(s,n);
}
static void handleSerial() {
    while(tud_cdc_available()) {
        char c=0; tud_cdc_read(&c,1);
        if(c=='s') status();
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
    auto spk=M5.Speaker.config(); spk.sample_rate=48000; spk.task_priority=5; spk.task_pinned_core=0;
    M5.Speaker.config(spk); M5.Speaker.setVolume(60); M5.Speaker.begin();
    speakerStream=xStreamBufferCreate(32768,1920);
    if(!speakerStream || xTaskCreatePinnedToCore(speakerTask,"pcm_playback",4096,nullptr,5,nullptr,0)!=pdPASS) {
        M5.Display.println("Audio allocation failed"); return;
    }
    uac_device_config_t usb={}; usb.output_cb=receiveAudio; usb.set_mute_cb=setMute; usb.set_volume_cb=setVolume;
    usb.spk_itf_num=1; usb.mic_itf_num=-1;
    ESP_ERROR_CHECK(uac_device_init(&usb));
    TickType_t tick=xTaskGetTickCount();
    for(;;) {
        handleTouch(); captureAudio(); drawScreen(); handleSerial();
        vTaskDelayUntil(&tick,pdMS_TO_TICKS(33));
    }
}
