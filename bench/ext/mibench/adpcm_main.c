/* BPX-1 bench/ext: driver for MiBench telecomm/adpcm (adpcm.c, the Intel/DVI ADPCM codec).
   Written for BPX-1.

   MiBench's rawcaudio and rawdaudio read sound files in blocks of 1000 samples and write the coded
   or decoded blocks. This driver replaces them:
     - once, it synthesises 16 blocks of sound in the program: two triangle tones (one sweeping in
       frequency) whose loudness steps through silence, quiet and loud passages, plus a little
       noise;
     - block by block (1000 samples, the 16 blocks over and over) it calls the unchanged
       adpcm_coder() and adpcm_decoder(); the codec state carries on from block to block;
     - after every block the decoder's state (predicted value, step index) must equal the coder's:
       the format is built so that both sides track the same prediction;
     - over the first 16 blocks the mean reconstruction error must be below 1/8 of the mean signal
       magnitude;
     - the checksum over all coded bytes and decoded samples must equal GOLDEN.

   Exit code: 0 = pass; 2 = coder and decoder state differ; 3 = reconstruction error too large;
   255 = checksum differs.
   Scale: SAMPLES (programs_ext.txt; rounded down to whole blocks), with the GOLDEN that belongs
   to it. */
#include "adpcm.h"
#include "drv.h"

#if !defined(SAMPLES) || !defined(GOLDEN)
#error "SAMPLES and GOLDEN must be defined on the command line (see programs_ext.txt)"
#endif

#define BLOCK 1000
#define INPUT_BLOCKS 16

static short pcm[INPUT_BLOCKS * BLOCK], decoded[BLOCK];
static char coded[BLOCK / 2];

/* Triangle wave: one period per 65536 units of phase, values -16384 .. 16383. */
static int triangle(u32 phase)
{
    int p = (int)(phase & 0xffffu);
    return (p < 0x8000 ? p : 0xffff - p) - 0x4000;
}

static void synthesise(void)
{
    static const int level[8] = { 0, 1, 4, 16, 64, 16, 2, 64 };    /* loudness per 2048 samples, of 64 */
    u32 noise = 2024, phase1 = 0, phase2 = 0, t;

    for (t = 0; t < INPUT_BLOCKS * BLOCK; t++) {
        int s;
        phase1 += 700u + ((t >> 4) & 0x3ffu);
        phase2 += 5003u;
        s = (3 * triangle(phase1) + triangle(phase2)) * level[(t >> 11) & 7u] / 128;
        s += (int)(drv_rand(&noise) & 0x3fu) - 32;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        pcm[t] = (short)s;
    }
}

int main(void)
{
    struct adpcm_state coder = { 0, 0 }, decoder = { 0, 0 };
    u32 sum = DRV_SUM_INIT;
    unsigned long long error = 0, magnitude = 0;
    int block, k;

    synthesise();
    for (block = 0; block < SAMPLES / BLOCK; block++) {
        short *in = pcm + (block % INPUT_BLOCKS) * BLOCK;

        adpcm_coder(in, coded, BLOCK, &coder);
        adpcm_decoder(coded, decoded, BLOCK, &decoder);
        if (coder.valprev != decoder.valprev || coder.index != decoder.index)
            return 2;
        for (k = 0; k < BLOCK / 2; k++)
            sum = drv_sum(sum, (unsigned char)coded[k]);
        for (k = 0; k < BLOCK; k++)
            sum = drv_sum(sum, (unsigned short)decoded[k]);
        if (block < INPUT_BLOCKS) {
            for (k = 0; k < BLOCK; k++) {
                int d = in[k] - decoded[k];
                error += (unsigned)(d < 0 ? -d : d);
                magnitude += (unsigned)(in[k] < 0 ? -in[k] : in[k]);
            }
        }
    }
#ifdef BPX_HOST
    fprintf(stderr, "adpcm: %d blocks, mean |error| / mean |signal| = %.4f\n", block,
            magnitude ? (double)error / (double)magnitude : 0.0);
#endif
    if (error * 8 >= magnitude)
        return 3;
    return drv_finish(sum, GOLDEN);
}
