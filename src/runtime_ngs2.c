/* libSceNgs2 reimplemented from scratch: the software mixer Sony's "sndx" middleware drives.
 *
 * Written against the API surface traced from the console module in The Last Guardian and
 * Gravity Rush 2 (docs/NGS2_LEVANTAMENTO.md): ten functions, five rack types, and the
 * VoiceControl parameter blocks those games send. Nothing here is specific to one game; the
 * selection between this mixer and the firmware module is TLG_NGS2 (native = the console's
 * libSceNgs2.sprx linked as a module; anything else = this file).
 *
 * Model:
 *   - A system owns racks; a rack owns voices. Handles are host objects tagged with a magic.
 *     Voice 0 of a rack is the rack handle itself (the console module does the same: sndx
 *     patches sampler ports to rack handles), any other voice has its own object.
 *   - Sampler voices decode PCM, VAG/HEVAG or ATRAC9 (LibAtrac9) from queued waveform blocks,
 *     resample to the system rate (cubic, pitch ratio * source rate), apply the envelope and mix
 *     through each patched port (volume and an output-major matrix: level[out * in_ch + in]).
 *   - Submixer, reverb, custom and mastering voices are buses: inputs are summed, effects run,
 *     and the result goes on through their own ports. Buses are processed in dependency order
 *     each render. Mastering voices write the render buffers (output id = buffer index).
 *   - Effects follow the console module's processing (studied in the firmware binary, written
 *     anew): RBJ biquads at locations 0 (input), 1 (after effects) and 2+p (port p only); the
 *     compressor/limiter (32-sample peak or RMS blocks, slope curve, attack/release smoothing);
 *     mastering LFE low-pass (4th-order 0.1 dB Chebyshev, 120 Hz by default), limiter at 1.0
 *     and gain; I3DL2 reverb (early-reflection patterns, 16 damped combs mixed per channel,
 *     4 allpasses, room HF); custom-rack chorus and multi-tap delay; envelope curves; 64-sample
 *     fades on PLAY/STOP; port volume/matrix changes ramp over a grain. Distortion is not done.
 *   - VoiceGetState: sndx frees a voice when INUSE (bit 0) drops, so INUSE stays set from setup
 *     until the voice ends (blocks exhausted, release finished, STOP/KILL).
 *
 * sotc: taken from HayatoG/tlg_pc (port/src/runtime_ngs2.c @ bd122ed, 14 Oct 2026, GPL-2.0 as
 * that port), proposed for this port by erenyildiz23 (fork issue #155). Shadow of the Colossus
 * imports the same ten functions plus sceNgs2VoiceGetStateFlags. It plays when the player's own
 * libSceNgs2.sprx is not linked (probe.c prefer_native() keeps the firmware module first).
 *
 * Environment (SOTC_ prefix, TLG_ also read; ports with another prefix alias it):
 *   TLG_NGS2=native|hle      firmware module or this mixer (default: this mixer)
 *   TLG_NGS2_LOG=1           unknown parameters, decoder errors, a summary every 30 s
 *   TLG_NGS2_WAV=<file>      writes render buffer 0 as float WAV (compare with the native mix)
 *   TLG_NGS2_USERFX=0        do not call the game's user FX handlers
 *   TLG_NGS2_REVERB=pass     reverb racks pass their input through unprocessed (debug)
 *   TLG_NGS2_CUSTOM=mute     custom racks (chorus/delay) output silence (debug)
 *   TLG_NGS2_DELAY_TYPE=0    custom delay on a mono downmix instead of per channel (the default)
 *
 * HEVAG coefficients: vgmstream (ISC license), src/coding/psv_decoder.c. */
#define _GNU_SOURCE
#include "runtime.h"
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xmmintrin.h>
#include "third_party/LibAtrac9/C/src/libatrac9.h"

#define NGS2_OK 0
#define NGS2_ERR_HANDLE ((int32_t)0x804A0230)
#define NGS2_ERR_ARG ((int32_t)0x804A0201)
#define NGS2_ERR_MEMORY ((int32_t)0x804A0050)

#define MAGIC_SYSTEM UINT32_C(0x4e325359)
#define MAGIC_RACK UINT32_C(0x4e325241)
#define MAGIC_VOICE UINT32_C(0x4e32564f)

#define MAX_CH 8
#define MAX_GRAIN 1024
#define MAX_PORTS 16
#define MAX_MATRICES 16
#define MAX_LEVELS (MAX_CH * MAX_CH)
#define MAX_BLOCKS 64
#define MAX_ENV 16
#define MAX_FILTERS 8
#define FIFO_FRAMES 2048
#define UNIT_FRAMES 256
#define AT9_SUPERFRAME_MAX 8192   /* sotc: largest ATRAC9 superframe (2048-byte frames, 4 per superframe) */
#define END_GRACE_RENDERS 2

enum { RACK_SAMPLER = 0x1000, RACK_SUBMIXER = 0x2000, RACK_REVERB = 0x2001, RACK_MASTERING = 0x3000 };
enum { KIND_SAMPLER, KIND_SUBMIXER, KIND_REVERB, KIND_MASTERING, KIND_CUSTOM };
enum { EV_PLAY, EV_RELEASE, EV_STOP, EV_KILL, EV_PAUSE, EV_RESUME };
enum { ST_FREE, ST_READY, ST_PLAYING };
#define FLAG_INUSE 0x1u
#define FLAG_PLAYING 0x2u
#define FLAG_PAUSED 0x4u
#define FLAG_EMPTY 0x20u

enum {
    WAVE_PCM_U8 = 0x10, WAVE_PCM_I8 = 0x11, WAVE_PCM_I16L = 0x12, WAVE_PCM_I16B = 0x13,
    WAVE_PCM_I24L = 0x14, WAVE_PCM_I24B = 0x15, WAVE_PCM_I32L = 0x16, WAVE_PCM_I32B = 0x17,
    WAVE_PCM_F32L = 0x18, WAVE_PCM_F32B = 0x19, WAVE_PCM_F64L = 0x1a, WAVE_PCM_F64B = 0x1b,
    WAVE_VAG = 0x1c, WAVE_ATRAC9 = 0x40,
};

/* ---- Guest-visible layouts ---- */
typedef struct { uint16_t size; int16_t next; uint32_t id; } ParamHeader;
typedef struct { void *host_buffer; uint64_t host_size; uintptr_t reserved[5]; uintptr_t user_data; } BufferInfo;
typedef ABI int32_t (*AllocHandler)(BufferInfo *);
typedef struct { AllocHandler alloc, free; uintptr_t user_data; } Allocator;
typedef struct { void *buffer; uint64_t size; uint32_t waveform, channels; } RenderBuffer;
typedef struct { uint32_t data_offset, data_size, repeats, skip, samples, reserved; uint64_t user_data; } GuestBlock;
typedef struct { uint32_t curve, duration; float height; } EnvPoint;
typedef struct {
    float **channels;
    uintptr_t user0, user1, user2;
    uint32_t flags, num_channels, num_grain_samples, sample_rate;
} UserFxContext;
typedef ABI int32_t (*UserFxHandler)(UserFxContext *);
typedef struct {
    uintptr_t callback_data, voice;
    uint32_t flag, reserved;
    uintptr_t user_data;
    const void *data;
    uint32_t data_size, repeated, attributes, reserved2;
} CallbackInfo;
typedef ABI void (*CallbackHandler)(const CallbackInfo *);
_Static_assert(sizeof(BufferInfo) == 64, "NGS2 buffer info");
_Static_assert(sizeof(GuestBlock) == 32, "NGS2 waveform block");
_Static_assert(sizeof(EnvPoint) == 12, "NGS2 envelope point");
_Static_assert(sizeof(UserFxContext) == 48, "NGS2 user FX context");
_Static_assert(sizeof(CallbackInfo) == 56, "NGS2 callback info");

/* ---- Host objects ---- */
typedef struct System System;
typedef struct Rack Rack;
typedef struct Voice Voice;

/* A port's effective levels from the last render ramp to the new ones over the next grain,
 * as the console module does (no zipper noise on volume/pan changes). */
typedef struct { Voice *dest; uint32_t input, delay; float volume; int32_t matrix; int primed; float last[MAX_LEVELS]; } Port;
typedef struct { const uint8_t *data; uint32_t size, repeats, skip, samples; uint64_t user; } Block;
typedef struct { uint32_t count; float level[MAX_LEVELS]; } Matrix;

/* Biquad filter module (sampler 0x1000000a, submixer 0x20000006). location 0 runs on the voice
 * input, 1 after the effects, 2+p only on what port p sends. mask = channels left unfiltered. */
typedef struct {
    int used, dirty;
    uint32_t location, type, mask;
    float fc, q, level, direct[5];
    float c[5];
    float z[MAX_CH][4];      /* x1, x2, y1, y2 */
} Biquad;

/* Compressor (submixer 0x20000002) and the mastering limiter: block peak/RMS detector (32
 * samples), static curve thr + (x - thr) * ratio, per-sample attack/release smoothing. */
typedef struct {
    int enabled, rms, link, dirty;
    float threshold, ratio, knee, attack, release, level;
    float attack_coef, release_coef;
    float gain[MAX_CH];
} Dynamics;

/* I3DL2 reverb (0x20010001): early reflections from a pattern of taps, 16 damped combs mixed
 * per output channel and decorrelated by 4 allpasses, room HF low-pass, wet/dry. */
#define REVERB_COMBS 16
#define REVERB_ALLPASS 4
typedef struct {
    float *line; uint32_t length, index;
    float gain, lp_in, lp_state, damp_a, damp_b, shelf_a, shelf_b;
    float fir_a, fir_b, x1;
} Comb;
typedef struct { float *line; uint32_t length, index; } Allpass;
typedef struct {
    int dirty;
    float wet, dry, room, room_hf, decay_time, decay_hf_ratio, reflections, reflections_delay,
          reverb, reverb_delay, diffusion, density, hf_reference;
    uint32_t pattern;
    float *er_line; uint32_t er_length, er_index;
    uint32_t tap_delay[5]; float tap_gain[5];
    float er_gain, late_gain, ap_coef, hf_k;
    Comb comb[REVERB_COMBS];
    Allpass ap[MAX_CH][REVERB_ALLPASS];
    float hf_z[MAX_CH][2];
    float *work;             /* mono, ER, late input, 16 comb outputs: (3 + 16) * MAX_GRAIN */
} Reverb;

/* Custom rack modules: chorus (0x40001c00) and multi-tap delay (0x40001d00). */
#define CHORUS_LINE 4096
typedef struct {
    int used;
    uint32_t mask, phases;
    float gain, delay_ms, rate_hz, depth, feedback, dry, wet;
    float phase;
    uint32_t write;
    float *line;             /* MAX_CH * 4 phases * CHORUS_LINE */
} Chorus;
typedef struct {
    int used;
    float dry, wet, gain, feedback, lowpass_hz;
    uint32_t taps, flags, mask, type;
    float tap_level[8], tap_ms[8];
    float in_gain[MAX_CH];
    uint32_t tap_delay[8];
    float lp_coef, lp_state[MAX_CH];
    float *line; uint32_t length, write;
} Delay;

typedef struct {
    /* Waveform */
    uint32_t wave, wave_channels, rate, config;
    float pitch;
    Block blocks[MAX_BLOCKS];
    uint32_t head, count;
    /* Current block */
    int started;
    uint32_t byte, out_left, skip_left, repeated;
    uint32_t ends_without_output; /* sotc: block ends with no frame in between (see block_end) */
    /* Decoders */
    float hist[MAX_CH][4];
    void *at9;
    Atrac9CodecInfo at9_info;
    uint32_t at9_frame_in_superframe;
    int at9_ok;
    /* sotc: the superframe being decoded. Streamed sounds come in blocks that are not whole
     * superframes (64 KiB blocks of 960-byte superframes): one superframe starts at the end of a
     * block and ends in the next, so it is put together here; reset with the decoder. */
    uint32_t at9_sf_fill, at9_sf_pos;
    uint8_t *at9_sf;
    /* Decoded frames waiting for the resampler (interleaved, MAX_CH stride) */
    float *fifo;
    uint32_t fifo_len;
    double pos;
    int source_done, empty_renders;
    /* Envelope */
    EnvPoint env[MAX_ENV];
    uint32_t env_forward, env_release, env_segment, env_position;
    float env_start, env_level;
    int releasing;
    int fade_in, stopping;   /* 64-sample fades on PLAY and STOP/KILL */
    /* State reported to the game */
    uint64_t decoded_samples, decoded_bytes, user;
    const void *waveform_pointer;
} Sampler;

struct Voice {
    uint32_t magic;
    Rack *rack;
    uint32_t index;
    uint32_t state;
    int paused;
    uint32_t channels;
    Port ports[MAX_PORTS];
    Matrix *matrices;
    float peak;
    Biquad filters[MAX_FILTERS];
    UserFxHandler user_fx;
    uintptr_t user_fx_data[3];
    CallbackHandler callback;
    uintptr_t callback_data;
    uint32_t callback_flags;
    Sampler *sampler;
    /* Buses */
    float *input;           /* MAX_CH planar channels of MAX_GRAIN */
    int has_input;
    int fed;                /* something was mixed into the input this render */
    Dynamics compressor;
    Reverb *reverb;
    Chorus *chorus;
    Delay *delay;
    uint32_t output_id, out_channels;
    /* Mastering: LFE low-pass on channel 3, limiter, per-channel gain */
    int lfe_enabled; uint32_t lfe_fc; float lfe_c[2][5], lfe_z[2][4];
    Dynamics limiter;
    int gain_set; float gain[MAX_CH], gain_last[MAX_CH];
    int stopped;            /* bus: RELEASE/STOP/KILL received (buses never kicked still run) */
    /* Render order */
    int pending, visited;
};

struct Rack {
    uint32_t magic;          /* first: the rack handle is also voice 0's handle */
    System *system;
    uint32_t id, kind, max_voices, max_ports, max_matrices;
    Voice **voices;
    BufferInfo buffer;
    Allocator allocator;
    int allocated;
    Rack *next;
};

struct System {
    uint32_t magic;
    uint32_t grain, max_grain, rate;
    Rack *racks;
    BufferInfo buffer;
    Allocator allocator;
    int allocated;
    uint64_t renders;
    System *next;
};

static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static System *systems;
static FILE *wav;
static uint64_t wav_frames;
static uint32_t wav_channels;
static uint64_t stat_plays, stat_finished, stat_blocks, stat_errors, stat_user_fx, stat_starved, stat_underruns;
static double stat_render_ns, stat_render_max_ns;
static uint64_t stat_render_count;

static const float hevag_coefs[128][4] = {
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.9375f, 0.0f, 0.0f, 0.0f},
    {1.796875f, -0.8125f, 0.0f, 0.0f},
    {1.53125f, -0.859375f, 0.0f, 0.0f},
    {1.90625f, -0.9375f, 0.0f, 0.0f},
    {1.7982178f, -0.86169434f, 0.0f, 0.0f},
    {1.770874f, -0.89916992f, 0.0f, 0.0f},
    {1.6992188f, -0.91821289f, 0.0f, 0.0f},
    {1.6031494f, -0.9375f, 0.0f, 0.0f},
    {1.4682617f, -0.9375f, 0.0f, 0.0f},
    {1.3139648f, -0.9375f, 0.0f, 0.0f},
    {1.1424561f, -0.9375f, 0.0f, 0.0f},
    {0.95605469f, -0.9375f, 0.0f, 0.0f},
    {0.75695801f, -0.9375f, 0.0f, 0.0f},
    {0.54785156f, -0.9375f, 0.0f, 0.0f},
    {0.33166504f, -0.9375f, 0.0f, 0.0f},
    {0.11108398f, -0.9375f, 0.0f, 0.0f},
    {-0.11108398f, -0.9375f, 0.0f, 0.0f},
    {-0.33166504f, -0.9375f, 0.0f, 0.0f},
    {-0.54785156f, -0.9375f, 0.0f, 0.0f},
    {-0.75695801f, -0.9375f, 0.0f, 0.0f},
    {-0.95605469f, -0.9375f, 0.0f, 0.0f},
    {-1.1424561f, -0.9375f, 0.0f, 0.0f},
    {-1.3139648f, -0.9375f, 0.0f, 0.0f},
    {-1.4682617f, -0.9375f, 0.0f, 0.0f},
    {-1.6031494f, -0.9375f, 0.0f, 0.0f},
    {-1.6992188f, -0.91821289f, 0.0f, 0.0f},
    {-1.770874f, -0.89916992f, 0.0f, 0.0f},
    {-1.7982178f, -0.86169434f, 0.0f, 0.0f},
    {0.65625f, -1.125f, 0.40625f, -0.375f},
    {-0.78125f, -0.875f, -0.40625f, -0.28125f},
    {-1.28125f, -0.90625f, -0.4375f, -0.125f},
    {-0.020385742f, -0.33227539f, -0.060302734f, -0.066040039f},
    {-0.90698242f, -0.27111816f, -0.28051758f, 0.051757812f},
    {-0.97668457f, -0.38647461f, -0.34350586f, 0.03527832f},
    {0.73461914f, -0.57983398f, 0.32336426f, -0.15844727f},
    {0.46362305f, -0.84790039f, 0.47302246f, -0.1484375f},
    {-1.0054932f, -0.31689453f, -0.25280762f, 0.027709961f},
    {1.1229248f, 0.24194336f, -0.16870117f, -0.28271484f},
    {1.5894775f, -0.37158203f, -0.46289062f, 0.15466309f},
    {1.6005859f, -0.54772949f, -0.2746582f, 0.20324707f},
    {-0.20361328f, -0.45703125f, -0.78808594f, 0.10253906f},
    {0.95446777f, -0.52832031f, 0.25769043f, -0.061767578f},
    {1.168335f, -0.16308594f, -0.092407227f, 0.059448242f},
    {1.2246094f, -0.31274414f, 0.036621094f, 0.024291992f},
    {-0.57922363f, -0.50317383f, -0.66967773f, -0.18225098f},
    {-0.71972656f, 0.2902832f, -0.58435059f, -0.84802246f},
    {-0.14562988f, -1.112915f, -0.15100098f, -0.38012695f},
    {0.33972168f, -0.86767578f, -0.19226074f, -0.17663574f},
    {-0.89526367f, -0.25170898f, -0.27001953f, 0.054443359f},
    {0.7479248f, -0.3145752f, -0.038452148f, -0.0021972656f},
    {1.1544189f, -0.22680664f, 0.012451172f, 0.031494141f},
    {0.96142578f, -0.54724121f, 0.25952148f, -0.065673828f},
    {-0.87548828f, -0.21911621f, -0.25256348f, 0.058837891f},
    {-0.89819336f, -0.2565918f, -0.27258301f, 0.053710938f},
    {-1.1193848f, -0.42834473f, -0.32641602f, -0.047729492f},
    {-0.32202148f, -0.32312012f, -0.23547363f, -0.1998291f},
    {0.2286377f, 1.1209717f, 0.22705078f, -0.70141602f},
    {1.1247559f, 0.22692871f, -0.13720703f, -0.29626465f},
    {1.6118164f, -0.36767578f, -0.50524902f, 0.16723633f},
    {1.5181885f, -0.58496094f, -0.03125f, 0.075927734f},
    {-0.32385254f, -0.13964844f, -0.38842773f, -0.83959961f},
    {1.1390381f, -0.12792969f, -0.10107422f, 0.061889648f},
    {0.20043945f, -0.075683594f, -0.11547852f, -0.51623535f},
    {0.51831055f, -0.92590332f, -0.065063477f, -0.27575684f},
    {-1.097168f, -0.47497559f, -0.34265137f, 0.0053710938f},
    {-0.31274414f, -0.3338623f, -0.21118164f, -0.23181152f},
    {0.38842773f, -0.058959961f, -0.087158203f, -0.17346191f},
    {0.96887207f, -0.46923828f, 0.34436035f, -0.12438965f},
    {1.229126f, -0.31848145f, 0.038330078f, 0.023803711f},
    {1.0253906f, -0.40246582f, 0.18933105f, -0.018920898f},
    {-1.0411377f, -0.33874512f, -0.296875f, -0.041015625f},
    {1.1568604f, -0.22973633f, 0.013183594f, 0.03125f},
    {0.0091552734f, -0.27355957f, -0.036376953f, -0.84680176f},
    {-1.1160889f, -0.5078125f, -0.36169434f, 0.00061035156f},
    {-0.88745117f, -0.23901367f, -0.26318359f, 0.056152344f},
    {-0.33447266f, 0.45715332f, 0.72460938f, -0.13293457f},
    {1.0977783f, 0.23779297f, -0.083374023f, -0.33007812f},
    {1.5992432f, -0.34606934f, -0.47045898f, 0.12878418f},
    {1.164917f, -0.23937988f, 0.015869141f, 0.030517578f},
    {0.64355469f, -0.52124023f, 0.38134766f, -0.38537598f},
    {-0.93945312f, -0.41296387f, -0.3548584f, -0.055664062f},
    {0.89221191f, 0.3079834f, 0.052978516f, -0.30041504f},
    {1.2542725f, -0.34997559f, 0.047729492f, 0.020996094f},
    {1.3354492f, -0.45422363f, 0.081176758f, 0.01184082f},
    {0.0029296875f, -0.037841797f, -0.15405273f, 0.0390625f},
    {-0.99145508f, -0.29431152f, -0.28210449f, -0.033081055f},
    {-1.0389404f, -0.37438965f, -0.28527832f, 0.019897461f},
    {0.039794922f, -0.46948242f, 0.051147461f, -0.1138916f},
    {1.0858154f, 0.26782227f, -0.066040039f, -0.3515625f},
    {1.4737549f, -0.22900391f, -0.24621582f, -0.073364258f},
    {1.0655518f, -0.41784668f, 0.2043457f, -0.020629883f},
    {1.5808105f, -0.46960449f, -0.36706543f, 0.23754883f},
    {1.2253418f, -0.3137207f, 0.036865234f, 0.024169922f},
    {1.1456299f, -0.33654785f, 0.12304688f, 0.0050048828f},
    {-0.57617188f, -0.61108398f, -0.34814453f, -0.14172363f},
    {0.96057129f, -0.52807617f, 0.26062012f, -0.061157227f},
    {0.29907227f, -1.0494385f, 0.15856934f, -0.33935547f},
    {1.2441406f, -0.33728027f, 0.043945312f, 0.022094727f},
    {1.3809814f, -0.51428223f, 0.10168457f, 0.0064697266f},
    {1.239502f, -0.33154297f, 0.042114258f, 0.022583008f},
    {1.1765137f, -0.17297363f, -0.08996582f, 0.058837891f},
    {0.47045898f, -0.5559082f, 0.3470459f, -0.41467285f},
    {0.81774902f, -0.6907959f, 0.27453613f, -0.13110352f},
    {1.3527832f, -0.47705078f, 0.088867188f, 0.009765625f},
    {-0.12524414f, -1.1975098f, -0.098266602f, -0.42260742f},
    {1.269043f, -0.45727539f, 0.16687012f, -0.01171875f},
    {1.2557373f, 0.12060547f, -0.23376465f, -0.17541504f},
    {0.9708252f, 0.47338867f, -0.093261719f, -0.39831543f},
    {1.5489502f, -0.4119873f, -0.40942383f, 0.25378418f},
    {0.81066895f, 0.38647461f, 0.028198242f, -0.25500488f},
    {-0.28662109f, -0.89770508f, -0.23730469f, -0.50317383f},
    {1.1340332f, -0.49304199f, 0.23010254f, -0.030029297f},
    {0.56555176f, -0.78161621f, 0.21337891f, -0.19763184f},
    {1.3729248f, -0.50354004f, 0.097900391f, 0.0074462891f},
    {1.1971436f, -0.27880859f, 0.026733398f, 0.027099609f},
    {1.1884766f, -0.1875f, -0.086181641f, 0.057739258f},
    {1.0302734f, -0.41943359f, 0.19067383f, -0.021484375f},
    {1.1361084f, -0.12463379f, -0.10192871f, 0.062133789f},
    {0.20727539f, -1.1016846f, 0.083984375f, -0.37072754f},
    {1.2468262f, -0.34069824f, 0.044921875f, 0.021850586f},
    {1.0241699f, 0.39648438f, -0.092529297f, -0.36486816f},
    {0.87902832f, 0.40478516f, 0.0056152344f, -0.3190918f},
    {-0.010742188f, -0.95324707f, -0.065673828f, -0.5579834f},
    {0.75598145f, -0.63342285f, 0.33691406f, -0.15197754f},
    {1.5045166f, -0.1574707f, -0.40087891f, 0.030883789f},
    {1.5947266f, -0.49743652f, -0.34472656f, 0.22912598f},
    {0.65100098f, 0.36608887f, 0.094604492f, -0.13818359f},
};

/* ---- Configuration and logging ---- */
static const char *env(const char *name) {
    char key[64];
    snprintf(key, sizeof(key), "SOTC_%s", name); /* sotc */
    const char *value = getenv(key);
    if (value) return value;
    snprintf(key, sizeof(key), "TLG_%s", name);
    return getenv(key);
}
static int env_flag(const char *name, int fallback) {
    const char *v = env(name);
    return v && *v ? strcmp(v, "0") != 0 : fallback;
}
static int log_enabled(void) {
    static int value = -1;
    if (value < 0) value = env_flag("NGS2_LOG", 0);
    return value;
}
static void note(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *format, ...) {
    if (!log_enabled()) return;
    va_list args;
    va_start(args, format);
    fputs("NGS2: ", stdout);
    vprintf(format, args);
    fputc('\n', stdout);
    va_end(args);
}
/* Unknown parameter ids are logged once per (rack id, param id). */
static void note_unknown(uint32_t rack, uint32_t id, uint32_t size) {
    static uint64_t seen[128];
    static unsigned count;
    uint64_t key = (uint64_t)rack << 32 | id;
    for (unsigned i = 0; i < count; ++i) if (seen[i] == key) return;
    if (count < 128) seen[count++] = key;
    note("rack 0x%04x: parameter 0x%08x (%u bytes) stored but not processed", rack, id, size);
}

/* ---- DSP modules ----
 * Behaviour follows the console module's processing (studied in the firmware binary): the same
 * filter designs, curves, block sizes and parameter clamps, written anew here. */
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static float mb_to_gain(float mb) { return mb < -10000.0f ? 0.0f : powf(10.0f, mb / 2000.0f); }

/* Biquad coefficients c = {b0, b1, b2, -a1, -a2} / a0 (RBJ designs; 0x20 = direct coefficients). */
static void biquad_design(Biquad *f, uint32_t rate) {
    float *c = f->c;
    f->dirty = 0;
    if (f->type == 0x20) { memcpy(c, f->direct, sizeof(f->direct)); return; }
    if (f->type < 1 || f->type > 10) { c[0] = 1.0f; c[1] = c[2] = c[3] = c[4] = 0.0f; return; }
    float x = f->fc / (float)rate;
    float w = x < 0.001f ? 0.0062831853f : fminf(x, 0.49987f) * 6.2831855f;
    float q = f->q < 0.0001f ? 0.0001f : fminf(f->q, 32.0f);
    float s = sinf(w), cs = cosf(w);
    float b0, b1, b2, a0, a1, a2;
    switch (f->type) {
    case 6: case 7: {   /* low / high shelf, level = linear gain */
        float A = sqrtf(fmaxf(f->level, 0.0001f)), beta = sqrtf(A) / q * s;
        if (f->type == 6) {
            b0 = A * ((A + 1) - (A - 1) * cs + beta); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - beta);
            a0 = (A + 1) + (A - 1) * cs + beta; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - beta;
        } else {
            b0 = A * ((A + 1) + (A - 1) * cs + beta); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - beta);
            a0 = (A + 1) - (A - 1) * cs + beta; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - beta;
        }
        break;
    }
    case 8: {           /* peaking EQ */
        float A = sqrtf(fmaxf(f->level, 0.0001f)), alpha = s / (2 * q);
        b0 = 1 + alpha * A; b1 = -2 * cs; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cs; a2 = 1 - alpha / A;
        break;
    }
    case 9:             /* first-order low-pass */
        b0 = b1 = s; b2 = 0; a0 = s + cs + 1; a1 = s - cs - 1; a2 = 0;
        break;
    case 10:            /* first-order high-pass */
        b0 = cs + 1; b1 = -(cs + 1); b2 = 0; a0 = s + cs + 1; a1 = s - (cs + 1); a2 = 0;
        break;
    default: {          /* 1 LPF, 2 HPF, 3 BPF (skirt), 4 BPF (0 dB peak), 5 notch */
        float alpha = s / (2 * q);
        switch (f->type) {
        case 2: b0 = b2 = (1 + cs) * 0.5f; b1 = -(1 + cs); break;
        case 3: b0 = s * 0.5f; b1 = 0; b2 = -s * 0.5f; break;
        case 4: b0 = alpha; b1 = 0; b2 = -alpha; break;
        case 5: b0 = 1; b1 = -2 * cs; b2 = 1; break;
        default: b0 = b2 = (1 - cs) * 0.5f; b1 = 1 - cs; break;
        }
        a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
        break;
    }
    }
    c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = -a1 / a0; c[4] = -a2 / a0;
}
static void biquad_run(const float c[5], float z[4], const float *in, float *out, uint32_t n) {
    float x1 = z[0], x2 = z[1], y1 = z[2], y2 = z[3];
    for (uint32_t i = 0; i < n; ++i) {
        float x = in[i], y = c[0] * x + c[1] * x1 + c[2] * x2 + c[3] * y1 + c[4] * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        out[i] = y;
    }
    /* Flush denormals the recursion can reach in long silences. */
    if (fabsf(y1) < 1e-20f) y1 = 0.0f;
    if (fabsf(y2) < 1e-20f) y2 = 0.0f;
    z[0] = x1; z[1] = x2; z[2] = y1; z[3] = y2;
}
/* Runs the voice's filters whose location is `location` from `in` into `out` (may alias). */
static void filters_run(Voice *v, uint32_t location, float in[MAX_CH][MAX_GRAIN], float out[MAX_CH][MAX_GRAIN],
                        uint32_t channels, uint32_t n) {
    int copied = in == out;
    for (int i = 0; i < MAX_FILTERS; ++i) {
        Biquad *f = &v->filters[i];
        if (!f->used || f->location != location) continue;
        if (f->dirty) biquad_design(f, v->rack->system->rate);
        for (uint32_t c = 0; c < channels; ++c) {
            if (f->mask & (1u << c) || f->type == 0) { if (!copied) memcpy(out[c], in[c], n * sizeof(float)); continue; }
            biquad_run(f->c, f->z[c], copied ? out[c] : in[c], out[c], n);
        }
        copied = 1;
    }
    if (!copied) for (uint32_t c = 0; c < channels; ++c) memcpy(out[c], in[c], n * sizeof(float));
}
static int filters_at(const Voice *v, uint32_t location) {
    for (int i = 0; i < MAX_FILTERS; ++i) if (v->filters[i].used && v->filters[i].location == location) return 1;
    return 0;
}

/* Dynamics: game flags bit 0 = bypass, bit 1 = RMS detector, bit 3 = link all but LFE. */
static void dynamics_reset(Dynamics *d) { for (int c = 0; c < MAX_CH; ++c) d->gain[c] = 1.0f; }
static void dynamics_run(Dynamics *d, uint32_t rate, float data[MAX_CH][MAX_GRAIN], uint32_t channels, uint32_t n) {
    if (d->dirty) {
        uint32_t a = (uint32_t)((float)rate * d->attack), r = (uint32_t)((float)rate * d->release);
        d->attack_coef = a ? expf(-1.0f / (float)a) : 0.0f;
        d->release_coef = r ? expf(-1.0f / (float)r) : 0.0f;
        d->dirty = 0;
    }
    if (!d->enabled) { dynamics_reset(d); return; }
    for (uint32_t start = 0; start < n; start += 32) {
        uint32_t len = n - start < 32 ? n - start : 32;
        float level[MAX_CH] = {0};
        for (uint32_t c = 0; c < channels; ++c) {
            const float *x = data[c] + start;
            float m = 0.0f;
            if (d->rms) { for (uint32_t i = 0; i < len; ++i) m += x[i] * x[i]; m = sqrtf(m / (float)len); }
            else for (uint32_t i = 0; i < len; ++i) { float a = fabsf(x[i]); if (a > m) m = a; }
            level[c] = m;
        }
        float peak = 0.0f;
        for (uint32_t c = 0; c < channels; ++c) if ((!d->link || c != 3) && level[c] > peak) peak = level[c];
        for (uint32_t c = 0; c < channels; ++c) if (!d->link || c != 3) level[c] = peak;
        for (uint32_t c = 0; c < channels; ++c) {
            float det = level[c], target = 1.0f, thr = d->threshold;
            float knee_start = thr * (1.0f - d->knee * 0.5f), over = det - knee_start;
            if (d->knee > 0.0f && over > 0.0f && over < thr - knee_start)
                target = (knee_start + over * ((over / (thr - knee_start)) * 0.5f * (d->ratio - 1.0f) + 1.0f)) / det;
            else if (thr < det) target = (thr + (det - thr) * d->ratio) / det;
            float g = d->gain[c], coef = g <= target ? d->release_coef : d->attack_coef;
            float *x = data[c] + start;
            for (uint32_t i = 0; i < len; ++i) { g = target + coef * (g - target); x[i] *= d->level * g; }
            d->gain[c] = g;
        }
    }
}
static void dynamics_set(Dynamics *d, uint32_t flags, const float values[6]) {
    d->enabled = !(flags & 1);
    d->rms = (flags >> 1) & 1;
    d->link = (flags >> 3) & 1;
    d->threshold = clampf(values[0], 0.0f, 1.0f);
    d->ratio = clampf(values[1], 0.0f, 1.0f);
    d->knee = clampf(values[2], 0.0f, 1.0f);
    d->attack = clampf(values[3], 0.0f, 10.0f);
    d->release = clampf(values[4], 0.0f, 10.0f);
    d->level = clampf(values[5], 0.0f, 32.0f);
    d->dirty = 1;
}

/* Mastering LFE low-pass: 4th-order 0.1 dB Chebyshev (two sections), bilinear transform. */
static void lfe_design(Voice *v, uint32_t rate) {
    static const float pole_ratio[2] = {0.7891f, 1.1532f}, pole_q[2] = {0.61866f, 2.18276f};
    float k = 2.0f * (float)rate, gain = 0.98855f;
    for (int s = 0; s < 2; ++s) {
        float w = 6.2831855f * (float)v->lfe_fc * pole_ratio[s], w2 = w * w, wq = w / pole_q[s];
        float a0 = k * k + wq * k + w2, a1 = 2 * w2 - 2 * k * k, a2 = k * k - wq * k + w2;
        float g = s ? w2 / a0 * gain : w2 / a0;
        float *c = v->lfe_c[s];
        c[0] = g; c[1] = 2 * g; c[2] = g; c[3] = -a1 / a0; c[4] = -a2 / a0;
    }
}

/* Envelope curves (x in 0..1 -> 0..1 of the way from the start to the target height). */
static float envelope_curve(uint32_t curve, float x, int rising) {
    switch (curve) {
    case 0: return x;
    case 1: return (powf(10.0f, x) - 1.0f) / 9.0f;
    case 2: return log10f(1.0f + 9.0f * x);
    case 3: return 0.5f - 0.5f * cosf(3.14159265f * x);
    case 4: return 0.5f + asinf(2.0f * x - 1.0f) / 3.14159265f;
    case 5: return rising ? sinf(1.5707963f * x) : 1.0f - cosf(1.5707963f * x);
    default: return 0.0f;      /* unknown curve: the level holds */
    }
}

/* ---- Reverb ---- */
static const float er_span[8] = {35.0f, 38.03f, 29.0f, 53.0f, 30.0f, 55.0f, 40.0f, 62.0f};
static const float er_time[8][3] = {{11, 23, 35}, {14.75f, 26.55f, 38.03f}, {6, 17, 29}, {8, 20, 53},
                                    {11, 17, 30}, {16, 30, 55}, {20, 29, 40}, {37, 47, 62}};
static const float er_gain[8][3] = {{0.4f, 0.134f, 0.1f}, {0.4f, 0.3f, 0.1f}, {0.45f, -0.6f, 0.2f}, {0.55f, -0.6f, 0.2f},
                                    {-0.45f, 0.6f, 0.3f}, {0.7f, 0.7f, 0.4f}, {0.7f, -0.4f, 0.3f}, {0.7f, -0.4f, 0.4f}};
static const float comb_ms[REVERB_COMBS] = {22.01f, 23.25f, 24.75f, 26.6f, 28.25f, 29.63f, 31.06f, 32.44f,
                                            33.69f, 34.95f, 36.29f, 37.65f, 38.99f, 40.35f, 41.67f, 43.05f};
static const float allpass_ms[REVERB_ALLPASS] = {11.58f, 9.19f, 7.1f, 4.69f};
static const int allpass_offset[MAX_CH][REVERB_ALLPASS] = {{0, -32, -6, -11}, {-10, -5, 0, -34}, {-8, -3, -33, -5}, {-7, -9, -5, -8},
                                                          {-33, -8, -8, 0}, {-11, -21, -10, -7}, {-9, -10, -27, -3}, {-6, -11, -9, -24}};
static const uint16_t comb_mask[MAX_CH] = {0xaaaa, 0x5555, 0xcccc, 0x3333, 0x0ff0, 0x9ab2, 0x3c3c, 0x399c};

static Reverb *reverb_new(uint32_t rate) {
    Reverb *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->er_length = rate / 10;
    size_t floats = r->er_length + (3 + REVERB_COMBS) * MAX_GRAIN;
    uint32_t comb_max = (uint32_t)((float)rate * 43.05f / 1000.0f) + 8;
    floats += (size_t)comb_max * REVERB_COMBS;
    uint32_t ap_len[REVERB_ALLPASS];
    for (int i = 0; i < REVERB_ALLPASS; ++i) { ap_len[i] = (uint32_t)((float)rate * allpass_ms[i] * 0.001f + 0.5f); floats += (size_t)ap_len[i] * MAX_CH; }
    float *mem = calloc(floats, sizeof(float));
    if (!mem) { free(r); return NULL; }
    r->er_line = mem; mem += r->er_length;
    r->work = mem; mem += (3 + REVERB_COMBS) * MAX_GRAIN;
    for (int k = 0; k < REVERB_COMBS; ++k) { r->comb[k].line = mem; r->comb[k].length = comb_max; mem += comb_max; }
    for (int c = 0; c < MAX_CH; ++c)
        for (int i = 0; i < REVERB_ALLPASS; ++i) {
            int len = (int)ap_len[i] + allpass_offset[c][i];
            r->ap[c][i].line = mem; r->ap[c][i].length = len > 1 ? (uint32_t)len : 1; mem += ap_len[i];
        }
    /* Defaults the console module sets up: silent I3DL2 preset, wet 1, dry 0. */
    r->wet = 1.0f; r->dry = 0.0f; r->room = 0.0f; r->room_hf = 1.0f; r->decay_time = 1.0f; r->decay_hf_ratio = 0.5f;
    r->reflections = 0.0f; r->reflections_delay = 0.02f; r->reverb = 0.0f; r->reverb_delay = 0.04f;
    r->diffusion = 100.0f; r->density = 100.0f; r->hf_reference = 5000.0f;
    r->dirty = 1;
    return r;
}
static void reverb_free(Reverb *r) { if (r) { free(r->er_line); free(r); } }

static void reverb_design(Reverb *r, uint32_t rate) {
    r->dirty = 0;
    /* Early reflections: tap 0 at reflectionsDelay, three pattern taps, tap 4 feeds the combs. */
    uint32_t p = r->pattern % 8, len = r->er_length;
    uint32_t first = (uint32_t)(r->reflections_delay * (float)rate + 0.5f);
    uint32_t late = (uint32_t)((r->reflections_delay + r->reverb_delay) * (float)rate + 0.5f);
    if (first > len) first = len;
    if (late > len || r->pattern >= 8) late = len;
    uint32_t span = (uint32_t)((float)rate * er_span[p] * 0.001f + 0.5f);
    float scale = late < first + span && span ? (float)(late - first) / (float)span : 1.0f;
    r->tap_delay[0] = first; r->tap_gain[0] = 0.7f;
    uint32_t previous = first;
    for (int i = 0; i < 3; ++i) {
        uint32_t d = first + (uint32_t)(scale * er_time[p][i] * (float)rate * 0.001f + 0.5f);
        r->tap_delay[i + 1] = d;
        r->tap_gain[i + 1] = d != previous ? er_gain[p][i] : 0.0f;
        previous = d;
    }
    uint32_t d4 = first + (uint32_t)(r->reverb_delay * (float)rate + 0.5f);
    r->tap_delay[4] = d4 > len ? len : d4; r->tap_gain[4] = 1.0f;
    for (int i = 0; i < 5; ++i) if (r->tap_delay[i] >= len) r->tap_delay[i] = len - 1;
    r->er_gain = r->reflections * r->room;
    r->late_gain = r->reverb * r->room;
    r->ap_coef = r->diffusion * 0.005f;
    /* Combs: length from the table scaled by density (odd), feedback for decayTime, HF damping. */
    float ratio = r->decay_hf_ratio;
    for (int k = 0; k < REVERB_COMBS; ++k) {
        Comb *cb = &r->comb[k];
        uint32_t l = (uint32_t)((float)rate * comb_ms[k] / (r->density * -3.0f / 100.0f + 4.0f) * 0.001f + 0.5f);
        l += ~l & 1;
        uint32_t max = (uint32_t)((float)rate * 43.05f / 1000.0f) + 8;
        if (l > max) l = max;
        if (l != cb->length) { cb->length = l; if (cb->index >= l) cb->index = 0; }
        float g = powf(10.0f, (float)l / (float)rate / r->decay_time * -3.0f);
        cb->gain = g;
        if (ratio > 1.0f) {
            float f = powf(10.0f, (ratio - 1.0f) * log10f(g));
            cb->damp_a = 0.0506f; cb->damp_b = 0.9494f; cb->shelf_a = 1.0f; cb->shelf_b = 1.0f - f;
        } else {
            float f = ratio < 0.8f ? (0.8f - ratio) * -0.8571429f + 0.8f : ratio;
            f *= f;
            float damp = fminf(0.99978f, log10f(g) * 2.3025851f * 0.25f * (1.0f - 1.0f / f));
            cb->damp_a = 1.0f - damp; cb->damp_b = damp; cb->shelf_a = 0.0f; cb->shelf_b = 1.0f;
        }
        float s = sqrtf(ratio), t = (1.0f - s) / (s + 1.0f);
        cb->fir_a = t / (1.0f - t) + 1.0f;
        cb->fir_b = -t / (1.0f - t);
    }
    /* Room HF: two one-pole low-passes at HFReference reaching roomHF there. */
    r->hf_k = 0.0f;
    if (r->room_hf < 1.0f) {
        float g = r->room_hf, cs = cosf(r->hf_reference * 6.2831855f / (float)rate);
        float a = sqrtf(fmaxf(0.0f, (g + g) * (1.0f - cs) - g * g * (1.0f - cs * cs)));
        r->hf_k = ((1.0f - g * cs) - a) / (1.0f - g);
    }
}
static void reverb_set_i3dl2(Reverb *r, const ParamHeader *p, float (*f)(const ParamHeader *, uint32_t), uint32_t (*u)(const ParamHeader *, uint32_t)) {
    r->wet = clampf(f(p, 8), 0.0f, 1.0f);
    r->dry = clampf(f(p, 12), 0.0f, 1.0f);
    int32_t room = (int32_t)u(p, 16), room_hf = (int32_t)u(p, 20), refl = (int32_t)u(p, 36), rev = (int32_t)u(p, 44);
    r->room = fminf(1.0f, mb_to_gain((float)room));
    r->room_hf = mb_to_gain((float)(room_hf > 0 ? 0 : room_hf < -10000 ? -10000 : room_hf));
    r->pattern = u(p, 24) < 16 ? u(p, 24) : 0;
    r->decay_time = clampf(f(p, 28), 0.1f, 20.0f);
    r->decay_hf_ratio = clampf(f(p, 32), 0.01f, 2.0f);
    r->reflections = mb_to_gain((float)(refl > 1000 ? 1000 : refl < -10000 ? -10000 : refl));
    r->reflections_delay = clampf(f(p, 40), 0.0f, 0.3f);
    r->reverb = mb_to_gain((float)((rev > 2000 ? 2000 : rev < -10000 ? -10000 : rev) - 2400));
    r->reverb_delay = clampf(f(p, 48), 0.0f, 0.1f);
    r->diffusion = clampf(f(p, 52), 0.0f, 100.0f);
    r->density = clampf(f(p, 56), 0.0f, 100.0f);
    r->hf_reference = clampf(f(p, 60), 20.0f, 20000.0f);
    r->dirty = 1;
}
static void reverb_run(Reverb *r, uint32_t rate, float data[MAX_CH][MAX_GRAIN], uint32_t in_channels, uint32_t out_channels, uint32_t n) {
    if (r->dirty) reverb_design(r, rate);
    float *mono = r->work, *er = mono + MAX_GRAIN, *late_in = er + MAX_GRAIN, *combs = late_in + MAX_GRAIN;
    /* Mono downmix without the LFE. */
    float dg = in_channels >= 8 ? 0.267f : in_channels >= 6 ? 0.31623f : in_channels >= 2 ? 0.70711f : 1.0f;
    memset(mono, 0, n * sizeof(float));
    for (uint32_t c = 0; c < in_channels; ++c) {
        if (in_channels > 1 && c == 3 && in_channels >= 6) continue;
        for (uint32_t i = 0; i < n; ++i) mono[i] += data[c][i] * (in_channels == 1 ? 1.0f : dg);
    }
    /* Early reflection taps and the late input from one delay line. */
    for (uint32_t i = 0; i < n; ++i) {
        r->er_line[r->er_index] = mono[i];
        float e = 0.0f;
        for (int t = 0; t < 4; ++t) e += r->tap_gain[t] * r->er_line[(r->er_index + r->er_length - r->tap_delay[t]) % r->er_length];
        er[i] = e;
        late_in[i] = r->er_line[(r->er_index + r->er_length - r->tap_delay[4]) % r->er_length];
        if (++r->er_index == r->er_length) r->er_index = 0;
    }
    /* Combs. */
    for (int k = 0; k < REVERB_COMBS; ++k) {
        Comb *cb = &r->comb[k];
        float *out = combs + (size_t)k * MAX_GRAIN;
        for (uint32_t i = 0; i < n; ++i) {
            float x = cb->fir_a * late_in[i] + cb->fir_b * cb->x1;
            cb->x1 = late_in[i];
            float d = cb->line[cb->index];
            cb->lp_state = d * cb->damp_a + cb->lp_state * cb->damp_b;
            float y = (d * cb->shelf_a + cb->lp_state * cb->shelf_b) * cb->gain;
            cb->line[cb->index] = x + y;
            if (++cb->index >= cb->length) cb->index = 0;
            out[i] = y;
        }
        if (fabsf(cb->lp_state) < 1e-20f) cb->lp_state = 0.0f;
    }
    /* Per output channel: comb mix, 4 allpasses, ER + late, room HF, wet + dry. */
    float rev[MAX_GRAIN];
    for (uint32_t c = 0; c < out_channels; ++c) {
        if (c == 3 && out_channels >= 6) {
            float d = r->dry;
            for (uint32_t i = 0; i < n; ++i) data[c][i] = (c < in_channels ? data[c][i] : 0.0f) * d;
            continue;
        }
        memset(rev, 0, n * sizeof(float));
        uint32_t j = 0;
        for (int k = 0; k < REVERB_COMBS; ++k) {
            if (!(comb_mask[c] >> k & 1)) continue;
            float g = (j & 1) ? 1.0f : 1.0f / ((float)j * 0.5f * 20.0f + 40.0f) * (r->diffusion - 100.0f) + 1.0f;
            const float *o = combs + (size_t)k * MAX_GRAIN;
            for (uint32_t i = 0; i < n; ++i) rev[i] += o[i] * g;
            ++j;
        }
        for (int a = 0; a < REVERB_ALLPASS; ++a) {
            Allpass *ap = &r->ap[c][a];
            for (uint32_t i = 0; i < n; ++i) {
                float d = ap->line[ap->index], w = rev[i] + r->ap_coef * d;
                ap->line[ap->index] = w;
                rev[i] = d - r->ap_coef * w;
                if (++ap->index >= ap->length) ap->index = 0;
            }
        }
        float *z = r->hf_z[c], k = r->hf_k, b = (1.0f - k) * (1.0f - k);
        for (uint32_t i = 0; i < n; ++i) {
            float y = r->er_gain * er[i] + r->late_gain * rev[i];
            if (k != 0.0f) { y = b * y + 2.0f * k * z[0] - k * k * z[1]; z[1] = z[0]; z[0] = y; }
            data[c][i] = r->wet * y + r->dry * (c < in_channels ? data[c][i] : 0.0f);
        }
    }
}

/* ---- Chorus and delay (custom rack modules) ---- */
static void chorus_run(Chorus *ch, uint32_t rate, float data[MAX_CH][MAX_GRAIN], uint32_t channels, uint32_t n) {
    if (!ch->line) return;
    uint32_t phases = ch->phases < 1 ? 1 : ch->phases > 4 ? 4 : ch->phases;
    float base = (float)(int)(ch->delay_ms * 0.001f * (float)rate);
    float depth = base * ch->depth, step = ch->rate_hz / (float)rate;
    float dry = ch->gain * ch->dry, wet = ch->gain * ch->wet;
    float phase0 = ch->phase;
    uint32_t write0 = ch->write;
    for (uint32_t c = 0; c < channels; ++c) {
        if (ch->mask >> c & 1) continue;
        float phase = phase0;
        uint32_t w = write0;
        float *lines = ch->line + (size_t)c * 4 * CHORUS_LINE;
        for (uint32_t i = 0; i < n; ++i) {
            float x = data[c][i], in = x, v = 0.0f;
            for (uint32_t k = 0; k < phases; ++k) {
                float ph = phase + (float)k / (float)phases;
                ph -= floorf(ph);
                float d = base + 1.0f + depth * sinf(6.2831855f * ph);
                uint32_t di = (uint32_t)d;
                float fr = d - (float)di;
                float *line = lines + (size_t)k * CHORUS_LINE;
                v = fr * line[(w + di + 1) & (CHORUS_LINE - 1)] + (1.0f - fr) * line[(w + di) & (CHORUS_LINE - 1)];
                line[w] = in + ch->feedback * v;
                in = v;
            }
            data[c][i] = dry * x + wet * v;
            phase += step;
            w = (w - 1) & (CHORUS_LINE - 1);
        }
    }
    float p = phase0 + step * (float)n;
    ch->phase = p - floorf(p);
    ch->write = (write0 - n) & (CHORUS_LINE - 1);
}
static void delay_design(Delay *d, uint32_t rate, uint32_t grain) {
    uint32_t need = grain + 1;
    for (uint32_t t = 0; t < d->taps; ++t) {
        uint32_t s = (uint32_t)((float)rate * d->tap_ms[t] * 0.001f + 0.5f);
        if (s < grain) s = grain;
        d->tap_delay[t] = s;
        if (s + 1 > need) need = s + 1;
    }
    if (need > d->length) {
        float *line = calloc((size_t)need * MAX_CH, sizeof(float));
        if (line) { free(d->line); d->line = line; d->length = need; d->write = 0; }
    }
    float c = cosf(d->lowpass_hz * 6.2831855f / (float)rate), k = 2.0f - c;
    d->lp_coef = k - sqrtf(k * k - 1.0f);
}
static void delay_run(Delay *d, float data[MAX_CH][MAX_GRAIN], uint32_t channels, uint32_t n) {
    if (!d->line) return;
    float sum = 0.0f;
    for (uint32_t t = 0; t < d->taps; ++t) sum += fabsf(d->tap_level[t]);
    float fb = d->feedback / fmaxf(sum, 1.0f), dry = d->gain * d->dry, wet = d->gain * d->wet;
    float delayed[MAX_GRAIN], mono[MAX_GRAIN];
    uint32_t len = d->length;
    if (d->type == 0) {   /* one line fed by a downmix */
        memset(mono, 0, n * sizeof(float));
        for (uint32_t c = 0; c < channels; ++c) if (c != 3) for (uint32_t i = 0; i < n; ++i) mono[i] += data[c][i] * d->in_gain[c];
    }
    for (uint32_t c = 0; c < (d->type == 0 ? 1u : channels); ++c) {
        if (d->type != 0 && (c == 3 || (d->mask >> c & 1))) continue;
        float *line = d->line + (size_t)c * len;
        const float *in = d->type == 0 ? mono : data[c];
        float g_in = d->type == 0 ? 1.0f : d->in_gain[c];
        memset(delayed, 0, n * sizeof(float));
        for (uint32_t t = 0; t < d->taps; ++t) {
            if (d->tap_level[t] == 0.0f) continue;
            uint32_t r = (d->write + len - d->tap_delay[t]) % len;
            for (uint32_t i = 0; i < n; ++i) { delayed[i] += d->tap_level[t] * line[r]; if (++r == len) r = 0; }
        }
        if (d->flags & 1) {
            float k = d->lp_coef, s = d->lp_state[c];
            for (uint32_t i = 0; i < n; ++i) { s = (1.0f - k) * delayed[i] + k * s; delayed[i] = s; }
            d->lp_state[c] = fabsf(s) < 1e-20f ? 0.0f : s;
        }
        uint32_t w = d->write;
        for (uint32_t i = 0; i < n; ++i) { line[w] = g_in * in[i] + fb * delayed[i]; if (++w == len) w = 0; }
        if (d->type == 0) {
            for (uint32_t o = 0; o < channels; ++o) {
                if (o == 3 || (d->mask >> o & 1)) continue;
                for (uint32_t i = 0; i < n; ++i) data[o][i] = dry * mono[i] + wet * delayed[i];
            }
        } else for (uint32_t i = 0; i < n; ++i) data[c][i] = dry * data[c][i] + wet * delayed[i];
    }
    d->write = (d->write + n) % len;
}

/* ---- Objects and handles ---- */
static uint32_t rack_kind(uint32_t id) {
    switch (id) {
    case RACK_SAMPLER: return KIND_SAMPLER;
    case RACK_SUBMIXER: return KIND_SUBMIXER;
    case RACK_REVERB: return KIND_REVERB;
    case RACK_MASTERING: return KIND_MASTERING;
    default: return (id & 0xf000) == 0x4000 ? KIND_CUSTOM : KIND_SUBMIXER;
    }
}
static System *as_system(uintptr_t handle) {
    System *s = (System *)handle;
    return s && s->magic == MAGIC_SYSTEM ? s : NULL;
}
static Rack *as_rack(uintptr_t handle) {
    Rack *r = (Rack *)handle;
    return r && r->magic == MAGIC_RACK ? r : NULL;
}
/* A voice handle, or a rack handle meaning its voice 0. */
static Voice *as_voice(uintptr_t handle) {
    if (!handle) return NULL;
    uint32_t magic = *(const uint32_t *)handle;
    if (magic == MAGIC_VOICE) return (Voice *)handle;
    if (magic == MAGIC_RACK) { Rack *r = (Rack *)handle; return r->max_voices ? r->voices[0] : NULL; }
    return NULL;
}
static uintptr_t voice_handle(const Voice *v) { return v->index ? (uintptr_t)v : (uintptr_t)v->rack; }

static void sampler_reset_decoder(Sampler *s) {
    memset(s->hist, 0, sizeof(s->hist));
    s->at9_ok = 0;
    if (s->wave == WAVE_ATRAC9) {
        if (!s->at9) s->at9 = Atrac9GetHandle();
        else { Atrac9ReleaseHandle(s->at9); s->at9 = Atrac9GetHandle(); }
        unsigned char config[4];
        uint32_t c = s->config;
        if ((c & 0xff) == 0xfe) { config[0] = (unsigned char)c; config[1] = (unsigned char)(c >> 8); config[2] = (unsigned char)(c >> 16); config[3] = (unsigned char)(c >> 24); }
        else { config[0] = (unsigned char)(c >> 24); config[1] = (unsigned char)(c >> 16); config[2] = (unsigned char)(c >> 8); config[3] = (unsigned char)c; }
        if (s->at9 && !Atrac9InitDecoder(s->at9, config) && !Atrac9GetCodecInfo(s->at9, &s->at9_info) &&
            s->at9_info.channels > 0 && s->at9_info.channels <= MAX_CH && s->at9_info.frameSamples <= UNIT_FRAMES &&
            s->at9_info.superframeSize > 0 && s->at9_info.superframeSize <= AT9_SUPERFRAME_MAX &&
            s->at9_info.framesInSuperframe > 0 && (s->at9_sf || (s->at9_sf = calloc(1, AT9_SUPERFRAME_MAX + 64))))
            s->at9_ok = 1;
        else {
            ++stat_errors;
            note("ATRAC9 config %08x rejected", s->config);
        }
    }
    s->at9_frame_in_superframe = 0;
    s->at9_sf_fill = s->at9_sf_pos = 0;
}
static void sampler_clear_blocks(Sampler *s) {
    s->head = s->count = 0;
    s->started = 0;
    s->fifo_len = 1;   /* one silent frame of history before the first sample */
    if (s->fifo) memset(s->fifo, 0, MAX_CH * sizeof(float));
    s->pos = 1.0;
    s->source_done = 0;
    s->empty_renders = 0;
}

static void voice_init(Voice *v, Rack *r, uint32_t index) {
    v->magic = MAGIC_VOICE;
    v->rack = r;
    v->index = index;
    v->channels = r->kind == KIND_SAMPLER ? 1 : MAX_CH;
    for (int p = 0; p < MAX_PORTS; ++p) { v->ports[p].volume = 1.0f; v->ports[p].matrix = -1; }
    v->out_channels = MAX_CH;
    dynamics_reset(&v->compressor);
    v->compressor.dirty = 1;
    if (r->kind == KIND_MASTERING) {
        /* Mastering setup defaults of the console module: LFE low-pass at 120 Hz, limiter at
         * 1.0 (ratio 0, attack 1 ms, release 10 ms), no gain module until GAIN is set. */
        v->lfe_enabled = 1;
        v->lfe_fc = 120;
        lfe_design(v, r->system->rate);
        static const float limiter[6] = {1.0f, 0.0f, 0.0f, 0.001f, 0.01f, 1.0f};
        dynamics_set(&v->limiter, 0, limiter);
        dynamics_reset(&v->limiter);
        for (int c = 0; c < MAX_CH; ++c) v->gain[c] = v->gain_last[c] = 1.0f;
    }
}
static int voice_alloc_parts(Voice *v) {
    Rack *r = v->rack;
    v->matrices = calloc(r->max_matrices, sizeof(Matrix));
    if (!v->matrices) return 0;
    if (r->kind == KIND_SAMPLER) {
        v->sampler = calloc(1, sizeof(Sampler));
        if (!v->sampler) return 0;
        v->sampler->pitch = 1.0f;
        v->sampler->env_level = 1.0f;
    } else if (!(v->input = calloc((size_t)MAX_CH * MAX_GRAIN, sizeof(float)))) return 0;
    if (r->kind == KIND_REVERB && !(v->reverb = reverb_new(r->system->rate))) return 0;
    return 1;
}
static void voice_free(Voice *v) {
    if (!v) return;
    if (v->sampler) {
        if (v->sampler->at9) Atrac9ReleaseHandle(v->sampler->at9);
        free(v->sampler->fifo);
        free(v->sampler->at9_sf);
        free(v->sampler);
    }
    free(v->matrices);
    free(v->input);
    reverb_free(v->reverb);
    if (v->chorus) free(v->chorus->line);
    if (v->delay) free(v->delay->line);
    free(v->chorus);
    free(v->delay);
    v->magic = 0;
}

/* Guest allocator: ask for a small context buffer so the game's accounting sees the object.
 * The mixer itself lives in host memory. */
static void guest_alloc(const Allocator *a, BufferInfo *info, uint64_t size) {
    memset(info, 0, sizeof(*info));
    info->host_size = size;
    if (!a || !a->alloc) return;
    info->user_data = a->user_data;
    int32_t r = a->alloc(info);
    if (r) note("guest allocator returned 0x%08x", (uint32_t)r);
}
static void guest_free(const Allocator *a, BufferInfo *info) {
    if (a && a->free && info->host_buffer) a->free(info);
}

#define CONTEXT_BUFFER_SIZE 0x1000

/* ---- Exports: system ---- */
static ABI int32_t ngs2_system_create(const uint8_t *option, const Allocator *allocator, uintptr_t *out) {
    if (!out) return NGS2_ERR_ARG;
    System *s = calloc(1, sizeof(*s));
    if (!s) return NGS2_ERR_MEMORY;
    s->magic = MAGIC_SYSTEM;
    s->grain = 256; s->max_grain = 512; s->rate = 48000;
    if (option) {
        uint32_t v;
        memcpy(&v, option + 0x1c, 4); if (v) s->max_grain = v;
        memcpy(&v, option + 0x20, 4); if (v) s->grain = v;
        memcpy(&v, option + 0x24, 4); if (v) s->rate = v;
    }
    /* sotc: the reverb sizes, 30 s summaries and delay lines divide by the rate. */
    if (s->rate < 8000) s->rate = 8000;
    if (s->rate > 192000) s->rate = 192000;
    if (s->grain > MAX_GRAIN) s->grain = MAX_GRAIN;
    if (allocator) { s->allocator = *allocator; guest_alloc(allocator, &s->buffer, CONTEXT_BUFFER_SIZE); s->allocated = 1; }
    pthread_mutex_lock(&lock);
    s->next = systems; systems = s;
    pthread_mutex_unlock(&lock);
    static int announced;
    if (!announced++) printf("NGS2: built-in mixer (grain %u, %u Hz)\n", s->grain, s->rate);
    if (!wav) {
        const char *path = env("NGS2_WAV");
        if (path && *path && (wav = fopen(path, "wb"))) { static const char zero[44]; fwrite(zero, 1, 44, wav); }
    }
    *out = (uintptr_t)s;
    return NGS2_OK;
}

static void rack_release(Rack *r) {
    for (uint32_t i = 0; i < r->max_voices; ++i) {
        voice_free(r->voices[i]);
        if (i) free(r->voices[i]);
    }
    free(r->voices[0]);
    free(r->voices);
}
static void unpatch_all(System *s, const Rack *gone) {
    for (Rack *r = s->racks; r; r = r->next)
        for (uint32_t i = 0; i < r->max_voices; ++i)
            for (int p = 0; p < MAX_PORTS; ++p)
                if (r->voices[i]->ports[p].dest && r->voices[i]->ports[p].dest->rack == gone) r->voices[i]->ports[p].dest = NULL;
}

static ABI int32_t ngs2_system_destroy(uintptr_t handle, BufferInfo *out) {
    pthread_mutex_lock(&lock);
    System *s = as_system(handle);
    if (!s) { pthread_mutex_unlock(&lock); return NGS2_ERR_HANDLE; }
    for (System **p = &systems; *p; p = &(*p)->next) if (*p == s) { *p = s->next; break; }
    while (s->racks) { Rack *r = s->racks; s->racks = r->next; rack_release(r); r->magic = 0; free(r); }
    if (out) *out = s->buffer;
    /* sotc: the guest's free callback runs after the lock (a guest allocator that waits for
     * a thread inside NGS2 would deadlock). */
    const Allocator allocator = s->allocator;
    BufferInfo buffer = s->buffer;
    const int allocated = s->allocated;
    s->magic = 0;
    free(s);
    pthread_mutex_unlock(&lock);
    if (allocated) guest_free(&allocator, &buffer);
    return NGS2_OK;
}

/* ---- Exports: racks ---- */
static ABI int32_t ngs2_query_buffer(uint32_t rack_id, const void *option, BufferInfo *out) {
    (void)rack_id; (void)option;
    if (!out) return NGS2_ERR_ARG;
    memset(out, 0, sizeof(*out));
    out->host_size = CONTEXT_BUFFER_SIZE;
    return NGS2_OK;
}

static int32_t rack_create(uintptr_t system, uint32_t rack_id, const uint8_t *option, const BufferInfo *buffer,
                           const Allocator *allocator, uintptr_t *out) {
    if (!out) return NGS2_ERR_ARG;
    /* sotc: the guest's alloc callback runs before the lock, its free callback after it. */
    BufferInfo guest_buffer;
    if (allocator) guest_alloc(allocator, &guest_buffer, CONTEXT_BUFFER_SIZE);
    pthread_mutex_lock(&lock);
    System *s = as_system(system);
    Rack *r = s ? calloc(1, sizeof(*r)) : NULL;
    if (!r) {
        pthread_mutex_unlock(&lock);
        if (allocator) guest_free(allocator, &guest_buffer);
        return s ? NGS2_ERR_MEMORY : NGS2_ERR_HANDLE;
    }
    r->magic = MAGIC_RACK;
    r->system = s;
    r->id = rack_id;
    r->kind = rack_kind(rack_id);
    r->max_voices = r->kind == KIND_SAMPLER ? 256 : 1;
    r->max_ports = r->kind == KIND_SAMPLER ? 8 : 4;
    r->max_matrices = 1;
    if (option) {
        uint32_t v;
        memcpy(&v, option + 0x20, 4); if (v) r->max_voices = v;
        memcpy(&v, option + 0x28, 4); if (v) r->max_matrices = v;
        memcpy(&v, option + 0x2c, 4); if (v) r->max_ports = v;
    }
    if (r->max_voices > 4096) r->max_voices = 4096;
    if (r->max_ports > MAX_PORTS) r->max_ports = MAX_PORTS;
    if (r->max_matrices > MAX_MATRICES) r->max_matrices = MAX_MATRICES;
    r->voices = calloc(r->max_voices, sizeof(Voice *));
    int ok = r->voices != NULL;
    for (uint32_t i = 0; ok && i < r->max_voices; ++i) {
        r->voices[i] = calloc(1, sizeof(Voice));
        if (!r->voices[i]) { ok = 0; break; }
        voice_init(r->voices[i], r, i);
        ok = voice_alloc_parts(r->voices[i]);
    }
    if (!ok) {
        if (r->voices) for (uint32_t i = 0; i < r->max_voices; ++i) if (r->voices[i]) { voice_free(r->voices[i]); free(r->voices[i]); }
        free(r->voices); free(r);
        pthread_mutex_unlock(&lock);
        if (allocator) guest_free(allocator, &guest_buffer);
        return NGS2_ERR_MEMORY;
    }
    if (buffer) r->buffer = *buffer;
    if (allocator) { r->allocator = *allocator; r->buffer = guest_buffer; r->allocated = 1; }
    /* Appended: creation order is a reasonable tie-break for the render order. */
    Rack **tail = &s->racks;
    while (*tail) tail = &(*tail)->next;
    *tail = r;
    pthread_mutex_unlock(&lock);
    note("rack 0x%04x created: %u voices, %u ports, %u matrices", rack_id, r->max_voices, r->max_ports, r->max_matrices);
    *out = (uintptr_t)r;
    return NGS2_OK;
}
static ABI int32_t ngs2_rack_create(uintptr_t system, uint32_t rack_id, const uint8_t *option, const BufferInfo *buffer, uintptr_t *out) {
    return rack_create(system, rack_id, option, buffer, NULL, out);
}
static ABI int32_t ngs2_rack_create_alloc(uintptr_t system, uint32_t rack_id, const uint8_t *option, const Allocator *allocator, uintptr_t *out) {
    return rack_create(system, rack_id, option, NULL, allocator, out);
}
static ABI int32_t ngs2_rack_destroy(uintptr_t handle, BufferInfo *out) {
    pthread_mutex_lock(&lock);
    Rack *r = as_rack(handle);
    if (!r) { pthread_mutex_unlock(&lock); return NGS2_ERR_HANDLE; }
    System *s = r->system;
    for (Rack **p = &s->racks; *p; p = &(*p)->next) if (*p == r) { *p = r->next; break; }
    unpatch_all(s, r);
    if (out) *out = r->buffer;
    const Allocator allocator = r->allocator;
    BufferInfo buffer = r->buffer;
    const int allocated = r->allocated;
    rack_release(r);
    r->magic = 0;
    free(r);
    pthread_mutex_unlock(&lock);
    if (allocated) guest_free(&allocator, &buffer);
    return NGS2_OK;
}
static ABI int32_t ngs2_voice_handle(uintptr_t rack, uint32_t index, uintptr_t *out) {
    Rack *r = as_rack(rack);
    if (!r || !out) return NGS2_ERR_HANDLE;
    if (index >= r->max_voices) return NGS2_ERR_ARG;
    *out = voice_handle(r->voices[index]);
    return NGS2_OK;
}

/* ---- Voice control ---- */
static uint32_t u32_at(const ParamHeader *p, uint32_t offset) {
    uint32_t v = 0;
    if (offset + 4 <= p->size) memcpy(&v, (const uint8_t *)p + offset, 4);
    return v;
}
static float f32_at(const ParamHeader *p, uint32_t offset) {
    float v = 0.0f;
    if (offset + 4 <= p->size) memcpy(&v, (const uint8_t *)p + offset, 4);
    return v;
}
static uintptr_t ptr_at(const ParamHeader *p, uint32_t offset) {
    uintptr_t v = 0;
    if (offset + 8 <= p->size) memcpy(&v, (const uint8_t *)p + offset, 8);
    return v;
}

static void voice_finish(Voice *v) {
    if (v->state == ST_PLAYING) ++stat_finished;
    v->state = ST_FREE;
    v->paused = 0;
    if (v->sampler) {
        sampler_clear_blocks(v->sampler);
        v->sampler->releasing = 0;
        v->sampler->stopping = 0;
        v->sampler->fade_in = 0;
    }
}
/* STOP/KILL (and a release without release points) end a playing sampler after one more grain
 * that fades out over 64 samples, as the console module does. */
static void voice_stop(Voice *v) {
    if (v->sampler && v->state == ST_PLAYING && !v->paused) v->sampler->stopping = 1;
    else voice_finish(v);
}

static void envelope_start(Sampler *s) {
    s->env_segment = 0;
    s->env_position = 0;
    s->releasing = 0;
    s->env_start = s->env_level = s->env_forward ? 0.0f : 1.0f;
}
static void voice_event(Voice *v, uint32_t event) {
    switch (event) {
    case EV_PLAY:
        if (v->state == ST_FREE && v->sampler) break;          /* no waveform set up */
        v->stopped = 0;
        if (v->state != ST_PLAYING) {
            v->state = ST_PLAYING;
            v->paused = 0;
            if (v->sampler) {
                envelope_start(v->sampler);
                v->sampler->fade_in = 1;
                v->sampler->stopping = 0;
                for (int p = 0; p < MAX_PORTS; ++p) v->ports[p].primed = 0;
                ++stat_plays;
            }
        }
        break;
    case EV_RELEASE:
        if (!v->sampler) v->stopped = 1;
        if (v->state != ST_PLAYING) { if (v->state == ST_READY && v->sampler) voice_finish(v); break; }
        if (v->sampler && v->sampler->env_release) {
            Sampler *s = v->sampler;
            s->releasing = 1;
            s->env_segment = s->env_forward;
            s->env_position = 0;
            s->env_start = s->env_level;
        } else voice_stop(v);
        break;
    case EV_STOP:
    case EV_KILL:
        if (v->sampler) voice_stop(v);
        else { voice_finish(v); v->state = ST_READY; v->stopped = 1; } /* buses stay set up */
        break;
    case EV_PAUSE: if (v->state == ST_PLAYING) v->paused = 1; break;
    case EV_RESUME: v->paused = 0; break;
    default: note("unknown event %u", event); break;
    }
}

static void sampler_setup(Voice *v, const ParamHeader *p) {
    Sampler *s = v->sampler;
    s->wave = u32_at(p, 8);
    s->wave_channels = u32_at(p, 12);
    s->rate = u32_at(p, 16);
    s->config = u32_at(p, 20);
    if (s->wave_channels > MAX_CH) s->wave_channels = MAX_CH;
    if (!s->rate) s->rate = v->rack->system->rate;
    v->channels = s->wave_channels ? s->wave_channels : 1;
    s->pitch = 1.0f;
    s->env_forward = s->env_release = 0;
    s->env_level = 1.0f;
    s->releasing = 0;
    s->decoded_samples = s->decoded_bytes = s->user = 0;
    s->waveform_pointer = NULL;
    v->paused = 0;
    if (!s->fifo) s->fifo = calloc((size_t)FIFO_FRAMES * MAX_CH, sizeof(float));
    sampler_clear_blocks(s);
    sampler_reset_decoder(s);
    v->state = s->wave && s->wave_channels && s->fifo ? ST_READY : ST_FREE;
    if (s->wave && s->wave != WAVE_VAG && s->wave != WAVE_ATRAC9 && (s->wave < WAVE_PCM_U8 || s->wave > WAVE_PCM_F64B)) {
        static uint32_t seen[16]; static unsigned n;
        int known = 0;
        for (unsigned i = 0; i < n; ++i) known |= seen[i] == s->wave;
        if (!known && n < 16) { seen[n++] = s->wave; printf("NGS2: waveform type 0x%x not supported (silent)\n", s->wave); }
    }
}
static void sampler_add_blocks(Voice *v, const ParamHeader *p) {
    Sampler *s = v->sampler;
    const uint8_t *data = (const uint8_t *)ptr_at(p, 8);
    uint32_t flags = u32_at(p, 16), count = u32_at(p, 20);
    const GuestBlock *blocks = (const GuestBlock *)ptr_at(p, 24);
    (void)flags;
    if (!data || !blocks) return;
    if (v->state == ST_PLAYING && (s->empty_renders || (s->source_done && s->count == 0))) ++stat_underruns; /* data came late */
    if (s->source_done && s->count == 0) s->source_done = 0;  /* more data for a starved stream */
    for (uint32_t i = 0; i < count && i < MAX_BLOCKS; ++i) {
        if (s->count == MAX_BLOCKS) { note("waveform block queue full"); break; }
        Block *b = &s->blocks[(s->head + s->count) % MAX_BLOCKS];
        b->data = data + blocks[i].data_offset;
        b->size = blocks[i].data_size;
        b->repeats = blocks[i].repeats;
        b->skip = blocks[i].skip;
        b->samples = blocks[i].samples;
        b->user = blocks[i].user_data;
        ++s->count;
        ++stat_blocks;
    }
}
static void sampler_envelope(Voice *v, const ParamHeader *p) {
    Sampler *s = v->sampler;
    uint32_t forward = u32_at(p, 8), release = u32_at(p, 12);
    const EnvPoint *points = (const EnvPoint *)ptr_at(p, 16);
    if (!points || forward + release == 0) { s->env_forward = s->env_release = 0; if (!s->releasing) s->env_level = 1.0f; return; }
    if (forward > MAX_ENV) forward = MAX_ENV;
    if (forward + release > MAX_ENV) release = MAX_ENV - forward;
    memcpy(s->env, points, (forward + release) * sizeof(EnvPoint));
    s->env_forward = forward;
    s->env_release = release;
    if (v->state != ST_PLAYING) envelope_start(s);
}
static void store_filter(Voice *v, const ParamHeader *p) {
    uint32_t index = u32_at(p, 8);
    if (index >= MAX_FILTERS || u32_at(p, 12) > 17) return;
    Biquad *f = &v->filters[index];
    uint32_t type = u32_at(p, 16) & 0xff;
    if (!f->used || f->type != type) memset(f->z, 0, sizeof(f->z));
    f->used = 1;
    f->location = u32_at(p, 12) & 0xff;
    f->type = type;
    f->mask = u32_at(p, 20) & 0xff;
    f->fc = f32_at(p, 24); f->q = f32_at(p, 28); f->level = f32_at(p, 32);
    for (int i = 0; i < 5; ++i) f->direct[i] = f32_at(p, 24 + 4 * (uint32_t)i);
    f->dirty = 1;
}
static void store_compressor(Dynamics *d, const ParamHeader *p) {
    float values[6] = {f32_at(p, 12), f32_at(p, 16), f32_at(p, 20), f32_at(p, 24), f32_at(p, 28), f32_at(p, 32)};
    int was = d->enabled;
    dynamics_set(d, u32_at(p, 8), values);
    if (!was) dynamics_reset(d);
}
static Chorus *voice_chorus(Voice *v) { if (!v->chorus) v->chorus = calloc(1, sizeof(Chorus)); return v->chorus; }
static Delay *voice_delay(Voice *v) {
    if (!v->delay) {
        v->delay = calloc(1, sizeof(Delay));
        if (v->delay) {
            const char *type = env("NGS2_DELAY_TYPE");
            v->delay->type = type && *type == '0' ? 0 : 1;   /* the console default (per channel) */
        }
    }
    return v->delay;
}
static void store_chorus(Voice *v, const ParamHeader *p) {
    Chorus *c = voice_chorus(v);
    if (!c) return;
    if (!c->line && !(c->line = calloc((size_t)MAX_CH * 4 * CHORUS_LINE, sizeof(float)))) return;  /* not on the audio thread */
    c->used = 1;
    c->mask = u32_at(p, 16) & 0xff;
    c->phases = u32_at(p, 12);
    if (c->phases < 1 || c->phases > 4) c->phases = 1;
    c->gain = clampf(f32_at(p, 20), 0.0f, 1.0f);
    c->delay_ms = clampf(f32_at(p, 24), 0.1f, 40.0f);
    c->rate_hz = clampf(f32_at(p, 28), 0.001f, 20.0f);
    c->depth = clampf(f32_at(p, 32), 0.0f, 1.0f);
    c->feedback = clampf(f32_at(p, 36), -1.0f, 1.0f);
    c->wet = clampf(f32_at(p, 40), -1.0f, 1.0f);
    c->dry = clampf(f32_at(p, 44), 0.0f, 1.0f);
}
static void store_delay(Voice *v, const ParamHeader *p) {
    Delay *d = voice_delay(v);
    if (!d) return;
    d->used = 1;
    d->dry = clampf(f32_at(p, 8), -1.0f, 1.0f);
    d->wet = clampf(f32_at(p, 12), -1.0f, 1.0f);
    d->gain = clampf(f32_at(p, 16), -1.0f, 1.0f);
    d->feedback = clampf(f32_at(p, 20), -1.0f, 1.0f);
    d->lowpass_hz = clampf(f32_at(p, 24), 0.0f, 24000.0f);
    d->taps = u32_at(p, 28);
    if (d->taps < 1) d->taps = 1;
    if (d->taps > 8) d->taps = 8;
    for (uint32_t t = 0; t < 8; ++t) {
        d->tap_level[t] = t < d->taps ? clampf(f32_at(p, 32 + 8 * t), -1.0f, 1.0f) : 0.0f;
        d->tap_ms[t] = t < d->taps ? clampf(f32_at(p, 36 + 8 * t), 0.0f, 20000.0f) : 0.0f;
    }
    d->mask = u32_at(p, 128);
    d->flags = u32_at(p, 132);
    for (uint32_t c = 0; c < MAX_CH; ++c) {
        if (d->flags & 2) d->in_gain[c] = clampf(f32_at(p, 96 + 4 * c), -1.0f, 1.0f);
        else if (d->type == 1) d->in_gain[c] = 1.0f;
        else d->in_gain[c] = v->channels >= 8 ? 0.267f : v->channels >= 6 ? 0.31623f : v->channels >= 2 ? 0.70711f : 1.0f;
    }
    delay_design(d, v->rack->system->rate, v->rack->system->grain);
}
static void store_user_fx(Voice *v, const ParamHeader *p) {
    v->user_fx = (UserFxHandler)ptr_at(p, 8);
    v->user_fx_data[0] = ptr_at(p, 16);
    v->user_fx_data[1] = ptr_at(p, 24);
    v->user_fx_data[2] = ptr_at(p, 32);
}

static void apply_param(Voice *v, const ParamHeader *p) {
    Rack *r = v->rack;
    switch (p->id) {
    case 0x1: { /* MATRIX_LEVELS: matrix, count, levels */
        uint32_t m = u32_at(p, 8), count = u32_at(p, 12);
        const float *levels = (const float *)ptr_at(p, 16);
        if (m >= r->max_matrices) return;
        if (count > MAX_LEVELS) count = MAX_LEVELS;
        v->matrices[m].count = levels ? count : 0;
        if (levels && count) memcpy(v->matrices[m].level, levels, count * sizeof(float));
        return;
    }
    case 0x2: { uint32_t port = u32_at(p, 8); if (port < MAX_PORTS) v->ports[port].volume = f32_at(p, 12); return; }
    case 0x3: { uint32_t port = u32_at(p, 8); if (port < MAX_PORTS) v->ports[port].matrix = (int32_t)u32_at(p, 12); return; }
    case 0x4: { uint32_t port = u32_at(p, 8); if (port < MAX_PORTS) v->ports[port].delay = u32_at(p, 12); return; }
    case 0x5: { /* PATCH: port, dest input, dest handle (voice or rack = voice 0; 0 = unpatch) */
        uint32_t port = u32_at(p, 8);
        if (port >= MAX_PORTS) return;
        uintptr_t dest = ptr_at(p, 16);
        Voice *d = dest ? as_voice(dest) : NULL;
        if (dest && (!d || !d->input)) { note("patch to unknown handle %#lx ignored", (unsigned long)dest); d = NULL; }
        v->ports[port].dest = d;
        v->ports[port].input = u32_at(p, 12);
        return;
    }
    case 0x6: voice_event(v, u32_at(p, 8)); return;
    case 0x7:
        v->callback = (CallbackHandler)ptr_at(p, 8);
        v->callback_data = ptr_at(p, 16);
        v->callback_flags = u32_at(p, 24);
        return;
    }
    if (v->sampler) {
        Sampler *s = v->sampler;
        switch (p->id) {
        case 0x10000000: sampler_setup(v, p); return;
        case 0x10000001: sampler_add_blocks(v, p); return;
        case 0x10000004: /* EXIT_LOOP */
            for (uint32_t i = 0; i < s->count; ++i) s->blocks[(s->head + i) % MAX_BLOCKS].repeats = 0;
            return;
        case 0x10000005: { float ratio = f32_at(p, 8); if (ratio >= 0.0f && ratio < 64.0f) s->pitch = ratio; return; }
        case 0x10000006: sampler_envelope(v, p); return;
        case 0x10000008: store_user_fx(v, p); return;
        case 0x1000000a: store_filter(v, p); return;
        case 0x10000002: case 0x10000003: case 0x10000007: case 0x10000009: case 0x1000000b:
            return; /* waveform address, frame offset, distortion, peak meter, filter count */
        }
    } else {
        switch (p->id) {
        case 0x20000000: case 0x30000000:
            v->channels = u32_at(p, 8);
            if (!v->channels || v->channels > MAX_CH) v->channels = MAX_CH;
            if (v->state == ST_FREE) v->state = ST_READY;
            return;
        case 0x20000002: store_compressor(&v->compressor, p); return;
        case 0x20000004: store_user_fx(v, p); return;
        case 0x20000006: store_filter(v, p); return;
        case 0x20000001: case 0x20000003: case 0x20000005: case 0x20000007: return;
        case 0x20010000:
            v->channels = u32_at(p, 8);
            v->out_channels = u32_at(p, 12);
            if (!v->channels || v->channels > MAX_CH) v->channels = MAX_CH;
            if (!v->out_channels || v->out_channels > MAX_CH) v->out_channels = MAX_CH;
            if (v->state == ST_FREE) v->state = ST_READY;
            return;
        case 0x20010001: if (v->reverb && p->size >= 0x40) reverb_set_i3dl2(v->reverb, p, f32_at, u32_at); return;
        case 0x30000002: {  /* LFE: enable, cut-off 80/120/240 Hz */
            uint32_t fc = u32_at(p, 12);
            if (fc != 80 && fc != 120 && fc != 240) return;
            v->lfe_enabled = u32_at(p, 8) != 0;
            if (fc != v->lfe_fc) { v->lfe_fc = fc; lfe_design(v, r->system->rate); }
            return;
        }
        case 0x30000003: {  /* LIMITER: enable, threshold */
            int was = v->limiter.enabled;
            v->limiter.enabled = u32_at(p, 8) != 0;
            v->limiter.threshold = clampf(f32_at(p, 12), 0.0f, 4.0f);
            if (!was) dynamics_reset(&v->limiter);
            return;
        }
        case 0x30000004: {  /* GAIN: full band, LFE */
            float fbw = clampf(f32_at(p, 8), 0.0f, 4.0f), lfe = clampf(f32_at(p, 12), 0.0f, 4.0f);
            for (int c = 0; c < MAX_CH; ++c) v->gain[c] = c == 3 ? lfe : fbw;
            if (!v->gain_set) for (int c = 0; c < MAX_CH; ++c) v->gain_last[c] = v->gain[c];
            v->gain_set = 1;
            return;
        }
        case 0x30000005: v->output_id = u32_at(p, 8); return;
        case 0x30000001: case 0x30000006: return;
        }
        if (r->kind == KIND_CUSTOM && (p->id & 0xffff0000) == 0x40020000) {
            v->channels = u32_at(p, 8);
            v->out_channels = u32_at(p, 12);
            if (!v->channels || v->channels > MAX_CH) v->channels = MAX_CH;
            if (!v->out_channels || v->out_channels > MAX_CH) v->out_channels = MAX_CH;
            if (v->state == ST_FREE) v->state = ST_READY;
            return;
        }
        if (r->kind == KIND_CUSTOM && p->id == 0x40001c00 && p->size >= 0x30) { store_chorus(v, p); return; }
        if (r->kind == KIND_CUSTOM && p->id == 0x40001d00 && p->size >= 0x88) { store_delay(v, p); return; }
    }
    note_unknown(r->id, p->id, p->size);
}

static ABI int32_t ngs2_voice_control(uintptr_t handle, const ParamHeader *params) {
    pthread_mutex_lock(&lock);
    Voice *v = as_voice(handle);
    if (!v) { pthread_mutex_unlock(&lock); return NGS2_ERR_HANDLE; }
    const uint8_t *p = (const uint8_t *)params;
    for (int guard = 0; p && guard < 256; ++guard) {
        const ParamHeader *h = (const ParamHeader *)p;
        if (h->size >= sizeof(ParamHeader)) apply_param(v, h);
        if (!h->next) break;
        p += h->next;
    }
    pthread_mutex_unlock(&lock);
    return NGS2_OK;
}

static uint32_t voice_flags(const Voice *v) {
    switch (v->state) {
    case ST_READY: return FLAG_INUSE;
    case ST_PLAYING: {
        uint32_t f = FLAG_INUSE | FLAG_PLAYING;
        if (v->paused) f |= FLAG_PAUSED;
        if (v->sampler && v->sampler->source_done && !v->sampler->releasing) f |= FLAG_EMPTY;
        return f;
    }
    default: return 0;
    }
}
/* sotc: sceNgs2VoiceGetStateFlags, the first word of VoiceGetState (SotC imports it). */
static ABI int32_t ngs2_voice_state_flags(uintptr_t handle, uint32_t *out) {
    if (!out) return NGS2_ERR_ARG;
    pthread_mutex_lock(&lock);
    Voice *v = as_voice(handle);
    if (!v) { pthread_mutex_unlock(&lock); return NGS2_ERR_HANDLE; }
    *out = voice_flags(v);
    pthread_mutex_unlock(&lock);
    return NGS2_OK;
}
static ABI int32_t ngs2_voice_state(uintptr_t handle, void *out, uint64_t size) {
    if (!out) return NGS2_ERR_ARG;
    pthread_mutex_lock(&lock);
    Voice *v = as_voice(handle);
    if (!v) { pthread_mutex_unlock(&lock); return NGS2_ERR_HANDLE; }
    uint8_t state[0x60];
    memset(state, 0, sizeof(state));
    uint32_t flags = voice_flags(v);
    memcpy(state, &flags, 4);
    if (v->sampler) {
        const Sampler *s = v->sampler;
        float env_height = s->env_level, peak = v->peak;
        uint64_t data = (uint64_t)(uintptr_t)s->waveform_pointer;
        memcpy(state + 4, &env_height, 4);
        memcpy(state + 8, &peak, 4);
        memcpy(state + 0x10, &s->decoded_samples, 8);
        memcpy(state + 0x18, &s->decoded_bytes, 8);
        memcpy(state + 0x20, &s->user, 8);
        memcpy(state + 0x28, &data, 8);
    } else if (v->rack->kind == KIND_MASTERING) {
        float peak = v->peak;
        memcpy(state + 4, &peak, 4);
    } else {
        float one = 1.0f, peak = v->peak;
        memcpy(state + 4, &one, 4);
        memcpy(state + 8, &peak, 4);
        memcpy(state + 12, &one, 4);
    }
    memcpy(out, state, size < sizeof(state) ? size : sizeof(state));
    pthread_mutex_unlock(&lock);
    return NGS2_OK;
}

/* ---- Decoding ---- */
static float pcm_sample(uint32_t wave, const uint8_t *p) {
    switch (wave) {
    case WAVE_PCM_U8: return ((float)p[0] - 128.0f) / 128.0f;
    case WAVE_PCM_I8: return (float)(int8_t)p[0] / 128.0f;
    case WAVE_PCM_I16L: return (float)(int16_t)(p[0] | p[1] << 8) / 32768.0f;
    case WAVE_PCM_I16B: return (float)(int16_t)(p[1] | p[0] << 8) / 32768.0f;
    case WAVE_PCM_I24L: return (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8) / 8388608.0f;
    case WAVE_PCM_I24B: return (float)((int32_t)((uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24) >> 8) / 8388608.0f;
    case WAVE_PCM_I32L: return (float)(int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) / 2147483648.0f;
    case WAVE_PCM_I32B: return (float)(int32_t)((uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24) / 2147483648.0f;
    case WAVE_PCM_F32L: { float f; memcpy(&f, p, 4); return f; }
    case WAVE_PCM_F32B: { uint8_t b[4] = {p[3], p[2], p[1], p[0]}; float f; memcpy(&f, b, 4); return f; }
    case WAVE_PCM_F64L: { double d; memcpy(&d, p, 8); return (float)d; }
    case WAVE_PCM_F64B: { uint8_t b[8] = {p[7], p[6], p[5], p[4], p[3], p[2], p[1], p[0]}; double d; memcpy(&d, b, 8); return (float)d; }
    }
    return 0.0f;
}
static uint32_t pcm_bytes(uint32_t wave) {
    static const uint8_t sizes[] = {1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 8, 8};
    return wave >= WAVE_PCM_U8 && wave <= WAVE_PCM_F64B ? sizes[wave - WAVE_PCM_U8] : 0;
}

/* One VAG/HEVAG frame (16 bytes, 28 samples) of one channel. Indexes above 4 are HEVAG's
 * extended predictors; flag 7 marks an empty frame. */
static void vag_frame(const uint8_t *frame, float hist[4], float *out, uint32_t stride) {
    uint32_t index = (uint32_t)(frame[0] >> 4) | (frame[1] & 0xf0u), shift = frame[0] & 0xfu, flag = frame[1] & 0xfu;
    if (index > 127) index = 0;
    const float *c = hevag_coefs[index];
    for (uint32_t i = 0; i < 28; ++i) {
        float sample = 0.0f;
        if (flag != 7) {
            int code = (i & 1) ? frame[2 + i / 2] >> 4 : frame[2 + i / 2] & 0xf;
            if (code >= 8) code -= 16;
            sample = (float)((code * 4096) >> shift) + hist[0] * c[0] + hist[1] * c[1] + hist[2] * c[2] + hist[3] * c[3];
        }
        hist[3] = hist[2]; hist[2] = hist[1]; hist[1] = hist[0]; hist[0] = sample;
        float v = sample / 32768.0f;
        out[i * stride] = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
    }
}

/* Appends decoded frames to the FIFO, dropping the block's skip samples and clipping to its
 * sample count. */
static void fifo_push(Sampler *s, uint32_t channels, const float *frames, uint32_t count) {
    if (s->skip_left) {
        uint32_t drop = s->skip_left < count ? s->skip_left : count;
        s->skip_left -= drop;
        frames += (size_t)drop * MAX_CH;
        count -= drop;
    }
    if (count > s->out_left) count = s->out_left;
    s->out_left -= count;
    if (s->fifo_len + count > FIFO_FRAMES) count = FIFO_FRAMES - s->fifo_len;
    if (count) s->ends_without_output = 0;
    float *dst = s->fifo + (size_t)s->fifo_len * MAX_CH;
    for (uint32_t i = 0; i < count; ++i) for (uint32_t c = 0; c < MAX_CH; ++c) dst[i * MAX_CH + c] = c < channels ? frames[i * MAX_CH + c] : 0.0f;
    s->fifo_len += count;
    s->decoded_samples += count;
}

static void block_begin(Sampler *s, const Block *b, int restart) {
    s->byte = 0;
    s->skip_left = b->skip;
    s->out_left = b->samples ? b->samples : UINT32_MAX;
    s->started = 1;
    if (!restart) s->repeated = 0;
    if (s->wave == WAVE_ATRAC9 && (restart || b->skip)) sampler_reset_decoder(s);
    s->user = b->user;
    s->waveform_pointer = b->data;
}
/* The block finished: repeat it or move on. Calls the voice callback when one is set. */
static void block_end(Voice *v) {
    Sampler *s = v->sampler;
    /* sotc: a block that yields no frame (smaller than one frame, an ATRAC9 frame that never
     * decodes, a skip over all of it) and repeats forever spun the audio thread with the lock
     * held, which froze every VoiceControl caller: after 64 such ends the voice's queue ends. */
    if (++s->ends_without_output > 64) {
        ++stat_errors;
        note("a block queue produced no audio after 64 block ends: voice ended");
        s->count = 0;
        s->started = 0;
        return;
    }
    Block *b = &s->blocks[s->head];
    int repeat = b->repeats != 0;
    if (v->callback && (v->callback_flags & (repeat ? 2u : 1u))) {
        CallbackInfo info = {v->callback_data, voice_handle(v), repeat ? 2u : 1u, 0, b->user, b->data, b->size, s->repeated, 0, 0};
        v->callback(&info);
    }
    if (repeat) {
        if (b->repeats != UINT32_MAX) --b->repeats;
        ++s->repeated;
        block_begin(s, b, 1);
        return;
    }
    s->head = (s->head + 1) % MAX_BLOCKS;
    if (s->count) --s->count; /* sotc: the callback above may have reset the queue */
    s->started = 0;
}

/* Decodes one unit (PCM chunk, one VAG frame per channel, one ATRAC9 frame) of the current
 * block. Returns 0 when the queue is empty. */
static int decode_unit(Voice *v) {
    Sampler *s = v->sampler;
    if (!s->count) return 0;
    Block *b = &s->blocks[s->head];
    if (!s->started) block_begin(s, b, 0);
    const uint32_t ch = s->wave_channels;
    float frames[UNIT_FRAMES * MAX_CH];
    uint32_t produced = 0, used = 0;
    if (!s->out_left) { block_end(v); return 1; }
    if (s->wave == WAVE_VAG) {
        uint32_t group = 16 * ch;
        if (s->byte + group <= b->size) {
            for (uint32_t c = 0; c < ch; ++c) vag_frame(b->data + s->byte + 16 * c, s->hist[c], frames + c, MAX_CH);
            produced = 28; used = group;
        }
    } else if (s->wave == WAVE_ATRAC9) {
        const Atrac9CodecInfo *info = &s->at9_info;
        const uint32_t sf = (uint32_t)info->superframeSize;
        if (s->at9_ok && s->at9_sf_fill < sf) {
            /* sotc: fill the superframe from this block; one cut by the block's end is
             * completed from the next block. */
            uint32_t take = b->size > s->byte ? b->size - s->byte : 0;
            if (take > sf - s->at9_sf_fill) take = sf - s->at9_sf_fill;
            memcpy(s->at9_sf + s->at9_sf_fill, b->data + s->byte, take);
            s->at9_sf_fill += take;
            s->byte += take;
            s->decoded_bytes += take;
            s->at9_sf_pos = 0;
            s->at9_frame_in_superframe = 0;
        }
        if (s->at9_ok && s->at9_sf_fill == sf) {
            int16_t pcm[UNIT_FRAMES * MAX_CH];
            int bytes = 0;
            if (s->at9_sf_pos >= sf || Atrac9Decode(s->at9, s->at9_sf + s->at9_sf_pos, pcm, &bytes) || bytes <= 0) {
                ++stat_errors;
                note("ATRAC9 decode error in frame %u of a %u-byte superframe", s->at9_frame_in_superframe, sf);
                s->at9_sf_fill = 0;   /* the rest of the superframe is dropped */
                produced = 0;
            } else {
                uint32_t n = (uint32_t)info->frameSamples, c_in = (uint32_t)info->channels;
                for (uint32_t i = 0; i < n; ++i)
                    for (uint32_t c = 0; c < MAX_CH; ++c) frames[i * MAX_CH + c] = c < c_in ? pcm[i * c_in + c] / 32768.0f : 0.0f;
                produced = n;
                s->at9_sf_pos += (uint32_t)bytes;
                if (++s->at9_frame_in_superframe == (uint32_t)info->framesInSuperframe) s->at9_sf_fill = 0;
            }
            if (!produced) {   /* a bad superframe: go on with the next one if the block has more */
                s->waveform_pointer = b->data + s->byte;
                if (s->byte >= b->size) block_end(v);
                return 1;
            }
        }
    } else {
        uint32_t bytes = pcm_bytes(s->wave), frame = bytes * ch;
        if (frame && s->byte + frame <= b->size) {
            uint32_t n = (b->size - s->byte) / frame;
            if (n > UNIT_FRAMES) n = UNIT_FRAMES;
            for (uint32_t i = 0; i < n; ++i)
                for (uint32_t c = 0; c < MAX_CH; ++c)
                    frames[i * MAX_CH + c] = c < ch ? pcm_sample(s->wave, b->data + s->byte + i * frame + c * bytes) : 0.0f;
            produced = n; used = n * frame;
        }
    }
    if (!produced) { block_end(v); return 1; }   /* data exhausted before the sample count */
    s->byte += used;
    s->decoded_bytes += used;
    s->waveform_pointer = b->data + s->byte;
    fifo_push(s, ch, frames, produced);
    if (!s->out_left) block_end(v);
    return 1;
}

/* Makes frame `need` available (or marks the source done). */
static void fifo_fill(Voice *v, uint32_t need) {
    Sampler *s = v->sampler;
    while (s->fifo_len <= need && !s->source_done) {
        if (s->fifo_len + UNIT_FRAMES > FIFO_FRAMES) {
            /* Keep one frame of history before the read position. */
            uint32_t drop = (uint32_t)s->pos;
            drop = drop > 1 ? drop - 1 : 0;
            /* sotc: the read position can be past the end (a high pitch step): never drop more
             * than the FIFO holds, or fifo_len - drop wraps and memmove runs off the heap. */
            if (s->fifo_len && drop > s->fifo_len - 1) drop = s->fifo_len - 1;
            if (!drop) break;
            memmove(s->fifo, s->fifo + (size_t)drop * MAX_CH, (size_t)(s->fifo_len - drop) * MAX_CH * sizeof(float));
            s->fifo_len -= drop;
            s->pos -= drop;
            need -= drop;
            continue;
        }
        if ((s->wave == WAVE_ATRAC9 && !s->at9_ok) || (s->wave != WAVE_VAG && s->wave != WAVE_ATRAC9 && !pcm_bytes(s->wave))) {
            s->count = 0;   /* nothing this mixer can decode: the voice ends */
            s->source_done = 1;
            break;
        }
        if (!decode_unit(v)) s->source_done = 1;
    }
}

/* Envelope: each point moves from the level at its start to `height` over `duration` samples
 * along its curve. After the forward points the level holds (a voice whose forward envelope
 * ends at 0 stops); after the release points the voice ends. */
static float envelope_next(Sampler *s, int *done) {
    uint32_t end = s->env_forward + (s->releasing ? s->env_release : 0);
    if (!s->env_forward && !s->releasing) return 1.0f;
    while (s->env_segment < end) {
        const EnvPoint *p = &s->env[s->env_segment];
        if (s->env_position < p->duration) {
            float t = (float)s->env_position / (float)p->duration;
            ++s->env_position;
            if (p->curve < 6) s->env_level = s->env_start + (p->height - s->env_start) * envelope_curve(p->curve, t, p->height > s->env_start);
            return s->env_level;
        }
        s->env_level = s->env_start = p->height;
        s->env_position = 0;
        ++s->env_segment;
    }
    if (s->releasing || s->env_level == 0.0f) *done = 1;
    return s->env_level;
}

/* Renders `n` frames of a playing sampler voice into planar `out`; returns frames written. */
static uint32_t sampler_render(Voice *v, uint32_t n, float out[MAX_CH][MAX_GRAIN]) {
    Sampler *s = v->sampler;
    const uint32_t ch = v->channels;
    double step = (double)s->pitch * (double)s->rate / (double)v->rack->system->rate;
    if (step > 16.0) step = 16.0;
    uint32_t i = 0;
    int ended = 0;
    for (; i < n; ++i) {
        uint32_t index = (uint32_t)s->pos;
        fifo_fill(v, index + 2);
        index = (uint32_t)s->pos;
        if (index >= s->fifo_len) { ended = 1; break; }
        float t = (float)(s->pos - index);
        const float *f0 = s->fifo + (size_t)(index - 1) * MAX_CH, *f1 = f0 + MAX_CH;
        const float *f2 = index + 1 < s->fifo_len ? f1 + MAX_CH : NULL, *f3 = index + 2 < s->fifo_len ? f1 + 2 * MAX_CH : NULL;
        int done = 0;
        float gain = envelope_next(s, &done);
        for (uint32_t c = 0; c < ch; ++c) {
            float y0 = f0[c], y1 = f1[c], y2 = f2 ? f2[c] : 0.0f, y3 = f3 ? f3[c] : 0.0f;
            /* Catmull-Rom */
            float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
            float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            float c1 = -0.5f * y0 + 0.5f * y2;
            out[c][i] = (((a * t + b) * t + c1) * t + y1) * gain;
        }
        s->pos += step;
        if (done) { ++i; ended = 2; break; }
    }
    for (uint32_t c = 0; c < ch; ++c) for (uint32_t k = i; k < n; ++k) out[c][k] = 0.0f;
    if (s->fade_in || s->stopping) {
        uint32_t len = n < 64 ? n : 64;
        for (uint32_t c = 0; c < ch; ++c)
            for (uint32_t k = 0; k < len; ++k) out[c][k] *= s->fade_in ? (float)k / 64.0f : 1.0f - (float)k / 64.0f;
        if (s->stopping) for (uint32_t c = 0; c < ch; ++c) for (uint32_t k = len; k < n; ++k) out[c][k] = 0.0f;
        s->fade_in = 0;
        if (s->stopping) { voice_finish(v); return i; }
    }
    if (ended == 2) voice_finish(v);
    else if (ended == 1 || (s->source_done && (uint32_t)s->pos >= s->fifo_len)) {
        /* Out of data: a stream may still queue blocks for a moment (reported as EMPTY). */
        if (s->count == 0 && ++s->empty_renders > END_GRACE_RENDERS) voice_finish(v);
        else if (s->count) { s->source_done = 0; s->empty_renders = 0; }
        if (v->state != ST_FREE) ++stat_starved;
    } else s->empty_renders = 0;
    return i;
}

/* ---- Mixing ---- */
/* Adds `in` through a port into its destination's input; level = volume * matrix level, each
 * ramped linearly over the grain from the previous render's value. */
static void mix_port(const Voice *src, Port *port, float in[MAX_CH][MAX_GRAIN], uint32_t n) {
    Voice *dst = port->dest;
    if (!dst || !dst->input) return;
    uint32_t sc = src->channels, dc = dst->channels;
    const Matrix *m = port->matrix >= 0 && (uint32_t)port->matrix < src->rack->max_matrices && src->matrices[port->matrix].count
                          ? &src->matrices[port->matrix] : NULL;
    if (!port->primed) memset(port->last, 0, sizeof(port->last));
    int any = 0;
    for (uint32_t o = 0; o < dc; ++o) {
        float *acc = dst->input + (size_t)o * MAX_GRAIN;
        for (uint32_t i = 0; i < sc; ++i) {
            float level;
            if (m) {
                uint32_t k = o * sc + i;
                level = k < m->count ? m->level[k] : 0.0f;
            } else level = o == i ? 1.0f : 0.0f;
            level *= port->volume;
            float *last = &port->last[o * MAX_CH + i];
            float from = port->primed ? *last : level;
            *last = level;
            if (from == 0.0f && level == 0.0f) continue;
            const float *x = in[i];
            if (from == level) for (uint32_t k = 0; k < n; ++k) acc[k] += x[k] * level;
            else {
                float step = (level - from) / (float)n, g = from;
                for (uint32_t k = 0; k < n; ++k) { g += step; acc[k] += x[k] * g; }
            }
            any = 1;
        }
    }
    port->primed = 1;
    if (any) dst->has_input = dst->fed = 1;
}
/* Sends through every patched port; filters at location 2 + p shape only what port p sends. */
static float port_scratch[MAX_CH][MAX_GRAIN];
static void send_ports(Voice *v, float data[MAX_CH][MAX_GRAIN], uint32_t n) {
    for (uint32_t p = 0; p < v->rack->max_ports; ++p) {
        Port *port = &v->ports[p];
        if (!port->dest || port->dest == v) continue;
        if (filters_at(v, 2 + p)) {
            filters_run(v, 2 + p, data, port_scratch, v->channels, n);
            mix_port(v, port, port_scratch, n);
        } else mix_port(v, port, data, n);
    }
}
static void call_user_fx(Voice *v, float data[MAX_CH][MAX_GRAIN], uint32_t n) {
    static int enabled = -1;
    /* sotc: off by default. SotC's user FX handler (guest 0x9a17e0) reads a null pointer from
     * this context ~15 s after a save loads (its sndx build expects another layout); the voice
     * then passes its input through. SOTC_NGS2_USERFX=1 calls it anyway. */
    if (enabled < 0) enabled = env_flag("NGS2_USERFX", 0);
    if (!v->user_fx || !enabled) return;
    float *channels[MAX_CH];
    for (int c = 0; c < MAX_CH; ++c) channels[c] = data[c];
    UserFxContext context = {channels, v->user_fx_data[0], v->user_fx_data[1], v->user_fx_data[2], 0, v->channels, n, v->rack->system->rate};
    UserFxHandler handler = v->user_fx;
    ++stat_user_fx;
    /* Guest code may take its own locks while another game thread holding them waits in
     * VoiceControl: run it without the mixer lock. */
    pthread_mutex_unlock(&lock);
    handler(&context);
    pthread_mutex_lock(&lock);
}
static float peak_of(float data[MAX_CH][MAX_GRAIN], uint32_t channels, uint32_t n) {
    float peak = 0.0f;
    for (uint32_t c = 0; c < channels; ++c) for (uint32_t k = 0; k < n; ++k) { float a = fabsf(data[c][k]); if (a > peak) peak = a; }
    return peak;
}

static float scratch[MAX_CH][MAX_GRAIN];
static float outputs[8][MAX_CH][MAX_GRAIN];
static uint32_t output_channels[8];

static void process_sampler(Voice *v, uint32_t n) {
    if (v->state != ST_PLAYING || v->paused) { if (v->state != ST_PLAYING) v->peak = 0.0f; return; }
    uint32_t written = sampler_render(v, n, scratch);
    if (!written) { v->peak = 0.0f; return; }
    /* Console order: filters (location 0), envelope, user FX, filters (1), ports (2 + p). */
    filters_run(v, 0, scratch, scratch, v->channels, n);
    call_user_fx(v, scratch, n);
    filters_run(v, 1, scratch, scratch, v->channels, n);
    v->peak = peak_of(scratch, v->channels, n);
    send_ports(v, scratch, n);
}

static void process_bus(Voice *v, uint32_t n) {
    static int reverb_pass = -1, custom_mute = -1;
    if (reverb_pass < 0) { const char *e = env("NGS2_REVERB"); reverb_pass = e && !strcmp(e, "pass"); }
    if (custom_mute < 0) { const char *e = env("NGS2_CUSTOM"); custom_mute = e && !strcmp(e, "mute"); }
    /* The traced games kick every bus; one that was never kicked still passes its input. */
    if ((v->state != ST_PLAYING && v->stopped) || v->paused) { v->peak = 0.0f; return; }
    float (*data)[MAX_GRAIN] = (float (*)[MAX_GRAIN])v->input;
    uint32_t kind = v->rack->kind, rate = v->rack->system->rate;
    uint32_t out_ch = kind == KIND_REVERB || kind == KIND_CUSTOM ? v->out_channels : v->channels;
    /* A submixer with nothing mixed in this render stays silent (its filter tails are far
     * below audibility); reverb and custom racks keep running for their tails. */
    if (kind == KIND_SUBMIXER && !v->fed) { v->peak = 0.0f; return; }
    v->has_input = 1;   /* effects write into the input buffer: clear it before the next render */
    switch (kind) {
    case KIND_REVERB:
        if (!reverb_pass && v->reverb) reverb_run(v->reverb, rate, data, v->channels, v->out_channels, n);
        break;
    case KIND_CUSTOM:
        if (custom_mute) { for (uint32_t c = 0; c < MAX_CH; ++c) memset(data[c], 0, n * sizeof(float)); break; }
        if (v->chorus && v->chorus->used) chorus_run(v->chorus, rate, data, out_ch, n);
        if (v->delay && v->delay->used) delay_run(v->delay, data, out_ch, n);
        break;
    case KIND_MASTERING: {
        if (v->lfe_enabled && v->channels > 4) {
            biquad_run(v->lfe_c[0], v->lfe_z[0], data[3], data[3], n);
            biquad_run(v->lfe_c[1], v->lfe_z[1], data[3], data[3], n);
        }
        dynamics_run(&v->limiter, rate, data, v->channels, n);
        if (v->gain_set)
            for (uint32_t c = 0; c < v->channels; ++c) {
                float from = v->gain_last[c], to = v->gain[c], step = (to - from) / (float)n, g = from;
                for (uint32_t k = 0; k < n; ++k) { g += step; data[c][k] *= g; }
                v->gain_last[c] = to;
            }
        v->peak = peak_of(data, v->channels, n);
        uint32_t o = v->output_id < 8 ? v->output_id : 0;
        for (uint32_t c = 0; c < v->channels; ++c) for (uint32_t k = 0; k < n; ++k) outputs[o][c][k] += data[c][k];
        if (v->channels > output_channels[o]) output_channels[o] = v->channels;
        return;
    }
    default:   /* submixer: filters, compressor, user FX, filters */
        filters_run(v, 0, data, data, v->channels, n);
        dynamics_run(&v->compressor, rate, data, v->channels, n);
        call_user_fx(v, data, n);
        filters_run(v, 1, data, data, v->channels, n);
        break;
    }
    v->peak = peak_of(data, out_ch, n);
    uint32_t saved = v->channels;
    v->channels = out_ch;   /* ports see the bus output width */
    send_ports(v, data, n);
    v->channels = saved;
}

/* Buses in dependency order: a bus runs once every bus patched into it has run. */
static void render_buses(System *s, uint32_t n) {
    for (Rack *r = s->racks; r; r = r->next) {
        if (r->kind == KIND_SAMPLER) continue;
        for (uint32_t i = 0; i < r->max_voices; ++i) { r->voices[i]->pending = 0; r->voices[i]->visited = 0; }
    }
    for (Rack *r = s->racks; r; r = r->next) {
        if (r->kind == KIND_SAMPLER) continue;
        for (uint32_t i = 0; i < r->max_voices; ++i) {
            Voice *v = r->voices[i];
            for (uint32_t p = 0; p < r->max_ports; ++p) if (v->ports[p].dest && v->ports[p].dest != v) ++v->ports[p].dest->pending;
        }
    }
    int progress = 1;
    while (progress) {
        progress = 0;
        Voice *blocked = NULL;
        for (Rack *r = s->racks; r; r = r->next) {
            if (r->kind == KIND_SAMPLER) continue;
            for (uint32_t i = 0; i < r->max_voices; ++i) {
                Voice *v = r->voices[i];
                if (v->visited) continue;
                if (v->pending) { if (!blocked) blocked = v; continue; }
                v->visited = 1;
                process_bus(v, n);
                for (uint32_t p = 0; p < r->max_ports; ++p) if (v->ports[p].dest && v->ports[p].dest != v && v->ports[p].dest->pending) --v->ports[p].dest->pending;
                progress = 1;
            }
        }
        if (!progress && blocked) {   /* a patch cycle: break it at the first waiting bus */
            static int warned;
            if (!warned++) printf("NGS2: patch cycle between buses; processing in creation order\n");
            blocked->pending = 0;
            progress = 1;
        }
    }
}

static void write_output(const RenderBuffer *b, uint32_t index, uint32_t n) {
    if (!b->buffer || !b->channels) return;
    uint32_t bytes = b->waveform == WAVE_PCM_I16L ? 2 : b->waveform == WAVE_PCM_F32L ? 4 : 0;
    memset(b->buffer, 0, b->size);
    if (!bytes) return;
    uint64_t frames = b->size / ((uint64_t)bytes * b->channels);
    if (frames > n) frames = n;
    const uint32_t src_ch = index < 8 ? output_channels[index] : 0;
    for (uint64_t k = 0; k < frames; ++k)
        for (uint32_t c = 0; c < b->channels; ++c) {
            float x = 0.0f;
            if (src_ch) x = outputs[index][c < src_ch ? c : 0][k];
            if (c >= src_ch && src_ch != 1) x = 0.0f;   /* mono output feeds every channel */
            if (bytes == 4) ((float *)b->buffer)[k * b->channels + c] = x;
            else {
                float y = x * 32768.0f;
                ((int16_t *)b->buffer)[k * b->channels + c] = (int16_t)(y > 32767.0f ? 32767.0f : y < -32768.0f ? -32768.0f : y);
            }
        }
}

static void wav_update(void) {
    uint32_t ch = wav_channels ? wav_channels : 8, rate = 48000, fmt_size = 16, data = (uint32_t)(wav_frames * ch * 4), riff = 36 + data;
    uint32_t byte_rate = rate * ch * 4;
    uint16_t format = 3, channels = (uint16_t)ch, align = (uint16_t)(ch * 4), bits = 32;
    long here = ftell(wav);
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
    fwrite(&fmt_size, 4, 1, wav); fwrite(&format, 2, 1, wav); fwrite(&channels, 2, 1, wav); fwrite(&rate, 4, 1, wav);
    fwrite(&byte_rate, 4, 1, wav); fwrite(&align, 2, 1, wav); fwrite(&bits, 2, 1, wav);
    fwrite("data", 1, 4, wav); fwrite(&data, 4, 1, wav);
    fseek(wav, here, SEEK_SET);
    fflush(wav);
}

static void summary(const System *s) {
    unsigned playing = 0, ready = 0;
    for (const Rack *r = s->racks; r; r = r->next)
        if (r->kind == KIND_SAMPLER)
            for (uint32_t i = 0; i < r->max_voices; ++i) { playing += r->voices[i]->state == ST_PLAYING; ready += r->voices[i]->state == ST_READY; }
    note("render %llu: %u voices playing, %u ready; %llu plays, %llu finished, %llu blocks, %llu end-of-data renders, "
         "%llu stream underruns, %llu user FX calls, %llu errors; render %.3f ms avg, %.3f ms max (grain %.2f ms)",
         (unsigned long long)s->renders, playing, ready, (unsigned long long)stat_plays, (unsigned long long)stat_finished,
         (unsigned long long)stat_blocks, (unsigned long long)stat_starved, (unsigned long long)stat_underruns,
         (unsigned long long)stat_user_fx, (unsigned long long)stat_errors,
         stat_render_count ? stat_render_ns / (double)stat_render_count / 1e6 : 0.0, stat_render_max_ns / 1e6,
         s->rate ? 1000.0 * s->grain / s->rate : 0.0);
    stat_render_ns = stat_render_max_ns = 0.0;
    stat_render_count = 0;
}

static ABI int32_t ngs2_render(uintptr_t handle, const RenderBuffer *buffers, uint32_t count) {
    pthread_mutex_lock(&lock);
    System *s = as_system(handle);
    if (!s || (count && !buffers)) { pthread_mutex_unlock(&lock); return NGS2_ERR_HANDLE; }
    uint32_t n = s->grain;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* Flush denormals (reverb and filter tails) to zero while mixing: 0x8040 = FTZ | DAZ. */
    unsigned int csr = _mm_getcsr();
    _mm_setcsr(csr | 0x8040);
    memset(outputs, 0, sizeof(outputs));
    memset(output_channels, 0, sizeof(output_channels));
    for (Rack *r = s->racks; r; r = r->next) {
        if (r->kind == KIND_SAMPLER) continue;
        for (uint32_t i = 0; i < r->max_voices; ++i) {
            Voice *v = r->voices[i];
            v->fed = 0;
            if (v->has_input) { for (int c = 0; c < MAX_CH; ++c) memset(v->input + (size_t)c * MAX_GRAIN, 0, n * sizeof(float)); v->has_input = 0; }
        }
    }
    for (Rack *r = s->racks; r; r = r->next)
        if (r->kind == KIND_SAMPLER)
            for (uint32_t i = 0; i < r->max_voices; ++i) process_sampler(r->voices[i], n);
    render_buses(s, n);
    for (uint32_t i = 0; i < count; ++i) write_output(&buffers[i], i, n);
    if (wav && count && buffers[0].buffer && buffers[0].waveform == WAVE_PCM_F32L && buffers[0].channels) {
        uint64_t frames = buffers[0].size / (4u * buffers[0].channels);
        if (frames > n) frames = n;
        wav_channels = buffers[0].channels;
        fwrite(buffers[0].buffer, 4u * buffers[0].channels, frames, wav);
        wav_frames += frames;
        if (!(s->renders % 500)) wav_update();
    }
    ++s->renders;
    _mm_setcsr(csr);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
    stat_render_ns += ns;
    if (ns > stat_render_max_ns) stat_render_max_ns = ns;
    ++stat_render_count;
    if (log_enabled() && s->rate && !(s->renders % (30u * s->rate / n))) summary(s);
    pthread_mutex_unlock(&lock);
    return NGS2_OK;
}

static const RuntimeExport exports[] = {
    {"sceNgs2SystemCreateWithAllocator", ngs2_system_create}, {"sceNgs2SystemDestroy", ngs2_system_destroy},
    {"sceNgs2SystemRender", ngs2_render}, {"sceNgs2RackQueryBufferSize", ngs2_query_buffer},
    {"sceNgs2RackCreate", ngs2_rack_create}, {"sceNgs2RackCreateWithAllocator", ngs2_rack_create_alloc},
    {"sceNgs2RackDestroy", ngs2_rack_destroy}, {"sceNgs2RackGetVoiceHandle", ngs2_voice_handle},
    {"sceNgs2VoiceControl", ngs2_voice_control}, {"sceNgs2VoiceGetState", ngs2_voice_state},
    {"sceNgs2VoiceGetStateFlags", ngs2_voice_state_flags}, /* sotc */
};

/* SOTC_NGS2=native (or TLG_NGS2): libSceNgs2.sprx from the console firmware is linked as a module and its
 * exports must win, so nothing resolves here. */
int runtime_ngs2_native(void) {
    static int value = -1;
    if (value < 0) { const char *v = env("NGS2"); value = v && !strcmp(v, "native"); }
    return value;
}
uintptr_t runtime_ngs2_resolve(const char *name) {
    if (runtime_ngs2_native()) return 0;
    return RUNTIME_LOOKUP(exports, name);
}
/* Direct access by symbol name for the offline tests (tests/test_ngs2.c). */
void *runtime_ngs2_function(const char *symbol) {
    for (size_t i = 0; i < sizeof(exports) / sizeof(*exports); ++i)
        if (!strcmp(exports[i].name, symbol)) return exports[i].function;
    return NULL;
}
