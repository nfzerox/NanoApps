/* OpenAL adapter for the original PCM WAV effects. One source per effect, as
 * in the original SoundEngine: different effects overlap; UFO audio loops. */
#define AL_SILENCE_DEPRECATION 1
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

static const char *names[]={"usaugateufo.wav","feder.wav","jump.wav","lomise.wav",
    "pucanje.wav","pucanje2.wav","ufo.wav","bijeli.wav","crnarupa.wav","pada.wav",
    "ufopogodak.wav","monstersudar.wav","monsterpogodak.wav"};
static ALuint buffers[13],sources[13];
static float lengths[13];
static ALCdevice *device;
static ALCcontext *context;
static unsigned u16(const unsigned char *p){return p[0]|p[1]<<8;}
static unsigned u32(const unsigned char *p){return u16(p)|u16(p+2)<<16;}
static int index_of(const char *name){for(int i=0;i<13;i++)if(!strcmp(name,names[i]))return i;return -1;}

void host_audio_shutdown(void) {
    if(!context)return;
    alDeleteSources(13,sources);alDeleteBuffers(13,buffers);
    alcMakeContextCurrent(NULL);alcDestroyContext(context);alcCloseDevice(device);context=NULL;
}
void host_audio_init(void) {
    device=alcOpenDevice(NULL);if(!device){plat_log("OpenAL output unavailable");return;}
    context=alcCreateContext(device,NULL);
    if(!context){alcCloseDevice(device);return;}
    alcMakeContextCurrent(context);atexit(host_audio_shutdown);
    alGenBuffers(13,buffers);alGenSources(13,sources);
    int loaded=0;
    for(int i=0;i<13;i++) {
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
            alSourcei(sources[i],AL_BUFFER,buffers[i]);alSourcei(sources[i],AL_SOURCE_RELATIVE,AL_TRUE);
            lengths[i]=(float)bytes/(rate*channels*2);loaded++;
        } else plat_log("Could not load PCM effect %s",names[i]);
        free(d);
    }
    plat_log("OpenAL: %d original effects loaded",loaded);
}
float plat_audio_play(const char *name,float volume,int loop,int track) {
    (void)track;int i=index_of(name);if(!context||i<0||!lengths[i])return 0;
    alSourceStop(sources[i]);alSourcef(sources[i],AL_GAIN,volume);
    alSourcei(sources[i],AL_LOOPING,loop?AL_TRUE:AL_FALSE);alSourcePlay(sources[i]);return lengths[i];
}
void plat_audio_stop(const char *name){int i=index_of(name);if(context&&i>=0)alSourceStop(sources[i]);}
void plat_audio_track_volume(int track,float volume){(void)track;if(context)alListenerf(AL_GAIN,volume);}
void plat_audio_clip_volume(const char *name,float volume){int i=index_of(name);if(context&&i>=0)alSourcef(sources[i],AL_GAIN,volume);}
