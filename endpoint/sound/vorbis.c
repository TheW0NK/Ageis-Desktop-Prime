// The Ogg Vorbis decoder (stb_vorbis), built on its own: its internal
// names clash with minimp3's.
#include "aegis.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.h"
