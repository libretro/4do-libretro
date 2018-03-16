#ifndef _MSC_VER
#include <stdbool.h>
#include <sched.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#pragma pack(1)
#endif

#include <libretro.h>
#include <streams/file_stream.h>
#include <file/file_path.h>

#include "cuefile.h"
#include "nvram.h"
#include "retro_callbacks.h"

#include "libfreedo/freedocore.h"
#include "libfreedo/IsoXBUS.h"
#include "libfreedo/frame.h"
#include "libfreedo/quarz.h"

extern int ARM_CLOCK;

#define TEMP_BUFFER_SIZE 512
#define ROM1_SIZE 1 * 1024 * 1024
#define ROM2_SIZE 933636 /* was 1 * 1024 * 1024, */

#define INPUTBUTTONL     (1<<4)
#define INPUTBUTTONR     (1<<5)
#define INPUTBUTTONX     (1<<6)
#define INPUTBUTTONP     (1<<7)
#define INPUTBUTTONC     (1<<8)
#define INPUTBUTTONB     (1<<9)
#define INPUTBUTTONA     (1<<10)
#define INPUTBUTTONLEFT  (1<<11)
#define INPUTBUTTONRIGHT (1<<12)
#define INPUTBUTTONUP    (1<<13)
#define INPUTBUTTONDOWN  (1<<14)

typedef struct
{
   int buttons; /* buttons bitfield */
}inputState;

inputState internal_input_state[6];

static char biosPath[1024];
static struct VDLFrame *frame;

extern int HightResMode;
extern unsigned int _3do_SaveSize(void);
extern void _3do_Save(void *buff);
extern bool _3do_Load(void *buff);
extern void* Getp_NVRAM();

static int cd_sector_size;
static int cd_sector_offset;

static RFILE *fcdrom;
static int currentSector;

static uint32_t *videoBuffer;
static int videoWidth, videoHeight;
static int32_t sampleBuffer[TEMP_BUFFER_SIZE];
static unsigned int sampleCurrent;

static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_audio_sample_batch_t audio_batch_cb;

void retro_set_environment(retro_environment_t cb)
{
   struct retro_vfs_interface_info vfs_iface_info;
   static const struct retro_variable vars[] = {
      { "4do_cpu_overclock",        "CPU overclock; 1x|2x|4x" },
      { "4do_high_resolution",      "High Resolution; disabled|enabled" },
      { "4do_nvram_storage",        "NVRAM Storage; per game|shared" },
      { "4do_hack_timing_1",        "Timing Hack 1 (Crash 'n Burn); disabled|enabled" },
      { "4do_hack_timing_3",        "Timing Hack 3 (Dinopark Tycoon); disabled|enabled" },
      { "4do_hack_timing_5",        "Timing Hack 5 (Microcosm); disabled|enabled" },
      { "4do_hack_timing_6",        "Timing Hack 6 (Alone in the Dark); disabled|enabled" },
      { "4do_hack_graphics_step_y", "Graphics Step Y Hack (Samurai Shodown); disabled|enabled" },
      { NULL, NULL },
   };

   retro_set_environment_cb(cb);

   retro_environment_cb(RETRO_ENVIRONMENT_SET_VARIABLES,(void*)vars);

   vfs_iface_info.required_interface_version = 1;
   vfs_iface_info.iface                      = NULL;
   if (retro_environment_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
     filestream_vfs_init(&vfs_iface_info);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

static void fsReadBios(const char *bios_path, void *prom)
{
   long fsize;
   int readcount;
   RFILE *bios1 = filestream_open(bios_path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!bios1)
      return;

   filestream_seek(bios1, 0, RETRO_VFS_SEEK_POSITION_END);
   fsize = filestream_tell(bios1);
   filestream_rewind(bios1);
   readcount = filestream_read(bios1, prom, fsize);
   (void)readcount;

   filestream_close(bios1);
}

static void fsDetectCDFormat(const char *path, cueFile *cue_file)
{
   CD_format cd_format;

   if (cue_file)
   {
      cd_format = cue_file->cd_format;
      retro_log_printf_cb(RETRO_LOG_INFO, "[4DO]: File format from cue file resolved to %s", cue_get_cd_format_name(cd_format));
   }
   else
   {
      int size = 0;
      RFILE *fp = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);
      if (fp)
      {
         filestream_seek(fp, 0L, RETRO_VFS_SEEK_POSITION_END);
         size = filestream_tell(fp);
         filestream_close(fp);
      }
      if (size % SECTOR_SIZE_2048 == 0) /* most standard guess first */
         cd_format = MODE1_2048;
      else if (size % SECTOR_SIZE_2352 == 0)
         cd_format = MODE1_2352;
      else
      {
         cd_format = MODE1_2048;
         retro_log_printf_cb(RETRO_LOG_INFO, "[4DO]: File format cannot be detected, using default");
      }
      retro_log_printf_cb(RETRO_LOG_INFO, "[4DO]: File format guessed by file size is %s", cue_get_cd_format_name(cd_format));
   }

   switch (cd_format)
   {
      case MODE1_2048:
         cd_sector_size = SECTOR_SIZE_2048;
         cd_sector_offset = SECTOR_OFFSET_MODE1_2048;
         break;
      case MODE1_2352:
         cd_sector_size = SECTOR_SIZE_2352;
         cd_sector_offset = SECTOR_OFFSET_MODE1_2352;
         break;
      case MODE2_2352:
         cd_sector_size = SECTOR_SIZE_2352;
         cd_sector_offset = SECTOR_OFFSET_MODE2_2352;
         break;
      case CUE_MODE_UNKNOWN:
         break;
   }

   retro_log_printf_cb(RETRO_LOG_INFO, "[4DO]: Using sector size %i offset %i", cd_sector_size, cd_sector_offset);
}


static int fsOpenIso(const char *path)
{
   const char *cd_image_path = NULL;
   cueFile *cue_file         = cue_get(path);

   fsDetectCDFormat(path, cue_file);

   cd_image_path             = cue_is_cue_path(path) ? cue_file->cd_image : path;
   fcdrom                    = filestream_open(cd_image_path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);

   free(cue_file);

   if(!fcdrom)
      return 0;

   return 1;
}

static int fsCloseIso(void)
{
   filestream_close(fcdrom);
   return 1;
}

static int fsReadBlock(void *buffer, int sector)
{
   filestream_seek(fcdrom, (cd_sector_size * sector) + cd_sector_offset,
         RETRO_VFS_SEEK_POSITION_START);
   filestream_read(fcdrom, buffer, SECTOR_SIZE_2048);
   filestream_rewind(fcdrom);

   return 1;
}

static char *fsReadSize(void)
{
   char *buffer = (char *)malloc(sizeof(char) * 4);

   filestream_rewind(fcdrom);
   filestream_seek(fcdrom, 80 + cd_sector_offset,
         RETRO_VFS_SEEK_POSITION_START);
   filestream_read(fcdrom, buffer, 4);
   filestream_rewind(fcdrom);

   return buffer;
}

static unsigned int fsReadDiscSize(void)
{
   unsigned int size;
   unsigned int temp;
   char *ssize = fsReadSize();

   memcpy(&temp, ssize, 4);

   free(ssize);
   size = (temp & 0x000000FFU) << 24 | (temp & 0x0000FF00U) << 8 |
      (temp & 0x00FF0000U) >> 8 | (temp & 0xFF000000U) >> 24;

   retro_log_printf_cb(RETRO_LOG_INFO, "[4DO]: disc size: %d sectors\n", size);

   return size;
}

static void initVideo(void)
{
   if (!videoBuffer)
      videoBuffer = (uint32_t*)malloc(640 * 480 * 4);

   if (!frame)
      frame = (struct VDLFrame*)malloc(sizeof(struct VDLFrame));

   memset(frame, 0, sizeof(struct VDLFrame));
}

/* Input helper functions */
static int CheckDownButton(int deviceNumber,int button)
{
   if(internal_input_state[deviceNumber].buttons&button)
      return 1;
   return 0;
}

static char CalculateDeviceLowByte(int deviceNumber)
{
   char returnValue = 0;

   returnValue |= 0x01 & 0; /* unknown */
   returnValue |= 0x02 & 0; /* unknown */
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONL) ? (char)0x04 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONR) ? (char)0x08 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONX) ? (char)0x10 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONP) ? (char)0x20 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONC) ? (char)0x40 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONB) ? (char)0x80 : (char)0;

   return returnValue;
}

static char CalculateDeviceHighByte(int deviceNumber)
{
   char returnValue = 0;

   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONA)     ? (char)0x01 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONLEFT)  ? (char)0x02 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONRIGHT) ? (char)0x04 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONUP)    ? (char)0x08 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONDOWN)  ? (char)0x10 : (char)0;
   returnValue |= 0x20 & 0; /* unknown */
   returnValue |= 0x40 & 0; /* unknown */
   returnValue |= 0x80;     /* This last bit seems to indicate power and/or connectivity. */

   return returnValue;
}

/* libfreedo callback */
static void *fdcCallback(int procedure, void *data)
{
   switch(procedure)
   {
      case EXT_READ_ROMS:
         fsReadBios(biosPath, data);
         break;
      case EXT_SWAPFRAME:
        Get_Frame_Bitmap(frame, videoBuffer, videoWidth, videoHeight);
         return frame;
      case EXT_PUSH_SAMPLE:
         /* TODO: fix all this, not right */
         sampleBuffer[sampleCurrent] = (uintptr_t)data;
         sampleCurrent++;
         if(sampleCurrent >= TEMP_BUFFER_SIZE)
         {
            sampleCurrent = 0;
            audio_batch_cb((int16_t *)sampleBuffer, TEMP_BUFFER_SIZE);
         }
         break;
      case EXT_GET_PBUSLEN:
         return (void*)16;
      case EXT_GETP_PBUSDATA:
         {
            /* Set up raw data to return */
            unsigned char *pbusData = (unsigned char *)
               malloc(sizeof(unsigned char) * 16);

            pbusData[0x0] = 0x00;
            pbusData[0x1] = 0x48;
            pbusData[0x2] = CalculateDeviceLowByte(0);
            pbusData[0x3] = CalculateDeviceHighByte(0);
            pbusData[0x4] = CalculateDeviceLowByte(2);
            pbusData[0x5] = CalculateDeviceHighByte(2);
            pbusData[0x6] = CalculateDeviceLowByte(1);
            pbusData[0x7] = CalculateDeviceHighByte(1);
            pbusData[0x8] = CalculateDeviceLowByte(4);
            pbusData[0x9] = CalculateDeviceHighByte(4);
            pbusData[0xA] = CalculateDeviceLowByte(3);
            pbusData[0xB] = CalculateDeviceHighByte(3);
            pbusData[0xC] = 0x00;
            pbusData[0xD] = 0x80;
            pbusData[0xE] = CalculateDeviceLowByte(5);
            pbusData[0xF] = CalculateDeviceHighByte(5);

            return pbusData;
         }
      case EXT_KPRINT:
         break;
      case EXT_FRAMETRIGGER_MT:
         _freedo_Interface(FDP_DO_FRAME_MT, frame);
         break;
      case EXT_READ2048:
         fsReadBlock(data, currentSector);
         break;
      case EXT_GET_DISC_SIZE:
         return (void *)(intptr_t)fsReadDiscSize();
      case EXT_ON_SECTOR:
         currentSector = *((int*)&data);
         break;
      case EXT_ARM_SYNC:
#if 0
         printf("fdcCallback EXT_ARM_SYNC\n");
#endif
         break;

      default:
         break;
   }
   return (void*)0;
}

static void update_input(void)
{
   unsigned i;
   if (!input_poll_cb)
      return;

   input_poll_cb();

   /* Can possibly support up to 6 players but is currently set for 2 */
   for (i = 0; i < 2; i++)
   {
      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
         internal_input_state[i].buttons |= INPUTBUTTONUP;
      else
         internal_input_state[i].buttons &= ~INPUTBUTTONUP;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
         internal_input_state[i].buttons |= INPUTBUTTONDOWN;
      else
         internal_input_state[i].buttons &= ~INPUTBUTTONDOWN;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
         internal_input_state[i].buttons |= INPUTBUTTONLEFT;
      else
         internal_input_state[i].buttons &= ~INPUTBUTTONLEFT;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
         internal_input_state[i].buttons |= INPUTBUTTONRIGHT;
      else
         internal_input_state[i].buttons &= ~INPUTBUTTONRIGHT;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))
         internal_input_state[i].buttons |= INPUTBUTTONA;
      else
         internal_input_state[i].buttons &= ~INPUTBUTTONA;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
         internal_input_state[i].buttons |= INPUTBUTTONB;
      else
         internal_input_state[i].buttons &= ~INPUTBUTTONB;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
         internal_input_state[i].buttons |= INPUTBUTTONC;
      else
         internal_input_state[i].buttons &= ~INPUTBUTTONC;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))
         internal_input_state[i].buttons |= INPUTBUTTONL;
      else
         internal_input_state[i].buttons &= ~INPUTBUTTONL;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))
         internal_input_state[i].buttons |= INPUTBUTTONR;
      else
         internal_input_state[i].buttons &= ~INPUTBUTTONR;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))
         internal_input_state[i].buttons |= INPUTBUTTONP;
      else
         internal_input_state[i].buttons &= ~INPUTBUTTONP;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
         internal_input_state[i].buttons |= INPUTBUTTONX;
      else
         internal_input_state[i].buttons &= ~INPUTBUTTONX;
   }
}

/************************************
 * libretro implementation
 ************************************/

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name = "4DO";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version = "1.3.2.4" GIT_VERSION;
   info->need_fullpath = true;
   info->valid_extensions = "iso|img|bin|cue";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps            = 60;
   info->timing.sample_rate    = 44100;
   info->geometry.base_width   = videoWidth;
   info->geometry.base_height  = videoHeight;
   info->geometry.max_width    = videoWidth;
   info->geometry.max_height   = videoHeight;
   info->geometry.aspect_ratio = 4.0 / 3.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

size_t retro_serialize_size(void)
{
   return _3do_SaveSize();
}

bool retro_serialize(void *data, size_t size)
{
  if(size < _3do_SaveSize())
    return false;

  _3do_Save(data);

  return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   _3do_Load((void*)data);
   return true;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

static
bool
environ_enabled(const char *key)
{
  int rv;
  struct retro_variable var;

  var.key   = key;
  var.value = NULL;
  rv = retro_environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&var);
  if(rv && var.value)
    return (strcmp(var.value,"enabled") == 0);

  return false;
}

static
void
check_env_4do_high_resolution(void)
{
  if(environ_enabled("4do_high_resolution"))
    {
      HightResMode = 1;
      videoWidth   = 640;
      videoHeight  = 480;
    }
  else
    {
      HightResMode = 0;
      videoWidth   = 320;
      videoHeight  = 240;
    }
}

static
void
check_env_4do_cpu_overclock(void)
{
  int                   rv;
  struct retro_variable var;

  ARM_CLOCK = ARM_FREQUENCY;

  var.key   = "4do_cpu_overclock";
  var.value = NULL;

  rv = retro_environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&var);
  if(rv && var.value)
    {
      if (!strcmp(var.value, "1x"))
        ARM_CLOCK = ARM_FREQUENCY;
      else if (!strcmp(var.value, "2x"))
        ARM_CLOCK = ARM_FREQUENCY * 2;
      else if (!strcmp(var.value, "4x"))
        ARM_CLOCK = ARM_FREQUENCY * 4;
    }
}

static
void
check_env_set_reset_bits(const char *key,
                         int        *input,
                         int         bitmask)
{
  *input = (environ_enabled(key) ?
            (*input | bitmask) :
            (*input & ~bitmask));
}

static
bool
check_env_nvram_per_game(void)
{
  int rv;
  struct retro_variable var;

  var.key   = "4do_nvram_storage";
  var.value = NULL;

  rv = retro_environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&var);
  if(rv && var.value)
    {
      if(strcmp(var.value,"per game"))
        return false;
    }

  return true;
}

static
bool
check_env_nvram_shared(void)
{
  return !check_env_nvram_per_game();
}

static
void
check_variables(void)
{
   check_env_4do_high_resolution();
   check_env_4do_cpu_overclock();
   check_env_set_reset_bits("4do_hack_timing_1",&fixmode,FIX_BIT_TIMING_1);
   check_env_set_reset_bits("4do_hack_timing_3",&fixmode,FIX_BIT_TIMING_3);
   check_env_set_reset_bits("4do_hack_timing_5",&fixmode,FIX_BIT_TIMING_5);
   check_env_set_reset_bits("4do_hack_timing_6",&fixmode,FIX_BIT_TIMING_6);
   check_env_set_reset_bits("4do_hack_graphics_step_y",&fixmode,FIX_BIT_GRAPHICS_STEP_Y);
}

bool retro_load_game(const struct retro_game_info *info)
{
   enum retro_pixel_format fmt          = RETRO_PIXEL_FORMAT_XRGB8888;
   const char *system_directory_c       = NULL;
   const char *full_path                = NULL;
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "C" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "X (Stop)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "P (Play/Pause)" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "C" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "X (Stop)" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "P (Play/Pause)" },

      { 0 },
   };

   if (!info)
      return false;

   retro_environment_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   if (!retro_environment_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      if (retro_log_printf_cb)
         retro_log_printf_cb(RETRO_LOG_INFO, "[4DO]: XRGB8888 is not supported.\n");
      return false;
   }

   currentSector = 0;
   sampleCurrent = 0;
   memset(sampleBuffer, 0, sizeof(int32_t) * TEMP_BUFFER_SIZE);

   full_path = info->path;

   *biosPath = '\0';

   if (!fsOpenIso(full_path))
      return false;

   retro_environment_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory_c);
   if (!system_directory_c)
   {
      if (retro_log_printf_cb)
         retro_log_printf_cb(RETRO_LOG_WARN, "[4DO]: no system directory defined, unable to look for panafz10.bin\n");
   }
   else
   {
      char bios_path[1024];
      RFILE *fp;
#ifdef _WIN32
      char slash = '\\';
#else
      char slash = '/';
#endif
      sprintf(bios_path, "%s%c%s", system_directory_c, slash, "panafz10.bin");

      fp = filestream_open(bios_path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);

      if (!fp)
      {
         if (retro_log_printf_cb)
            retro_log_printf_cb(RETRO_LOG_WARN, "[4DO]: panafz10.bin not found, cannot load BIOS\n");
         return false;
      }

      filestream_close(fp);
      strcpy(biosPath, bios_path);
   }

   /* Initialize libfreedo */
   check_variables();
   initVideo();
   _freedo_Interface(FDP_INIT, (void*)*fdcCallback);

   /* XXX: Is this really a frontend responsibility? */
   nvram_init(Getp_NVRAM());
   if(check_env_nvram_shared())
     retro_nvram_load();

   return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
   (void)game_type;
   (void)info;
   (void)num_info;
   return false;
}

void retro_unload_game(void)
{
   if(check_env_nvram_shared())
     retro_nvram_save();

   _freedo_Interface(FDP_DESTROY, (void*)0);
   fsCloseIso();

   if (isodrive)
      free(isodrive);
   isodrive = NULL;

   if (videoBuffer)
      free(videoBuffer);
   videoBuffer = NULL;

   if (frame)
      free(frame);
   frame       = NULL;
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void*
retro_get_memory_data(unsigned id)
{
  if(id != RETRO_MEMORY_SAVE_RAM)
    return NULL;
  if(check_env_nvram_shared())
    return NULL;

  return Getp_NVRAM();
}

size_t
retro_get_memory_size(unsigned id)
{
  if(id != RETRO_MEMORY_SAVE_RAM)
      return 0;
  if(check_env_nvram_shared())
    return 0;

   return NVRAM_SIZE;
}

void
retro_init(void)
{
  struct retro_log_callback log;
  unsigned level = 5;
  uint64_t serialization_quirks = RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION;

  if(retro_environment_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
    retro_set_log_printf_cb(log.log);

  retro_environment_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
  retro_environment_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &serialization_quirks);
}

void retro_deinit(void)
{
}

void
retro_reset(void)
{
  if(check_env_nvram_shared())
    retro_nvram_save();

  _freedo_Interface(FDP_DESTROY, NULL);

  currentSector = 0;

  sampleCurrent = 0;
  memset(sampleBuffer, 0, sizeof(int32_t) * TEMP_BUFFER_SIZE);

  check_variables();
  initVideo();

  _freedo_Interface(FDP_INIT, (void*)*fdcCallback);

  nvram_init(Getp_NVRAM());
  if(check_env_nvram_shared())
    retro_nvram_load();
}

void
retro_run(void)
{
  bool updated = false;
  if(retro_environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE,&updated) && updated)
    check_variables();

  update_input();

  _freedo_Interface(FDP_DO_EXECFRAME, frame); /* FDP_DO_EXECFRAME_MT ? */

  video_cb(videoBuffer, videoWidth, videoHeight, videoWidth << 2);
}
