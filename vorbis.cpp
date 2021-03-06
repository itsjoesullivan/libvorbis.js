#include <stdio.h>  // printf
#include <math.h>   // sin
#include <time.h>   // time
#include <stdlib.h> // srand
#include <string.h> // memcpy

#include <vorbis/vorbisenc.h>

struct tEncoderState
{
  ogg_stream_state os;
  
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state vd;
  vorbis_block vb;
  ogg_packet op;
  
  int packet_id;
  int rate;
  int num_channels;
  int sample_rate;
  int granulepos;
  
  int encoded_max_size;
  int encoded_length;
  
  unsigned char* encoded_buffer;
};

// write encoded ogg page to a file or buffer
int write_page(tEncoderState* state, ogg_page* page)
{
    memcpy(state->encoded_buffer + state->encoded_length, page->header, page->header_len);
    state->encoded_length += page->header_len;

    memcpy(state->encoded_buffer + state->encoded_length, page->body, page->body_len);
    state->encoded_length += page->body_len;

    //printf("write_page(); total encoded stream length: %i bytes\n", state->encoded_length);
    return 0;
}

// preps encoder, allocates output buffer
extern "C" tEncoderState* lexy_encoder_start(int sample_rate = 48000, float vbr_quality = 0.4f)
{
    tEncoderState *state = new tEncoderState();
    state->packet_id = 0;
    state->granulepos = 0;
    
    srand(time(NULL));
    ogg_stream_init(&state->os, rand());
    
    int size, error;
    
    state->num_channels = 2;
    state->sample_rate = sample_rate;
    
    // max duration. 3 mins = 180 sec @ 128kbit/s = ~3MB
    state->encoded_buffer = new unsigned char[3 * 1024 * 1024]; // final encoded-audio buffer
    
    printf("lexy_encoder_start(); initializing vorbis encoder with sample_rate = %i Hz and vbr quality = %3.2f\n", state->sample_rate, vbr_quality);
    
    state->encoded_max_size = 0;
    state->encoded_length = 0;
    
    // initialize vorbis
    vorbis_info_init(&state->vi);
    if(vorbis_encode_init_vbr(&state->vi, 2, state->sample_rate, vbr_quality)) // vbr
    //if(vorbis_encode_init(&state->vi,state->num_channels,sample_rate,-1,192000,-1)) // abr
    {
        printf("lexy_encoder_start(); error initializing vorbis encoder\n");
        return NULL;
    }
    
    vorbis_comment_init(&state->vc);
    vorbis_comment_add_tag(&state->vc, "ENCODER", "lexy-coder");
    
    vorbis_analysis_init(&state->vd, &state->vi);
    vorbis_block_init(&state->vd, &state->vb);
    
    ogg_packet vorbis_header, vorbis_header_comment, vorbis_header_code;
    
    // write out vorbis's headers
    vorbis_analysis_headerout(&state->vd, &state->vc, &vorbis_header, &vorbis_header_comment, &vorbis_header_code);
    
    ogg_stream_packetin(&state->os, &vorbis_header);
    ogg_stream_packetin(&state->os, &vorbis_header_comment);
    ogg_stream_packetin(&state->os, &vorbis_header_code);

    ogg_page og;

    // flush packet into its own page
    while(ogg_stream_flush(&state->os, &og))
        write_page(state, &og);

    return state;
}

// input should be more than 10ms long
extern "C" void lexy_encoder_write(tEncoderState* state, float* input_buffer_left, float* input_buffer_right, int num_samples)
{
    unsigned char* ogg_buffer = new unsigned char[state->rate];

    // get space in which to copy uncompressed data
    float** buffer = vorbis_analysis_buffer(&state->vd, num_samples);

    // copy non-interleaved channels
    for(int i = 0; i < num_samples; i ++) {
        buffer[0][i] = input_buffer_left[i];
        buffer[1][i] = input_buffer_right[i];
    }

    vorbis_analysis_wrote(&state->vd, num_samples);

    ogg_page og;    
    int num_packets = 0;

    while(vorbis_analysis_blockout(&state->vd, &state->vb) == 1)
    {
        vorbis_analysis(&state->vb, NULL);
        vorbis_bitrate_addblock(&state->vb);
        
        while(vorbis_bitrate_flushpacket(&state->vd, &state->op))
        {
            // push packet into ogg
            ogg_stream_packetin(&state->os, &state->op);
            num_packets++;
            
            // fetch page from ogg
            while(ogg_stream_pageout(&state->os, &og) || (state->op.e_o_s && ogg_stream_flush(&state->os, &og)))
            {
                printf("lexy_encoder_write(); writing ogg samples page after packet %i\n", num_packets);
                write_page(state, &og);
            }
        }
    }
}

// finish encoding
extern "C" void lexy_encoder_finish(tEncoderState* state)
{
    printf("lexy_encoder_finish(); ending stream\n");
    
    // write an end-of-stream packet
    vorbis_analysis_wrote(&state->vd, 0);
    
    ogg_page og;
    
    while(vorbis_analysis_blockout(&state->vd, &state->vb) == 1)
    {
        vorbis_analysis(&state->vb, NULL);
        vorbis_bitrate_addblock(&state->vb);
        
        while(vorbis_bitrate_flushpacket(&state->vd, &state->op))
        {
            ogg_stream_packetin(&state->os, &state->op);
            
            while(ogg_stream_flush(&state->os, &og))
                write_page(state, &og);
        }
    }
    
    printf("lexy_encoder_finish(); final encoded stream length: %i bytes\n", state->encoded_length);
    printf("lexy_encoder_finish(); cleaning up\n");
    
    ogg_stream_clear(&state->os);
    vorbis_block_clear(&state->vb);
    vorbis_dsp_clear(&state->vd);
    vorbis_comment_clear(&state->vc);
    vorbis_info_clear(&state->vi);
}

// grab buffer and its length
extern "C" unsigned char* lexy_get_buffer(tEncoderState* state)
{
    return state->encoded_buffer;
}

extern "C" int lexy_get_buffer_length(tEncoderState* state)
{
    return state->encoded_length;
}

// complete encoder test: init, encode, shutdown.
extern "C" tEncoderState* lexy_test()
{
    tEncoderState *state = lexy_encoder_start();
    
    // generate a test sound
    float* input_buffer_left = new float[state->sample_rate]; // one second long buffer
    float* input_buffer_right = new float[state->sample_rate]; // one second long buffer
    float test_frequency = 400; // hz

    for(int i = 0; i < state->sample_rate; i ++)
    {
        float fraction = (float) i / (float) state->sample_rate;
        input_buffer_left[i] =  sin(M_PI * 2 * test_frequency * fraction);
        input_buffer_right[i] =  sin(M_PI * 2 * test_frequency * fraction);
    }
    
    lexy_encoder_write(state, input_buffer_left, input_buffer_right, state->sample_rate);
    lexy_encoder_finish(state);
    return state;
}

// encodes a test signal
extern "C" void lexy_write_test(tEncoderState *state)
{
    printf("lexy_write_test(); writing test sound at %i samples/sec with %i channels\n", state->sample_rate, state->num_channels);

     // generate a test sound
    float* input_buffer_left = new float[state->sample_rate]; // one second long buffer
    float* input_buffer_right = new float[state->sample_rate]; // one second long buffer
    float test_frequency = 400; // hz

    for(int i = 0; i < state->sample_rate; i ++)
    {
        float fraction = (float) i / (float) state->sample_rate;
        input_buffer_left[i] =  sin(M_PI * 2 * test_frequency * fraction);
        input_buffer_right[i] =  sin(M_PI * 2 * test_frequency * fraction);
    }
    
    lexy_encoder_write(state, input_buffer_left, input_buffer_right, state->sample_rate);
}

// for testing in console
extern "C" int main()
{
    lexy_test();

    return 0;
}
