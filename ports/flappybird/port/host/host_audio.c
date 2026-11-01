/* OpenAL adapter for the five original effects (converted to PCM WAV). The original plays
 * them on three channels: point and hit share one, swoosh has its own, die and wing share
 * the third; a new sound on a channel replaces the one playing there. */
#define AL_SILENCE_DEPRECATION 1
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

static const char *names[FB_SND_COUNT]={"sfx_point.wav","sfx_swooshing.wav","sfx_die.wav","sfx_hit.wav","sfx_wing.wav"};
static const int channel_of[FB_SND_COUNT]={0,1,2,0,2};
static ALuint buffers[FB_SND_COUNT],sources[3];
static int loaded[FB_SND_COUNT];
static ALCdevice *device;
static ALCcontext *context;
static unsigned u16(const unsigned char *p){return p[0]|p[1]<<8;}
static unsigned u32(const unsigned char *p){return u16(p)|u16(p+2)<<16;}

void host_audio_shutdown(void) {
    if(!context)return;
    alDeleteSources(3,sources);alDeleteBuffers(FB_SND_COUNT,buffers);
    alcMakeContextCurrent(NULL);alcDestroyContext(context);alcCloseDevice(device);context=NULL;
}
void host_audio_init(void) {
    device=alcOpenDevice(NULL);if(!device){plat_log("OpenAL output unavailable");return;}
    context=alcCreateContext(device,NULL);
    if(!context){alcCloseDevice(device);return;}
    alcMakeContextCurrent(context);atexit(host_audio_shutdown);
    alGenBuffers(FB_SND_COUNT,buffers);alGenSources(3,sources);
    int count=0;
    for(int i=0;i<FB_SND_COUNT;i++) {
        uint32_t size=0;unsigned char *d=plat_read_file(names[i],&size,0);
        unsigned channels=0,rate=0,bits=0,encoding=0,bytes=0;const unsigned char *pcm=NULL;
        if(d && size>=12 && !memcmp(d,"RIFF",4) && !memcmp(d+8,"WAVE",4)) {
            for(unsigned off=12;off<=size-8;) {
                unsigned n=u32(d+off+4);if(n>size-off-8)break;
                if(!memcmp(d+off,"fmt ",4) && n>=16) {
                    encoding=u16(d+off+8);channels=u16(d+off+10);
                    rate=u32(d+off+12);bits=u16(d+off+22);
                } else if(!memcmp(d+off,"data",4)){pcm=d+off+8;bytes=n;}
                off+=8+n+(n&1);
            }
        }
        if(encoding==1 && (channels==1||channels==2) && bits==16 && rate && pcm && bytes) {
            alBufferData(buffers[i],channels==1?AL_FORMAT_MONO16:AL_FORMAT_STEREO16,pcm,bytes,rate);
            loaded[i]=1;count++;
        } else plat_log("Could not load PCM effect %s",names[i]);
        free(d);
    }
    for(int i=0;i<3;i++)alSourcei(sources[i],AL_SOURCE_RELATIVE,AL_TRUE);
    plat_log("OpenAL: %d original effects loaded",count);
}
void plat_sound(int sound,float gain) {
    if(!context||sound<0||sound>=FB_SND_COUNT||!loaded[sound])return;
    ALuint source=sources[channel_of[sound]];
    alSourceStop(source);alSourcei(source,AL_BUFFER,(ALint)buffers[sound]);
    alSourcef(source,AL_GAIN,gain);alSourcePlay(source);
}
