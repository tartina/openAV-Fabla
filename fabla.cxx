
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "uris.hxx"

// include faust stuff
#include "../dsp/cpp_ui.h"
#include "../dsp/reverb/reverb.cpp"

/**
   Print an error message to the host log if available, or stderr otherwise.
*/
LV2_LOG_FUNC(3, 4)
static void
print(Fabla* self, LV2_URID type, const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  if (self->log) {
    self->log->vprintf(self->log->handle, type, fmt, args);
  } else {
    vfprintf(stderr, fmt, args);
  }
  va_end(args);
}

/**
   An atom-like message used internally to apply/free samples.

   This is only used internally to communicate with the worker, it is never
   sent to the outside world via a port since it is not POD.  It is convenient
   to use an Atom header so actual atoms can be easily sent through the same
   ringbuffer.
*/
typedef struct {
  LV2_Atom atom;
  int      sampleNum;
  Sample*  sample;
} SampleMessage;

/**
   Load a new sample and return it.

   Since this is of course not a real-time safe action, this is called in the
   worker thread only.  The sample is loaded and returned only, plugin state is
   not modified.
*/
static SampleMessage*
load_sample(Fabla* self, int sampleNum, const char* path)
{
  const size_t path_len  = strlen(path);

  print(self, self->uris.log_Error,
        "Loading sample %s to pad number %i\n", path, sampleNum);

  SampleMessage* sampleMessage  = (SampleMessage*)malloc(sizeof(SampleMessage));
  Sample* const  sample  = (Sample*)malloc(sizeof(Sample));
  SF_INFO* const info    = &sample->info;
  SNDFILE* const sndfile = sf_open(path, SFM_READ, info);

  if (!sndfile || !info->frames || (info->channels != 2)) {
    print(self, self->uris.log_Error,
          "Stereo sample '%s'\n", path);
  }
  else if (!sndfile || !info->frames || (info->channels != 1)) {
    print(self, self->uris.log_Error,
          "Failed to open sample '%s'.\n", path);
    free(sample);
    return NULL;
  }

  /* Read data */
  float* const data = (float*)malloc(sizeof(float) * info->frames);
  if (!data)
  {
    print(self, self->uris.log_Error,
          "Failed to allocate memory for sample.\n");
    return NULL;
  }
  sf_seek(sndfile, 0ul, SEEK_SET);
  sf_read_float(sndfile, data, info->frames);
  sf_close(sndfile);

  /* Fill sample struct and return it. */
  sample->data     = data;
  sample->path     = (char*)malloc(path_len + 1);
  sample->path_len = path_len;
  memcpy(sample->path, path, path_len + 1);
  
  sampleMessage->sampleNum = sampleNum;
  sampleMessage->sample = sample;
  
  return sampleMessage;
}

static void
free_sample(Fabla* self, Sample* sample)
{
  if (sample) {
    print(self, self->uris.log_Trace, "Freeing %s\n", sample->path);
    free(sample->path);
    free(sample->data);
    free(sample);
  }
}

/**
   Do work in a non-realtime thread.

   This is called for every piece of work scheduled in the audio thread using
   self->schedule->schedule_work().  A reply can be sent back to the audio
   thread using the provided respond function.
*/
static LV2_Worker_Status
work(LV2_Handle                  instance,
     LV2_Worker_Respond_Function respond,
     LV2_Worker_Respond_Handle   handle,
     uint32_t                    size,
     const void*                 data)
{
  Fabla*  self = (Fabla*)instance;
  LV2_Atom* atom = (LV2_Atom*)data;
  
  if (atom->type == self->uris.eg_freeSample)
  {
    g_mutex_lock( &self->sampleMutex );
    {
      print(self, self->uris.log_Error, "Freeing sample now\n" );
      // lock mutex, then work with sample, as GUI might be drawing it!
      SampleMessage* msg = (SampleMessage*)data;
      free_sample(self, msg->sample);
    }
    g_mutex_unlock( &self->sampleMutex );
  }
  else
  {
    /* Handle set message (load sample). */
    LV2_Atom_Object* obj = (LV2_Atom_Object*)data;

    /* Get file path from message */
    const LV2_Atom_Int* sampleNum = read_set_file_sample_number(&self->uris, obj);
    const LV2_Atom* file_path = read_set_file(&self->uris, obj);
    
    if (!file_path) {
      return LV2_WORKER_ERR_UNKNOWN;
    }
    
    int padNum = sampleNum->body;
    
    /* Load sample. */
    SampleMessage* sampleMessage = load_sample(self, padNum, (const char*)LV2_ATOM_BODY(file_path) );
    
    if (sampleMessage) {
      /* Loaded sample, send it to run() to be applied. */
      respond(handle, sizeof(SampleMessage), &sampleMessage);
    }
  }

  return LV2_WORKER_SUCCESS;
}

/**
   Handle a response from work() in the audio thread.

   When running normally, this will be called by the host after run().  When
   freewheeling, this will be called immediately at the point the work was
   scheduled.
*/
static LV2_Worker_Status
work_response(LV2_Handle  instance,
              uint32_t    size,
              const void* data)
{
  Fabla* self = (Fabla*)instance;
  
  int sampleNum = 0;
  
  Sample* freeOldSample = 0;
  
  // lock the Sample array mutex, so GUI can't draw while we update contents
  g_mutex_lock( &self->sampleMutex );
  {
    
    // Get details from the message
    SampleMessage* message =  *(SampleMessage**)data;
    sampleNum = message->sampleNum;
    
    // check if there's currently a sample loaded on the pad
    // this gets used later to see if we need to de-allocate the old sample
    freeOldSample = self->sample[sampleNum];
    
    // point to the new sample
    self->sample[sampleNum] = message->sample;
    
    // set the "playback" of the current sample past the end by a frame:
    // stops the sample from playing just after being loaded
    self->playback[sampleNum].frame = message->sample->info.frames + 1;
    
    // Send a notification that we're using a new sample
    lv2_atom_forge_frame_time(&self->forge, self->frame_offset);
    write_set_file(&self->forge, &self->uris,
                   sampleNum,
                   self->sample[sampleNum]->path,
                   self->sample[sampleNum]->path_len);
  }
  g_mutex_unlock( &self->sampleMutex );
  
  
  
  // now check if we currently have a sample loaded on this pad:
  // we have the pad number from the load message earlier, and we've just
  // *UNLOCKED* the mutex: In freewheeling the worker thread gets called
  // immidiatly, so we have to ensure its unlocked before getting locked
  // again, since the GMutex is *NOT* defined to be a recursive mutex
  if ( freeOldSample )
  {
    // send worker to free the current sample, 
      
    SampleMessage msg = { { sizeof(Sample*), self->uris.eg_freeSample },
                        sampleNum,
                        freeOldSample };
    
    self->schedule->schedule_work(self->schedule->handle, sizeof(msg), &msg);
  }
  
  return LV2_WORKER_SUCCESS;
}

static void
connect_port(LV2_Handle instance,
             uint32_t   port,
             void*      data)
{
  Fabla* self = (Fabla*)instance;
  switch (port)
  {
    case SAMPLER_CONTROL:
      self->control_port = (LV2_Atom_Sequence*)data;
      break;
    case SAMPLER_RESPONSE:
      self->notify_port = (LV2_Atom_Sequence*)data;
      break;
    case SAMPLER_REVERB_SIZE:
      self->reverb_size = (float*)data;
      self->faust_reverb_size = self->reverbUI->getFloatPointer("---FreeverbRoomSize");
      break;
    case SAMPLER_REVERB_WET:
      self->reverb_wet = (float*)data;
      self->faust_reverb_wet  = self->reverbUI->getFloatPointer("---FreeverbWet");
      break;
    case SAMPLER_OUT_L:
      self->output_port_L = (float*)data;
      break;
    case SAMPLER_OUT_R:
      self->output_port_R = (float*)data;
      break;
    case SAMPLER_MASTER_VOL:
      self->master_vol = (float*)data;
      break;
    default:
      break;
  }
}

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               path,
            const LV2_Feature* const* features)
{
  /* Allocate and initialise instance structure. */
  Fabla* self = (Fabla*)malloc(sizeof(Fabla));
  if (!self) {
    return NULL;
  }
  memset(self, 0, sizeof(Fabla));
  
  g_mutex_init( &self->sampleMutex );
  
  /* Get host features */
  for (int i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID__map)) {
      self->map = (LV2_URID_Map*)features[i]->data;
    } else if (!strcmp(features[i]->URI, LV2_WORKER__schedule)) {
      self->schedule = (LV2_Worker_Schedule*)features[i]->data;
    } else if (!strcmp(features[i]->URI, LV2_LOG__log)) {
      self->log = (LV2_Log_Log*)features[i]->data;
    }
  }
  
  bool haveFeatures = true;
  
  if (!self->map) {
    print(self, self->uris.log_Error, "Missing feature urid:map.\n");
    haveFeatures = false;
  } else if (!self->schedule) {
    print(self, self->uris.log_Error, "Missing feature work:schedule.\n");
    haveFeatures = false;
  }
  
  if ( haveFeatures )
  {
    /* Map URIs and initialise forge */
    map_sampler_uris(self->map, &self->uris);
    lv2_atom_forge_init(&self->forge, self->map);
    
    // Set up the Faust DSP units
    char* nullArray[0];
    self->reverbUI = new CppUI(0, nullArray);
    self->reverbDSP = new ReverbDSP();
    self->reverbDSP->buildUserInterface(self->reverbUI);
    self->reverbDSP->init(rate);
    
    return (LV2_Handle)self;
  }
  
  free(self);
  return 0;
}

static void
cleanup(LV2_Handle instance)
{
  Fabla* self = (Fabla*)instance;
  free_sample(self, self->sample[0] );
  free(self);
}

static void
run(LV2_Handle instance,
    uint32_t   sample_count)
{
  Fabla*     self        = (Fabla*)instance;
  FablaURIs* uris        = &self->uris;
  sf_count_t   start_frame = 0;
  sf_count_t   pos         = 0;
  float*       output_L    = self->output_port_L;
  float*       output_R    = self->output_port_R;
  
  /* Set up forge to write directly to notify output port. */
  const uint32_t notify_capacity = self->notify_port->atom.size;
  lv2_atom_forge_set_buffer(&self->forge,
                            (uint8_t*)self->notify_port,
                            notify_capacity);

  /* Start a sequence in the notify output port. */
  lv2_atom_forge_sequence_head(&self->forge, &self->notify_frame, 0);
  
  /* Read incoming events */
  LV2_ATOM_SEQUENCE_FOREACH(self->control_port, ev)
  {
    self->frame_offset = ev->time.frames;
    
    if (ev->body.type == uris->midi_Event) // MIDI event on Atom port
    {
      uint8_t* const data = (uint8_t* const)(ev + 1);
      if ( (data[0] & 0xF0) == 0x90 ) // event & channel
      {
        if ( data[1] >= 36 ) // note
        {
          start_frame = ev->time.frames;
          self->playback[data[1]-36].frame = 0;
          self->playback[data[1]-36].play  = true;
          self->playback[data[1]-36].volume= (data[2] / 127.f);
        }
      }
    }
    else if (is_object_type(uris, ev->body.type) )
    {
      const LV2_Atom_Object* obj = (LV2_Atom_Object*)&ev->body;
      
      if (obj->body.otype == uris->patch_Set)
      {
        /* Received a set message, send it to the worker. */
        print(self, self->uris.log_Error, "Queueing set message\n");
        self->schedule->schedule_work(self->schedule->handle,
                                      lv2_atom_total_size(&ev->body),
                                      &ev->body);
      }
      else
      {
        print(self, self->uris.log_Trace,
              "Unknown object type %d\n", obj->body.otype);
      }
    }
    else
    {
      print(self, self->uris.log_Trace,
            "Unknown event type %d\n", ev->body.type);
    }
  }
  
  // copy the effect port values to FAUST variables
  *self->faust_reverb_wet     = *self->reverb_wet;
  *self->faust_reverb_size    = *self->reverb_size;
  
  float outL, outR;
  
  // nframes
  for (int i = 0; i < sample_count; i++)
  {
    float tmp = 1e-15;  // DC offset: float denormals
    
    // pads
    for ( int p = 0; p < 16; p++ )
    {
      // sample loaded && playing
      if ( self->sample[p] && self->playback[p].play ) // check sample loaded
      {
        // add sample
        if ( self->playback[p].frame < self->sample[p]->info.frames )
        {
          tmp += self->sample[p]->data[self->playback[p].frame++] // sample value
                  * self->playback[p].volume;                     // master volume
        }
        else // stop sample
        {
          self->playback[p].play  = false;
          self->playback[p].frame = 0;
        }
      }
    } // pads
    
    // compute faust units
    float* buf[3];
    buf[0] = &tmp;
    buf[1] = &outL;
    buf[2] = &outR;
    self->reverbDSP->compute( 1, &buf[0], &buf[1] );
    
    // write output
    output_L[i] = outL * (*self->master_vol);
    output_R[i] = outR * (*self->master_vol);
  }
  
}

static LV2_State_Status
save(LV2_Handle                instance,
     LV2_State_Store_Function  store,
     LV2_State_Handle          handle,
     uint32_t                  flags,
     const LV2_Feature* const* features)
{
  LV2_State_Map_Path* map_path = NULL;
  for (int i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_STATE__mapPath)) {
      map_path = (LV2_State_Map_Path*)features[i]->data;
    }
  }

  Fabla* self  = (Fabla*)instance;
  
  
  if ( self->sample[0] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[0]->path);
    store(handle, self->uris.sampleRestorePad1, apath, strlen(self->sample[0]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[1] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[1]->path);
    store(handle, self->uris.sampleRestorePad2, apath, strlen(self->sample[1]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[2] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[2]->path);
    store(handle, self->uris.sampleRestorePad3, apath, strlen(self->sample[2]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[3] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[3]->path);
    store(handle, self->uris.sampleRestorePad4, apath, strlen(self->sample[3]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[4] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[4]->path);
    store(handle, self->uris.sampleRestorePad5, apath, strlen(self->sample[4]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[5] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[5]->path);
    store(handle, self->uris.sampleRestorePad6, apath, strlen(self->sample[5]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[6] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[6]->path);
    store(handle, self->uris.sampleRestorePad7, apath, strlen(self->sample[6]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[7] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[7]->path);
    store(handle, self->uris.sampleRestorePad8, apath, strlen(self->sample[7]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[8] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[8]->path);
    store(handle, self->uris.sampleRestorePad9, apath, strlen(self->sample[8]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[9] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[9]->path);
    store(handle, self->uris.sampleRestorePad10, apath, strlen(self->sample[9]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[10] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[10]->path);
    store(handle, self->uris.sampleRestorePad11, apath, strlen(self->sample[10]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[11] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[11]->path);
    store(handle, self->uris.sampleRestorePad12, apath, strlen(self->sample[11]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[12] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[12]->path);
    store(handle, self->uris.sampleRestorePad13, apath, strlen(self->sample[12]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[13] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[13]->path);
    store(handle, self->uris.sampleRestorePad14, apath, strlen(self->sample[13]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[14] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[14]->path);
    store(handle, self->uris.sampleRestorePad15, apath, strlen(self->sample[14]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  if ( self->sample[15] )
  {
    char* apath = map_path->abstract_path(map_path->handle, self->sample[15]->path);
    store(handle, self->uris.sampleRestorePad16, apath, strlen(self->sample[15]->path) + 1, self->uris.atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(apath);
  }
  
  return LV2_STATE_SUCCESS;
}

static LV2_State_Status
restore(LV2_Handle                  instance,
        LV2_State_Retrieve_Function retrieve,
        LV2_State_Handle            handle,
        uint32_t                    flags,
        const LV2_Feature* const*   features)
{
  Fabla* self = (Fabla*)instance;

  size_t   size;
  uint32_t type;
  uint32_t valflags;

  const void* value = retrieve( handle, self->uris.sampleRestorePad1, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[0] ) {
      free_sample(self, self->sample[0] ); }
    SampleMessage* message = load_sample(self, 0, path);
    self->sample[0] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad2, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[1] ) {
      free_sample(self, self->sample[1] ); }
    SampleMessage* message = load_sample(self, 1, path);
    self->sample[1] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad3, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[2] ) {
      free_sample(self, self->sample[2] ); }
    SampleMessage* message = load_sample(self, 2, path);
    self->sample[2] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad4, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[3] ) {
      free_sample(self, self->sample[3] ); }
    SampleMessage* message = load_sample(self, 3, path);
    self->sample[3] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad5, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[4] ) {
      free_sample(self, self->sample[4] ); }
    SampleMessage* message = load_sample(self, 4, path);
    self->sample[4] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad6, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[5] ) {
      free_sample(self, self->sample[5] ); }
    SampleMessage* message = load_sample(self, 5, path);
    self->sample[5] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad7, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[6] ) {
      free_sample(self, self->sample[6] ); }
    SampleMessage* message = load_sample(self, 6, path);
    self->sample[6] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad8, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[7] ) {
      free_sample(self, self->sample[7] ); }
    SampleMessage* message = load_sample(self, 7, path);
    self->sample[7] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad9, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[8] ) {
      free_sample(self, self->sample[8] ); }
    SampleMessage* message = load_sample(self, 8, path);
    self->sample[8] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad10, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[9] ) {
      free_sample(self, self->sample[9] ); }
    SampleMessage* message = load_sample(self, 9, path);
    self->sample[9] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad11, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[10] ) {
      free_sample(self, self->sample[10] ); }
    SampleMessage* message = load_sample(self, 10, path);
    self->sample[10] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad12, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[11] ) {
      free_sample(self, self->sample[11] ); }
    SampleMessage* message = load_sample(self, 11, path);
    self->sample[11] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad13, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[12] ) {
      free_sample(self, self->sample[12] ); }
    SampleMessage* message = load_sample(self, 12, path);
    self->sample[12] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad14, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[13] ) {
      free_sample(self, self->sample[13] ); }
    SampleMessage* message = load_sample(self, 13, path);
    self->sample[13] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad15, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[14] ) {
      free_sample(self, self->sample[14] ); }
    SampleMessage* message = load_sample(self, 14, path);
    self->sample[14] = message->sample;
  }
  value = retrieve( handle, self->uris.sampleRestorePad16, &size, &type, &valflags);
  if (value) {
    const char* path = (const char*)value;
    print(self, self->uris.log_Trace, "Restoring file %s\n", path);
    if ( self->sample[15] ) {
      free_sample(self, self->sample[15] ); }
    SampleMessage* message = load_sample(self, 15, path);
    self->sample[15] = message->sample;
  }

  return LV2_STATE_SUCCESS;
}

static const void*
extension_data(const char* uri)
{
  static const LV2_State_Interface  state  = { save, restore };
  static const LV2_Worker_Interface worker = { work, work_response, NULL };
  if (!strcmp(uri, LV2_STATE__interface)) {
    return &state;
  } else if (!strcmp(uri, LV2_WORKER__interface)) {
    return &worker;
  }
  return NULL;
}

static const LV2_Descriptor descriptor = {
  FABLA_URI,
  instantiate,
  connect_port,
  NULL,  // activate,
  run,
  NULL,  // deactivate,
  cleanup,
  extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
  switch (index) {
  case 0:
    return &descriptor;
  default:
    return NULL;
  }
}
